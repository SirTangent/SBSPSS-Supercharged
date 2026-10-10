/*	Unit test for the frame dumper (port/psyq/host/framedump.cpp): the BMP
	is the display rect pixel for pixel, SetDispMask(0) gives a black file
	of the same size (issue #27), and a file that cannot be written reports
	0.  Writes to %TEMP% under per-process names, so trees running ctest at
	the same time do not trample each other.  No window, no GPU.
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <process.h>	/* _getpid */

#include "gpu/gpu_core.h"
#include "host/framedump.h"

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

/*	true only for a whole 24bpp file: every later read stays inside it  */
static bool load(const char *path, Bmp *b)
{
	memset(b, 0, sizeof(*b));
	FILE *f = std::fopen(path, "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	b->len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (b->len < 54)
	{
		std::fclose(f);
		return false;
	}
	b->bytes = (unsigned char *)std::malloc(b->len);
	bool ok = b->bytes && std::fread(b->bytes, 1, b->len, f) == (size_t)b->len;
	std::fclose(f);
	if (!ok || b->bytes[0] != 'B' || b->bytes[1] != 'M')
		return false;
	b->fileSize = rd32(b->bytes + 2);
	b->dataOff  = rd32(b->bytes + 10);
	b->w        = (int)rd32(b->bytes + 18);
	b->h        = (int)rd32(b->bytes + 22);
	b->bpp      = b->bytes[28] | (b->bytes[29] << 8);
	b->dataSize = rd32(b->bytes + 34);
	return b->bpp == 24 && b->w > 0 && b->h > 0 &&
		   (long)b->dataOff + (long)((b->w * 3 + 3) & ~3) * b->h <= b->len;
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

static int finish(Bmp *raw, Bmp *msk, const char *rawPath, const char *maskPath)
{
	std::free(raw->bytes);
	std::free(msk->bytes);
	std::remove(rawPath);
	std::remove(maskPath);
	if (g_failures)
	{
		std::printf("framedump_test: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("framedump_test: all passed\n");
	return 0;
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
	const int pid = _getpid();
	char rawPath[600], maskPath[600], badPath[600];
	std::snprintf(rawPath,  sizeof(rawPath),  "%s/framedump_test_%d_raw.bmp", tmp, pid);
	std::snprintf(maskPath, sizeof(maskPath), "%s/framedump_test_%d_masked.bmp", tmp, pid);
	std::snprintf(badPath,  sizeof(badPath),  "%s/framedump_test_%d_no_such_dir/x.bmp", tmp, pid);
	Bmp raw = {}, msk = {};

	/*	1. mask=1: header and every pixel from GPU_ReadDisplayPixelRGB  */
	setDisplay(512, 256, 512, 256, 1);
	check(Host_WriteDisplayBMP(rawPath) == 1, "raw dump written");
	if (!load(rawPath, &raw))
	{
		check(false, "raw dump loads");
		return finish(&raw, &msk, rawPath, maskPath);
	}
	check(raw.w == 512 && raw.h == 256, "raw dump is 512x256");
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
	check(Host_WriteDisplayBMP(maskPath) == 1, "masked dump written");
	if (!load(maskPath, &msk))
	{
		check(false, "masked dump loads");
		return finish(&raw, &msk, rawPath, maskPath);
	}
	check(msk.len == raw.len && memcmp(msk.bytes, raw.bytes, 54) == 0, "masked dump: same header and size as unmasked");
	bool allBlack = true;
	for (long i = 54; i < msk.len; i++)
		if (msk.bytes[i])
			allBlack = false;
	check(allBlack, "masked dump is all black");

	/*	3. a file that cannot be created reports 0 and leaves nothing  */
	setDisplay(512, 256, 512, 256, 1);
	check(Host_WriteDisplayBMP(badPath) == 0, "unwritable path returns 0");
	FILE *f = std::fopen(badPath, "rb");
	check(f == NULL, "unwritable path leaves no file");
	if (f)
		std::fclose(f);

	return finish(&raw, &msk, rawPath, maskPath);
}
