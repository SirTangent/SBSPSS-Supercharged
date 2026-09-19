/*	Differential test for the software rasterizer (M8 perf pass).

	tests/raster_ref.cpp is the pre-optimisation port/psyq/gpu/raster.cpp,
	frozen, with its entry points renamed RasterRef_*.  Every case here draws
	one seeded-random primitive with the reference and with the live
	rasterizer into identical noise VRAM and compares the result halfword
	for halfword: the perf pass is allowed to change how long a primitive
	takes, never a pixel of it.

	What the generator goes out of its way to hit:
	- every RasterCfg combination (textured x raw x semi x 4 ABR modes x
	  gouraud x dither x 4/8/15bpp x texture window);
	- clip rects anywhere in VRAM including its edges, 1-pixel ones, and the
	  whole of it; vertices inside, straddling and far outside the clip
	  rect, zero-area and over-size (>1023x511) triangles, both windings,
	  quads as the interpreter's 012/123 pair (shared-edge fill rule);
	- rects clipped on the left/top (the u/v start adjustment) with u/v
	  walking past 255; lines in every octant, gouraud both ascending and
	  descending (C division truncates toward zero), zero length;
	- texel 0 (transparent) and STP texels: the noise is salted with zeros;
	- uniform vertex colours on and around 128,128,128, where raster.cpp
	  folds gouraud to flat and neutral modulation to raw;
	- the texture page and CLUT OVERLAPPING the draw target, so the
	  read-after-write order inside one primitive is pinned too.

	Writes outside the clip rect are caught by a full-VRAM comparison against
	a shadow copy after EVERY case - see the loop for why a cadence is not
	good enough.  The two rasterizers take turns drawing first, so the
	closing table is not biased by one of them always meeting a warm cache.

	RASTER_DIFF_CASES=<n> overrides the case count (soak), RASTER_DIFF_SEED
	the seed.  The closing table is the micro-benchmark: reference vs live
	time over the identical primitives, per class.
*/
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "gpu/gpu_core.h"

void RasterRef_Triangle(const RasterVtx *v0, const RasterVtx *v1, const RasterVtx *v2,
						const RasterCfg *cfg);
void RasterRef_Rect(int x, int y, int w, int h, int u0, int v0,
					uint8_t r, uint8_t g, uint8_t b, const RasterCfg *cfg);
void RasterRef_Line(const RasterVtx *a, const RasterVtx *b, const RasterCfg *cfg);

#define DEFAULT_CASES		20000
#define RENOISE_EVERY		2048
#define MAX_REPORTS			10

/*****************************************************************************/
static uint32_t g_rng;

static uint32_t rnd(void)
{
	g_rng = g_rng * 1664525u + 1013904223u;
	return g_rng >> 8;
}

/* uniform in [lo, hi] */
static int rndRange(int lo, int hi)
{
	return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static int chance(int percent)
{
	return (int)(rnd() % 100) < percent;
}

/*****************************************************************************/
static uint16_t s_shadow[VRAM_H][VRAM_W];	/* what g_vram must be outside the case's clip rect */
static uint16_t s_pre[VRAM_H][VRAM_W];		/* clip-rect contents before the case */
static uint16_t s_first[VRAM_H][VRAM_W];	/* clip-rect contents after whichever drew first */

static void noiseVram(void)
{
	for (int y = 0; y < VRAM_H; y++)
		for (int x = 0; x < VRAM_W; x++)
		{
			uint32_t r = rnd();
			/* 1 in 8 halfwords is the transparent texel; STP rides in the noise */
			g_vram[y][x] = (r & 0x70000) ? (uint16_t)r : 0;
		}
	memcpy(s_shadow, g_vram, sizeof(s_shadow));
}

static void copyRect(uint16_t dst[VRAM_H][VRAM_W], uint16_t src[VRAM_H][VRAM_W],
					 int x0, int y0, int x1, int y1)
{
	for (int y = y0; y <= y1; y++)
		memcpy(&dst[y][x0], &src[y][x0], (size_t)(x1 - x0 + 1) * sizeof(uint16_t));
}

/*****************************************************************************/
enum { K_TRI, K_QUAD, K_RECT, K_LINE };

struct Case
{
	int			kind;
	RasterCfg	cfg;
	RasterVtx	v[4];
	int			rx, ry, rw, rh, ru, rv;		/* rect */
	int			clipX0, clipY0, clipX1, clipY1;
};

/*	timing classes  */
enum
{
	T_TRI_FLAT, T_TRI_GOURAUD, T_TRI_TEX, T_TRI_TEX_GOURAUD,
	T_RECT_FLAT, T_RECT_TEX, T_LINE, T_COUNT
};
static const char *const s_className[T_COUNT] =
{
	"tri flat", "tri gouraud", "tri textured", "tri tex+gouraud",
	"rect flat", "rect textured", "line",
};
static double	s_refNs[T_COUNT], s_newNs[T_COUNT];
static long		s_classCases[T_COUNT];

static int classOf(const Case &c)
{
	switch (c.kind)
	{
	case K_RECT:	return c.cfg.textured ? T_RECT_TEX : T_RECT_FLAT;
	case K_LINE:	return T_LINE;
	default:
		if (c.cfg.textured)
			return c.cfg.gouraud ? T_TRI_TEX_GOURAUD : T_TRI_TEX;
		return c.cfg.gouraud ? T_TRI_GOURAUD : T_TRI_FLAT;
	}
}

/*****************************************************************************/
static int coordNear(int lo, int hi)
{
	/*	the full range a vertex can reach: signext11() gives -1024..1023 and
		the drawing offset adds another -1024..1023  */
	if (chance(6))
		return rndRange(-2048, 2046);		/* far out: size rejects, heavy clipping */
	return rndRange(lo - 48, hi + 48);
}

static void makeVtx(RasterVtx *v, const Case &c)
{
	v->x = coordNear(c.clipX0, c.clipX1);
	v->y = coordNear(c.clipY0, c.clipY1);
	v->u = rndRange(0, 255);
	v->v = rndRange(0, 255);
	v->r = (uint8_t)rnd();
	v->g = (uint8_t)rnd();
	v->b = (uint8_t)rnd();
}

static void makeCase(Case *c)
{
	memset(c, 0, sizeof(*c));

	int k = rndRange(0, 99);
	c->kind = k < 40 ? K_TRI : k < 55 ? K_QUAD : k < 85 ? K_RECT : K_LINE;

	RasterCfg &cfg = c->cfg;
	cfg.semi     = chance(40);
	cfg.semiMode = rndRange(0, 3);
	/*	The interpreter never textures a line or shades/dithers a rect, but
		the entry points take any cfg and always ran it through the one pixel
		pipeline - keep a trickle of those so that stays true.  */
	if (c->kind != K_LINE || chance(8))
	{
		cfg.textured = chance(65);
		cfg.rawTex   = chance(35);
	}
	if (c->kind != K_RECT || chance(8))
	{
		cfg.gouraud = chance(50);
		cfg.dither  = chance(50);
	}
	cfg.texBaseX = rndRange(0, 15) << 6;
	cfg.texBaseY = rndRange(0, 1) << 8;
	/*	0..3, not 0..2: gp0.cpp takes texDepth straight out of the E1 bits
		((tp >> 7) & 3), so 3 reaches the rasterizer, and it is exactly the
		value the flag word folds away (the reference's switch `default:` vs
		the new depth-2 arm).  The fold is only proven if the fuzzer can
		produce it.  */
	cfg.texDepth = rndRange(0, 3);
	cfg.clutX    = rndRange(0, 63) << 4;
	cfg.clutY    = rndRange(0, 511);
	if (chance(25))
	{
		/* the E2 form GPU_ApplyTexWindow derives: mask*8, (offset & mask)*8 */
		int maskX = rndRange(0, 31), maskY = rndRange(0, 31);
		cfg.twMaskU = maskX * 8;
		cfg.twOrU   = (rndRange(0, 31) & maskX) * 8;
		cfg.twMaskV = maskY * 8;
		cfg.twOrV   = (rndRange(0, 31) & maskY) * 8;
	}

	/* clip rect */
	int w, h;
	if (chance(2))			{ w = VRAM_W; h = VRAM_H; }
	else if (chance(5))		{ w = rndRange(1, 2); h = rndRange(1, 2); }
	else					{ w = rndRange(1, 160); h = rndRange(1, 120); }
	if (cfg.textured && chance(35))
	{
		/* draw INTO the texture page / over the CLUT row */
		if (chance(50))
		{
			c->clipX0 = cfg.texBaseX + rndRange(-8, 40);
			c->clipY0 = cfg.texBaseY + rndRange(-8, 200);
		}
		else
		{
			c->clipX0 = cfg.clutX + rndRange(-20, 8);
			c->clipY0 = cfg.clutY + rndRange(-20, 0);
		}
		if (c->clipX0 < 0) c->clipX0 = 0;
		if (c->clipY0 < 0) c->clipY0 = 0;
		if (c->clipX0 > VRAM_W - 1) c->clipX0 = VRAM_W - 1;
		if (c->clipY0 > VRAM_H - 1) c->clipY0 = VRAM_H - 1;
	}
	else
	{
		c->clipX0 = rndRange(0, VRAM_W - w);
		c->clipY0 = rndRange(0, VRAM_H - h);
	}
	c->clipX1 = c->clipX0 + w - 1;
	c->clipY1 = c->clipY0 + h - 1;
	if (c->clipX1 > VRAM_W - 1) c->clipX1 = VRAM_W - 1;
	if (c->clipY1 > VRAM_H - 1) c->clipY1 = VRAM_H - 1;

	for (int i = 0; i < 4; i++)
		makeVtx(&c->v[i], *c);

	/*	Uniform colours, on and around 128,128,128: the game's neutral sprite
		colour is where raster.cpp folds modulation away, and equal vertex
		colours are where it folds gouraud to flat - the neighbours (127, 129,
		one channel off) must NOT fold.  */
	if (chance(20))
	{
		static const uint8_t greys[] = { 128, 128, 128, 127, 129, 0, 255, 64 };
		uint8_t r = greys[rndRange(0, 7)], g = r, b = r;
		if (chance(25))
			(chance(50) ? g : b) = greys[rndRange(0, 7)];
		for (int i = 0; i < 4; i++)
		{
			c->v[i].r = r;  c->v[i].g = g;  c->v[i].b = b;
		}
		if (chance(15))
			c->v[rndRange(1, 3)].b ^= 1;			/* almost uniform */
	}

	if (c->kind == K_TRI || c->kind == K_QUAD)
	{
		if (chance(4))
			c->v[2] = c->v[chance(50)];				/* zero area */
		if (chance(4))
			c->v[1].y = c->v[0].y;					/* flat top/bottom edge */
		if (chance(4))
			c->v[1].x = c->v[0].x;					/* vertical edge */
	}
	else if (c->kind == K_LINE)
	{
		if (chance(5))
			c->v[1] = c->v[0];						/* zero length */
		if (chance(10))	c->v[1].y = c->v[0].y;
		if (chance(10))	c->v[1].x = c->v[0].x;
	}
	else
	{
		static const int fixed[3] = { 1, 8, 16 };
		if (chance(45))
			c->rw = c->rh = fixed[rndRange(0, 2)];
		else if (chance(4))
		{
			c->rw = rndRange(1000, 1100);			/* around the 1023x511 reject */
			c->rh = rndRange(500, 520);
		}
		else
		{
			c->rw = rndRange(0, 300);				/* 0 = empty; >256 wraps u */
			c->rh = rndRange(0, 300);
		}
		c->rx = coordNear(c->clipX0, c->clipX1) - (chance(50) ? c->rw / 2 : 0);
		c->ry = coordNear(c->clipY0, c->clipY1) - (chance(50) ? c->rh / 2 : 0);
		c->ru = rndRange(0, 255);
		c->rv = rndRange(0, 255);
	}
}

/*****************************************************************************/
static void draw(const Case &c, int useRef)
{
	g_gpu.clipX0 = c.clipX0;  g_gpu.clipY0 = c.clipY0;
	g_gpu.clipX1 = c.clipX1;  g_gpu.clipY1 = c.clipY1;

	switch (c.kind)
	{
	case K_TRI:
		if (useRef)	RasterRef_Triangle(&c.v[0], &c.v[1], &c.v[2], &c.cfg);
		else		Raster_Triangle(&c.v[0], &c.v[1], &c.v[2], &c.cfg);
		break;
	case K_QUAD:	/* the interpreter's split - gp0.cpp execPoly */
		if (useRef)
		{
			RasterRef_Triangle(&c.v[0], &c.v[1], &c.v[2], &c.cfg);
			RasterRef_Triangle(&c.v[1], &c.v[2], &c.v[3], &c.cfg);
		}
		else
		{
			Raster_Triangle(&c.v[0], &c.v[1], &c.v[2], &c.cfg);
			Raster_Triangle(&c.v[1], &c.v[2], &c.v[3], &c.cfg);
		}
		break;
	case K_RECT:
		if (useRef)	RasterRef_Rect(c.rx, c.ry, c.rw, c.rh, c.ru, c.rv, c.v[0].r, c.v[0].g, c.v[0].b, &c.cfg);
		else		Raster_Rect(c.rx, c.ry, c.rw, c.rh, c.ru, c.rv, c.v[0].r, c.v[0].g, c.v[0].b, &c.cfg);
		break;
	default:
		if (useRef)	RasterRef_Line(&c.v[0], &c.v[1], &c.cfg);
		else		Raster_Line(&c.v[0], &c.v[1], &c.cfg);
		break;
	}
}

static void describe(const Case &c, long n)
{
	static const char *const kinds[] = { "tri", "quad", "rect", "line" };
	const RasterCfg &f = c.cfg;
	std::printf("  case %ld: %s  clip (%d,%d)-(%d,%d)\n", n, kinds[c.kind],
				c.clipX0, c.clipY0, c.clipX1, c.clipY1);
	std::printf("    cfg: tex=%d raw=%d semi=%d mode=%d gouraud=%d dither=%d depth=%d "
				"tpage=(%d,%d) clut=(%d,%d) tw=%d/%d/%d/%d\n",
				f.textured, f.rawTex, f.semi, f.semiMode, f.gouraud, f.dither, f.texDepth,
				f.texBaseX, f.texBaseY, f.clutX, f.clutY, f.twMaskU, f.twOrU, f.twMaskV, f.twOrV);
	if (c.kind == K_RECT)
		std::printf("    rect: x=%d y=%d w=%d h=%d u=%d v=%d rgb=%d,%d,%d\n",
					c.rx, c.ry, c.rw, c.rh, c.ru, c.rv, c.v[0].r, c.v[0].g, c.v[0].b);
	else
		for (int i = 0; i < (c.kind == K_QUAD ? 4 : c.kind == K_TRI ? 3 : 2); i++)
			std::printf("    v%d: x=%d y=%d u=%d v=%d rgb=%d,%d,%d\n", i,
						c.v[i].x, c.v[i].y, c.v[i].u, c.v[i].v, c.v[i].r, c.v[i].g, c.v[i].b);
}

/*****************************************************************************/
/*	Name the pixel this case wrote outside its clip rect.  Reached only when
	the per-case memcmp has already found one.  */
static void reportStray(const Case &c, long n)
{
	for (int y = 0; y < VRAM_H; y++)
		for (int x = 0; x < VRAM_W; x++)
			if (g_vram[y][x] != s_shadow[y][x])
			{
				std::printf("FAIL: case %ld wrote outside its clip rect - "
							"vram[%d][%d] = %04x, want %04x\n",
							n, y, x, g_vram[y][x], s_shadow[y][x]);
				describe(c, n);
				return;
			}
}

int main()
{
	typedef std::chrono::steady_clock Clock;

	long cases = DEFAULT_CASES;
	if (const char *e = std::getenv("RASTER_DIFF_CASES"))
		cases = std::atol(e);
	g_rng = 0x5B5B5053u;
	if (const char *e = std::getenv("RASTER_DIFF_SEED"))
		g_rng = (uint32_t)std::strtoul(e, NULL, 0);

	std::printf("raster_diff_test: %ld cases, seed %08X\n", cases, (unsigned)g_rng);

	int		failures = 0;
	long	drawn = 0;			/* cases where the reference changed at least one pixel */
	long	executed = 0;		/* the loop stops early once MAX_REPORTS is hit */

	for (long n = 0; n < cases && failures < MAX_REPORTS; n++)
	{
		if ((n % RENOISE_EVERY) == 0)
			noiseVram();

		Case c;
		makeCase(&c);
		int cls = classOf(c);
		executed++;

		/*	Alternate which rasterizer draws first.  Whichever runs second
			starts with the clip rect freshly written by the first, so a fixed
			order hands it a warm cache and biases the table below; over a run
			each side pays that cost equally.  */
		const int refFirst = (n & 1) == 0;

		copyRect(s_pre, g_vram, c.clipX0, c.clipY0, c.clipX1, c.clipY1);

		Clock::time_point t0 = Clock::now();
		draw(c, refFirst);
		Clock::time_point t1 = Clock::now();

		copyRect(s_first, g_vram, c.clipX0, c.clipY0, c.clipX1, c.clipY1);
		copyRect(g_vram, s_pre, c.clipX0, c.clipY0, c.clipX1, c.clipY1);

		Clock::time_point t2 = Clock::now();
		draw(c, !refFirst);
		Clock::time_point t3 = Clock::now();

		double nsFirst  = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
		double nsSecond = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count();
		s_refNs[cls] += refFirst ? nsFirst : nsSecond;
		s_newNs[cls] += refFirst ? nsSecond : nsFirst;
		s_classCases[cls]++;

		/*	the second draw's result is still in g_vram, the first's in s_first  */
		int bad = 0, changed = 0;
		for (int y = c.clipY0; y <= c.clipY1 && !bad; y++)
			for (int x = c.clipX0; x <= c.clipX1; x++)
			{
				uint16_t ref = refFirst ? s_first[y][x] : g_vram[y][x];
				uint16_t got = refFirst ? g_vram[y][x] : s_first[y][x];
				if (ref != s_pre[y][x])
					changed = 1;
				if (got != ref)
				{
					std::printf("FAIL: vram[%d][%d] = %04x, reference %04x (was %04x)\n",
								y, x, got, ref, s_pre[y][x]);
					describe(c, n);
					bad = 1;
					break;
				}
			}
		drawn += changed;
		failures += bad;

		/* carry the reference result forward so one miss is one report */
		if (refFirst)
			copyRect(g_vram, s_first, c.clipX0, c.clipY0, c.clipX1, c.clipY1);

		/*	Take the reference's legitimate writes into the shadow FIRST, so
			that afterwards any surviving difference is necessarily outside
			the clip rect - a pixel neither rasterizer was allowed to touch.  */
		copyRect(s_shadow, g_vram, c.clipX0, c.clipY0, c.clipX1, c.clipY1);

		/*	EVERY case, not on a cadence: this is the only check that sees a
			write outside the clip rect, and the sync above erases any stray a
			later case's rect happens to cover - on a cadence a rare escape
			(the kind a differential fuzzer exists for) is wiped before it is
			ever looked at, while the systematic one you would notice anyway
			still trips.  A 1MB memcmp per case is worth that.  */
		if (memcmp(g_vram, s_shadow, sizeof(s_shadow)) != 0)
		{
			reportStray(c, n);
			failures++;
			memcpy(g_vram, s_shadow, sizeof(s_shadow));
		}
	}

	std::printf("%ld of %ld cases drew pixels\n", drawn, executed);
	std::printf("%-18s %8s %12s %12s %8s\n", "class", "cases", "ref ns/case", "new ns/case", "speedup");
	double refAll = 0, newAll = 0;
	for (int i = 0; i < T_COUNT; i++)
	{
		if (!s_classCases[i])
			continue;
		refAll += s_refNs[i];
		newAll += s_newNs[i];
		std::printf("%-18s %8ld %12.0f %12.0f %7.2fx\n", s_className[i], s_classCases[i],
					s_refNs[i] / s_classCases[i], s_newNs[i] / s_classCases[i],
					s_newNs[i] > 0 ? s_refNs[i] / s_newNs[i] : 0.0);
	}
	std::printf("%-18s %8ld %12s %12s %7.2fx\n", "all", executed, "", "",
				newAll > 0 ? refAll / newAll : 0.0);

	/*	A generator that stopped drawing would pass vacuously.  Measured
		against the cases actually EXECUTED: a run that stopped early on real
		failures must not also accuse the generator - that is noise on exactly
		the output you need to read.  */
	if (executed >= 1000 && drawn < executed / 4)
	{
		std::printf("FAIL: only %ld of %ld cases drew anything - generator is broken\n", drawn, executed);
		failures++;
	}

	if (failures)
	{
		std::printf("raster_diff_test: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("raster_diff_test: all cases identical\n");
	return 0;
}
