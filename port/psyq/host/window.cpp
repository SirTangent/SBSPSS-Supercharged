/*	SDL3 host: window, event pump, and the M2 verification tooling.

	The game owns main() (source/system/main.cpp), so SDL's entry-point
	machinery is bypassed: SDL_MAIN_HANDLED here, SDL_SetMainReady() before
	SDL_Init, and video comes up lazily at the game's first ResetGraph()
	(from VidInit) - the headless/trig test exes link this TU but never
	create a window.

	Tooling (all optional, env-driven; they work even if Vulkan fails,
	because they read emulated VRAM directly):
	  SBSP_DUMP_FRAMES=n[,n...]  write the displayed VRAM region as BMP at
	                             those vblank numbers (sbsp_frame_<n>.bmp)
	  SBSP_DUMP_DIR=<dir>        where to write them (default .)
	  SBSP_EXIT_AFTER=<n>        clean exit(0) at vblank n (the game's
	                             MainLoop has no exit path of its own)
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>	/* SDL_SetMainReady; harmless with SDL_MAIN_HANDLED */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gpu_core.h"
#include "host/diag.h"
#include "host/pump.h"

bool VkPresent_Init(SDL_Window *window);	/* port/psyq/vk/vk_present.cpp */
void VkPresent_Frame(void);

extern "C" void Port_InputHandleEvent(const void *ev);	/* host/input.cpp */
extern "C" void Port_InputFrame(unsigned long vblank);

static SDL_Window	*g_window;
static int			g_videoUp;
static int			g_vkUp;

/* dump/exit config, parsed once */
static int			g_toolingParsed;
static unsigned long g_exitAfter;		/* 0 = off */
static unsigned long g_dumpAt[16];
static int			g_dumpCount;
static const char	*g_dumpDir = ".";
static int			g_frameCrc;			/* SBSP_FRAME_CRC=1: [frame] line per vblank */

static void parseTooling(void)
{
	if (g_toolingParsed)
		return;
	g_toolingParsed = 1;

	const char *e = getenv("SBSP_EXIT_AFTER");
	if (e)
		g_exitAfter = strtoul(e, NULL, 10);

	const char *dir = getenv("SBSP_DUMP_DIR");
	if (dir && *dir)
		g_dumpDir = _strdup(dir);

	const char *fc = getenv("SBSP_FRAME_CRC");
	g_frameCrc = fc && *fc && *fc != '0';

	const char *d = getenv("SBSP_DUMP_FRAMES");
	while (d && *d && g_dumpCount < 16)
	{
		g_dumpAt[g_dumpCount++] = strtoul(d, (char **)&d, 10);
		while (*d == ',' || *d == ' ')
			d++;
	}
}

/*****************************************************************************/
/*	24bpp bottom-up BMP of the currently displayed VRAM region.  */
static void put32(FILE *f, uint32_t v)	{ fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v)	{ fwrite(&v, 2, 1, f); }

static void dumpDisplayBMP(unsigned long vblank)
{
	int w = g_gpu.dispW ? g_gpu.dispW : 512;	/* pixels in BOTH modes:
												   fmv.cpp pre-divides for
												   isrgb24 */
	int h = g_gpu.dispH ? g_gpu.dispH : 256;

	char path[512];
	snprintf(path, sizeof(path), "%s/sbsp_frame_%lu.bmp", g_dumpDir, vblank);
	FILE *f = fopen(path, "wb");
	if (!f)
	{
		fprintf(stderr, "[host] cannot write %s\n", path);
		return;
	}

	int rowBytes = (w * 3 + 3) & ~3;
	uint32_t dataSize = (uint32_t)rowBytes * h;

	fwrite("BM", 2, 1, f);
	put32(f, 54 + dataSize);  put32(f, 0);  put32(f, 54);
	put32(f, 40);  put32(f, (uint32_t)w);  put32(f, (uint32_t)h);
	put16(f, 1);  put16(f, 24);
	put32(f, 0);  put32(f, dataSize);
	put32(f, 2835);  put32(f, 2835);  put32(f, 0);  put32(f, 0);

	unsigned char *row = (unsigned char *)malloc(rowBytes);
	memset(row, 0, rowBytes);
	for (int y = h - 1; y >= 0; y--)
	{
		for (int x = 0; x < w; x++)
		{
			unsigned char rgb[3];
			GPU_ReadDisplayPixelRGB(x, y, rgb);		/* 15bpp or isrgb24 */
			row[x * 3 + 0] = rgb[2];	/* B */
			row[x * 3 + 1] = rgb[1];	/* G */
			row[x * 3 + 2] = rgb[0];	/* R */
		}
		fwrite(row, 1, rowBytes, f);
	}
	free(row);
	fclose(f);
	fprintf(stderr, "[host] wrote %s (%dx%d, vblank %lu)\n", path, w, h, vblank);
}

/*****************************************************************************/
extern "C" void Host_EnsureVideo(void)
{
	if (g_videoUp)
		return;
	g_videoUp = 1;
	parseTooling();

	SDL_SetMainReady();

	/*	audio joins the lazy bring-up here, ahead of the video guards below:
		it needs no window and no video subsystem, and --dump-audio has to
		work on a host with no display at all.  (Not from SpuInit: the unit
		tests drive SpuInit directly and must never open a device.)  */
	extern void Host_EnsureAudio(void);
	Host_EnsureAudio();

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
	{
		fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
		return;
	}

	g_window = SDL_CreateWindow("SpongeBob SquarePants: SuperSponge",
								1024, 512,
								SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	if (!g_window)
	{
		fprintf(stderr, "[host] SDL_CreateWindow failed: %s\n", SDL_GetError());
		return;
	}

	g_vkUp = VkPresent_Init(g_window);
	if (!g_vkUp)
		fprintf(stderr, "[host] Vulkan presenter unavailable - window will stay "
						"black (frame dumps still work)\n");
}

/*****************************************************************************/
extern "C" void Host_VBlank(unsigned long vblankNo)
{
	static int inHere;
	if (inHere)
		return;		/* the game's vblank callback can pump recursively */
	inHere = 1;

	parseTooling();

	if (g_videoUp && g_window)
	{
		SDL_Event ev;
		while (SDL_PollEvent(&ev))
		{
			if (ev.type == SDL_EVENT_QUIT)
			{
				fprintf(stderr, "[host] window closed - exiting\n");
				Port_Exit(PORT_EXIT_CLEAN);
			}
			if (ev.type == SDL_EVENT_GAMEPAD_ADDED ||
				ev.type == SDL_EVENT_GAMEPAD_REMOVED)
				Port_InputHandleEvent(&ev);
		}
	}

	/*	Outside the video gate on purpose.  SBSP_PAD_SCRIPT needs no SDL at
		all, and the degraded no-window mode this file promises (frame dumps
		+ SBSP_EXIT_AFTER on a display-less machine or CI) is exactly where
		scripted input is the ONLY way to navigate - gating it here left
		such runs dumping the title screen forever, without even the
		"[input] SBSP_PAD_SCRIPT: N entries" line to say why.  The keyboard
		and gamepad readers already handle their absence.  */
	Port_InputFrame(vblankNo);	/* rebuild the PS1 pad packet */
	Port_MemWatch();

	for (int i = 0; i < g_dumpCount; i++)
	{
		if (g_dumpAt[i] == vblankNo)
			dumpDisplayBMP(vblankNo);
	}

	if (g_frameCrc)
	{
		int masked;
		uint32_t crc = GPU_DisplayCRC32(&masked);
		fprintf(stderr, "[frame] %lu crc=%08X%s\n", vblankNo, crc, masked ? " masked" : "");
	}

	/*	Uncapped runs would otherwise be re-capped by the presenter's vsync
		wait: present at most ~60 times a second of wall time and let the
		emulated vblanks run ahead.  */
	if (g_vkUp)
	{
		static double lastPresent = -1.0;
		double now = Port_NowSeconds();
		if (!Port_Uncapped() || now - lastPresent >= 1.0 / 60.0)
		{
			lastPresent = now;
			VkPresent_Frame();
		}
	}

	if (g_exitAfter && vblankNo >= g_exitAfter)
	{
		fprintf(stderr, "[host] SBSP_EXIT_AFTER=%lu reached - exiting\n", g_exitAfter);
		Port_Exit(Port_InputAtExit() ? PORT_EXIT_ORACLE : PORT_EXIT_CLEAN);
	}

	inHere = 0;
}
