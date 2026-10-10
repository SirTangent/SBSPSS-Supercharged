/*	Frame dumps (M2 tooling, split out of host/window.cpp): the displayed
	VRAM region as a 24bpp bottom-up BMP, pixel for pixel - dispW x dispH.

	SetDispMask(0) is honoured (issue #27): the presenter shows black while
	the display is masked, so the dump is black too - same size, same file
	name, so vblank/frame indexing stays stable - and the log line says
	"masked".  Pixels are read only through GPU_ReadDisplayPixelRGB, the one
	unpack shared with gpu_test.  framedump_test pins the format; no other
	test reads these files (raster_diff_test and the frame CRCs work on
	VRAM).  */
#include <stdio.h>
#include <stdlib.h>

#include "gpu/gpu_core.h"
#include "host/framedump.h"

static void put32(FILE *f, uint32_t v)	{ fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v)	{ fwrite(&v, 2, 1, f); }

int Host_WriteDisplayBMP(const char *path)
{
	int w = g_gpu.dispW ? g_gpu.dispW : 512;	/* pixels in BOTH modes:
												   fmv.cpp pre-divides for
												   isrgb24 */
	int h = g_gpu.dispH ? g_gpu.dispH : 256;
	int masked = !g_gpu.dispMask;

	int rowBytes = (w * 3 + 3) & ~3;
	uint32_t dataSize = (uint32_t)rowBytes * h;
	unsigned char *row = (unsigned char *)calloc(1, rowBytes);
	if (!row)
	{
		fprintf(stderr, "[host] cannot write %s (out of memory)\n", path);
		return 0;
	}

	FILE *f = fopen(path, "wb");
	if (!f)
	{
		fprintf(stderr, "[host] cannot write %s\n", path);
		free(row);
		return 0;
	}

	fwrite("BM", 2, 1, f);
	put32(f, 54 + dataSize);  put32(f, 0);  put32(f, 54);
	put32(f, 40);  put32(f, (uint32_t)w);  put32(f, (uint32_t)h);
	put16(f, 1);  put16(f, 24);
	put32(f, 0);  put32(f, dataSize);
	put32(f, 2835);  put32(f, 2835);  put32(f, 0);  put32(f, 0);

	for (int y = h - 1; y >= 0; y--)
	{
		if (!masked)			/* masked: the zeroed row - black */
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

	/*	One check covers every write: the stream's error flag is sticky, and
		fclose reports a failed final flush.  A short file is removed rather
		than left to pass for a dump.  */
	int ok = !ferror(f);
	if (fclose(f) != 0)
		ok = 0;
	if (!ok)
	{
		fprintf(stderr, "[host] cannot write %s (write failed)\n", path);
		remove(path);
		return 0;
	}
	fprintf(stderr, "[host] wrote %s (%dx%d%s)\n", path, w, h, masked ? ", masked" : "");
	return 1;
}
