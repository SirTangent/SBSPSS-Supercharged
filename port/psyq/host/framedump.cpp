/*	Frame dumps (M2 tooling, split out of host/window.cpp): the displayed
	VRAM region as a 24bpp bottom-up BMP.

	Two shapes:
	  raw (aspect 0)  the framebuffer pixel for pixel - dispW x dispH, the
	                  file raster_diff_test, the frame CRC work and the x64
	                  A/B compare against.  Always the default.
	  4:3 (aspect 1)  the window's shape (issue #53): the PS1 scans its
	                  frame out into a 4:3 box, so a source pixel is not
	                  square (2:3 for the 512x256 mode).  The size is
	                  Port_ViewportRect's integer mode at k=3 - exactly what
	                  a 1024x768 window shows: 1024x768 for 512x256, each
	                  pixel 2 across by 3 down; 960x720 for a 240-line
	                  frame.  Nearest-neighbour, so every colour is one the
	                  game drew.  Judge sprite art from this one.

	Either way SetDispMask(0) is honoured (issue #27): the presenter shows
	black while the display is masked, so the dump is black too - same size,
	same file name, so vblank/frame indexing stays stable - and the log line
	says "masked".  Pixels are read only through GPU_ReadDisplayPixelRGB,
	the one unpack shared with gpu_test.  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gpu_core.h"

extern "C" void Port_ViewportRect(int mode, int winW, int winH, int dispH, int out[4]);	/* vk/viewport.cpp */

static void put32(FILE *f, uint32_t v)	{ fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v)	{ fwrite(&v, 2, 1, f); }

/*	The 4:3 dump's size for a srcW x srcH frame: the integer viewport of a
	window 3*srcH tall and wide enough never to limit it (1<<14), i.e. k=3.  */
extern "C" void Host_DumpAspectSize(int srcW, int srcH, int *outW, int *outH)
{
	(void)srcW;		/* the 4:3 box depends on the line count alone */
	int r[4];
	Port_ViewportRect(1 /* SCALE_INTEGER */, 1 << 14, 3 * srcH, srcH, r);
	*outW = r[2];
	*outH = r[3];
}

/*	Returns 1 when the file was written.  */
extern "C" int Host_WriteDisplayBMP(const char *path, int aspect)
{
	int srcW = g_gpu.dispW ? g_gpu.dispW : 512;	/* pixels in BOTH modes:
												   fmv.cpp pre-divides for
												   isrgb24 */
	int srcH = g_gpu.dispH ? g_gpu.dispH : 256;
	int masked = !g_gpu.dispMask;

	int w = srcW, h = srcH;
	if (aspect)
		Host_DumpAspectSize(srcW, srcH, &w, &h);

	FILE *f = fopen(path, "wb");
	if (!f)
	{
		fprintf(stderr, "[host] cannot write %s\n", path);
		return 0;
	}

	int rowBytes = (w * 3 + 3) & ~3;
	uint32_t dataSize = (uint32_t)rowBytes * h;

	fwrite("BM", 2, 1, f);
	put32(f, 54 + dataSize);  put32(f, 0);  put32(f, 54);
	put32(f, 40);  put32(f, (uint32_t)w);  put32(f, (uint32_t)h);
	put16(f, 1);  put16(f, 24);
	put32(f, 0);  put32(f, dataSize);
	put32(f, 2835);  put32(f, 2835);  put32(f, 0);  put32(f, 0);

	unsigned char *row = (unsigned char *)calloc(1, rowBytes);
	int builtFrom = -1;			/* source row `row` currently holds */
	for (int y = h - 1; y >= 0; y--)
	{
		int sy = (int)((long long)y * srcH / h);
		if (!masked && sy != builtFrom)
		{
			for (int x = 0; x < w; x++)
			{
				unsigned char rgb[3];
				GPU_ReadDisplayPixelRGB((int)((long long)x * srcW / w), sy, rgb);	/* 15bpp or isrgb24 */
				row[x * 3 + 0] = rgb[2];	/* B */
				row[x * 3 + 1] = rgb[1];	/* G */
				row[x * 3 + 2] = rgb[0];	/* R */
			}
			builtFrom = sy;
		}
		fwrite(row, 1, rowBytes, f);	/* masked: the zeroed row - black */
	}
	free(row);
	fclose(f);
	fprintf(stderr, "[host] wrote %s (%dx%d%s)\n", path, w, h, masked ? ", masked" : "");
	return 1;
}
