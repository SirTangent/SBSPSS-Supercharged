/*	Frame dumps (host/framedump.cpp) - shared by host/window.cpp's
	--dump-frames path and framedump_test.  C++ linkage on purpose: a
	signature that drifts from this declaration fails to link.
*/
#ifndef PORT_FRAMEDUMP_H
#define PORT_FRAMEDUMP_H

/*	Write the displayed VRAM region to `path` as a 24bpp bottom-up BMP,
	black while SetDispMask(0) is in force (issue #27).  Returns 1 when the
	whole file was written, 0 on any failure (nothing is left at `path`).  */
int Host_WriteDisplayBMP(const char *path);

/*	The same picture at the window's 4:3 shape (issue #53, --dump-aspect),
	the one to judge art from.  Masking, return value and failure handling
	as Host_WriteDisplayBMP.  */
int Host_WriteDisplayAspectBMP(const char *path);

/*	Its size for a display `lines` tall: each line repeated
	k = HOST_ASPECT_DUMP_LINES / lines times (at least once), so the height
	is k * lines, and the width is the 4:3 width of that, (4 * h + 1) / 3.
	Returns k.  The display's width does not enter: the box is 4:3 whatever
	it holds.  */
#define HOST_ASPECT_DUMP_LINES	768		/* the default window's height (window.cpp, ini.cpp) */
int Host_AspectDumpSize(int lines, int *w, int *h);

#endif
