/*	Viewport scaling modes (M8 shell): where the emulated display lands in
	the window.  Pure arithmetic, no Vulkan, so viewport_test can pin it.

	The PS1 scans its 512x256 (or 320x240...) frame out on a 4:3 CRT, so
	the picture is always 4:3 and its pixels are not square.  The modes:

	  fit      the largest 4:3 rectangle that fits, centred (letterboxed /
	           pillarboxed with black) - the M2 behaviour and the default;
	  integer  like fit, but the height is a whole multiple k of the
	           display's line count (the 256-line frame at k=3 is 768 tall
	           and exactly 1024 wide), so rows are never resampled unevenly;
	           the width is the 4:3 width for that height, which for the
	           512-wide mode is exactly 2k pixels per source pixel.  Falls
	           back to fit when even k=1 does not fit;
	  stretch  the whole window, aspect ignored.

	SBSP_SCALE (sbsp.ini `scale`, --scale) picks the mode.  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SCALE_FIT = 0, SCALE_INTEGER = 1, SCALE_STRETCH = 2 };

/*	out = { x, y, w, h } in window pixels  */
extern "C" void Port_ViewportRect(int mode, int winW, int winH, int dispH, int out[4])
{
	if (winW < 1) winW = 1;
	if (winH < 1) winH = 1;
	if (dispH < 1) dispH = 1;

	int w = winW, h = winH;
	if (mode == SCALE_STRETCH)
	{
		w = winW;
		h = winH;
	}
	else
	{
		/*	fit: widest 4:3 box inside the window  */
		w = winW;
		h = (winW * 3) / 4;
		if (h > winH)
		{
			h = winH;
			w = (winH * 4) / 3;
		}
		if (mode == SCALE_INTEGER)
		{
			int k = h / dispH;					/* whole frames that fit vertically */
			while (k > 0 && ((k * dispH * 4) / 3) > winW)
				k--;							/* ...and whose 4:3 width fits too */
			if (k > 0)
			{
				h = k * dispH;
				w = (h * 4 + 1) / 3;			/* rounded 4:3 width (1024 for 768) */
			}
			/* else: too small for one whole frame - keep the fit box */
		}
	}
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	out[0] = (winW - w) / 2;
	out[1] = (winH - h) / 2;
	out[2] = w;
	out[3] = h;
}

extern "C" int Port_ScaleModeParse(const char *s)
{
	if (!s || !*s || _stricmp(s, "fit") == 0)
		return SCALE_FIT;
	if (_stricmp(s, "integer") == 0)
		return SCALE_INTEGER;
	if (_stricmp(s, "stretch") == 0)
		return SCALE_STRETCH;
	return -1;
}

/*	SBSP_SCALE, parsed once; a bad value is reported and means fit.  */
extern "C" int Port_ScaleMode(void)
{
	static int mode = -2;
	if (mode == -2)
	{
		const char *e = getenv("SBSP_SCALE");
		mode = Port_ScaleModeParse(e);
		if (mode < 0)
		{
			fprintf(stderr, "[host] bad scale '%s' - want fit|integer|stretch - using fit\n", e);
			mode = SCALE_FIT;
		}
	}
	return mode;
}
