/*	libgpu: VRAM transfers, environments, OT setup, reset/display control.

	Semantics pinned by the game's call sites (see the M2 recon in the plan):
	- LoadImage/MoveImage must be synchronous (animtex.cpp reuses the source
	  buffer immediately after LoadImage; LoadingIcon DrawPrims right after
	  MoveImage with no DrawSync).
	- ClearImage must mask/clamp rects like hardware: actor.cpp:299 passes
	  {512,256,2048,254}, which must degrade harmlessly.
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
/*	VRAM fill used by ClearImage and GP0 0x02: hardware masking.
	X masked to 0x3F0 steps? (fill x is in 16-halfword steps only for the
	0x02 command); ClearImage routes through the same rules so oversized
	rects clamp instead of overrunning.  */
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
	rgb[0] = (unsigned char)((px & 0x1F) << 3);
	rgb[1] = (unsigned char)(((px >> 5) & 0x1F) << 3);
	rgb[2] = (unsigned char)(((px >> 10) & 0x1F) << 3);
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
		uint16_t col = (uint16_t)(((env->r0 >> 3) & 0x1F)
					 | (((env->g0 >> 3) & 0x1F) << 5)
					 | (((env->b0 >> 3) & 0x1F) << 10));
		Raster_FillRect15(env->clip.x, env->clip.y, env->clip.w, env->clip.h, col);
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
/*	rect helper shared by the transfers: top-left masked into VRAM, sizes
	clamped so a bogus rect cannot overrun the array (hardware wraps; the
	one oversized caller - actor.cpp ClearImage w=2048 - just wants "a lot",
	and masking reproduces hardware's 10/9-bit coordinate space).  */
static void maskRect(const RECT *r, int *x, int *y, int *w, int *h)
{
	*x = r->x & 0x3FF;
	*y = r->y & 0x1FF;
	*w = ((r->w - 1) & 0x3FF) + 1;
	*h = ((r->h - 1) & 0x1FF) + 1;
}

extern "C" int LoadImage(RECT *rect, u_long *p)
{
	int x, y, w, h;
	maskRect(rect, &x, &y, &w, &h);
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
	maskRect(rect, &x, &y, &w, &h);
	uint16_t *dst = (uint16_t *)p;
	for (int row = 0; row < h; row++)
	{
		int vy = (y + row) & 0x1FF;
		for (int col = 0; col < w; col++)
			*dst++ = g_vram[vy][(x + col) & 0x3FF];
	}
	return 0;
}

extern "C" int MoveImage(RECT *rect, int x, int y)
{
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

extern "C" int ClearImage(RECT *rect, u_char r, u_char g, u_char b)
{
	uint16_t col = (uint16_t)(((r >> 3) & 0x1F)
				 | (((g >> 3) & 0x1F) << 5)
				 | (((b >> 3) & 0x1F) << 10));

	/*	libgpu CLAMPS the rect before emitting the GP0(02h) fill - it does
		not pass it verbatim (disassembled from LIBGPU.LIB's ClearImage
		packet builder): w -> [0,1023], h -> [0,511].  Without this,
		fmv.cpp's teardown ClearImage({0,0,512,512}) would mask h to 0 in
		the raw fill rules below and clear NOTHING, leaving the movie's
		RGB24 bytes on screen as garbage when playback stops (issue #26).
		The raw GP0 masking stays in Raster_FillRect15 - it is correct for
		the command level, wrong for the library call.  (libgpu also
		reroutes x not 64-aligned through a GP0(60h) draw, which clips by
		the DRAW env - no game caller does that; log it.)

		A rect crossing the VRAM edge then CLIPS instead of wrapping:
		actor.cpp's cache wipe ClearImage({512,256,2048,254}, green) means
		"to the right edge" - clamp alone gives x=512 w=1023, and a
		wrapping fill would paint the green cache marker across the
		framebuffer columns 0..511, a green overlay on every loading
		screen (the retail game never shows that, so the console result
		of this exact call cannot have wrapped into the display).  */
	int x = rect->x & 0x3F0;
	int y = rect->y & 0x1FF;
	int w = rect->w < 0 ? 0 : (rect->w > 1023 ? 1023 : (int)rect->w);
	int h = rect->h < 0 ? 0 : (rect->h > 511 ? 511 : (int)rect->h);
	if (x + w > 1024)
		w = 1024 - x;
	if (y + h > 512)
		h = 512 - y;
	if (rect->x & 0x3F)
		PSYQ_LOG_ONCE_KEYED(60, "[gpu] ClearImage x=%d not 64-aligned - "
							"libgpu would draw-clip this, shim fills raw\n",
							rect->x);
	Raster_FillRect15(x, y, w, h, col);
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
