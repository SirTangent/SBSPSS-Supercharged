/*	Software PS1 GPU - shared internal state (M2).

	VRAM is the real thing: 1024x512 halfwords, 15-bit MBBBBBGGGGGRRRRR
	(R in the low bits, bit 15 = STP/mask), verified against the game's own
	SaveScreen unpack.  The draw-state mirrors the hardware registers the
	GP0 E1-E6 commands program; PutDrawEnv/PutDispEnv are just batched
	writes to it.  Everything is single-threaded under the cooperative pump.
*/
#ifndef PORT_GPU_CORE_H
#define PORT_GPU_CORE_H

#include <stdint.h>

#define VRAM_W 1024
#define VRAM_H 512

extern uint16_t g_vram[VRAM_H][VRAM_W];

struct GpuState
{
	/* drawing area (inclusive), from E3/E4 */
	int		clipX0, clipY0, clipX1, clipY1;
	/* drawing offset, from E5 (11-bit signed) */
	int		ofsX, ofsY;
	/* E1 draw mode: texture page base, semi-trans mode, texture depth, dither */
	int		texBaseX;		/* halfwords */
	int		texBaseY;
	int		semiMode;		/* 0..3 (ABR) */
	int		texDepth;		/* 0=4bpp 1=8bpp 2=15bpp */
	int		dither;
	/*	E2 texture window.  The raw word is kept for readback; the four
		fields the sampler actually wants are derived once here (E2 is
		rare, primitives are not) and copied verbatim into every
		RasterCfg.  All-zero = identity, which is also the memset default
		for the cfgs built by hand.  */
	uint32_t	texWindow;
	int			twMaskU, twOrU, twMaskV, twOrV;

	/* display */
	int		dispX, dispY, dispW, dispH;	/* DISPENV.disp - VRAM region scanned out */
	int		screenX, screenY;			/* DISPENV.screen - CRT placement (PAL y offset) */
	int		dispMask;					/* SetDispMask: 1 = video enabled */
	int		dispRgb24;					/* DISPENV.isrgb24: scan out 24bpp (FMV).
										   dispW is then in PIXELS - fmv.cpp
										   pre-converts (disp.w*2/3) itself. */
};

extern GpuState g_gpu;

/* gp0.cpp: execute `count` GP0 words at `words` against g_gpu/g_vram */
void GPU_ExecWords(const uint32_t *words, int count);

/*	vram.cpp: CRC-32 of the raw VRAM halfwords the display rect scans out
	(same rect logic as GPU_ReadDisplayPixelRGB).  *masked = SetDispMask(0)
	is in force, i.e. the viewer sees black whatever the CRC says.  */
extern "C" uint32_t GPU_DisplayCRC32(int *masked);

/*	gp0.cpp: decode an E1 draw-mode bit pattern into g_gpu (texture page base,
	semi-transparency mode, depth, dither).  Shared with PutDrawEnv, which
	assembles the same layout from DRAWENV.tpage/dtd.  */
void GPU_ApplyTexpage(uint32_t tp);

/*	gp0.cpp: decode an E2 texture-window word into g_gpu (raw word plus the
	sampler's mask/or form).  Shared with the GPU reset path.  */
void GPU_ApplyTexWindow(uint32_t word);

/* raster.cpp entry points (coords already offset-applied, clip in g_gpu) */
struct RasterVtx { int x, y; int u, v; uint8_t r, g, b; };
struct RasterCfg
{
	int		textured;
	int		rawTex;			/* code bit0: no modulation */
	int		semi;			/* code bit1 */
	int		gouraud;
	/* texture state captured at prim time */
	int		texBaseX, texBaseY, texDepth, semiMode;
	int		clutX, clutY;
	/*	E2 texture window, precomputed to the sampler's form:
		coord = (coord & ~twMask) | twOr.  All-zero = no window (identity),
		so memset-zeroed configs keep full 0..255 wrapping.  */
	int		twMaskU, twOrU, twMaskV, twOrV;
	/*	E1 dtd captured at prim time; the pixel pipeline applies the PS1
		4x4 dither only to gouraud-shaded or texture-modulated pixels.  */
	int		dither;
};
void Raster_Triangle(const RasterVtx *v0, const RasterVtx *v1, const RasterVtx *v2,
					 const RasterCfg *cfg);
void Raster_Line(const RasterVtx *a, const RasterVtx *b, const RasterCfg *cfg);
void Raster_FillRect15(int x, int y, int w, int h, uint16_t col15);	/* raw, no clip/mask */

/*	vram.cpp: unpack one DISPLAYED pixel (x,y relative to the disp origin)
	to RGB888, honoring dispRgb24 - the single unpack shared by the BMP
	dumper and gpu_test, mirroring the present.frag math.  15bpp expands
	5-bit channels by <<3 (the dumper's historical scaling).  */
extern "C" void GPU_ReadDisplayPixelRGB(int x, int y, unsigned char rgb[3]);

/* host hooks (window.cpp; safe no-ops before the window exists).
   extern "C": pump.cpp forward-declares Host_VBlank inside an extern "C"
   function, which gives the declaration C linkage - keep them C throughout. */
extern "C" void Host_EnsureVideo(void);		/* create SDL window + presenter */
extern "C" void Host_VBlank(unsigned long vblankNo);	/* events + present + tooling */

#endif
