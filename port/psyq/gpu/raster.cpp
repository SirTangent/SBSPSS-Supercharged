/*	Software rasterizer - triangles, rects, lines into the 15-bit VRAM.

	Conventions (PS1):
	- Right/bottom edges excluded (top-left fill rule); quads arrive as two
	  triangles (012, 123) from the interpreter, matching hardware.
	- A triangle whose bounding box exceeds 1023x511 is not drawn.
	- Texel 0x0000 is fully transparent.  Modulation is
	  out5 = min(31, (tex5 * col8) >> 7); flat colour converts 8->5 bits.
	- Semi-transparency (four ABR modes) applies to all pixels of untextured
	  prims, and only to texels with the STP bit for textured ones.
	- Written pixels carry the texel's STP bit (0 for untextured).
	- Dithering (E1 dtd): the PS1 4x4 matrix added to the 8-bit channel
	  value before 8->5 truncation, for gouraud-shaded and texture-modulated
	  pixels only (flat untextured and raw-texture pixels bypass, as do
	  rects/fills - the interpreter never sets cfg->dither for those).
	  Dither lands on the foreground colour BEFORE any semi-transparency
	  blend; the blend itself stays in 5-bit space (M4's emulator A/B pass
	  is the authority if this ever shows).

	Structure (M8 perf pass).  The pixel rules above are unchanged and
	tests/raster_diff_test.cpp holds this file to its pre-optimisation copy
	(tests/raster_ref.cpp) halfword for halfword; what changed is where the
	decisions are taken:
	- Everything a RasterCfg fixes for a whole primitive - textured, raw,
	  semi, dither, texture depth, texture window - is a compile-time flag
	  word F.  Each entry point folds its cfg to the canonical F once and
	  jumps through a constexpr table to the loop instantiated for it, so
	  the pixel loops carry only the tests that depend on the pixel (texel
	  0, the texel's STP bit, the dither cell).
	- Triangles walk exact per-row spans instead of testing every pixel of
	  the bounding box, and step their interpolants with exact
	  quotient/remainder DDAs instead of a divide per attribute per pixel.
	- VRAM is still read per pixel, in the same left-to-right, top-to-bottom
	  order, texel and CLUT entry alike: a primitive that draws over its own
	  texture page or CLUT sees exactly the writes it saw before.
*/
#include <string.h>
#include <utility>

#include "gpu/gpu_core.h"

/*	psx-spx dither offsets, indexed [y&3][x&3]  */
static const int8_t s_dither[4][4] =
{
	{ -4,  0, -3,  1 },
	{  2, -2,  3, -1 },
	{ -3,  1, -4,  0 },
	{  3, -1,  2, -2 },
};

/*****************************************************************************/
/*	The per-primitive flag word.  Bits 0-6 select the pixel pipeline; the
	triangle adds F_GOURAUD for its colour interpolation.  */
enum
{
	F_TEX     = 1 << 0,
	F_RAW     = 1 << 1,		/* textured, no modulation */
	F_SEMI    = 1 << 2,
	F_DITHER  = 1 << 3,		/* this pixel pipeline dithers (see pixelFlags) */
	F_TWIN    = 1 << 4,		/* E2 texture window is not the identity */
	F_DEPTH_SHIFT = 5,		/* 2 bits: 0=4bpp 1=8bpp 2=15bpp */
	F_PIXEL_COUNT = 1 << 7,

	F_GOURAUD = 1 << 7,		/* triangles: interpolate the vertex colour */
	F_TRI_COUNT = 1 << 8,
};
#define F_DEPTH(F)	(((F) >> F_DEPTH_SHIFT) & 3)

/*	Only canonical words are instantiated: an untextured pipeline has no
	raw/window/depth variants, and a raw texel is never dithered.  */
static constexpr bool validPixel(unsigned F)
{
	if (F_DEPTH(F) == 3)
		return false;
	if (!(F & F_TEX))
		return !(F & (F_RAW | F_TWIN)) && F_DEPTH(F) == 0;
	if (F & F_RAW)
		return !(F & F_DITHER);
	return true;
}

/* raw texels ignore the vertex colour, so there is nothing to interpolate */
static constexpr bool validTri(unsigned F)
{
	return validPixel(F & (F_PIXEL_COUNT - 1))
		&& !((F & F_GOURAUD) && (F & F_TEX) && (F & F_RAW));
}

/*	cfg -> canonical pixel flags.  The dither rule is the one in the header:
	texture-modulated pixels, and untextured ones of a gouraud primitive.  */
static inline unsigned pixelFlags(const RasterCfg *cfg)
{
	unsigned F = cfg->semi ? F_SEMI : 0;
	if (cfg->textured)
	{
		F |= F_TEX;
		F |= (cfg->texDepth == 0 ? 0u : cfg->texDepth == 1 ? 1u : 2u) << F_DEPTH_SHIFT;
		if (cfg->twMaskU | cfg->twOrU | cfg->twMaskV | cfg->twOrV)
			F |= F_TWIN;
		if (cfg->rawTex)
			F |= F_RAW;
		else if (cfg->dither)
			F |= F_DITHER;
	}
	else if (cfg->dither && cfg->gouraud)
		F |= F_DITHER;
	return F;
}

/*	Undithered modulation by 128,128,128 - the game's neutral sprite colour -
	is the identity: min(31, (t*128)>>7) == t.  Such a primitive IS a raw
	one.  (Dithered it is not: the offset still lands on t*8.)  */
static inline unsigned foldNeutral(unsigned F, int r, int g, int b)
{
	if ((F & (F_TEX | F_RAW | F_DITHER)) == F_TEX && r == 128 && g == 128 && b == 128)
		F |= F_RAW;
	return F;
}

/*****************************************************************************/
template <unsigned F>
static inline uint16_t sampleTexel(int u, int v, const RasterCfg *cfg)
{
	/*	E2 texture window: coord = (coord & ~mask*8) | (offset & mask)*8,
		precomputed into twMask/twOr (all-zero = plain 8-bit wrap)  */
	if constexpr (F & F_TWIN)
	{
		u = ((u & ~cfg->twMaskU) | cfg->twOrU) & 0xFF;
		v = ((v & ~cfg->twMaskV) | cfg->twOrV) & 0xFF;
	}
	else
	{
		u &= 0xFF;
		v &= 0xFF;
	}
	int ty = (cfg->texBaseY + v) & 0x1FF;
	if constexpr (F_DEPTH(F) == 0)		/* 4bpp: 4 texels per halfword */
	{
		uint16_t cell = g_vram[ty][(cfg->texBaseX + (u >> 2)) & 0x3FF];
		int idx = (cell >> ((u & 3) << 2)) & 0xF;
		return g_vram[cfg->clutY][(cfg->clutX + idx) & 0x3FF];
	}
	else if constexpr (F_DEPTH(F) == 1)	/* 8bpp: 2 texels per halfword */
	{
		uint16_t cell = g_vram[ty][(cfg->texBaseX + (u >> 1)) & 0x3FF];
		int idx = (cell >> ((u & 1) << 3)) & 0xFF;
		return g_vram[cfg->clutY][(cfg->clutX + idx) & 0x3FF];
	}
	else								/* 15bpp direct */
		return g_vram[ty][(cfg->texBaseX + u) & 0x3FF];
}

static inline int mod5(int tex5, int col8)
{
	int v = (tex5 * col8) >> 7;
	return v > 31 ? 31 : v;
}

static inline int clamp8(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/*	Modulation kept at 8 bits so the dither offset lands before truncation:
	(t5*c8)>>4 is exactly the mod5 rule times 8.  */
static inline int mod8dith(int tex5, int col8, int dith)
{
	int v = (tex5 * col8) >> 4;
	if (v > 255)
		v = 255;
	return clamp8(v + dith) >> 3;
}

static inline uint16_t blendSemi(uint16_t back, int fr, int fg, int fb, int mode)
{
	int br = back & 0x1F, bg = (back >> 5) & 0x1F, bb = (back >> 10) & 0x1F;
	int r, g, b;
	switch (mode)
	{
	case 0:  r = (br + fr) >> 1;  g = (bg + fg) >> 1;  b = (bb + fb) >> 1;  break;
	case 1:  r = br + fr;         g = bg + fg;         b = bb + fb;         break;
	case 2:  r = br - fr;         g = bg - fg;         b = bb - fb;         break;
	default: r = br + (fr >> 2);  g = bg + (fg >> 2);  b = bb + (fb >> 2);  break;
	}
	if (r < 0) r = 0; else if (r > 31) r = 31;
	if (g < 0) g = 0; else if (g > 31) g = 31;
	if (b < 0) b = 0; else if (b > 31) b = 31;
	return (uint16_t)(r | (g << 5) | (b << 10));
}

/*	The shared per-pixel pipeline.  Returns without writing when the pixel
	is transparent.  cr/cg/cb are the 8-bit vertex colour at this pixel.  */
template <unsigned F>
static inline void shadePixel(int x, int y, int u, int v,
							  int cr, int cg, int cb, const RasterCfg *cfg)
{
	int fr, fg, fb;

	if constexpr (F & F_TEX)
	{
		uint16_t tex = sampleTexel<F>(u, v, cfg);
		if (tex == 0)
			return;						/* transparent black */
		int stp = tex >> 15;

		if constexpr ((F & F_RAW) && !(F & F_SEMI))
		{
			g_vram[y][x] = tex;			/* raw, opaque: the texel is the pixel */
			return;
		}
		else
		{
			int tr = tex & 0x1F, tg = (tex >> 5) & 0x1F, tb = (tex >> 10) & 0x1F;
			if constexpr (F & F_RAW)
			{
				fr = tr;  fg = tg;  fb = tb;	/* raw texels: never dithered */
			}
			else if constexpr (F & F_DITHER)
			{
				/*	8-bit modulation result (min(255,(t5*c8)>>4) == the 5-bit
					rule below carried at full precision), dither, truncate  */
				int dith = s_dither[y & 3][x & 3];
				fr = mod8dith(tr, cr, dith);
				fg = mod8dith(tg, cg, dith);
				fb = mod8dith(tb, cb, dith);
			}
			else
			{
				fr = mod5(tr, cr);  fg = mod5(tg, cg);  fb = mod5(tb, cb);
			}

			uint16_t out;
			if constexpr (F & F_SEMI)
			{
				if (stp)
					out = (uint16_t)(blendSemi(g_vram[y][x], fr, fg, fb, cfg->semiMode) | 0x8000);
				else
					out = (uint16_t)(fr | (fg << 5) | (fb << 10));
			}
			else
				out = (uint16_t)(fr | (fg << 5) | (fb << 10) | (stp << 15));
			g_vram[y][x] = out;
		}
	}
	else
	{
		if constexpr (F & F_DITHER)
		{
			int dith = s_dither[y & 3][x & 3];
			fr = clamp8(cr + dith) >> 3;
			fg = clamp8(cg + dith) >> 3;
			fb = clamp8(cb + dith) >> 3;
		}
		else
		{
			fr = cr >> 3;  fg = cg >> 3;  fb = cb >> 3;
		}

		if constexpr (F & F_SEMI)
			g_vram[y][x] = blendSemi(g_vram[y][x], fr, fg, fb, cfg->semiMode);
		else
			g_vram[y][x] = (uint16_t)(fr | (fg << 5) | (fb << 10));
	}
}

/*****************************************************************************/
/*	constexpr dispatch tables: Ops::run<F> for every Ops::valid(F), built at
	compile time (no static constructor - the shim rides into the unit exes).  */
template <class Ops, unsigned F>
static constexpr typename Ops::Fn tableEntry()
{
	if constexpr (Ops::valid(F))
		return &Ops::template run<F>;
	else
		return nullptr;
}

template <class Ops, class Seq> struct DispatchTable;
template <class Ops, size_t... I>
struct DispatchTable<Ops, std::index_sequence<I...> >
{
	static constexpr typename Ops::Fn fn[sizeof...(I)] = { tableEntry<Ops, (unsigned)I>()... };
};

/*****************************************************************************/
static inline int floorDiv(int a, int b)		/* b > 0 */
{
	int q = a / b;
	return (a % b < 0) ? q - 1 : q;
}

static inline int ceilDiv(int a, int b)			/* b > 0 */
{
	return -floorDiv(-a, b);
}

/*	Exact incremental floor(n / area) for a numerator that moves by a fixed
	dX per pixel: q and rem hold n = q*area + rem with 0 <= rem < area, and a
	step adds floor(dX/area) to q and (dX mod area) >= 0 to rem, carrying at
	most once.  No approximation anywhere - every pixel gets the quotient the
	divide would have produced.  */
struct Dda
{
	int q, rem, dq, dr;
};

static inline void ddaSlope(Dda *d, int dX, int area)
{
	d->dq = floorDiv(dX, area);
	d->dr = dX - d->dq * area;
}

static inline void ddaStart(Dda *d, int n, int area)	/* n >= 0 */
{
	d->q   = n / area;
	d->rem = n % area;
}

static inline void ddaStep(Dda *d, int area)
{
	d->q   += d->dq;
	d->rem += d->dr;
	if (d->rem >= area)
	{
		d->rem -= area;
		d->q++;
	}
}

struct TriSetup
{
	const RasterVtx *a, *b, *c;		/* wound so the doubled area is positive */
	int		area;
	int		minx, miny, maxx, maxy;	/* clipped bounding box, inclusive */
	int		A[3], B[3], K[3];		/* edge i: A*x + B*y + K >= 0 inside (K = C + fill-rule bias) */
	int		bias[3];
};

struct TriOps
{
	typedef void (*Fn)(const TriSetup &, const RasterCfg *);
	static constexpr bool valid(unsigned F) { return validTri(F); }

	template <unsigned F>
	static void run(const TriSetup &s, const RasterCfg *cfg)
	{
		const RasterVtx *a = s.a, *b = s.b, *c = s.c;
		const int area = s.area;

		/*	Interpolation.  Each attribute q is (ua*aq + ub*bq + uc*cq) / area
			with the unbiased edge values ua+ub+uc == area, so the numerator is
			linear in x: dX per pixel.  Inside the triangle ua,ub,uc >= 0, hence
			0 <= numerator <= 255*area < 2^29 (the bounding-box reject caps the
			doubled area at 2*1023*511 < 2^21) - plain int arithmetic.  */
		Dda du, dv, dr, dg, db;
		if constexpr (F & F_TEX)
		{
			ddaSlope(&du, s.A[0] * a->u + s.A[1] * b->u + s.A[2] * c->u, area);
			ddaSlope(&dv, s.A[0] * a->v + s.A[1] * b->v + s.A[2] * c->v, area);
		}
		if constexpr (F & F_GOURAUD)
		{
			ddaSlope(&dr, s.A[0] * a->r + s.A[1] * b->r + s.A[2] * c->r, area);
			ddaSlope(&dg, s.A[0] * a->g + s.A[1] * b->g + s.A[2] * c->g, area);
			ddaSlope(&db, s.A[0] * a->b + s.A[1] * b->b + s.A[2] * c->b, area);
		}

		for (int y = s.miny; y <= s.maxy; y++)
		{
			/*	The row's span, solved exactly from the three biased edge
				functions e(x) = A*x + k >= 0: a left edge (A > 0) gives a lower
				bound ceil(-k/A), a right edge (A < 0) an upper bound
				floor(k/-A), a horizontal one admits or rejects the whole row.
				The interior is convex, so [xs,xe] is precisely the set of
				pixels the per-pixel edge test accepted.  */
			int xs = s.minx, xe = s.maxx;
			int k[3];
			bool empty = false;
			for (int i = 0; i < 3; i++)
			{
				k[i] = s.B[i] * y + s.K[i];
				if (s.A[i] > 0)
				{
					int lo = ceilDiv(-k[i], s.A[i]);
					if (lo > xs) xs = lo;
				}
				else if (s.A[i] < 0)
				{
					int hi = floorDiv(k[i], -s.A[i]);
					if (hi < xe) xe = hi;
				}
				else if (k[i] < 0)
					empty = true;
			}
			if (empty || xs > xe)
				continue;

			/* unbiased barycentric weights at the span start */
			if constexpr ((F & F_TEX) || (F & F_GOURAUD))
			{
				int ua = s.A[0] * xs + k[0] - s.bias[0];
				int ub = s.A[1] * xs + k[1] - s.bias[1];
				int uc = s.A[2] * xs + k[2] - s.bias[2];
				if constexpr (F & F_TEX)
				{
					ddaStart(&du, ua * a->u + ub * b->u + uc * c->u, area);
					ddaStart(&dv, ua * a->v + ub * b->v + uc * c->v, area);
				}
				if constexpr (F & F_GOURAUD)
				{
					ddaStart(&dr, ua * a->r + ub * b->r + uc * c->r, area);
					ddaStart(&dg, ua * a->g + ub * b->g + uc * c->g, area);
					ddaStart(&db, ua * a->b + ub * b->b + uc * c->b, area);
				}
			}

			for (int x = xs; x <= xe; x++)
			{
				int u = 0, v = 0, cr, cg, cb;
				if constexpr (F & F_TEX)
				{
					u = du.q;  v = dv.q;
				}
				if constexpr (F & F_GOURAUD)
				{
					cr = dr.q;  cg = dg.q;  cb = db.q;
				}
				else
				{
					cr = a->r;  cg = a->g;  cb = a->b;
				}

				shadePixel<F & (F_PIXEL_COUNT - 1)>(x, y, u, v, cr, cg, cb, cfg);

				if constexpr (F & F_TEX)
				{
					ddaStep(&du, area);  ddaStep(&dv, area);
				}
				if constexpr (F & F_GOURAUD)
				{
					ddaStep(&dr, area);  ddaStep(&dg, area);  ddaStep(&db, area);
				}
			}
		}
	}
};

void Raster_Triangle(const RasterVtx *v0, const RasterVtx *v1, const RasterVtx *v2,
					 const RasterCfg *cfg)
{
	TriSetup s;
	const RasterVtx *a = v0, *b = v1, *c = v2;

	/* size reject on the raw bounding box */
	int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
	int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
	int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
	int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);
	if (maxx - minx > 1023 || maxy - miny > 511)
		return;

	/* wind consistently (PS1 draws both windings) */
	long area = (long)(b->x - a->x) * (c->y - a->y)
			  - (long)(b->y - a->y) * (c->x - a->x);
	if (area == 0)
		return;
	if (area < 0)
	{
		const RasterVtx *t = b;  b = c;  c = t;
		area = -area;
	}

	if (minx < g_gpu.clipX0) minx = g_gpu.clipX0;
	if (miny < g_gpu.clipY0) miny = g_gpu.clipY0;
	if (maxx > g_gpu.clipX1) maxx = g_gpu.clipX1;
	if (maxy > g_gpu.clipY1) maxy = g_gpu.clipY1;
	if (minx < 0) minx = 0;
	if (miny < 0) miny = 0;
	if (maxx > VRAM_W - 1) maxx = VRAM_W - 1;
	if (maxy > VRAM_H - 1) maxy = VRAM_H - 1;
	if (minx > maxx || miny > maxy)
		return;

	/*	Edge functions (interior positive).  Top-left fill rule in y-down
		coordinates: LEFT edges have A > 0 (interior to their right), TOP
		edges are horizontal with the interior below (A == 0, B > 0) - those
		are inclusive; right/bottom edges get a -1 bias so shared boundary
		pixels belong to exactly one primitive.  */
	s.A[0] = b->y - c->y;  s.B[0] = c->x - b->x;  s.K[0] = b->x * c->y - b->y * c->x;
	s.A[1] = c->y - a->y;  s.B[1] = a->x - c->x;  s.K[1] = c->x * a->y - c->y * a->x;
	s.A[2] = a->y - b->y;  s.B[2] = b->x - a->x;  s.K[2] = a->x * b->y - a->y * b->x;
	for (int i = 0; i < 3; i++)
	{
		s.bias[i] = (s.A[i] > 0 || (s.A[i] == 0 && s.B[i] > 0)) ? 0 : -1;
		s.K[i] += s.bias[i];
	}

	s.a = a;  s.b = b;  s.c = c;
	s.area = (int)area;
	s.minx = minx;  s.miny = miny;  s.maxx = maxx;  s.maxy = maxy;

	/*	Equal vertex colours interpolate to exactly that colour (numerator
		col*area), so such a gouraud primitive takes the flat loop - its pixel
		pipeline, dither included, is already fixed by pixelFlags.  */
	unsigned F = pixelFlags(cfg);
	bool flat = !cfg->gouraud
			 || (a->r == b->r && a->r == c->r && a->g == b->g && a->g == c->g
				 && a->b == b->b && a->b == c->b);
	if (flat)
		F = foldNeutral(F, v0->r, v0->g, v0->b);
	if (!flat && !(F & F_RAW))
		F |= F_GOURAUD;

	DispatchTable<TriOps, std::make_index_sequence<F_TRI_COUNT> >::fn[F](s, cfg);
}

/*****************************************************************************/
struct RectOps
{
	typedef void (*Fn)(int x0, int y0, int x1, int y1, int ustart, int vstart,
					   int r, int g, int b, const RasterCfg *cfg);
	static constexpr bool valid(unsigned F) { return validPixel(F); }

	template <unsigned F>
	static void run(int x0, int y0, int x1, int y1, int ustart, int vstart,
					int r, int g, int b, const RasterCfg *cfg)
	{
		for (int py = y0, v = vstart; py <= y1; py++, v++)
			for (int px = x0, u = ustart; px <= x1; px++, u++)
				shadePixel<F>(px, py, u, v, r, g, b, cfg);
	}
};

void Raster_Rect(int x, int y, int w, int h, int u0, int v0,
				 uint8_t r, uint8_t g, uint8_t b, const RasterCfg *cfg)
{
	if (w > 1023 || h > 511)
		return;

	int x0 = x, y0 = y;
	int x1 = x + w - 1, y1 = y + h - 1;
	int ustart = u0, vstart = v0;

	if (x0 < g_gpu.clipX0) { ustart += g_gpu.clipX0 - x0; x0 = g_gpu.clipX0; }
	if (y0 < g_gpu.clipY0) { vstart += g_gpu.clipY0 - y0; y0 = g_gpu.clipY0; }
	if (x1 > g_gpu.clipX1) x1 = g_gpu.clipX1;
	if (y1 > g_gpu.clipY1) y1 = g_gpu.clipY1;
	if (x0 < 0) { ustart += -x0; x0 = 0; }
	if (y0 < 0) { vstart += -y0; y0 = 0; }
	if (x1 > VRAM_W - 1) x1 = VRAM_W - 1;
	if (y1 > VRAM_H - 1) y1 = VRAM_H - 1;
	if (x0 > x1 || y0 > y1)
		return;

	unsigned F = foldNeutral(pixelFlags(cfg), r, g, b);
	DispatchTable<RectOps, std::make_index_sequence<F_PIXEL_COUNT> >::fn[F]
		(x0, y0, x1, y1, ustart, vstart, r, g, b, cfg);
}

/*****************************************************************************/
struct LineOps
{
	typedef void (*Fn)(const RasterVtx *pa, const RasterVtx *pb, const RasterCfg *cfg);
	static constexpr bool valid(unsigned F) { return validPixel(F); }

	template <unsigned F>
	static void run(const RasterVtx *pa, const RasterVtx *pb, const RasterCfg *cfg)
	{
		int x0 = pa->x, y0 = pa->y, x1 = pb->x, y1 = pb->y;

		int dx = x1 > x0 ? x1 - x0 : x0 - x1;
		int dy = y1 > y0 ? y1 - y0 : y0 - y1;

		int sx = x0 < x1 ? 1 : -1;
		int sy = y0 < y1 ? 1 : -1;
		int err = dx - dy;
		int steps = (dx > dy ? dx : dy);

		int n = 0;

		/*	The colour stays a divide per VISIBLE pixel: an incremental walk
			has to step through the clipped part of the line too, and measured
			slower (raster_diff_test) - and the game only builds LINE_F2.  */
		const bool shaded = cfg->gouraud && steps > 0;

		for (;;)
		{
			if (x0 >= g_gpu.clipX0 && x0 <= g_gpu.clipX1 &&
				y0 >= g_gpu.clipY0 && y0 <= g_gpu.clipY1 &&
				x0 >= 0 && x0 < VRAM_W && y0 >= 0 && y0 < VRAM_H)
			{
				/*	lines are never textured by the interpreter, so this is the
					untextured pipeline (8->5 bit colour, optional dither and
					semi-transparency, STP 0); a textured cfg samples (0,0) as
					it always did  */
				if (shaded)
					shadePixel<F>(x0, y0, 0, 0,
								  pa->r + (pb->r - pa->r) * n / steps,
								  pa->g + (pb->g - pa->g) * n / steps,
								  pa->b + (pb->b - pa->b) * n / steps, cfg);
				else
					shadePixel<F>(x0, y0, 0, 0, pa->r, pa->g, pa->b, cfg);
			}

			if (x0 == x1 && y0 == y1)
				break;
			int e2 = err << 1;
			if (e2 > -dy) { err -= dy; x0 += sx; }
			if (e2 <  dx) { err += dx; y0 += sy; }
			n++;
		}
	}
};

void Raster_Line(const RasterVtx *pa, const RasterVtx *pb, const RasterCfg *cfg)
{
	int dx = pb->x > pa->x ? pb->x - pa->x : pa->x - pb->x;
	int dy = pb->y > pa->y ? pb->y - pa->y : pa->y - pb->y;
	if (dx > 1023 || dy > 511)
		return;

	DispatchTable<LineOps, std::make_index_sequence<F_PIXEL_COUNT> >::fn[pixelFlags(cfg)](pa, pb, cfg);
}
