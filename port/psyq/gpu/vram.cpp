/*	libgpu: VRAM transfers, environments, OT setup, reset/display control.

	Semantics pinned by the game's call sites (see the M2 recon in the plan):
	- LoadImage/MoveImage must be synchronous (animtex.cpp reuses the source
	  buffer immediately after LoadImage; LoadingIcon DrawPrims right after
	  MoveImage with no DrawSync).
	- ClearImage clamps and splits rects like libgpu (libgpuFill): actor.cpp
	  passes {512,256,2048,254}, which must degrade harmlessly.
	- PutDrawEnv applies ofs/clip/tpage, honours isbg (fill clip with r0g0b0)
	  and IGNORES dfe - the loading icon deliberately draws into the
	  displayed VRAM half, which dfe=0 would veto.
	- ClearOTagR builds the reverse chain (ot[0] = terminator, drawn last);
	  ClearOTag the forward one (ASSERT screen only).
*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <sys/types.h>
#include <libgte.h>
#include <libgpu.h>

#include "stub_log.h"
#include "gpu/gpu_core.h"

uint16_t g_vram[VRAM_H][VRAM_W];
GpuState g_gpu;

/*****************************************************************************/
/*	The GP0(02h) fill: hardware masking - x in 16-halfword steps, w rounded
	up to 16, and the rect wraps at the VRAM edge.  libgpu's aligned
	ClearImage lands here after its own clamp (libgpuFill below).  */
void Raster_FillRect15(int x, int y, int w, int h, uint16_t col15)
{
	x &= 0x3F0;
	y &= 0x1FF;
	w = ((w & 0x3FF) + 0xF) & ~0xF;
	h &= 0x1FF;
	for (int row = 0; row < h; row++)
	{
		int vy = (y + row) & 0x1FF;
		for (int col = 0; col < w; col++)
			g_vram[vy][(x + col) & 0x3FF] = col15;
	}
}

/*****************************************************************************/
extern "C" int ResetGraph(int mode)
{
	Host_EnsureVideo();		/* first GPU touch: bring up SDL + presenter */

	if (mode == 0)
	{
		memset(&g_gpu, 0, sizeof(g_gpu));
		g_gpu.clipX1 = VRAM_W - 1;
		g_gpu.clipY1 = VRAM_H - 1;
		g_gpu.dispW = 256;
		g_gpu.dispH = 240;
		g_gpu.dispMask = 0;		/* display disabled until SetDispMask(1) */
	}
	return 0;
}

extern "C" int SetGraphDebug(int level)
{
	(void)level;
	return 0;
}

extern "C" void SetDispMask(int mask)
{
	g_gpu.dispMask = mask;
}

/*****************************************************************************/
extern "C" DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h)
{
	memset(env, 0, sizeof(*env));
	env->disp.x = (short)x;
	env->disp.y = (short)y;
	env->disp.w = (short)w;
	env->disp.h = (short)h;
	return env;
}

extern "C" DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h)
{
	memset(env, 0, sizeof(*env));
	env->clip.x = (short)x;
	env->clip.y = (short)y;
	env->clip.w = (short)w;
	env->clip.h = (short)h;
	env->ofs[0] = (short)x;
	env->ofs[1] = (short)y;
	env->tpage  = (u_short)(0x10 * 0);	/* getTPage(0,0,x,y): 4bpp, abr 0 */
	env->dtd    = 1;
	env->dfe    = 0;
	env->isbg   = 0;
	return env;
}

extern "C" DISPENV *PutDispEnv(DISPENV *env)
{
	g_gpu.dispX = env->disp.x;
	g_gpu.dispY = env->disp.y;
	g_gpu.dispW = env->disp.w;
	g_gpu.dispH = env->disp.h;
	g_gpu.screenX = env->screen.x;
	g_gpu.screenY = env->screen.y;
	g_gpu.dispRgb24 = env->isrgb24;
	return env;
}

extern "C" void GPU_ReadDisplayPixelRGB(int x, int y, unsigned char rgb[3])
{
	int vy = (g_gpu.dispY + y) & 0x1FF;
	if (g_gpu.dispRgb24)
	{
		for (int i = 0; i < 3; i++)
		{
			int b = x * 3 + i;
			uint16_t hw = g_vram[vy][(g_gpu.dispX + (b >> 1)) & 0x3FF];
			rgb[i] = (unsigned char)((b & 1) ? (hw >> 8) : (hw & 0xFF));
		}
		return;
	}
	uint16_t px = g_vram[vy][(g_gpu.dispX + x) & 0x3FF];
	/*	5 -> 8 bits as (c << 3) | (c >> 2), full range (31 -> 255): the same
		integer the presenter's shader shows, so a --dump-frames BMP and the
		window agree to the bit.  It was c << 3 here (white 0xF8) against
		c / 31 on screen (0xFF), up to 7 levels apart per channel (#63).  */
	const int r = px & 0x1F, g = (px >> 5) & 0x1F, b = (px >> 10) & 0x1F;
	rgb[0] = (unsigned char)((r << 3) | (r >> 2));
	rgb[1] = (unsigned char)((g << 3) | (g >> 2));
	rgb[2] = (unsigned char)((b << 3) | (b >> 2));
}

extern "C" uint32_t GPU_DisplayCRC32(int *masked)
{
	static uint32_t	table[256];
	static int		tableInit;
	if (!tableInit)
	{
		for (uint32_t n = 0; n < 256; n++)
		{
			uint32_t c = n;
			for (int k = 0; k < 8; k++)
				c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
			table[n] = c;
		}
		tableInit = 1;
	}

	int w  = g_gpu.dispW ? g_gpu.dispW : 512;		/* pixels in both modes */
	int h  = g_gpu.dispH ? g_gpu.dispH : 256;
	int hw = g_gpu.dispRgb24 ? (w * 3 + 1) / 2 : w;	/* halfwords per row */

	uint32_t crc = 0xFFFFFFFFu;
	for (int y = 0; y < h; y++)
	{
		const uint16_t *row = g_vram[(g_gpu.dispY + y) & 0x1FF];
		for (int x = 0; x < hw; x++)
		{
			uint16_t px = row[(g_gpu.dispX + x) & 0x3FF];
			crc = table[(crc ^ (px & 0xFF)) & 0xFF] ^ (crc >> 8);
			crc = table[(crc ^ (px >> 8)) & 0xFF] ^ (crc >> 8);
		}
	}
	if (masked)
		*masked = !g_gpu.dispMask;
	return crc ^ 0xFFFFFFFFu;
}

static void libgpuFill(int x, int y, int w, int h, uint16_t col15,
					   int cx0, int cy0, int cx1, int cy1);
static uint16_t rgb15(u_char r, u_char g, u_char b);

extern "C" DRAWENV *PutDrawEnv(DRAWENV *env)
{
	/*	E1: the hardware word is tpage with dtd in bit 9 (and dfe in bit 10,
		deliberately ignored - see the header comment).  Assemble it and run
		the same decode the 0xE1 command uses, so the two cannot drift.  */
	GPU_ApplyTexpage((uint32_t)env->tpage | ((uint32_t)(env->dtd & 1) << 9));
	/* E2 */
	GPU_ApplyTexWindow(0);		/* PutDrawEnv resets the window to identity */
	/* E3/E4 */
	g_gpu.clipX0 = env->clip.x;
	g_gpu.clipY0 = env->clip.y;
	g_gpu.clipX1 = env->clip.x + env->clip.w - 1;
	g_gpu.clipY1 = env->clip.y + env->clip.h - 1;
	/* E5 */
	g_gpu.ofsX = env->ofs[0];
	g_gpu.ofsY = env->ofs[1];

	if (env->isbg)
	{
		/*	libgpu's own clear packet, emitted after the env: the same
			clamp and 02h/60h split as ClearImage, clipped by the env  */
		libgpuFill(env->clip.x, env->clip.y, env->clip.w, env->clip.h,
				   rgb15(env->r0, env->g0, env->b0),
				   g_gpu.clipX0, g_gpu.clipY0, g_gpu.clipX1, g_gpu.clipY1);
	}
	return env;
}

extern "C" void SetDrawEnv(DR_ENV *dr_env, DRAWENV *env)
{
	/*	Packs the env as a primitive.  The game fills DRAWENV.dr_env at
		VidSetDrawEnv but never links or draws it (DrawOTagEnv unused), so a
		well-formed, inert packet is sufficient.  */
	(void)env;
	dr_env->tag = 0;	/* len 0 */
	for (int i = 0; i < 15; i++)
		dr_env->code[i] = 0;
}

extern "C" void SetDrawArea(DR_AREA *p, RECT *r)
{
	/* len=2: E3 top-left, E4 bottom-right (inclusive) */
	int x0 = r->x, y0 = r->y;
	int x1 = r->x + r->w - 1, y1 = r->y + r->h - 1;
	p->tag = ((u_long)2 << 24) | (p->tag & 0xFFFFFF);
	p->code[0] = 0xE3000000u | ((y0 & 0x1FF) << 10) | (x0 & 0x3FF);
	p->code[1] = 0xE4000000u | ((y1 & 0x1FF) << 10) | (x1 & 0x3FF);
}

/*****************************************************************************/
/*	The transfers model two layers, like ClearImage below (issue #26): the
	GP0 command masks a rect into the 10/9-bit VRAM space, but the libgpu
	call in front of it decides first what reaches the command.

	maskRect is the raw GP0 rule - top-left masked into VRAM, size
	((n-1) & mask) + 1, so 0 means the full 1024/512.  Only MoveImage
	passes a rect to it verbatim.  */
static void maskRect(const RECT *r, int *x, int *y, int *w, int *h)
{
	*x = r->x & 0x3FF;
	*y = r->y & 0x1FF;
	*w = ((r->w - 1) & 0x3FF) + 1;
	*h = ((r->h - 1) & 0x1FF) + 1;
}

/*	LoadImage/StoreImage (LIBGPU.LIB module SYS, executors 0x1b44 and
	0x1d80) clamp w to [0,1024] and h to [0,512] and size the DMA from
	w*h, so a zero-size rect moves nothing.  Through maskRect it would
	have become a full 1024- or 512-wide transfer through the caller's
	buffer (issue #60).  Returns 0 when there is nothing to move.  */
static int libgpuXferRect(const RECT *r, int *x, int *y, int *w, int *h)
{
	int cw = r->w < 0 ? 0 : (r->w > VRAM_W ? VRAM_W : (int)r->w);
	int ch = r->h < 0 ? 0 : (r->h > VRAM_H ? VRAM_H : (int)r->h);
	if (cw * ch == 0)
		return 0;
	*x = r->x & 0x3FF;
	*y = r->y & 0x1FF;
	*w = cw;
	*h = ch;
	return 1;
}

extern "C" int LoadImage(RECT *rect, u_long *p)
{
	int x, y, w, h;
	if (!libgpuXferRect(rect, &x, &y, &w, &h))
		return 0;
	const uint16_t *src = (const uint16_t *)p;
	for (int row = 0; row < h; row++)
	{
		int vy = (y + row) & 0x1FF;
		for (int col = 0; col < w; col++)
			g_vram[vy][(x + col) & 0x3FF] = *src++;
	}
	return 0;
}

extern "C" int StoreImage(RECT *rect, u_long *p)
{
	int x, y, w, h;
	if (!libgpuXferRect(rect, &x, &y, &w, &h))
		return 0;
	uint16_t *dst = (uint16_t *)p;
	for (int row = 0; row < h; row++)
	{
		int vy = (y + row) & 0x1FF;
		for (int col = 0; col < w; col++)
			*dst++ = g_vram[vy][(x + col) & 0x3FF];
	}
	return 0;
}

/*	libgpu's MoveImage (module SYS, 0x6e8) refuses a zero width or height
	with -1 and otherwise hands the rect to GP0(80h) unclamped, so the raw
	rule applies to everything else.  */
extern "C" int MoveImage(RECT *rect, int x, int y)
{
	if (rect->w == 0 || rect->h == 0)
		return -1;
	int sx, sy, w, h;
	maskRect(rect, &sx, &sy, &w, &h);
	x &= 0x3FF;
	y &= 0x1FF;
	/*	The game's only call never overlaps (icon erase: (422,184)->(422,440));
		copy via a row buffer anyway so any future overlap behaves sanely.  */
	uint16_t row[VRAM_W];
	for (int r = 0; r < h; r++)
	{
		int syr = (sy + r) & 0x1FF;
		int dyr = (y + r) & 0x1FF;
		for (int c = 0; c < w; c++)
			row[c] = g_vram[syr][(sx + c) & 0x3FF];
		for (int c = 0; c < w; c++)
			g_vram[dyr][(x + c) & 0x3FF] = row[c];
	}
	return 0;
}

/*****************************************************************************/
/*	libgpu's fill, shared by ClearImage, ClearImage2 and PutDrawEnv's isbg
	clear (issues #26, #28, #29).  Disassembled from LIBGPU.LIB module SYS:
	ClearImage (.text 0x500) and ClearImage2 (0x590) queue the same
	executor (0x1914), and PutDrawEnv's background clear comes from the
	same packet builder (0x1470).

	The builder first CLAMPS the rect: w -> [0,1023], h -> [0,511].  Without
	that, fmv.cpp's teardown ClearImage({0,0,512,512}) would mask h to 0 in
	the raw fill rule and clear NOTHING, leaving the movie's RGB24 bytes on
	screen as garbage when playback stops (issue #26).  Then it splits:

	- x and w both 64-aligned: E6=0, E1 (the current mode), then GP0(02h).
	  That is the raw fill - Raster_FillRect15, which masks and WRAPS at
	  the VRAM edge like the hardware command.

	- otherwise: E3=0, E4=FFFFFF, E5=0, E6=0, E1, then a GP0(60h) opaque
	  monochrome rect of (x, y, w, h), and E3/E4/E5 restored from the
	  library's copy afterwards.  So the draw is NOT clipped by the game's
	  draw env (issue #28 assumed it was) and ignores the mask bits: it is
	  an exact, undithered fill with the coordinates sign-extended to 11
	  bits like any GP0 vertex.  x clips at 1023; y clips at 511 (the
	  older 160-pin GPU's behaviour; newer GPUs may wrap, since libgpu
	  emits E4=FFFFFF raw).  No game caller reaches the y edge.
	  PutDrawEnv's clear runs with the env's own clip and offset in force
	  and compensates the offset, so it clips to the env's clip rect
	  instead - pass that as (cx0,cy0)-(cx1,cy1), inclusive; ClearImage
	  passes the whole of VRAM.

	What survives: libgpu restores only E3/E4/E5.  E6=0 (both paths) and
	the E1 word it wrote - ClearImage2's with the dfe bit set - stay in
	force until the next E1 or PutDrawEnv.  The shim models neither: it
	ignores E6 (the game never sets mask bits) and dfe (header comment),
	so no state change is visible here.

	The game's calls all land where they did before this split: fmv
	{0,0,512,512} and {0,0,320,480} and the ClearVRam stripes are aligned
	(02h), and actor.cpp's cache wipe {512,256,2048,254} clamps to w=1023
	and so takes the 60h path, which stops at the right edge - the green
	cache marker never wraps into the framebuffer columns (a wrap there
	was once a green block behind the loading token).  */
static int signext11(int v)
{
	return ((int)((unsigned)v << 21)) >> 21;
}

static void libgpuFill(int x, int y, int w, int h, uint16_t col15,
					   int cx0, int cy0, int cx1, int cy1)
{
	w = w < 0 ? 0 : (w > 1023 ? 1023 : w);
	h = h < 0 ? 0 : (h > 511 ? 511 : h);

	if (!(x & 0x3F) && !(w & 0x3F))
	{
		Raster_FillRect15(x, y, w, h, col15);		/* GP0(02h) */
		return;
	}

	/* GP0(60h): inclusive edges, clipped to the draw area and to VRAM */
	int x0 = signext11(x), y0 = signext11(y);
	int x1 = x0 + w - 1,   y1 = y0 + h - 1;
	if (x0 < cx0) x0 = cx0;
	if (y0 < cy0) y0 = cy0;
	if (x1 > cx1) x1 = cx1;
	if (y1 > cy1) y1 = cy1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
	if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
	for (int row = y0; row <= y1; row++)
		for (int c = x0; c <= x1; c++)
			g_vram[row][c] = col15;
}

static uint16_t rgb15(u_char r, u_char g, u_char b)
{
	return (uint16_t)(((r >> 3) & 0x1F)
					| (((g >> 3) & 0x1F) << 5)
					| (((b >> 3) & 0x1F) << 10));
}

extern "C" int ClearImage(RECT *rect, u_char r, u_char g, u_char b)
{
	libgpuFill(rect->x, rect->y, rect->w, rect->h, rgb15(r, g, b),
			   0, 0, VRAM_W - 1, VRAM_H - 1);
	return 0;
}

/*	The same executor with bit 31 set in the colour word, which only sets
	E1 bit 10 (dfe) for the fill - and the shim ignores dfe (see the header
	comment), so the pixels are ClearImage's.  */
extern "C" int ClearImage2(RECT *rect, u_char r, u_char g, u_char b)
{
	libgpuFill(rect->x, rect->y, rect->w, rect->h, rgb15(r, g, b),
			   0, 0, VRAM_W - 1, VRAM_H - 1);
	return 0;
}

/*****************************************************************************/
/*	OT initialisation.  Tag = (len<<24) | addr24; terminator addr 0xFFFFFF.
	Reverse: ot[i] links to ot[i-1], ot[0] terminated - DrawOTag(&ot[n-1])
	then draws index 0 LAST (topmost), matching otpos.h.  */
extern "C" u_long *ClearOTagR(u_long *ot, int n)
{
	ot[0] = 0x00FFFFFF;
	for (int i = 1; i < n; i++)
		ot[i] = (u_long)(uintptr_t)&ot[i - 1] & 0x00FFFFFF;
	return ot;
}

extern "C" u_long *ClearOTag(u_long *ot, int n)
{
	for (int i = 0; i < n - 1; i++)
		ot[i] = (u_long)(uintptr_t)&ot[i + 1] & 0x00FFFFFF;
	ot[n - 1] = 0x00FFFFFF;
	return ot;
}

/*****************************************************************************/
extern "C" void SetPolyG4(POLY_G4 *p)
{
	setlen(p, 8);
	setcode(p, 0x38);
}
