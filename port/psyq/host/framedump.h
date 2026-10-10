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

#endif
