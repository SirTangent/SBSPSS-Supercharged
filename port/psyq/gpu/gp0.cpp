/*	GP0 command-stream interpreter + DrawOTag/DrawPrim.

	The game's packets are word streams, not one-struct-one-prim: the
	primplus types (TPOLY_*, TSPRT) carry a full 0xE1 draw-mode word before
	the drawing command, and DR_AREA is E3+E4 - so the OT walker interprets
	exactly `len` words per packet, dispatching on each command byte.

	Prim tags hold 24-bit addresses (P_TAG.addr): the whole game heap lives
	in one 16MB-aligned VirtualAlloc window (port/psyq/api/arena.cpp), and
	pointers reconstruct as window | addr24.  If the arena missed its
	aligned base at boot, that reconstruction is impossible - abort loudly
	rather than chase wild pointers.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <libgte.h>
#include <libgpu.h>

#include "system/types.h"
#include "system/lnkopt.h"
#include "stub_log.h"
#include "gpu/gpu_core.h"
#include "host/diag.h"

/*****************************************************************************/
static int signext11(int v)
{
	return ((int)((unsigned)v << 21)) >> 21;
}

static void vtxFromWord(RasterVtx *v, uint32_t xy)
{
	v->x = signext11(xy & 0xFFFF) + g_gpu.ofsX;
	v->y = signext11(xy >> 16) + g_gpu.ofsY;
}

/*	The one decode of E1 draw-mode bits.  Reached three ways - a standalone
	0xE1 word, a textured poly's tpage attribute, and PutDrawEnv (which
	assembles the same bit layout out of DRAWENV.tpage/dtd) - so it lives
	here rather than being spelled out at each.  */
void GPU_ApplyTexpage(uint32_t tp)
{
	g_gpu.texBaseX = (tp & 0xF) << 6;
	g_gpu.texBaseY = ((tp >> 4) & 1) << 8;
	g_gpu.semiMode = (tp >> 5) & 3;
	g_gpu.texDepth = (tp >> 7) & 3;
	g_gpu.dither   = (tp >> 9) & 1;
}

/*****************************************************************************/
/*	The one decode of the E2 texture-window word into the form the sampler
	uses (coord = (coord & ~(mask*8)) | ((offset & mask) * 8)).  Called from
	the 0xE2 handler and GPU reset - never per primitive.  */
void GPU_ApplyTexWindow(uint32_t word)
{
	int maskX = word & 0x1F, maskY = (word >> 5) & 0x1F;
	int offX = (word >> 10) & 0x1F, offY = (word >> 15) & 0x1F;

	g_gpu.texWindow = word;
	g_gpu.twMaskU   = maskX * 8;
	g_gpu.twOrU     = (offX & maskX) * 8;
	g_gpu.twMaskV   = maskY * 8;
	g_gpu.twOrV     = (offY & maskY) * 8;
}

/*	Snapshot the E1/E2 texture state a prim samples with.  Callers apply any
	embedded tpage attribute first (execPoly runs its vertex loop before
	this).  */
static void snapshotTexState(RasterCfg *cfg)
{
	cfg->texBaseX = g_gpu.texBaseX;
	cfg->texBaseY = g_gpu.texBaseY;
	cfg->texDepth = g_gpu.texDepth;
	cfg->semiMode = g_gpu.semiMode;

	cfg->twMaskU = g_gpu.twMaskU;
	cfg->twOrU   = g_gpu.twOrU;
	cfg->twMaskV = g_gpu.twMaskV;
	cfg->twOrV   = g_gpu.twOrV;
}

/*****************************************************************************/
/*	Polygons 0x20-0x3F.  Word layout per vertex k:
	  [k>0 && gouraud: colour]  [xy]  [textured: uv | (k==0?clut:k==1?tpage:0)<<16]
	Vertex 0's colour rides in the command word.  Returns words consumed.  */
static int execPoly(const uint32_t *w, int avail)
{
	uint8_t		cmd      = w[0] >> 24;
	int			gouraud  = (cmd >> 4) & 1;
	int			quad     = (cmd >> 3) & 1;
	int			textured = (cmd >> 2) & 1;
	int			verts    = quad ? 4 : 3;
	int			need     = 1 + verts + (textured ? verts : 0) + (gouraud ? verts - 1 : 0);

	if (need > avail)
		return avail;	/* malformed - consume what's left */

	RasterVtx	v[4];
	RasterCfg	cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.textured = textured;
	cfg.rawTex   = cmd & 1;
	cfg.semi     = (cmd >> 1) & 1;
	cfg.gouraud  = gouraud;

	int i = 1;
	for (int k = 0; k < verts; k++)
	{
		uint32_t col = (k == 0 || !gouraud) ? (k == 0 ? w[0] : 0) : w[i++];
		if (k > 0 && !gouraud)
			col = w[0];
		v[k].r = (uint8_t)(col & 0xFF);
		v[k].g = (uint8_t)((col >> 8) & 0xFF);
		v[k].b = (uint8_t)((col >> 16) & 0xFF);
		vtxFromWord(&v[k], w[i++]);
		if (textured)
		{
			uint32_t uv = w[i++];
			v[k].u = uv & 0xFF;
			v[k].v = (uv >> 8) & 0xFF;
			if (k == 0)
			{
				uint32_t clut = uv >> 16;
				cfg.clutX = (clut & 0x3F) << 4;
				cfg.clutY = (clut >> 6) & 0x1FF;
			}
			else if (k == 1)
			{
				GPU_ApplyTexpage(uv >> 16);	/* poly tpage attribute programs E1 bits */
			}
		}
	}

	snapshotTexState(&cfg);
	cfg.dither = g_gpu.dither;

	Raster_Triangle(&v[0], &v[1], &v[2], &cfg);
	if (quad)
		Raster_Triangle(&v[1], &v[2], &v[3], &cfg);
	return need;
}

/*****************************************************************************/
/*	Rectangles 0x60-0x7F: size bits 3-4 (0=variable,1=1x1,2=8x8,3=16x16).
	No tpage word - texture state comes from the current E1 (which the
	game's TSPRT packets set via their embedded t_code word).  */
static void rasterRect(int x, int y, int w, int h, int u0, int v0,
					   uint32_t colw, const RasterCfg *cfg);

static int execRect(const uint32_t *w, int avail)
{
	uint8_t		cmd      = w[0] >> 24;
	int			sizebits = (cmd >> 3) & 3;
	int			textured = (cmd >> 2) & 1;
	int			need     = 2 + (textured ? 1 : 0) + (sizebits == 0 ? 1 : 0);

	if (need > avail)
		return avail;

	RasterCfg	cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.textured = textured;
	cfg.rawTex   = cmd & 1;
	cfg.semi     = (cmd >> 1) & 1;
	snapshotTexState(&cfg);				/* rects are never dithered */

	int i = 1;
	int x = signext11(w[i] & 0xFFFF) + g_gpu.ofsX;
	int y = signext11(w[i] >> 16) + g_gpu.ofsY;
	i++;
	int u0 = 0, v0 = 0;
	if (textured)
	{
		u0 = w[i] & 0xFF;
		v0 = (w[i] >> 8) & 0xFF;
		uint32_t clut = w[i] >> 16;
		cfg.clutX = (clut & 0x3F) << 4;
		cfg.clutY = (clut >> 6) & 0x1FF;
		i++;
	}
	int rw, rh;
	switch (sizebits)
	{
	case 1:  rw = rh = 1;  break;
	case 2:  rw = rh = 8;  break;
	case 3:  rw = rh = 16; break;
	default: rw = w[i] & 0x3FF; rh = (w[i] >> 16) & 0x1FF; i++; break;
	}

	rasterRect(x, y, rw, rh, u0, v0, w[0], &cfg);
	return need;
}

/*	Axis-aligned rect: no edge rules, texture walks u/v with byte wrap.
	Shares the pixel pipeline with the triangle path via Raster_Triangle's
	helpers - implemented in raster.cpp; declared here for clarity.  */
void Raster_Rect(int x, int y, int w, int h, int u0, int v0,
				 uint8_t r, uint8_t g, uint8_t b, const RasterCfg *cfg);

static void rasterRect(int x, int y, int w, int h, int u0, int v0,
					   uint32_t colw, const RasterCfg *cfg)
{
	Raster_Rect(x, y, w, h, u0, v0,
				(uint8_t)(colw & 0xFF), (uint8_t)((colw >> 8) & 0xFF),
				(uint8_t)((colw >> 16) & 0xFF), cfg);
}

/*****************************************************************************/
/*	Lines 0x40-0x5F: gouraud bit4, polyline bit3, semi bit1.  Polylines
	draw segment chains to the 0x5xxx5xxx terminator word.  (The game
	builds only LINE_F2; the rest is coverage for the M3 exit criterion.)

	The terminator is recognised ONLY at the first word of a vertex group -
	the colour word when shaded, the vertex word otherwise (vertex 1 has no
	colour word either way: colour 1 rides in the command).  Vertex X/Y are
	full 16-bit fields, so a legitimate vertex may well look like
	0x5xxx5xxx; testing it there would drop a real segment and, worse,
	hand the rest of the chain back to the command dispatcher as fresh GP0
	words (0x55 decodes as another line), desyncing the whole stream.  */
#define POLYLINE_TERM(word)	(((word) & 0xF000F000u) == 0x50005000u)

static int execLine(const uint32_t *w, int avail)
{
	uint8_t		cmd      = w[0] >> 24;
	int			gouraud  = (cmd >> 4) & 1;
	int			polyline = (cmd >> 3) & 1;

	if (polyline)
	{
		RasterVtx a, b;
		RasterCfg cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.semi     = (cmd >> 1) & 1;
		cfg.gouraud  = gouraud;
		cfg.semiMode = g_gpu.semiMode;
		cfg.dither   = g_gpu.dither;

		a.r = (uint8_t)(w[0] & 0xFF);
		a.g = (uint8_t)((w[0] >> 8) & 0xFF);
		a.b = (uint8_t)((w[0] >> 16) & 0xFF);
		b.r = a.r;  b.g = a.g;  b.b = a.b;		/* flat: command colour */

		int i = 1;

		/*	group 1 is the bare vertex 1 - terminator-checked like any
			other group start, so an empty chain draws nothing instead of
			swallowing the terminator as a coordinate and decoding the
			following primitive as polyline data  */
		if (i >= avail || POLYLINE_TERM(w[i]))
			return (i < avail) ? i + 1 : avail;
		vtxFromWord(&a, w[i++]);

		/*	groups 2..n: [colour] vertex  */
		while (i < avail && !POLYLINE_TERM(w[i]))
		{
			if (gouraud)
			{
				b.r = (uint8_t)(w[i] & 0xFF);
				b.g = (uint8_t)((w[i] >> 8) & 0xFF);
				b.b = (uint8_t)((w[i] >> 16) & 0xFF);
				i++;
				if (i >= avail)
					break;						/* truncated: colour, no vertex */
			}
			vtxFromWord(&b, w[i++]);
			Raster_Line(&a, &b, &cfg);
			a = b;
		}
		return (i < avail) ? i + 1 : avail;		/* consume the terminator */
	}

	int need = gouraud ? 4 : 3;
	if (need > avail)
		return avail;

	RasterVtx a, b;
	RasterCfg cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.semi     = (cmd >> 1) & 1;
	cfg.gouraud  = gouraud;
	cfg.semiMode = g_gpu.semiMode;
	cfg.dither   = g_gpu.dither;

	a.r = (uint8_t)(w[0] & 0xFF);
	a.g = (uint8_t)((w[0] >> 8) & 0xFF);
	a.b = (uint8_t)((w[0] >> 16) & 0xFF);
	int i = 1;
	vtxFromWord(&a, w[i++]);
	if (gouraud)
	{
		b.r = (uint8_t)(w[i] & 0xFF);
		b.g = (uint8_t)((w[i] >> 8) & 0xFF);
		b.b = (uint8_t)((w[i] >> 16) & 0xFF);
		i++;
	}
	else
	{
		b.r = a.r; b.g = a.g; b.b = a.b;
	}
	vtxFromWord(&b, w[i++]);

	Raster_Line(&a, &b, &cfg);
	return need;
}

/*****************************************************************************/
void GPU_ExecWords(const uint32_t *words, int count)
{
	int i = 0;
	while (i < count)
	{
		uint32_t word = words[i];
		uint8_t  cmd  = word >> 24;

		if (cmd >= 0x20 && cmd <= 0x3F)
			i += execPoly(words + i, count - i);
		else if (cmd >= 0x40 && cmd <= 0x5F)
			i += execLine(words + i, count - i);
		else if (cmd >= 0x60 && cmd <= 0x7F)
			i += execRect(words + i, count - i);
		else if (cmd == 0xE1)
		{
			GPU_ApplyTexpage(word);
			i++;
		}
		else if (cmd == 0xE2)
		{
			GPU_ApplyTexWindow(word);
			i++;
		}
		else if (cmd == 0xE3)
		{
			g_gpu.clipX0 = word & 0x3FF;
			g_gpu.clipY0 = (word >> 10) & 0x1FF;
			i++;
		}
		else if (cmd == 0xE4)
		{
			g_gpu.clipX1 = word & 0x3FF;
			g_gpu.clipY1 = (word >> 10) & 0x1FF;
			i++;
		}
		else if (cmd == 0xE5)
		{
			g_gpu.ofsX = signext11(word & 0x7FF);
			g_gpu.ofsY = signext11((word >> 11) & 0x7FF);
			i++;
		}
		else if (cmd == 0x02)
		{
			if (count - i < 3)
				return;
			uint32_t col = words[i];
			uint16_t c15 = (uint16_t)(((col >> 3) & 0x1F)
						 | (((col >> 11) & 0x1F) << 5)
						 | (((col >> 19) & 0x1F) << 10));
			Raster_FillRect15(words[i + 1] & 0xFFFF, words[i + 1] >> 16,
							  words[i + 2] & 0xFFFF, words[i + 2] >> 16, c15);
			i += 3;
		}
		else if (cmd == 0x00 || cmd == 0x01 || cmd == 0xE6)
		{
			i++;	/* nop / cache clear / mask bits (game never sets E6) */
		}
		else
		{
			PSYQ_LOG_ONCE_KEYED(cmd, "[gpu] unknown/unimplemented GP0 "
									 "command 0x%02X - skipping packet\n", cmd);
			return;	/* unknown: abandon the rest of this packet */
		}
	}
}

/*****************************************************************************/
static uintptr_t g_arenaBase, g_arenaEnd;	/* [base, end) - the reconstructable range */

static void verifyArenaWindowOnce(void)
{
	static int checked;
	if (checked)
		return;
	checked = 1;
	uintptr_t base = (uintptr_t)OPT_LinkerOpts.FreeMemAddress;
	uintptr_t end  = base + OPT_LinkerOpts.FreeMemSize - 1;
	if ((base >> 24) != (end >> 24))
	{
		fprintf(stderr, "[gpu] arena straddles a 16MB window (%p..%p) - "
						"24-bit prim tags cannot be reconstructed; aborting\n",
				(void *)base, (void *)end);
		Port_Exit(PORT_EXIT_FAULT);
	}
	g_arenaBase = base;
	g_arenaEnd  = end + 1;
}

/*	Prim-pool high-water watch.  These are the game's own pool pointers
	(source/gfx/prim.cpp) - plain file-scope globals, so unmangled.

	It lives here rather than in the game because prim.cpp's own overflow
	check is post-hoc (`if(CurrPrim>=EndPrim) ASSERT` in PrimDisplay, AFTER
	a frame has already written past the buffer) and ASSERT compiles to
	nothing in FINAL, so an overrun corrupts whatever MemAlloc placed after
	the pool, silently, in the shipping variant.  CLayerTile3d::render()
	writes through a raw pointer with no bound check of its own, and the PC
	build widened the 3D tile window (conv_pc.md #18), which raised the
	peak - so the remaining headroom is worth watching in both variants.

	DrawOTag is called from PrimDisplay at the exact peak of a frame's
	fill, which makes it the cheapest correct sampling point.

	The pool pointers are read through the diag registry
	(Port_RegisterGameGlobals, called once from system/main.cpp): the shim's
	own unit tests (gpu_test and friends) link no game code, never register,
	and the watch simply does nothing.  */
static size_t	g_primPeak;

extern "C" unsigned long GPU_PrimPoolPeak(void)
{
	return (unsigned long)g_primPeak;
}

static void primPoolWatch(void)
{
	static int		warned;
	static int		logging = -1;
	int				advanced = 0;
	const PortGameGlobals *g = Port_GameGlobals();

	if (!g->currPrim)
		return;						/* no game code registered (shim unit tests) */
	if (!*g->primListStart || !*g->primListEnd || !*g->currPrim || !*g->endPrim)
		return;						/* PrimInit has not run yet */

	/*	Both buffers are one allocation, so the per-buffer budget is half
		the span - no need to duplicate PRIMPOOL_SIZE here.  `used` is
		signed on purpose: past the end it reads above the pool size.  */
	ptrdiff_t	pool = (*g->primListEnd - *g->primListStart) / 2;
	ptrdiff_t	used = pool - (*g->endPrim - *g->currPrim);

	if (used > (ptrdiff_t)g_primPeak)
	{
		g_primPeak = (size_t)used;
		advanced = 1;
	}
	if (logging < 0)
	{
		const char *e = getenv("SBSP_PRIM_LOG");
		logging = (e && *e && *e != '0');
	}
	if (logging && advanced)
		fprintf(stderr, "[gpu] prim pool high-water %ld/%ld bytes (%.1f%%)\n",
				(long)used, (long)pool, 100.0 * (double)used / (double)pool);

	if (!warned && used > pool - pool / 8)		/* inside the last 12.5% */
	{
		warned = 1;
		fprintf(stderr, "[gpu] WARNING: prim pool at %ld/%ld bytes (%.1f%%) - "
						"raise MAX_PRIMS (source/gfx/prim.h) before it overruns; "
						"the game's own check is DEBUG-only and post-hoc\n",
				(long)used, (long)pool, 100.0 * (double)used / (double)pool);
	}
}

extern "C" void DrawOTag(u_long *p)
{
	verifyArenaWindowOnce();
	primPoolWatch();

	uintptr_t	window = (uintptr_t)p & ~(uintptr_t)0xFFFFFF;
	uint32_t	*tagp  = (uint32_t *)p;
	int			guard  = 1 << 20;

	for (;;)
	{
		uint32_t tag  = *tagp;
		int      len  = tag >> 24;
		uint32_t next = tag & 0xFFFFFF;

		if (len)
			GPU_ExecWords(tagp + 1, len);
		if (next == 0xFFFFFF)
			break;

		uintptr_t addr = window | next;
		/*	The reconstruction only holds while every linked prim lives in the
			arena.  A prim allocated elsewhere (a static, a stack POLY, a
			future non-arena pool) yields a plausible-looking wild pointer that
			would be interpreted as GP0 words - state the invariant here so it
			fails at the cause instead of somewhere downstream.  */
		if (addr < g_arenaBase || addr + 4 > g_arenaEnd || (addr & 3))
		{
			fprintf(stderr, "[gpu] DrawOTag: tag %08lX at %p reconstructs to %p, "
							"outside the prim arena (%p..%p) - a linked primitive "
							"was not allocated from the game heap\n",
					(unsigned long)tag, (void *)tagp, (void *)addr,
					(void *)g_arenaBase, (void *)g_arenaEnd);
			Port_Exit(PORT_EXIT_FAULT);
		}
		tagp = (uint32_t *)addr;
		if (--guard == 0)
		{
			fprintf(stderr, "[gpu] DrawOTag: runaway tag chain - corrupt OT?\n");
			Port_Exit(PORT_EXIT_FAULT);
		}
	}
}

extern "C" void DrawPrim(void *p)
{
	uint32_t *w = (uint32_t *)p;
	GPU_ExecWords(w + 1, w[0] >> 24);
}
