/*	Unit test for the frame dumper (port/psyq/host/framedump.cpp): the BMP
	is the display rect pixel for pixel, SetDispMask(0) gives a black file
	of the same size (issue #27), and a file that cannot be written reports
	0.  The 4:3 dump (issue #53) is the size the default window shows at
	scale=integer, every source line repeated whole and every column the
	source column under its centre - exact 2x3 blocks for the game's
	512x256 frame, 3x3 for an FMV's 320x240 - and masks the same way.
	Writes to %TEMP% under per-process names, so trees running ctest at
	the same time do not trample each other.  No window, no GPU.
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <process.h>	/* _getpid */

#include "gpu/gpu_core.h"
#include "host/framedump.h"

extern "C" void Port_ViewportRect(int mode, int winW, int winH, int dispH, int out[4]);	/* vk/viewport.cpp */

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

/*	header sizes agree with a w x h file and with the file's length  */
static bool headerIs(const Bmp *b, int w, int h)
{
	uint32_t data = (uint32_t)((w * 3 + 3) & ~3) * h;
	return b->w == w && b->h == h && b->dataOff == 54 && b->dataSize == data &&
		   b->fileSize == 54 + data && (long)b->fileSize == b->len;
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

/*	every dump this test loads or writes, freed and removed at the end  */
enum { RAW, MASKED, ASP, ASP_FMV, ASP_ODD, ASP_MASKED, NFILES };
static Bmp	g_bmp[NFILES];
static char	g_path[NFILES][600];

static int finish(void)
{
	for (int i = 0; i < NFILES; i++)
	{
		std::free(g_bmp[i].bytes);
		std::remove(g_path[i]);
	}
	if (g_failures)
	{
		std::printf("framedump_test: %d FAILED\n", g_failures);
		return 1;
	}
	std::printf("framedump_test: all passed\n");
	return 0;
}

/*	write file i with `writer` and load it back; false (FAIL logged) if
	either step fails, so the caller stops before reading past the buffer  */
static bool dump(int i, int (*writer)(const char *), const char *what)
{
	char msg[200];
	std::snprintf(msg, sizeof(msg), "%s written", what);
	check(writer(g_path[i]) == 1, msg);
	if (load(g_path[i], &g_bmp[i]))
		return true;
	std::snprintf(msg, sizeof(msg), "%s loads", what);
	check(false, msg);
	return false;
}

static bool allBlack(const Bmp *b)
{
	for (long i = 54; i < b->len; i++)
		if (b->bytes[i])
			return false;
	return true;
}

/*	every pixel of a 4:3 dump against the display rect: output column x
	shows source column ((2x+1)*srcW) / (2*w), output row y source line y/k  */
static int aspectMismatches(const Bmp *b, int srcW, int k)
{
	int bad = 0;
	for (int y = 0; y < b->h; y++)
		for (int x = 0; x < b->w; x++)
		{
			unsigned char want[3], got[3];
			GPU_ReadDisplayPixelRGB(((2 * x + 1) * srcW) / (2 * b->w), y / k, want);
			pixel(b, x, y, got);
			if (memcmp(want, got, 3) != 0)
				bad++;
		}
	return bad;
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
	static const char *const names[NFILES] = { "raw", "masked", "4x3", "4x3_fmv", "4x3_odd", "4x3_masked" };
	for (int i = 0; i < NFILES; i++)
		std::snprintf(g_path[i], sizeof(g_path[i]), "%s/framedump_test_%d_%s.bmp", tmp, pid, names[i]);
	char badPath[600];
	std::snprintf(badPath, sizeof(badPath), "%s/framedump_test_%d_no_such_dir/x.bmp", tmp, pid);

	/*	1. mask=1: header and every pixel from GPU_ReadDisplayPixelRGB  */
	setDisplay(512, 256, 512, 256, 1);
	if (!dump(RAW, Host_WriteDisplayBMP, "raw dump"))
		return finish();
	const Bmp &raw = g_bmp[RAW];
	check(raw.w == 512 && raw.h == 256, "raw dump is 512x256");
	check(headerIs(&raw, 512, 256), "raw header sizes");
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
	if (!dump(MASKED, Host_WriteDisplayBMP, "masked dump"))
		return finish();
	const Bmp &msk = g_bmp[MASKED];
	check(msk.len == raw.len && memcmp(msk.bytes, raw.bytes, 54) == 0, "masked dump: same header and size as unmasked");
	check(allBlack(&msk), "masked dump is all black");

	/*	3. a file that cannot be created reports 0 and leaves nothing  */
	setDisplay(512, 256, 512, 256, 1);
	check(Host_WriteDisplayBMP(badPath) == 0, "unwritable path returns 0");
	check(Host_WriteDisplayAspectBMP(badPath) == 0, "unwritable path returns 0 (4:3)");
	FILE *f = std::fopen(badPath, "rb");
	check(f == NULL, "unwritable path leaves no file");
	if (f)
		std::fclose(f);

	/*	4. the 4:3 size: whole repeats of the line count, 4:3 wide - the
		picture the default 1024x768 window shows at scale=integer
		(vk/viewport.cpp), for every line count VRAM can scan out  */
	int w = 0, h = 0, k = 0;
	k = Host_AspectDumpSize(256, &w, &h);
	check(k == 3 && w == 1024 && h == 768, "4:3 size for 256 lines is 1024x768, k=3");
	k = Host_AspectDumpSize(240, &w, &h);
	check(k == 3 && w == 960 && h == 720, "4:3 size for 240 lines is 960x720, k=3");
	k = Host_AspectDumpSize(480, &w, &h);
	check(k == 1 && w == 640 && h == 480, "4:3 size for 480 lines (interlaced) is 640x480, k=1");
	k = Host_AspectDumpSize(512, &w, &h);
	check(k == 1 && w == 683 && h == 512, "4:3 size for 512 lines (interlaced) is 683x512, k=1");
	bad = 0;
	for (int lines = 1; lines <= VRAM_H; lines++)
	{
		int r[4];
		Port_ViewportRect(1 /* integer */, 1024, 768, lines, r);
		k = Host_AspectDumpSize(lines, &w, &h);
		if (w != r[2] || h != r[3] || h != k * lines)
			bad++;
	}
	check(bad == 0, "4:3 size = the default window's integer viewport, whole lines, for 1..512 lines");

	/*	5. the game's frame: 1024x768, each pixel exactly 2 across by 3 down  */
	setDisplay(512, 256, 512, 256, 1);
	if (!dump(ASP, Host_WriteDisplayAspectBMP, "4:3 dump"))
		return finish();
	const Bmp &asp = g_bmp[ASP];
	check(headerIs(&asp, 1024, 768), "4:3 dump is 1024x768 with a matching header");
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

	/*	6. an FMV frame: 320x240 at 24bpp, offset - 960x720, exact 3x3  */
	setDisplay(16, 8, 320, 240, 1);
	g_gpu.dispRgb24 = 1;
	if (!dump(ASP_FMV, Host_WriteDisplayAspectBMP, "4:3 FMV dump"))
		return finish();
	const Bmp &fmv = g_bmp[ASP_FMV];
	check(headerIs(&fmv, 960, 720), "4:3 FMV dump is 960x720 with a matching header");
	check(aspectMismatches(&fmv, 320, 3) == 0, "4:3 FMV pixel (x,y) is display pixel (x/3, y/3), 24bpp");

	/*	7. a width that does not divide: 256x240 (the reset mode) - columns
		3 or 4 wide, in order, every source column present  */
	setDisplay(0, 0, 256, 240, 1);
	if (!dump(ASP_ODD, Host_WriteDisplayAspectBMP, "4:3 256-wide dump"))
		return finish();
	const Bmp &odd = g_bmp[ASP_ODD];
	check(headerIs(&odd, 960, 720), "4:3 256x240 dump is 960x720");
	check(aspectMismatches(&odd, 256, 3) == 0, "4:3 256x240 pixels are the source columns under each centre");
	{
		int count[256] = {0}, prev = 0;
		bool ordered = true;
		for (int x = 0; x < 960; x++)
		{
			int sx = ((2 * x + 1) * 256) / (2 * 960);
			ordered = ordered && sx >= prev && sx < 256;
			prev = sx;
			if (sx >= 0 && sx < 256)
				count[sx]++;
		}
		bool even = true;
		for (int i = 0; i < 256; i++)
			even = even && (count[i] == 3 || count[i] == 4);
		check(ordered && even, "4:3 256x240 columns run in order, each 3 or 4 wide");
	}

	/*	8. the mask rule holds for the 4:3 dump too  */
	setDisplay(512, 256, 512, 256, 0);
	if (!dump(ASP_MASKED, Host_WriteDisplayAspectBMP, "masked 4:3 dump"))
		return finish();
	const Bmp &am = g_bmp[ASP_MASKED];
	check(am.len == asp.len && memcmp(am.bytes, asp.bytes, 54) == 0, "masked 4:3 dump: same header and size");
	check(allBlack(&am), "masked 4:3 dump is all black");

	return finish();
}
