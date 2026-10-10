/*	Unit test for the frame dumper (port/psyq/host/framedump.cpp): the raw
	BMP is the display rect pixel for pixel, SetDispMask(0) gives a black
	file of the same size (issue #27), and the 4:3 dump (issue #53) is the
	window's k=3 shape with every source pixel replicated 2 across by 3
	down.  Writes to %TEMP%, no window, no GPU.
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "gpu/gpu_core.h"

extern "C" int  Host_WriteDisplayBMP(const char *path, int aspect);
extern "C" void Host_DumpAspectSize(int srcW, int srcH, int *outW, int *outH);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

struct Bmp
{
	uint32_t		fileSize, dataOff, dataSize;
	int				w, h, bpp;
	unsigned char	*bytes;		/* the whole file */
	long			len;
};

static uint32_t rd32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static bool load(const char *path, Bmp *b)
{
	memset(b, 0, sizeof(*b));
	FILE *f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	b->len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	b->bytes = (unsigned char *)std::malloc(b->len);
	bool ok = std::fread(b->bytes, 1, b->len, f) == (size_t)b->len;
	std::fclose(f);
	if (!ok || b->len < 54 || b->bytes[0] != 'B' || b->bytes[1] != 'M')
		return false;
	b->fileSize = rd32(b->bytes + 2);
	b->dataOff  = rd32(b->bytes + 10);
	b->w        = (int)rd32(b->bytes + 18);
	b->h        = (int)rd32(b->bytes + 22);
	b->bpp      = b->bytes[28] | (b->bytes[29] << 8);
	b->dataSize = rd32(b->bytes + 34);
	return true;
}

/*	RGB at (x,y), y counted from the top (the file is bottom-up BGR)  */
static void pixel(const Bmp *b, int x, int y, unsigned char rgb[3])
{
	int rowBytes = (b->w * 3 + 3) & ~3;
	const unsigned char *p = b->bytes + b->dataOff + (size_t)(b->h - 1 - y) * rowBytes + x * 3;
	rgb[0] = p[2];
	rgb[1] = p[1];
	rgb[2] = p[0];
}

static void setDisplay(int x, int y, int w, int h, int mask)
{
	memset(&g_gpu, 0, sizeof(g_gpu));
	g_gpu.dispX = x;
	g_gpu.dispY = y;
	g_gpu.dispW = w;
	g_gpu.dispH = h;
	g_gpu.dispMask = mask;
}

int main(void)
{
	/*	every halfword different, all three channels busy  */
	for (int y = 0; y < VRAM_H; y++)
		for (int x = 0; x < VRAM_W; x++)
			g_vram[y][x] = (uint16_t)((x * 7 + y * 131) ^ (y << 5));

	const char *tmp = std::getenv("TEMP");
	if (!tmp || !*tmp)
		tmp = ".";
	char rawPath[600], maskPath[600], aspPath[600], aspMaskPath[600];
	std::snprintf(rawPath,     sizeof(rawPath),     "%s/framedump_test_raw.bmp", tmp);
	std::snprintf(maskPath,    sizeof(maskPath),    "%s/framedump_test_masked.bmp", tmp);
	std::snprintf(aspPath,     sizeof(aspPath),     "%s/framedump_test_4x3.bmp", tmp);
	std::snprintf(aspMaskPath, sizeof(aspMaskPath), "%s/framedump_test_4x3_masked.bmp", tmp);

	/*	1. mask=1, raw: header and every pixel from GPU_ReadDisplayPixelRGB  */
	setDisplay(512, 256, 512, 256, 1);
	check(Host_WriteDisplayBMP(rawPath, 0) == 1, "raw dump written");
	Bmp raw;
	check(load(rawPath, &raw), "raw dump loads");
	check(raw.w == 512 && raw.h == 256 && raw.bpp == 24, "raw dump is 512x256x24");
	check(raw.dataOff == 54 && raw.dataSize == 512u * 3 * 256 && raw.fileSize == 54 + raw.dataSize &&
		  (long)raw.fileSize == raw.len, "raw header sizes");
	int bad = 0;
	for (int y = 0; y < 256; y++)
		for (int x = 0; x < 512; x++)
		{
			unsigned char want[3], got[3];
			GPU_ReadDisplayPixelRGB(x, y, want);
			pixel(&raw, x, y, got);
			if (memcmp(want, got, 3) != 0)
				bad++;
		}
	check(bad == 0, "raw pixels match GPU_ReadDisplayPixelRGB");
	{
		unsigned char got[3];
		uint16_t px = g_vram[256 + 10][512 + 20];
		const int r = px & 31, g = (px >> 5) & 31, b = (px >> 10) & 31;
		pixel(&raw, 20, 10, got);
		check(got[0] == ((r << 3) | (r >> 2)) && got[1] == ((g << 3) | (g >> 2)) &&
			  got[2] == ((b << 3) | (b >> 2)), "raw pixel (20,10) is the display rect's, not VRAM (20,10)");
	}

	/*	2. mask=0: black, same header and size  */
	setDisplay(512, 256, 512, 256, 0);
	check(Host_WriteDisplayBMP(maskPath, 0) == 1, "masked dump written");
	Bmp msk;
	check(load(maskPath, &msk), "masked dump loads");
	check(msk.len == raw.len && memcmp(msk.bytes, raw.bytes, 54) == 0, "masked dump: same header and size as unmasked");
	bool allBlack = true;
	for (long i = 54; i < msk.len; i++)
		if (msk.bytes[i])
			allBlack = false;
	check(allBlack, "masked dump is all black");

	/*	3. the 4:3 size: the window's k=3 integer shape  */
	int aw = 0, ah = 0;
	Host_DumpAspectSize(512, 256, &aw, &ah);
	check(aw == 1024 && ah == 768, "4:3 size of 512x256 is 1024x768");
	Host_DumpAspectSize(320, 240, &aw, &ah);
	check(aw == 960 && ah == 720, "4:3 size of 320x240 is 960x720");
	Host_DumpAspectSize(640, 480, &aw, &ah);
	check(aw == 1920 && ah == 1440, "4:3 size of 640x480 is 1920x1440");

	/*	4. the 4:3 dump: 1024x768, each source pixel 2 across by 3 down  */
	setDisplay(512, 256, 512, 256, 1);
	check(Host_WriteDisplayBMP(aspPath, 1) == 1, "4:3 dump written");
	Bmp asp;
	check(load(aspPath, &asp), "4:3 dump loads");
	check(asp.w == 1024 && asp.h == 768 && asp.dataSize == 1024u * 3 * 768 &&
		  (long)asp.fileSize == asp.len, "4:3 dump is 1024x768 with a matching header");
	bad = 0;
	for (int y = 0; y < 768; y++)
		for (int x = 0; x < 1024; x++)
		{
			unsigned char want[3], got[3];
			pixel(&raw, x / 2, y / 3, want);
			pixel(&asp, x, y, got);
			if (memcmp(want, got, 3) != 0)
				bad++;
		}
	check(bad == 0, "4:3 pixel (x,y) is raw pixel (x/2, y/3)");

	/*	5. the mask rule holds for the 4:3 dump too  */
	setDisplay(512, 256, 512, 256, 0);
	check(Host_WriteDisplayBMP(aspMaskPath, 1) == 1, "masked 4:3 dump written");
	Bmp am;
	check(load(aspMaskPath, &am), "masked 4:3 dump loads");
	check(am.len == asp.len && memcmp(am.bytes, asp.bytes, 54) == 0, "masked 4:3 dump: same header and size");
	allBlack = true;
	for (long i = 54; i < am.len; i++)
		if (am.bytes[i])
			allBlack = false;
	check(allBlack, "masked 4:3 dump is all black");

	std::free(raw.bytes);
	std::free(msk.bytes);
	std::free(asp.bytes);
	std::free(am.bytes);
	std::remove(rawPath);
	std::remove(maskPath);
	std::remove(aspPath);
	std::remove(aspMaskPath);

	if (g_failures)
	{
		std::printf("framedump_test: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("framedump_test: all passed\n");
	return 0;
}
