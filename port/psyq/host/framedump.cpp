/*	Frame dumps (M2 tooling, split out of host/window.cpp): the displayed
	VRAM region as a 24bpp bottom-up BMP, in two shapes.

	  raw  pixel for pixel - dispW x dispH (Host_WriteDisplayBMP).  The
	       default, and the only shape written unless --dump-aspect asks
	       for the other one as well.
	  4:3  the window's shape (Host_WriteDisplayAspectBMP, issue #53).  The
	       PS1 scans its frame out into a 4:3 box, and the presenter does
	       the same (vk/viewport.cpp), so a pixel is square only when the
	       frame is 4:3: in the game's 512x256 mode it is 2 wide by 3 tall,
	       and art looked at in a raw dump comes out 1.5 times too wide.
	       Each source line is written k = 768 / lines times (3 for the
	       240- and 256-line modes, 1 for an interlaced 480 or 512) and the
	       width is the 4:3 width of that height:

	         512x256  game and frontend     1024x768  2 x 3 per pixel
	         320x240  FMV (24bpp)            960x720  3 x 3
	         256x240  ResetGraph's mode,     960x720  3.75 x 3 (columns
	                  gone by vblank 1                3 or 4 wide)
	         640x480  interlaced (unused)    640x480  1 x 1

	       That is the picture the default 1024x768 window shows at
	       scale=integer (framedump_test holds the two together).  Rows
	       are always whole repeats.  Columns are nearest-neighbour, each
	       output column showing the source column under its centre:
	       exact for the game's two live modes, uneven only for a width
	       that does not divide, and never a colour the game did not draw.
	       Judge sprite art from this one.

	SetDispMask(0) is honoured in both (issue #27): the presenter shows
	black while the display is masked, so the dump is black too - same
	size, same file name, so vblank/frame indexing stays stable - and the
	log line says "masked".  Pixels are read only through
	GPU_ReadDisplayPixelRGB, the one unpack shared with gpu_test.
	framedump_test pins the format; no other test reads these files
	(raster_diff_test and the frame CRCs work on VRAM).  */
#include <stdio.h>
#include <stdlib.h>

#include "gpu/gpu_core.h"
#include "host/framedump.h"

static void put32(FILE *f, uint32_t v)	{ fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v)	{ fwrite(&v, 2, 1, f); }

/*	Both shapes: a w x (lines * repeat) file in which output column x shows
	source column srcX[x] (NULL: column x itself, the raw dump) and every
	source line is written `repeat` times.  */
static int writeDisplay(const char *path, int w, int repeat, const int *srcX)
{
	int lines = GPU_DisplayHeight();
	int h = lines * repeat;
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

	for (int y = lines - 1; y >= 0; y--)
	{
		if (!masked)			/* masked: the zeroed row - black */
			for (int x = 0; x < w; x++)
			{
				unsigned char rgb[3];
				GPU_ReadDisplayPixelRGB(srcX ? srcX[x] : x, y, rgb);	/* 15bpp or isrgb24 */
				row[x * 3 + 0] = rgb[2];	/* B */
				row[x * 3 + 1] = rgb[1];	/* G */
				row[x * 3 + 2] = rgb[0];	/* R */
			}
		for (int r = 0; r < repeat; r++)
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

int Host_WriteDisplayBMP(const char *path)
{
	/*	the display width is in pixels in BOTH modes: fmv.cpp pre-divides
		for isrgb24  */
	return writeDisplay(path, GPU_DisplayWidth(), 1, NULL);
}

int Host_AspectDumpSize(int lines, int *w, int *h)
{
	if (lines < 1)
		lines = 1;
	int k = HOST_ASPECT_DUMP_LINES / lines;
	if (k < 1)
		k = 1;
	*h = k * lines;
	*w = (*h * 4 + 1) / 3;		/* rounded, as vk/viewport.cpp: 1024 for 768 */
	return k;
}

int Host_WriteDisplayAspectBMP(const char *path)
{
	int srcW = GPU_DisplayWidth();
	int w, h;
	int k = Host_AspectDumpSize(GPU_DisplayHeight(), &w, &h);

	/*	The source column under each output column's centre, (x + 1/2) *
		srcW / w, worked out once per dump rather than once per pixel.  */
	int *srcX = (int *)malloc(w * sizeof(int));
	if (!srcX)
	{
		fprintf(stderr, "[host] cannot write %s (out of memory)\n", path);
		return 0;
	}
	for (int x = 0; x < w; x++)
		srcX[x] = ((2 * x + 1) * srcW) / (2 * w);

	int ok = writeDisplay(path, w, k, srcX);
	free(srcX);
	return ok;
}
