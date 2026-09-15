/*	Vulkan presenter (M2): puts emulated VRAM on screen.

	Pipeline: whole VRAM (1024x512 R16_UINT) uploaded each emulated vblank
	through a persistently-mapped staging buffer, then a fullscreen-triangle
	pass whose fragment shader unpacks the PS1 15-bit format and crops to
	the DISPENV rect (push constants).  Presentation is FIFO (or MAILBOX /
	IMMEDIATE with vsync off - M8 shell); game speed stays on the pump's QPC vblank clock, so a 144Hz monitor does not speed
	the game up.

	Vulkan is reached through SDL (SDL_Vulkan_LoadLibrary + the returned
	vkGetInstanceProcAddr) - no link dependency on vulkan-1; the runtime
	loader comes from the user's GPU driver.  Sync model is deliberately
	simple for a 60fps 1MB blit: one command buffer, acquire/render
	semaphores, vkQueueWaitIdle per frame.

	Aspect: the PS1's 512x256 mode scans out on a 4:3 CRT, so the image is
	letterboxed to 4:3 regardless of window shape (authentic-first).
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gpu_core.h"

#include "vk/shaders/present.vert.spv.h"
#include "vk/shaders/present.frag.spv.h"

extern "C" void Port_ViewportRect(int mode, int winW, int winH, int dispH, int out[4]);	/* vk/viewport.cpp */
extern "C" int  Port_ScaleMode(void);

/*****************************************************************************/
/*	Hand-rolled loader for exactly the functions we use.  */
#define VK_FNS(F)															\
	F(vkCreateInstance)														\
	F(vkEnumeratePhysicalDevices)											\
	F(vkGetPhysicalDeviceProperties)										\
	F(vkGetPhysicalDeviceQueueFamilyProperties)								\
	F(vkGetPhysicalDeviceSurfaceSupportKHR)									\
	F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)							\
	F(vkGetPhysicalDeviceSurfaceFormatsKHR)									\
	F(vkGetPhysicalDeviceSurfacePresentModesKHR)							\
	F(vkGetPhysicalDeviceMemoryProperties)									\
	F(vkCreateDevice)														\
	F(vkGetDeviceProcAddr)

#define VK_DEV_FNS(F)														\
	F(vkGetDeviceQueue)														\
	F(vkCreateSwapchainKHR)													\
	F(vkDestroySwapchainKHR)												\
	F(vkGetSwapchainImagesKHR)												\
	F(vkAcquireNextImageKHR)												\
	F(vkQueuePresentKHR)													\
	F(vkCreateImage)														\
	F(vkGetImageMemoryRequirements)											\
	F(vkAllocateMemory)														\
	F(vkBindImageMemory)													\
	F(vkCreateImageView)													\
	F(vkDestroyImageView)													\
	F(vkCreateBuffer)														\
	F(vkGetBufferMemoryRequirements)										\
	F(vkBindBufferMemory)													\
	F(vkMapMemory)															\
	F(vkCreateCommandPool)													\
	F(vkAllocateCommandBuffers)												\
	F(vkResetCommandBuffer)													\
	F(vkBeginCommandBuffer)													\
	F(vkEndCommandBuffer)													\
	F(vkQueueSubmit)														\
	F(vkQueueWaitIdle)														\
	F(vkDeviceWaitIdle)														\
	F(vkCmdPipelineBarrier)													\
	F(vkCmdCopyBufferToImage)												\
	F(vkCmdBeginRenderPass)													\
	F(vkCmdEndRenderPass)													\
	F(vkCmdBindPipeline)													\
	F(vkCmdBindDescriptorSets)												\
	F(vkCmdSetViewport)														\
	F(vkCmdSetScissor)														\
	F(vkCmdPushConstants)													\
	F(vkCmdDraw)															\
	F(vkCreateRenderPass)													\
	F(vkCreateFramebuffer)													\
	F(vkDestroyFramebuffer)													\
	F(vkCreateShaderModule)													\
	F(vkDestroyShaderModule)												\
	F(vkCreatePipelineLayout)												\
	F(vkCreateGraphicsPipelines)											\
	F(vkCreateDescriptorSetLayout)											\
	F(vkCreateDescriptorPool)												\
	F(vkAllocateDescriptorSets)												\
	F(vkUpdateDescriptorSets)												\
	F(vkCreateSampler)														\
	F(vkCreateSemaphore)													\
	F(vkDestroySemaphore)													\
	F(vkCreateFence)														\
	F(vkWaitForFences)														\
	F(vkGetFenceStatus)														\
	F(vkResetFences)

#define DECL(name) static PFN_##name p_##name;
VK_FNS(DECL)
VK_DEV_FNS(DECL)
#undef DECL

/*****************************************************************************/
static SDL_Window		*s_window;
static VkInstance		s_instance;
static VkSurfaceKHR		s_surface;
static VkPhysicalDevice	s_phys;
static VkDevice			s_dev;
static VkQueue			s_queue;
static uint32_t			s_queueFamily;
static VkPhysicalDeviceMemoryProperties s_memProps;

/*	Swapchain image counts are the driver's call, not ours: a mailbox-happy or
	HDR-composited driver can hand back far more than the minImageCount+1 we
	ask for, and vkAcquireNextImageKHR then returns indices past the end of a
	fixed array.  Query the real count and refuse loudly if it exceeds this.  */
#define MAX_SWAP_IMAGES		16
/*	Frames the CPU may run ahead of the GPU.  Each needs its own command
	buffer, staging buffer and acquire semaphore.  */
#define FRAMES_IN_FLIGHT	2

static VkSwapchainKHR	s_swapchain;
static VkFormat			s_swapFormat;
static VkExtent2D		s_swapExtent;
static uint32_t			s_swapCount;
static uint32_t			s_swapValid;	/* views/fbs actually built; 0 = unusable */
static VkImage			s_swapImages[MAX_SWAP_IMAGES];
static VkImageView		s_swapViews[MAX_SWAP_IMAGES];
static VkFramebuffer	s_swapFbs[MAX_SWAP_IMAGES];

static VkRenderPass		s_renderPass;
static VkPipelineLayout	s_pipeLayout;
static VkPipeline		s_pipeline;
static VkDescriptorSetLayout s_dsLayout;
static VkDescriptorSet	s_dset;
static VkSampler		s_sampler;

static VkImage			s_vramImage;
static VkImageView		s_vramView;
static VkBuffer			s_staging[FRAMES_IN_FLIGHT];
static void				*s_stagingMap[FRAMES_IN_FLIGHT];

static VkCommandPool	s_cmdPool;
static VkCommandBuffer	s_cmd[FRAMES_IN_FLIGHT];
static VkSemaphore		s_semAcquire[FRAMES_IN_FLIGHT];
static VkFence			s_fence[FRAMES_IN_FLIGHT];
static VkSemaphore		s_semRender[MAX_SWAP_IMAGES];	/* per image: present waits on it */
static uint32_t			s_frame;						/* frame slot cursor */

static int				s_vramInShaderLayout;	/* image layout tracking */

struct PushConsts { int32_t disp[4]; int32_t mask; int32_t rgb24; };

#define CHECK(expr)															\
	do {																	\
		VkResult _r = (expr);												\
		if (_r != VK_SUCCESS)												\
		{																	\
			fprintf(stderr, "[vk] %s failed (%d)\n", #expr, (int)_r);		\
			return false;													\
		}																	\
	} while (0)

/*	Enumeration calls that write into a fixed-size array report VK_INCOMPLETE
	when the array was too small.  That is a successful call returning a
	truncated list - never a failure - so it must not travel the CHECK path
	(which at the swapchain sites would abandon recreation half-done).  */
#define CHECK_ENUM(expr)													\
	do {																	\
		VkResult _r = (expr);												\
		if (_r != VK_SUCCESS && _r != VK_INCOMPLETE)						\
		{																	\
			fprintf(stderr, "[vk] %s failed (%d)\n", #expr, (int)_r);		\
			return false;													\
		}																	\
	} while (0)

/*****************************************************************************/
static uint32_t findMemType(uint32_t typeBits, VkMemoryPropertyFlags want)
{
	for (uint32_t i = 0; i < s_memProps.memoryTypeCount; i++)
		if ((typeBits & (1u << i)) &&
			(s_memProps.memoryTypes[i].propertyFlags & want) == want)
			return i;
	return 0;
}

static VkColorSpaceKHR s_swapColorSpace;

static bool pickSurfaceFormat(void)
{
	VkSurfaceFormatKHR fmts[32];
	uint32_t nf = 32;
	CHECK_ENUM(p_vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &nf, fmts));
	if (nf == 0)
	{
		fprintf(stderr, "[vk] surface reports no formats\n");
		return false;
	}
	/* VK_INCOMPLETE just means the driver had more; nf counts what it wrote */
	VkSurfaceFormatKHR pick = fmts[0];
	for (uint32_t i = 0; i < nf; i++)
		if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM)
			pick = fmts[i];
	s_swapFormat     = pick.format;
	s_swapColorSpace = pick.colorSpace;
	return true;
}

static void destroySwapResources(void)
{
	for (uint32_t i = 0; i < s_swapValid; i++)
	{
		p_vkDestroyFramebuffer(s_dev, s_swapFbs[i], NULL);
		p_vkDestroyImageView(s_dev, s_swapViews[i], NULL);
	}
	s_swapValid = 0;
}

/*	A present whose swapchain went out of date may never consume the render
	semaphore it was told to wait on, leaving it signalled with no matching
	wait.  Reusing such a semaphore is undefined, so retire the whole set
	whenever the swapchain is rebuilt.  (No-op on the very first build - they
	do not exist yet.)  */
static bool refreshRenderSemaphores(void)
{
	if (!s_semRender[0])
		return true;

	VkSemaphoreCreateInfo sci;
	memset(&sci, 0, sizeof(sci));
	sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	for (int i = 0; i < MAX_SWAP_IMAGES; i++)
	{
		p_vkDestroySemaphore(s_dev, s_semRender[i], NULL);
		s_semRender[i] = VK_NULL_HANDLE;
		CHECK(p_vkCreateSemaphore(s_dev, &sci, NULL, &s_semRender[i]));
	}
	return true;
}

/*	SBSP_VSYNC (sbsp.ini `vsync`, --vsync; M8 shell).  FIFO is the only mode
	every driver must offer and is vsync by definition.  With vsync off the
	preference is MAILBOX (the newest frame replaces a queued one, no tear)
	then IMMEDIATE (tears, never waits); a driver offering neither keeps
	FIFO and says so.  The choice never blocks emulated time either way -
	acquire is timeout-0 and a busy frame is skipped (see VkPresent_Frame).  */
static VkPresentModeKHR choosePresentMode(void)
{
	static int vsync = -1;
	if (vsync < 0)
	{
		const char *e = getenv("SBSP_VSYNC");
		vsync = !(e && *e && (*e == '0' || _stricmp(e, "off") == 0 || _stricmp(e, "no") == 0 ||
							  _stricmp(e, "false") == 0));
	}
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	const char *name = "FIFO (vsync)";
	if (!vsync)
	{
		VkPresentModeKHR modes[16];
		uint32_t n = 16;
		if (p_vkGetPhysicalDeviceSurfacePresentModesKHR(s_phys, s_surface, &n, modes) >= 0)
		{
			bool mailbox = false, immediate = false;
			for (uint32_t i = 0; i < n; i++)
			{
				if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)   mailbox   = true;
				if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) immediate = true;
			}
			if (mailbox)        { mode = VK_PRESENT_MODE_MAILBOX_KHR;   name = "MAILBOX (vsync off)"; }
			else if (immediate) { mode = VK_PRESENT_MODE_IMMEDIATE_KHR; name = "IMMEDIATE (vsync off)"; }
			else                name = "FIFO (vsync off requested, but the driver offers no other mode)";
		}
	}
	static bool reported;
	if (!reported)
	{
		reported = true;
		fprintf(stderr, "[host] present mode: %s\n", name);
	}
	return mode;
}

static bool buildSwapchain(void)
{
	VkSurfaceCapabilitiesKHR caps;
	CHECK(p_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surface, &caps));

	int pw = 0, ph = 0;
	SDL_GetWindowSizeInPixels(s_window, &pw, &ph);
	VkExtent2D extent = caps.currentExtent;
	if (extent.width == 0xFFFFFFFFu)
	{
		extent.width  = (uint32_t)pw;
		extent.height = (uint32_t)ph;
	}
	if (extent.width == 0 || extent.height == 0)
		return false;	/* minimized - retry later */

	uint32_t imgCount = caps.minImageCount + 1;
	if (caps.maxImageCount && imgCount > caps.maxImageCount)
		imgCount = caps.maxImageCount;

	VkSwapchainCreateInfoKHR sci;
	memset(&sci, 0, sizeof(sci));
	sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	sci.surface          = s_surface;
	sci.minImageCount    = imgCount;
	sci.imageFormat      = s_swapFormat;
	sci.imageColorSpace  = s_swapColorSpace;
	sci.imageExtent      = extent;
	sci.imageArrayLayers = 1;
	sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	sci.preTransform     = caps.currentTransform;
	sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	sci.presentMode      = choosePresentMode();
	sci.clipped          = VK_TRUE;
	sci.oldSwapchain     = s_swapchain;

	VkSwapchainKHR newChain;
	CHECK(p_vkCreateSwapchainKHR(s_dev, &sci, NULL, &newChain));

	if (s_swapchain)
	{
		p_vkDeviceWaitIdle(s_dev);
		destroySwapResources();
		p_vkDestroySwapchainKHR(s_dev, s_swapchain, NULL);
	}
	s_swapchain  = newChain;	/* owned from here on, even if the rest fails */
	s_swapExtent = extent;
	if (!refreshRenderSemaphores())
		return false;

	/*	Count first, then fetch: asking for at most MAX and taking VK_INCOMPLETE
		as "good enough" would leave images we never build framebuffers for,
		while acquire could still hand back their indices.  */
	uint32_t nImages = 0;
	CHECK(p_vkGetSwapchainImagesKHR(s_dev, s_swapchain, &nImages, NULL));
	if (nImages == 0 || nImages > MAX_SWAP_IMAGES)
	{
		fprintf(stderr, "[vk] driver created %u swapchain images (limit %d)\n",
				nImages, MAX_SWAP_IMAGES);
		return false;
	}
	s_swapCount = nImages;
	CHECK(p_vkGetSwapchainImagesKHR(s_dev, s_swapchain, &s_swapCount, s_swapImages));

	for (uint32_t i = 0; i < s_swapCount; i++)
	{
		VkImageViewCreateInfo ivci;
		memset(&ivci, 0, sizeof(ivci));
		ivci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivci.image    = s_swapImages[i];
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format   = s_swapFormat;
		ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ivci.subresourceRange.levelCount = 1;
		ivci.subresourceRange.layerCount = 1;
		CHECK(p_vkCreateImageView(s_dev, &ivci, NULL, &s_swapViews[i]));

		VkFramebufferCreateInfo fbci;
		memset(&fbci, 0, sizeof(fbci));
		fbci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbci.renderPass      = s_renderPass;
		fbci.attachmentCount = 1;
		fbci.pAttachments    = &s_swapViews[i];
		fbci.width           = extent.width;
		fbci.height          = extent.height;
		fbci.layers          = 1;
		CHECK(p_vkCreateFramebuffer(s_dev, &fbci, NULL, &s_swapFbs[i]));

		s_swapValid = i + 1;	/* only these are safe to use - and to destroy */
	}
	return true;
}

/*	Wrapper enforcing the invariant the frame loop relies on: either the
	swapchain and its framebuffers are entirely valid (s_swapValid == count),
	or s_swapValid is 0 and nothing stale is left to be drawn into.  A failure
	partway through recreation is transient (a resize mid-flight, a driver
	still settling) - the next frame retries.  */
static bool createSwapchain(void)
{
	if (buildSwapchain() && s_swapValid == s_swapCount)
		return true;
	p_vkDeviceWaitIdle(s_dev);	/* nothing may still be rendering into them */
	destroySwapResources();
	return false;
}

/*****************************************************************************/
bool VkPresent_Init(SDL_Window *window)
{
	s_window = window;

	if (!SDL_Vulkan_LoadLibrary(NULL))
	{
		fprintf(stderr, "[vk] SDL_Vulkan_LoadLibrary: %s\n", SDL_GetError());
		return false;
	}
	PFN_vkGetInstanceProcAddr gipa =
		(PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
	if (!gipa)
		return false;

	/* instance */
	Uint32 nExt = 0;
	const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&nExt);

	VkApplicationInfo ai;
	memset(&ai, 0, sizeof(ai));
	ai.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	ai.pApplicationName = "SBSPSS";
	ai.apiVersion       = VK_API_VERSION_1_0;

	VkInstanceCreateInfo ici;
	memset(&ici, 0, sizeof(ici));
	ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	ici.pApplicationInfo        = &ai;
	ici.enabledExtensionCount   = nExt;
	ici.ppEnabledExtensionNames = ext;

	PFN_vkCreateInstance createInstance =
		(PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	if (!createInstance || createInstance(&ici, NULL, &s_instance) != VK_SUCCESS)
	{
		fprintf(stderr, "[vk] vkCreateInstance failed\n");
		return false;
	}

#define LOAD(name)															\
	p_##name = (PFN_##name)gipa(s_instance, #name);							\
	if (!p_##name) { fprintf(stderr, "[vk] missing " #name "\n"); return false; }
	VK_FNS(LOAD)
#undef LOAD

	if (!SDL_Vulkan_CreateSurface(s_window, s_instance, NULL, &s_surface))
	{
		fprintf(stderr, "[vk] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
		return false;
	}

	/* physical device + queue family (graphics + present) */
	VkPhysicalDevice devs[16];
	uint32_t nd = 16;
	CHECK_ENUM(p_vkEnumeratePhysicalDevices(s_instance, &nd, devs));
	if (nd == 0)
	{
		fprintf(stderr, "[vk] no Vulkan devices\n");
		return false;
	}
	s_phys = devs[0];
	s_queueFamily = ~0u;
	for (uint32_t d = 0; d < nd && s_queueFamily == ~0u; d++)
	{
		VkQueueFamilyProperties qf[16];
		uint32_t nq = 16;
		p_vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
		for (uint32_t q = 0; q < nq; q++)
		{
			VkBool32 present = VK_FALSE;
			p_vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, s_surface, &present);
			if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
			{
				s_phys = devs[d];
				s_queueFamily = q;
				break;
			}
		}
	}
	if (s_queueFamily == ~0u)
	{
		fprintf(stderr, "[vk] no graphics+present queue family\n");
		return false;
	}
	p_vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memProps);

	VkPhysicalDeviceProperties props;
	p_vkGetPhysicalDeviceProperties(s_phys, &props);
	fprintf(stderr, "[vk] using %s\n", props.deviceName);

	/* device + queue */
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci;
	memset(&qci, 0, sizeof(qci));
	qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = s_queueFamily;
	qci.queueCount       = 1;
	qci.pQueuePriorities = &prio;

	const char *devExt[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
	VkDeviceCreateInfo dci;
	memset(&dci, 0, sizeof(dci));
	dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount    = 1;
	dci.pQueueCreateInfos       = &qci;
	dci.enabledExtensionCount   = 1;
	dci.ppEnabledExtensionNames = devExt;
	CHECK(p_vkCreateDevice(s_phys, &dci, NULL, &s_dev));

#define LOADD(name)															\
	p_##name = (PFN_##name)p_vkGetDeviceProcAddr(s_dev, #name);				\
	if (!p_##name) { fprintf(stderr, "[vk] missing " #name "\n"); return false; }
	VK_DEV_FNS(LOADD)
#undef LOADD

	p_vkGetDeviceQueue(s_dev, s_queueFamily, 0, &s_queue);

	/* VRAM image (R16_UINT 1024x512) + staging buffer */
	{
		VkImageCreateInfo ici2;
		memset(&ici2, 0, sizeof(ici2));
		ici2.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici2.imageType     = VK_IMAGE_TYPE_2D;
		ici2.format        = VK_FORMAT_R16_UINT;
		ici2.extent.width  = VRAM_W;
		ici2.extent.height = VRAM_H;
		ici2.extent.depth  = 1;
		ici2.mipLevels     = 1;
		ici2.arrayLayers   = 1;
		ici2.samples       = VK_SAMPLE_COUNT_1_BIT;
		ici2.tiling        = VK_IMAGE_TILING_OPTIMAL;
		ici2.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ici2.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		CHECK(p_vkCreateImage(s_dev, &ici2, NULL, &s_vramImage));

		VkMemoryRequirements mr;
		p_vkGetImageMemoryRequirements(s_dev, s_vramImage, &mr);
		VkMemoryAllocateInfo mai;
		memset(&mai, 0, sizeof(mai));
		mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mai.allocationSize  = mr.size;
		mai.memoryTypeIndex = findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory mem;
		CHECK(p_vkAllocateMemory(s_dev, &mai, NULL, &mem));
		CHECK(p_vkBindImageMemory(s_dev, s_vramImage, mem, 0));

		VkImageViewCreateInfo vci;
		memset(&vci, 0, sizeof(vci));
		vci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vci.image    = s_vramImage;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format   = VK_FORMAT_R16_UINT;
		vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		vci.subresourceRange.levelCount = 1;
		vci.subresourceRange.layerCount = 1;
		CHECK(p_vkCreateImageView(s_dev, &vci, NULL, &s_vramView));

		/*	One staging buffer per in-flight frame: the CPU fills slot N's
			while the GPU may still be reading slot N-1's.  */
		for (int f = 0; f < FRAMES_IN_FLIGHT; f++)
		{
			VkBufferCreateInfo bci;
			memset(&bci, 0, sizeof(bci));
			bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bci.size  = VRAM_W * VRAM_H * 2;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
			CHECK(p_vkCreateBuffer(s_dev, &bci, NULL, &s_staging[f]));

			p_vkGetBufferMemoryRequirements(s_dev, s_staging[f], &mr);
			mai.allocationSize  = mr.size;
			mai.memoryTypeIndex = findMemType(mr.memoryTypeBits,
								VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VkDeviceMemory smem;
			CHECK(p_vkAllocateMemory(s_dev, &mai, NULL, &smem));
			CHECK(p_vkBindBufferMemory(s_dev, s_staging[f], smem, 0));
			CHECK(p_vkMapMemory(s_dev, smem, 0, VK_WHOLE_SIZE, 0, &s_stagingMap[f]));
		}
	}

	/* surface format first - the render pass must match the swapchain */
	if (!pickSurfaceFormat())
		return false;

	/* render pass */
	{
		VkAttachmentDescription att;
		memset(&att, 0, sizeof(att));
		att.format        = s_swapFormat;
		att.samples       = VK_SAMPLE_COUNT_1_BIT;
		att.loadOp        = VK_ATTACHMENT_LOAD_OP_CLEAR;
		att.storeOp       = VK_ATTACHMENT_STORE_OP_STORE;
		att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att.stencilStoreOp= VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		att.finalLayout   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference ref;
		ref.attachment = 0;
		ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription sub;
		memset(&sub, 0, sizeof(sub));
		sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
		sub.colorAttachmentCount = 1;
		sub.pColorAttachments    = &ref;

		VkRenderPassCreateInfo rpci;
		memset(&rpci, 0, sizeof(rpci));
		rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		rpci.attachmentCount = 1;
		rpci.pAttachments    = &att;
		rpci.subpassCount    = 1;
		rpci.pSubpasses      = &sub;
		CHECK(p_vkCreateRenderPass(s_dev, &rpci, NULL, &s_renderPass));
	}

	if (!createSwapchain())
		return false;

	/* descriptors: one usampler2D */
	{
		VkSamplerCreateInfo smci;
		memset(&smci, 0, sizeof(smci));
		smci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		smci.magFilter    = VK_FILTER_NEAREST;
		smci.minFilter    = VK_FILTER_NEAREST;
		smci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		smci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		CHECK(p_vkCreateSampler(s_dev, &smci, NULL, &s_sampler));

		VkDescriptorSetLayoutBinding bind;
		memset(&bind, 0, sizeof(bind));
		bind.binding         = 0;
		bind.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		bind.descriptorCount = 1;
		bind.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

		VkDescriptorSetLayoutCreateInfo dlci;
		memset(&dlci, 0, sizeof(dlci));
		dlci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		dlci.bindingCount = 1;
		dlci.pBindings    = &bind;
		CHECK(p_vkCreateDescriptorSetLayout(s_dev, &dlci, NULL, &s_dsLayout));

		VkDescriptorPoolSize psz;
		psz.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		psz.descriptorCount = 1;
		VkDescriptorPoolCreateInfo dpci;
		memset(&dpci, 0, sizeof(dpci));
		dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dpci.maxSets       = 1;
		dpci.poolSizeCount = 1;
		dpci.pPoolSizes    = &psz;
		VkDescriptorPool pool;
		CHECK(p_vkCreateDescriptorPool(s_dev, &dpci, NULL, &pool));

		VkDescriptorSetAllocateInfo dsai;
		memset(&dsai, 0, sizeof(dsai));
		dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dsai.descriptorPool     = pool;
		dsai.descriptorSetCount = 1;
		dsai.pSetLayouts        = &s_dsLayout;
		CHECK(p_vkAllocateDescriptorSets(s_dev, &dsai, &s_dset));

		VkDescriptorImageInfo dii;
		dii.sampler     = s_sampler;
		dii.imageView   = s_vramView;
		dii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		VkWriteDescriptorSet wds;
		memset(&wds, 0, sizeof(wds));
		wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wds.dstSet          = s_dset;
		wds.dstBinding      = 0;
		wds.descriptorCount = 1;
		wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wds.pImageInfo      = &dii;
		p_vkUpdateDescriptorSets(s_dev, 1, &wds, 0, NULL);
	}

	/* pipeline */
	{
		VkShaderModuleCreateInfo smc;
		memset(&smc, 0, sizeof(smc));
		smc.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		smc.codeSize = sizeof(g_present_vert_spv);
		smc.pCode    = g_present_vert_spv;
		VkShaderModule vs, fs;
		CHECK(p_vkCreateShaderModule(s_dev, &smc, NULL, &vs));
		smc.codeSize = sizeof(g_present_frag_spv);
		smc.pCode    = g_present_frag_spv;
		CHECK(p_vkCreateShaderModule(s_dev, &smc, NULL, &fs));

		VkPushConstantRange pcr;
		pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		pcr.offset     = 0;
		pcr.size       = sizeof(PushConsts);

		VkPipelineLayoutCreateInfo plci;
		memset(&plci, 0, sizeof(plci));
		plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		plci.setLayoutCount         = 1;
		plci.pSetLayouts            = &s_dsLayout;
		plci.pushConstantRangeCount = 1;
		plci.pPushConstantRanges    = &pcr;
		CHECK(p_vkCreatePipelineLayout(s_dev, &plci, NULL, &s_pipeLayout));

		VkPipelineShaderStageCreateInfo stages[2];
		memset(stages, 0, sizeof(stages));
		stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
		stages[0].module = vs;
		stages[0].pName  = "main";
		stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
		stages[1].module = fs;
		stages[1].pName  = "main";

		VkPipelineVertexInputStateCreateInfo vin;
		memset(&vin, 0, sizeof(vin));
		vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		VkPipelineInputAssemblyStateCreateInfo ia;
		memset(&ia, 0, sizeof(ia));
		ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

		VkPipelineViewportStateCreateInfo vp;
		memset(&vp, 0, sizeof(vp));
		vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		vp.viewportCount = 1;
		vp.scissorCount  = 1;

		VkPipelineRasterizationStateCreateInfo rs;
		memset(&rs, 0, sizeof(rs));
		rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rs.polygonMode = VK_POLYGON_MODE_FILL;
		rs.cullMode    = VK_CULL_MODE_NONE;
		rs.lineWidth   = 1.0f;

		VkPipelineMultisampleStateCreateInfo ms;
		memset(&ms, 0, sizeof(ms));
		ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState cba;
		memset(&cba, 0, sizeof(cba));
		cba.colorWriteMask = 0xF;

		VkPipelineColorBlendStateCreateInfo cb;
		memset(&cb, 0, sizeof(cb));
		cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		cb.attachmentCount = 1;
		cb.pAttachments    = &cba;

		VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dsi;
		memset(&dsi, 0, sizeof(dsi));
		dsi.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dsi.dynamicStateCount = 2;
		dsi.pDynamicStates    = dyn;

		VkGraphicsPipelineCreateInfo gpci;
		memset(&gpci, 0, sizeof(gpci));
		gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		gpci.stageCount          = 2;
		gpci.pStages             = stages;
		gpci.pVertexInputState   = &vin;
		gpci.pInputAssemblyState = &ia;
		gpci.pViewportState      = &vp;
		gpci.pRasterizationState = &rs;
		gpci.pMultisampleState   = &ms;
		gpci.pColorBlendState    = &cb;
		gpci.pDynamicState       = &dsi;
		gpci.layout              = s_pipeLayout;
		gpci.renderPass          = s_renderPass;
		CHECK(p_vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &gpci, NULL, &s_pipeline));

		p_vkDestroyShaderModule(s_dev, vs, NULL);
		p_vkDestroyShaderModule(s_dev, fs, NULL);
	}

	/* command buffer + semaphores */
	{
		VkCommandPoolCreateInfo cpci;
		memset(&cpci, 0, sizeof(cpci));
		cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		cpci.queueFamilyIndex = s_queueFamily;
		CHECK(p_vkCreateCommandPool(s_dev, &cpci, NULL, &s_cmdPool));

		VkCommandBufferAllocateInfo cbai;
		memset(&cbai, 0, sizeof(cbai));
		cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cbai.commandPool        = s_cmdPool;
		cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cbai.commandBufferCount = FRAMES_IN_FLIGHT;
		CHECK(p_vkAllocateCommandBuffers(s_dev, &cbai, s_cmd));

		VkSemaphoreCreateInfo sci2;
		memset(&sci2, 0, sizeof(sci2));
		sci2.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fci;
		memset(&fci, 0, sizeof(fci));
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;	/* frame 0 must not block */

		for (int f = 0; f < FRAMES_IN_FLIGHT; f++)
		{
			CHECK(p_vkCreateSemaphore(s_dev, &sci2, NULL, &s_semAcquire[f]));
			CHECK(p_vkCreateFence(s_dev, &fci, NULL, &s_fence[f]));
		}
		/*	Render-finished semaphores are per swapchain IMAGE, not per frame:
			present waits on one, and it stays pending until that image comes
			back round through acquire.  */
		for (int i = 0; i < MAX_SWAP_IMAGES; i++)
			CHECK(p_vkCreateSemaphore(s_dev, &sci2, NULL, &s_semRender[i]));
	}

	return true;
}

/*****************************************************************************/
void VkPresent_Frame(void)
{
	if (!s_dev)
		return;

	/*	handle resize (and any earlier failed recreation) before touching the
		swapchain: s_swapValid == 0 means the framebuffers are gone  */
	int pw = 0, ph = 0;
	SDL_GetWindowSizeInPixels(s_window, &pw, &ph);
	if (pw == 0 || ph == 0)
		return;		/* minimized */
	if (s_swapValid == 0 ||
		(uint32_t)pw != s_swapExtent.width || (uint32_t)ph != s_swapExtent.height)
	{
		if (!createSwapchain())
			return;
	}

	/*	NEVER block emulated time on the display (M4).  This runs once per
		delivered emulated vblank, and a game frame passes ~6 pump sites -
		when each of those waited a real FIFO refresh here, one game frame
		cost ~6 refreshes and the game ran at ~10fps (getFramesSinceLast~6).
		So: if this slot's previous frame is still on the GPU, or the FIFO
		swapchain has no image free yet, SKIP the present entirely - the
		next vblank will show the newer VRAM anyway.  Present rate
		self-limits to the display; the emulated vblank clock stays purely
		QPC-paced.  (Frame dumps are unaffected - they read VRAM directly.)

		The fence is reset only once we are certain we will submit - an
		abandoned frame must leave it signalled, or the next lap round the
		ring waits forever.  */
	uint32_t slot = s_frame % FRAMES_IN_FLIGHT;
	if (p_vkGetFenceStatus(s_dev, s_fence[slot]) != VK_SUCCESS)
		return;		/* slot busy on the GPU - skip, don't stall */

	uint32_t imgIdx = 0;
	VkResult r = p_vkAcquireNextImageKHR(s_dev, s_swapchain, 0,
										 s_semAcquire[slot], VK_NULL_HANDLE, &imgIdx);
	int recreateAfter = 0;
	if (r == VK_NOT_READY || r == VK_TIMEOUT)
		return;		/* no image free this instant - skip (semaphore unsignalled) */
	if (r == VK_ERROR_OUT_OF_DATE_KHR)
	{
		createSwapchain();
		return;
	}
	if (r == VK_SUBOPTIMAL_KHR)
	{
		/*	SUBOPTIMAL is a SUCCESSFUL acquire: the image is ours and the
			semaphore has a signal pending.  Returning here would abandon that
			signal and the next frame would wait on an already-pending
			semaphore - a valid-usage violation.  Draw this frame, then
			rebuild.  */
		recreateAfter = 1;
	}
	else if (r != VK_SUCCESS)
		return;

	/* fresh VRAM into this slot's staging buffer */
	memcpy(s_stagingMap[slot], g_vram, sizeof(g_vram));

	VkCommandBuffer cmd = s_cmd[slot];
	VkCommandBufferBeginInfo bi;
	memset(&bi, 0, sizeof(bi));
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	p_vkResetCommandBuffer(cmd, 0);
	p_vkBeginCommandBuffer(cmd, &bi);

	/* VRAM image: (whatever) -> TRANSFER_DST, copy, -> SHADER_READ */
	VkImageMemoryBarrier bar;
	memset(&bar, 0, sizeof(bar));
	bar.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	bar.srcAccessMask       = s_vramInShaderLayout ? VK_ACCESS_SHADER_READ_BIT : 0;
	bar.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar.oldLayout           = s_vramInShaderLayout ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
												   : VK_IMAGE_LAYOUT_UNDEFINED;
	bar.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	bar.image               = s_vramImage;
	bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	bar.subresourceRange.levelCount = 1;
	bar.subresourceRange.layerCount = 1;
	/*	src stage FRAGMENT_SHADER also orders this against the PREVIOUS frame's
		sampling of the same image: for one queue, a barrier's first scope
		covers everything submitted earlier.  That is what makes a single
		shared VRAM image safe with frames in flight.  */
	p_vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 0, NULL, 1, &bar);

	VkBufferImageCopy cpy;
	memset(&cpy, 0, sizeof(cpy));
	cpy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	cpy.imageSubresource.layerCount = 1;
	cpy.imageExtent.width  = VRAM_W;
	cpy.imageExtent.height = VRAM_H;
	cpy.imageExtent.depth  = 1;
	p_vkCmdCopyBufferToImage(cmd, s_staging[slot], s_vramImage,
							 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cpy);

	bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	p_vkCmdPipelineBarrier(cmd,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		0, 0, NULL, 0, NULL, 1, &bar);
	s_vramInShaderLayout = 1;

	/* render pass: clear + letterboxed fullscreen triangle */
	VkClearValue clear;
	memset(&clear, 0, sizeof(clear));
	VkRenderPassBeginInfo rbi;
	memset(&rbi, 0, sizeof(rbi));
	rbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass        = s_renderPass;
	rbi.framebuffer       = s_swapFbs[imgIdx];
	rbi.renderArea.extent = s_swapExtent;
	rbi.clearValueCount   = 1;
	rbi.pClearValues      = &clear;
	p_vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

	p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_pipeline);
	p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
							  s_pipeLayout, 0, 1, &s_dset, 0, NULL);

	/*	4:3 letterbox (fit), whole-frame multiples (integer) or the full
		window (stretch) - vk/viewport.cpp, SBSP_SCALE.  The PS1's high-res
		512-wide mode still scans out on a 4:3 CRT: non-square pixels are
		authentic, so 4:3 is the shape in every mode but stretch.  */
	{
		int rect[4];
		Port_ViewportRect(Port_ScaleMode(), (int)s_swapExtent.width, (int)s_swapExtent.height,
						  g_gpu.dispH ? g_gpu.dispH : 256, rect);
		VkViewport vpp;
		vpp.x        = (float)rect[0];
		vpp.y        = (float)rect[1];
		vpp.width    = (float)rect[2];
		vpp.height   = (float)rect[3];
		vpp.minDepth = 0.0f;
		vpp.maxDepth = 1.0f;
		p_vkCmdSetViewport(cmd, 0, 1, &vpp);

		VkRect2D sc;
		sc.offset.x      = 0;
		sc.offset.y      = 0;
		sc.extent        = s_swapExtent;
		p_vkCmdSetScissor(cmd, 0, 1, &sc);
	}

	PushConsts pc;
	pc.disp[0] = g_gpu.dispX;
	pc.disp[1] = g_gpu.dispY;
	pc.disp[2] = g_gpu.dispW ? g_gpu.dispW : 512;
	pc.disp[3] = g_gpu.dispH ? g_gpu.dispH : 256;
	pc.mask    = g_gpu.dispMask;
	pc.rgb24   = g_gpu.dispRgb24;
	p_vkCmdPushConstants(cmd, s_pipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
						 0, sizeof(pc), &pc);

	p_vkCmdDraw(cmd, 3, 1, 0, 0);
	p_vkCmdEndRenderPass(cmd);
	p_vkEndCommandBuffer(cmd);

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo si;
	memset(&si, 0, sizeof(si));
	si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.waitSemaphoreCount   = 1;
	si.pWaitSemaphores      = &s_semAcquire[slot];
	si.pWaitDstStageMask    = &waitStage;
	si.commandBufferCount   = 1;
	si.pCommandBuffers      = &cmd;
	si.signalSemaphoreCount = 1;
	si.pSignalSemaphores    = &s_semRender[imgIdx];

	p_vkResetFences(s_dev, 1, &s_fence[slot]);
	if (p_vkQueueSubmit(s_queue, 1, &si, s_fence[slot]) != VK_SUCCESS)
	{
		/*	nothing was queued against the fence - drain and re-signal it by
			hand so the ring stays consistent  */
		p_vkQueueWaitIdle(s_queue);
		p_vkDeviceWaitIdle(s_dev);
		p_vkResetFences(s_dev, 1, &s_fence[slot]);
		p_vkQueueSubmit(s_queue, 0, NULL, s_fence[slot]);
		return;
	}
	s_frame++;

	VkPresentInfoKHR pi;
	memset(&pi, 0, sizeof(pi));
	pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores    = &s_semRender[imgIdx];
	pi.swapchainCount     = 1;
	pi.pSwapchains        = &s_swapchain;
	pi.pImageIndices      = &imgIdx;
	r = p_vkQueuePresentKHR(s_queue, &pi);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
		recreateAfter = 1;

	/*	Recreation waits until here: createSwapchain drains the device, so the
		frame we just submitted completes first and nothing is abandoned.  */
	if (recreateAfter)
		createSwapchain();
}
