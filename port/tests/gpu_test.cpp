/*	Unit tests for the software GPU (port/psyq/gpu/) - no window, no game.
	Pins the semantics the game depends on: transfer rect masking, OT chain
	shape, the GP0 interpreter, FT4 texture sampling + modulation, clipping,
	drawing offset, and semi-transparency mode 0.
*/
#include <cstdio>
#include <cstring>

#include <sys/types.h>
#include <libgte.h>
#include <libgpu.h>

#include "gpu/gpu_core.h"

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static void checkPx(int x, int y, uint16_t want, const char *what)
{
	if (g_vram[y][x] != want)
	{
		std::printf("FAIL: %s - vram[%d][%d] = %04x, want %04x\n",
					what, y, x, g_vram[y][x], want);
		g_failures++;
	}
}

/*	fresh full-VRAM draw environment  */
static void resetEnv(void)
{
	memset(g_vram, 0, sizeof(g_vram));
	DRAWENV env;
	SetDefDrawEnv(&env, 0, 0, 1024, 512);
	PutDrawEnv(&env);
}

int main()
{
	/* --- ClearOTagR / ClearOTag chain shape ------------------------------- */
	{
		static u_long ot[16];
		ClearOTagR(ot, 16);
		check(ot[0] == 0x00FFFFFF, "ClearOTagR: ot[0] is the terminator");
		for (int i = 1; i < 16; i++)
			check((ot[i] & 0xFFFFFF) == ((u_long)(uintptr_t)&ot[i - 1] & 0xFFFFFF)
				  && (ot[i] >> 24) == 0,
				  "ClearOTagR: reverse links with len 0");

		ClearOTag(ot, 16);
		check(ot[15] == 0x00FFFFFF, "ClearOTag: last entry terminated");
		check((ot[0] & 0xFFFFFF) == ((u_long)(uintptr_t)&ot[1] & 0xFFFFFF),
			  "ClearOTag: forward links");
	}

	/* --- LoadImage / StoreImage round trip -------------------------------- */
	{
		resetEnv();
		static uint16_t src[8 * 4], back[8 * 4];
		for (int i = 0; i < 8 * 4; i++)
			src[i] = (uint16_t)(0x1234 + i);
		RECT r = { 100, 200, 8, 4 };
		LoadImage(&r, (u_long *)src);
		checkPx(100, 200, 0x1234, "LoadImage top-left");
		checkPx(107, 203, (uint16_t)(0x1234 + 31), "LoadImage bottom-right");
		StoreImage(&r, (u_long *)back);
		check(memcmp(src, back, sizeof(src)) == 0, "StoreImage round trip");
	}

	/* --- MoveImage -------------------------------------------------------- */
	{
		RECT r = { 100, 200, 8, 4 };
		MoveImage(&r, 300, 400);
		checkPx(300, 400, 0x1234, "MoveImage dest top-left");
		checkPx(307, 403, (uint16_t)(0x1234 + 31), "MoveImage dest bottom-right");
		checkPx(100, 200, 0x1234, "MoveImage source intact");
	}

	/* --- ClearImage oversized rects: libgpu CLAMPS, then fills ------------
		Disassembled from LIBGPU.LIB's ClearImage packet builder: w -> [0,1023],
		h -> [0,511] BEFORE the fill command.  The old assertion here ("w=2048
		masks to a no-op") modelled the raw hardware rule at the library layer,
		which is what left FMV frames on screen as garbage at movie teardown
		(fmv.cpp clears {0,0,512,512} - h=512 must clear 511 rows, not 0).  */
	{
		resetEnv();
		RECT big = { 512, 256, 2048, 254 };
		ClearImage(&big, 255, 0, 0);
		/*	w clamps to 1023 (libgpu), which is not 64-aligned, so the fill is
			the GP0(60h) draw that clips at the VRAM edge instead of wrapping -
			actor.cpp's green cache wipe must never cross into the framebuffer
			columns (the level-loading green-overlay regression).  The row
			checks sample column 512, inside the rect's columns.  */
		checkPx(512, 256, 0x001F, "ClearImage w=2048 fills from its origin");
		checkPx(1023, 256, 0x001F, "ClearImage w=2048 fills to the right edge");
		checkPx(0, 256, 0x0000, "ClearImage does NOT wrap into the framebuffer");
		checkPx(512, 255, 0x0000, "ClearImage did not touch the row above the rect");
		checkPx(512, 509, 0x001F, "ClearImage h=254 fills row 509");
		checkPx(512, 510, 0x0000, "ClearImage h=254 stops at row 509");

		RECT ok = { 512, 256, 64, 32 };
		ClearImage(&ok, 255, 0, 0);
		checkPx(512, 256, 0x001F, "ClearImage in-range rect fills");

		/*	the FMV teardown shape: h=512 clamps to 511 (rows 0..510)  */
		resetEnv();
		RECT mark = { 0, 511, 16, 1 };
		ClearImage(&mark, 0, 0, 255);
		RECT fmv = { 0, 0, 512, 512 };
		ClearImage(&fmv, 0, 255, 0);
		checkPx(0, 0, 0x03E0, "ClearImage h=512 clears the top row");
		checkPx(511, 510, 0x03E0, "ClearImage h=512 clears row 510");
		checkPx(0, 511, 0x7C00, "ClearImage h=512 leaves row 511 (clamp to 511)");
	}

	/* --- POLY_F4 flat fill + drawing offset + clip ------------------------ */
	{
		resetEnv();
		DRAWENV env;
		SetDefDrawEnv(&env, 0, 256, 512, 256);	/* Screen[1]-style: ofs (0,256) */
		PutDrawEnv(&env);

		POLY_F4 q;
		setPolyF4(&q);
		setRGB0(&q, 255, 255, 255);
		setXYWH(&q, 10, 10, 16, 16);
		DrawPrim(&q);

		checkPx(10, 266, 0x7FFF, "F4: offset applied (10,10)->(10,266)");
		checkPx(25, 281, 0x7FFF, "F4: bottom-right inside (exclusive edges)");
		checkPx(26, 282, 0x0000, "F4: right/bottom edges excluded");
		checkPx(10, 10, 0x0000, "F4: clip kept it out of the top half");
	}

	/* --- POLY_FT4 4bpp CLUT texture + modulation -------------------------- */
	{
		resetEnv();
		/*	texture page 0 at VRAM (0,0); put a 4bpp pattern at texel row 0:
			indices 1,2,3,4 -> one halfword 0x4321  */
		static uint16_t texRow[4] = { 0x4321, 0x4321, 0x4321, 0x4321 };
		RECT tr = { 0, 0, 4, 1 };
		LoadImage(&tr, (u_long *)texRow);
		/*	CLUT at (0,500): entry 0 transparent-black, 1..4 solid colours  */
		static uint16_t clut[16];
		memset(clut, 0, sizeof(clut));
		clut[1] = 0x001F;	/* red   */
		clut[2] = 0x03E0;	/* green */
		clut[3] = 0x7C00;	/* blue  */
		clut[4] = 0x7FFF;	/* white */
		RECT cr = { 0, 500, 16, 1 };
		LoadImage(&cr, (u_long *)clut);

		DRAWENV env;
		SetDefDrawEnv(&env, 0, 0, 1024, 512);
		env.dtd = 0;						/* exact texels: undithered */
		PutDrawEnv(&env);

		POLY_FT4 p;
		setPolyFT4(&p);
		setRGB0(&p, 128, 128, 128);			/* 128 = identity modulation */
		setXYWH(&p, 200, 100, 4, 1);
		setUVWH(&p, 0, 0, 4, 1);
		p.tpage = getTPage(0, 0, 0, 0);		/* 4bpp, page (0,0) */
		p.clut  = getClut(0, 500);
		DrawPrim(&p);

		checkPx(200, 100, 0x001F, "FT4: texel 1 -> CLUT red");
		checkPx(201, 100, 0x03E0, "FT4: texel 2 -> CLUT green");
		checkPx(202, 100, 0x7C00, "FT4: texel 3 -> CLUT blue");
		checkPx(203, 100, 0x7FFF, "FT4: texel 4 -> CLUT white");

		/* modulation at half brightness: 64/128 halves each 5-bit channel */
		setRGB0(&p, 64, 64, 64);
		setXYWH(&p, 210, 100, 4, 1);
		DrawPrim(&p);
		checkPx(213, 100, (uint16_t)(15 | (15 << 5) | (15 << 10)),
				"FT4: colour 64 halves white to 15/15/15");

		/* texel 0 (CLUT entry 0 = 0x0000) is transparent */
		g_vram[220][100] = 0x1234;			/* y=100? row is [y][x] */
		g_vram[100][220] = 0x1234;
		POLY_FT4 t0 = p;
		setRGB0(&t0, 128, 128, 128);
		setXYWH(&t0, 220, 100, 1, 1);
		setUVWH(&t0, 4, 0, 1, 1);			/* texel index 0 lives at u=4..7 (0x4321 nibbles) */
		/* u=4 -> halfword 1 (0x4321), nibble 0 -> index 1... build a real zero:
		   use u beyond the loaded row: VRAM is zeroed there -> index 0 */
		setUVWH(&t0, 32, 0, 1, 1);
		DrawPrim(&t0);
		checkPx(220, 100, 0x1234, "FT4: texel 0000 leaves dest untouched");

		/*	The poly tpage attribute leaves E1 dither alone (issue #60): with
			dtd=1 in force, a modulated FT4 dithers - white 31*128 is 248 in
			8 bits, so the (0,0) cell's -4 gives 30 and the (1,0) cell's 0
			gives 31 - and a G4 drawn after it still dithers.  */
		env.dtd = 1;
		PutDrawEnv(&env);
		POLY_FT4 wd = p;
		setRGB0(&wd, 128, 128, 128);
		setXYWH(&wd, 240, 100, 2, 1);		/* x 240 = cell column 0 */
		setUVWH(&wd, 3, 0, 0, 0);			/* every pixel samples u=3: index 4, white */
		DrawPrim(&wd);
		checkPx(240, 100, (uint16_t)(30 * 0x421), "FT4 with dtd=1: dither cell (0,0)");
		checkPx(241, 100, (uint16_t)(31 * 0x421), "FT4 with dtd=1: dither cell (1,0)");
		check(g_gpu.dither == 1, "the FT4's tpage attribute kept E1 dither on");

		static const uint32_t g4[] =
		{
			0x38808080, 0x00C80064,				/* (100,200) mid grey */
			0x00808080, 0x00C80068,				/* (104,200) */
			0x00808080, 0x00CC0064,				/* (100,204) */
			0x00808080, 0x00CC0068,				/* (104,204) */
		};
		GPU_ExecWords(g4, 8);
		checkPx(100, 200, 15 * 0x421, "G4 after an FT4 still dithers (0,0): 128-4 >> 3");
		checkPx(101, 200, 16 * 0x421, "G4 after an FT4 still dithers (1,0): 128+0 >> 3");

		/*	and the reverse: dtd=0 in force, an attribute with bit 9 set
			(which getTPage never makes) must not turn dither on  */
		env.dtd = 0;
		PutDrawEnv(&env);
		POLY_FT4 nd = wd;
		nd.tpage = (u_short)(wd.tpage | 0x200);
		setXYWH(&nd, 244, 100, 2, 1);
		DrawPrim(&nd);
		checkPx(244, 100, 0x7FFF, "dtd=0 and attribute bit 9: FT4 undithered (0,0)");
		checkPx(245, 100, 0x7FFF, "dtd=0 and attribute bit 9: FT4 undithered (1,0)");
		check(g_gpu.dither == 0, "attribute bit 9 did not turn E1 dither on");
	}

	/*	--- POLY_G3 gouraud interpolation ------------------------------------
		Pins the barycentric path (raster.cpp steps each attribute as an
		exact floor(n / area) quotient-and-remainder DDA rather than a
		per-pixel divide).  Right-angled triangle
		a=(100,100) red, b=(164,100) green, c=(100,164) blue: the vertex pixel
		must be the vertex colour exactly, and each edge midpoint the exact
		mean of its two endpoints.  */
	{
		resetEnv();

		uint32_t g3[7];
		g3[0] = 0x06000000;						/* tag: len 6 */
		g3[1] = 0x30000000u | 0x0000FF;			/* G3 + colour0 = red   (BGR word) */
		g3[2] = (100 << 16) | 100;
		g3[3] = 0x00FF00;						/* colour1 = green */
		g3[4] = (100 << 16) | 164;
		g3[5] = 0xFF0000;						/* colour2 = blue  */
		g3[6] = (164 << 16) | 100;
		DrawPrim(g3);

		checkPx(100, 100, 0x001F, "G3: vertex 0 pixel is vertex 0 colour exactly");
		checkPx(132, 100, (uint16_t)(15 | (15 << 5)),
				"G3: a-b midpoint is (red+green)/2");
		checkPx(100, 132, (uint16_t)(15 | (15 << 10)),
				"G3: a-c midpoint is (red+blue)/2");
		checkPx(163, 163, 0x0000, "G3: outside the hypotenuse stays clear");
	}

	/*	--- gouraud at the maximum triangle size ------------------------------
		The colour DDA must be the exact floor(n / area) at every pixel, and
		the biggest triangle the size-reject allows (area 1023*511 = 522753)
		has the largest numerators and the longest runs of remainder carries.
		With dither off, compare every pixel against an independent int64
		reference: a pixel is inside when 511x + 1023y < 522753 (the
		hypotenuse is a right/bottom edge, excluded), and each channel is
		floor(255 * weight / area) >> 3 with the vertex weights
		red 522753 - 511x - 1023y, green 1023y, blue 511x.  (A vertex pixel
		alone could not fail: its numerator is exactly 255 * area.)  */
	{
		resetEnv();

		uint32_t g3[8];
		g3[0] = 0x07000000;						/* tag: len 7 */
		g3[1] = 0xE1000000;						/* dtd = 0 */
		g3[2] = 0x30000000u | 0x0000FF;			/* red   at (0,0)     */
		g3[3] = 0;
		g3[4] = 0x00FF00;						/* green at (0,511)   */
		g3[5] = (511 << 16) | 0;
		g3[6] = 0xFF0000;						/* blue  at (1023,0)  */
		g3[7] = 1023;
		DrawPrim(g3);

		const int64_t area = 1023 * 511;
		long bad = 0;
		for (int y = 0; y < VRAM_H && bad < 8; y++)
			for (int x = 0; x < VRAM_W; x++)
			{
				int64_t wg = 1023 * (int64_t)y, wb = 511 * (int64_t)x;
				int64_t wr = area - wg - wb;
				uint16_t want = 0;
				if (wr > 0)
					want = (uint16_t)(((255 * wr / area) >> 3)
									  | (((255 * wg / area) >> 3) << 5)
									  | (((255 * wb / area) >> 3) << 10));
				if (g_vram[y][x] != want && bad++ < 8)
					std::printf("FAIL: G3 (1023x511) - vram[%d][%d] = %04x, "
								"want %04x\n", y, x, g_vram[y][x], want);
			}
		if (bad)
			g_failures++;
		checkPx(0, 0, 0x001F, "G3 (1023x511): vertex colour still exact");
	}

	/* --- semi-transparency mode 0 (B/2 + F/2) via TPOLY-style E1+poly ----- */
	{
		resetEnv();
		/* background: mid grey 16/16/16 */
		RECT r = { 50, 50, 16, 16 };
		ClearImage(&r, 128, 128, 128);
		uint16_t bg = g_vram[50][50];

		/*	packet: E1 word (abr mode 0) then semi-transparent flat white quad
			- exactly the TPOLY_F4 wire format  */
		uint32_t packet[7];
		packet[0] = 0x06000000;								/* tag: len 6 (ignored addr) */
		packet[1] = 0xE1000200;								/* draw mode: dtd, abr 0 */
		packet[2] = 0x2A000000u | (255) | (255 << 8) | (255 << 16);	/* F4 + semi */
		packet[3] = (50 << 16) | 50;
		packet[4] = (50 << 16) | 66;
		packet[5] = (66 << 16) | 50;
		packet[6] = (66 << 16) | 66;
		DrawPrim(packet);

		int br = bg & 31, fr = 31;
		uint16_t want = (uint16_t)(((br + fr) >> 1) * 0x421);	/* same on all channels */
		checkPx(50, 50, want, "semi mode 0: (B+F)/2");
	}

	/* --- dithering: PS1 4x4 matrix on gouraud pixels (E1 dtd) ------------- */
	{
		resetEnv();
		/*	uniform mid-grey gouraud triangle: interpolation is exact, so
			every inner pixel is 128 + dither[y&3][x&3], then >>3  */
		static const uint32_t on[] =
		{
			0xE1000200,							/* dtd = 1 */
			0x30808080, 0x00640064,				/* (100,100) */
			0x00808080, 0x006400A4,				/* (164,100) */
			0x00808080, 0x00A40064,				/* (100,164) */
		};
		GPU_ExecWords(on, 7);
		checkPx(100, 100, 15 * 0x421, "dither (0,0): 128-4 >> 3");
		checkPx(101, 100, 16 * 0x421, "dither (1,0): 128+0 >> 3");
		checkPx(100, 101, 16 * 0x421, "dither (0,1): 128+2 >> 3");
		checkPx(101, 101, 15 * 0x421, "dither (1,1): 128-2 >> 3");

		resetEnv();
		static const uint32_t off[] =
		{
			0xE1000000,							/* dtd = 0 */
			0x30808080, 0x00640064,
			0x00808080, 0x006400A4,
			0x00808080, 0x00A40064,
		};
		GPU_ExecWords(off, 7);
		checkPx(100, 100, 16 * 0x421, "no dither: 128 >> 3");
		checkPx(101, 101, 16 * 0x421, "no dither: 128 >> 3 everywhere");
	}

	/*	--- dithering: every line, flat or gouraud (issue #76) ---------------
		The hardware dithers lines whenever E1 asks, unlike polygons, which
		need gouraud or texture modulation.  Mid-grey 128 along rows 100-102
		walks the table's first three rows: -4 0 -3 1 / 2 -2 3 -1 /
		-3 1 -4 0, i.e. 15 16 15 16 / 16 15 16 15 / 15 16 15 16.  A flat
		triangle under the same E1 stays undithered.  */
	{
		resetEnv();
		static const uint32_t on[] =
		{
			0xE1000200,							/* dtd = 1 */
			0x40808080, 0x00640064, 0x00640067,	/* LINE_F2 (100,100)-(103,100) */
			0x48808080, 0x00650064, 0x00650067,	/* flat polyline, row 101 */
			0x55555555,
			0x50808080, 0x00660064,				/* LINE_G2, uniform, row 102 */
			0x00808080, 0x00660067,
			0x20808080, 0x00C80064, 0x00C800A4, 0x00F00064,	/* flat F3 at y 200 */
		};
		GPU_ExecWords(on, (int)(sizeof(on) / sizeof(on[0])));
		static const int row0[4] = { 15, 16, 15, 16 };
		static const int row1[4] = { 16, 15, 16, 15 };
		static const int row2[4] = { 15, 16, 15, 16 };
		for (int i = 0; i < 4; i++)
		{
			checkPx(100 + i, 100, (uint16_t)(row0[i] * 0x421), "flat LINE_F2 with dtd=1 dithers");
			checkPx(100 + i, 101, (uint16_t)(row1[i] * 0x421), "flat polyline with dtd=1 dithers");
			checkPx(100 + i, 102, (uint16_t)(row2[i] * 0x421), "gouraud LINE_G2 with dtd=1 dithers as before");
		}
		checkPx(100, 200, 16 * 0x421, "flat F3 with dtd=1 stays undithered (0,0)");
		checkPx(101, 200, 16 * 0x421, "flat F3 with dtd=1 stays undithered (1,0)");

		resetEnv();
		static const uint32_t off[] =
		{
			0xE1000000,							/* dtd = 0 */
			0x40808080, 0x00640064, 0x00640067,
		};
		GPU_ExecWords(off, 4);
		for (int i = 0; i < 4; i++)
			checkPx(100 + i, 100, 16 * 0x421, "flat LINE_F2 with dtd=0: 128 >> 3");
	}

	/*	--- libgpu transfers: zero-size rects move nothing (issue #60) --------
		LoadImage/StoreImage clamp w to [0,1024] and h to [0,512] and size
		the DMA from w*h; the raw GP0 rule would have made w=0 a full
		1024-wide transfer through the caller's buffer.  MoveImage refuses
		a zero dimension with -1.  */
	{
		resetEnv();
		static uint16_t src[1024 * 4];
		for (int i = 0; i < 1024 * 4; i++)
			src[i] = 0x5555;
		g_vram[10][10]  = 0x1111;
		g_vram[10][500] = 0x2222;
		g_vram[13][10]  = 0x3333;
		RECT zw = { 10, 10, 0, 4 };
		RECT zh = { 10, 10, 8, 0 };
		LoadImage(&zw, (u_long *)src);
		LoadImage(&zh, (u_long *)src);
		checkPx(10, 10, 0x1111, "LoadImage w=0 writes nothing");
		checkPx(500, 10, 0x2222, "LoadImage w=0 is not a 1024-wide load");
		checkPx(10, 13, 0x3333, "LoadImage h=0 writes nothing");

		static uint16_t dst[1024 * 4 + 8];		/* room for the old w=0 */
		for (int i = 0; i < 1024 * 4 + 8; i++)
			dst[i] = 0xCAFE;
		StoreImage(&zw, (u_long *)dst);
		StoreImage(&zh, (u_long *)dst);
		check(dst[0] == 0xCAFE, "StoreImage zero-size rect stores nothing");

		/*	w=1025 clamps to 1024: exactly one VRAM row, not a halfword more.
			The raw GP0 rule would make it ((1025-1) & 0x3FF) + 1 = 1 halfword,
			so this separates the clamp from the mask (w=2048 would not: the
			mask also gives 1024 there).  */
		for (int x = 0; x < 1024; x++)
			g_vram[20][x] = (uint16_t)(0x4000 + x);
		RECT wide = { 0, 20, 1025, 1 };
		StoreImage(&wide, (u_long *)dst);
		bool row = true;
		for (int x = 0; x < 1024; x++)
			row = row && dst[x] == (uint16_t)(0x4000 + x);
		check(row, "StoreImage w=1025 stores the whole row");
		check(dst[1024] == 0xCAFE, "StoreImage w=1025 stores exactly 1024 halfwords");

		/*	h=513 clamps to 512 rows (the mask would give 1 row)  */
		for (int i = 0; i < 1024 * 4 + 8; i++)
			dst[i] = 0xCAFE;
		for (int y = 0; y < 512; y++)
			g_vram[y][700] = (uint16_t)(0x2000 + y);
		RECT tall = { 700, 0, 2, 513 };
		StoreImage(&tall, (u_long *)dst);
		check(dst[0] == 0x2000 && dst[2 * 511] == 0x2000 + 511,
			  "StoreImage h=513 stores all 512 rows");
		check(dst[1024] == 0xCAFE, "StoreImage h=513 stores exactly 512 rows");

		g_vram[30][40] = 0x0777;
		RECT mz = { 40, 30, 0, 4 };
		check(MoveImage(&mz, 300, 30) == -1, "MoveImage w=0 returns -1");
		checkPx(300, 30, 0x0000, "MoveImage w=0 moves nothing");
	}

	/*	--- ClearImage: libgpu's 02h/60h split (issue #28) --------------------
		After the clamp, x and w both 64-aligned emit the GP0(02h) fill (raw:
		wraps at the VRAM edge); anything else is a GP0(60h) opaque rect with
		the draw area set to all of VRAM and the offset zeroed around it -
		exact to the pixel, untouched by the game's draw env, clipped only by
		the VRAM edge.  */
	{
		resetEnv();
		RECT un = { 520, 20, 16, 4 };			/* x not aligned: 60h */
		ClearImage(&un, 255, 0, 0);
		checkPx(519, 20, 0x0000, "ClearImage x=520 leaves 519 (not rounded to 512)");
		checkPx(520, 20, 0x001F, "ClearImage x=520 fills from 520");
		checkPx(535, 23, 0x001F, "ClearImage x=520 w=16 fills to 535");
		checkPx(536, 20, 0x0000, "ClearImage x=520 w=16 stops at 535");
		checkPx(520, 24, 0x0000, "ClearImage h=4 stops at row 23");

		RECT w100 = { 512, 40, 100, 2 };		/* w not aligned: 60h */
		ClearImage(&w100, 255, 0, 0);
		checkPx(611, 40, 0x001F, "ClearImage w=100 fills to x=611");
		checkPx(612, 40, 0x0000, "ClearImage w=100 is not rounded up to 112");

		/*	an offset, clipped draw env: the 60h fill ignores it and leaves it  */
		DRAWENV env;
		SetDefDrawEnv(&env, 0, 0, 64, 64);
		env.ofs[0] = 100;
		env.ofs[1] = 50;
		PutDrawEnv(&env);
		RECT out = { 130, 10, 20, 5 };
		ClearImage(&out, 0, 255, 0);
		checkPx(130, 10, 0x03E0, "ClearImage ignores the draw clip and offset");
		checkPx(149, 14, 0x03E0, "ClearImage fills its rect outside the clip");
		checkPx(230, 60, 0x0000, "ClearImage did not add the draw offset");
		check(g_gpu.clipX0 == 0 && g_gpu.clipY0 == 0
			  && g_gpu.clipX1 == 63 && g_gpu.clipY1 == 63,
			  "ClearImage leaves the draw area as it was");
		check(g_gpu.ofsX == 100 && g_gpu.ofsY == 50,
			  "ClearImage leaves the draw offset as it was");

		/*	right edge: the 60h draw clips, the aligned 02h fill wraps  */
		resetEnv();
		RECT edgeU = { 1000, 30, 50, 2 };
		ClearImage(&edgeU, 255, 0, 0);
		checkPx(1023, 31, 0x001F, "unaligned ClearImage fills to the right edge");
		checkPx(0, 30, 0x0000, "unaligned ClearImage clips at the edge");
		checkPx(25, 31, 0x0000, "unaligned ClearImage does not wrap");

		RECT edgeA = { 960, 40, 128, 2 };
		ClearImage(&edgeA, 0, 0, 255);
		checkPx(960, 40, 0x7C00, "aligned ClearImage fills from its origin");
		checkPx(1023, 41, 0x7C00, "aligned ClearImage fills to the right edge");
		checkPx(0, 40, 0x7C00, "aligned ClearImage wraps like GP0(02h)");
		checkPx(63, 41, 0x7C00, "aligned ClearImage wraps its whole overhang");
		checkPx(64, 40, 0x0000, "aligned ClearImage wraps no further");

		/*	PutDrawEnv's isbg clear takes the same split, clipped by the env  */
		resetEnv();
		SetDefDrawEnv(&env, 520, 100, 40, 8);
		env.isbg = 1;
		setRGB0(&env, 255, 255, 255);
		PutDrawEnv(&env);
		checkPx(519, 100, 0x0000, "isbg clear x=520 leaves 519");
		checkPx(520, 100, 0x7FFF, "isbg clear fills the clip rect");
		checkPx(559, 107, 0x7FFF, "isbg clear fills to the clip's corner");
		checkPx(560, 107, 0x0000, "isbg clear stops at the clip's right edge");
		checkPx(559, 108, 0x0000, "isbg clear stops at the clip's bottom edge");

		/*	isbg with an offset that is not the clip origin, on a canary
			field: the fill lands exactly on the clip rect (the offset is
			compensated, not added) and every pixel around it keeps its
			canary.  The isbg fill rect is built from env->clip itself, so
			the clip can never be smaller than the fill through PutDrawEnv;
			what this pins is the rect and the offset.  */
		resetEnv();
		for (int y = 190; y < 216; y++)
			for (int x = 590; x < 640; x++)
				g_vram[y][x] = 0x1234;
		SetDefDrawEnv(&env, 600, 200, 24, 6);
		env.ofs[0] = 100;
		env.ofs[1] = 50;
		env.isbg = 1;
		setRGB0(&env, 0, 0, 255);
		PutDrawEnv(&env);
		int wrong = 0;
		for (int y = 190; y < 216; y++)
			for (int x = 590; x < 640; x++)
			{
				bool in = x >= 600 && x < 624 && y >= 200 && y < 206;
				wrong += g_vram[y][x] != (in ? 0x7C00 : 0x1234);
			}
		check(wrong == 0, "offset isbg clear fills exactly its clip, canaries intact");
		checkPx(700, 250, 0x0000, "offset isbg clear does not add the draw offset");

		/*	ClearImage2 is the same executor with dfe set, which the shim
			ignores: the same explicit pixels as the ClearImage cases.
			rgb15(200,100,50) = 25 | 12<<5 | 6<<10.  */
		const uint16_t c2 = 0x1999;
		resetEnv();
		RECT u2 = { 520, 20, 16, 4 };			/* unaligned: 60h */
		ClearImage2(&u2, 200, 100, 50);
		checkPx(519, 20, 0x0000, "ClearImage2 x=520 leaves 519");
		checkPx(520, 20, c2, "ClearImage2 x=520 fills from 520");
		checkPx(535, 23, c2, "ClearImage2 x=520 w=16 fills to 535");
		checkPx(536, 20, 0x0000, "ClearImage2 x=520 w=16 stops at 535");
		checkPx(520, 24, 0x0000, "ClearImage2 h=4 stops at row 23");

		RECT a2 = { 960, 40, 128, 2 };			/* aligned: 02h wraps */
		ClearImage2(&a2, 200, 100, 50);
		checkPx(1023, 41, c2, "ClearImage2 aligned fills to the right edge");
		checkPx(0, 40, c2, "ClearImage2 aligned wraps like GP0(02h)");
		checkPx(63, 41, c2, "ClearImage2 aligned wraps its whole overhang");
		checkPx(64, 40, 0x0000, "ClearImage2 aligned wraps no further");

		resetEnv();
		RECT wipe2 = { 512, 256, 2048, 254 };	/* clamps to w=1023: 60h */
		ClearImage2(&wipe2, 200, 100, 50);
		checkPx(512, 256, c2, "ClearImage2 cache wipe fills from its origin");
		checkPx(1023, 509, c2, "ClearImage2 cache wipe fills to the edge");
		checkPx(511, 256, 0x0000, "ClearImage2 cache wipe leaves x=511");
		checkPx(0, 256, 0x0000, "ClearImage2 cache wipe does not wrap");
		checkPx(512, 510, 0x0000, "ClearImage2 cache wipe stops at row 509");

		resetEnv();
		RECT neg2 = { -8, -4, 40, 30 };			/* unaligned, off the top-left */
		ClearImage2(&neg2, 200, 100, 50);
		checkPx(0, 0, c2, "ClearImage2 negative origin fills from (0,0)");
		checkPx(31, 25, c2, "ClearImage2 negative origin fills to (31,25)");
		checkPx(32, 0, 0x0000, "ClearImage2 negative origin stops at x=31");
		checkPx(0, 26, 0x0000, "ClearImage2 negative origin stops at y=25");
		checkPx(1016, 0, 0x0000, "ClearImage2 negative x does not wrap right");
		checkPx(0, 508, 0x0000, "ClearImage2 negative y does not wrap down");

		resetEnv();
		RECT big2 = { 0, 0, 512, 512 };			/* h clamps to 511: 02h */
		ClearImage2(&big2, 200, 100, 50);
		checkPx(511, 510, c2, "ClearImage2 512x512 fills 512x511");
		checkPx(0, 511, 0x0000, "ClearImage2 512x512 clamps h to 511");
		checkPx(512, 0, 0x0000, "ClearImage2 512x512 stops at x=511");
	}

	/* --- texture window (E2): u -> (u & ~mask*8) | (offset&mask)*8 -------- */
	{
		resetEnv();
		for (int k = 0; k < 8; k++)
		{
			g_vram[0][k]     = (uint16_t)(0x7C00 + k);	/* visible if window ignored */
			g_vram[0][8 + k] = (uint16_t)(0x108 + k);	/* the windowed texels */
		}
		static const uint32_t rect[] =
		{
			0xE1000100,							/* 15bpp, base (0,0), dtd 0 */
			0xE2000401,							/* maskX=1, offX=1: u |= 8 */
			0x65808080,							/* raw textured rect */
			0x0050012C,							/* xy = (300,80) */
			0x00000000,							/* uv = (0,0), clut unused */
			0x00010008,							/* 8x1 */
			0xE2000000,							/* window off again */
		};
		GPU_ExecWords(rect, 7);
		for (int k = 0; k < 8; k++)
			checkPx(300 + k, 80, (uint16_t)(0x108 + k), "texture window remaps u");
	}

	/* --- polylines: segment chains to the 0x5xxx5xxx terminator ----------- */
	{
		resetEnv();
		static const uint32_t lines[] =
		{
			0xE1000000,							/* dtd 0 */
			0x48FFFFFF,							/* flat white polyline */
			0x000A000A,							/* (10,10) */
			0x000A0014,							/* (20,10) */
			0x00140014,							/* (20,20) */
			0x55555555,							/* terminator */
			0x580000FF,							/* gouraud polyline, red */
			0x000A0028,							/* (40,10) */
			0x000000FF, 0x000A003C,				/* red again, (60,10) */
			0x55555555,
		};
		GPU_ExecWords(lines, 11);
		checkPx(10, 10, 0x7FFF, "polyline start");
		checkPx(15, 10, 0x7FFF, "polyline mid segment 1");
		checkPx(20, 10, 0x7FFF, "polyline joint");
		checkPx(20, 15, 0x7FFF, "polyline mid segment 2");
		checkPx(20, 20, 0x7FFF, "polyline end");
		checkPx(50, 10, 0x001F, "gouraud polyline, uniform red");
	}

	/* --- polyline terminator rules: only at a vertex GROUP's first word --- */
	{
		resetEnv();

		/*	An empty chain (terminator where vertex 1 would be) must draw
			nothing AND consume exactly two words, so the primitive that
			follows is decoded as a command and not as polyline data.  */
		static const uint32_t empty[] =
		{
			0xE1000000,							/* dtd 0 */
			0x48FFFFFF,							/* flat white polyline... */
			0x55555555,							/* ...terminated immediately */
			0x68FFFFFF, 0x00500050,				/* 1x1 white dot at (80,80) */
		};
		GPU_ExecWords(empty, 5);
		checkPx(80, 80, 0x7FFF, "stream stays in sync past an empty polyline");

		/*	A vertex whose halfwords both start with nibble 5 is a legal
			coordinate, not a terminator: for a shaded chain the terminator
			is only read where a COLOUR word belongs.  (0x50A0_5078 =
			x=0x5078=20600, y=0x50A0=20640 - both wrap to sane 11-bit
			signed coords, and the segment to it must still be drawn.)  */
		static const uint32_t shaded[] =
		{
			0x580000FF,							/* gouraud polyline, red */
			0x00780028,							/* (40,120) */
			0x000000FF, 0x00780032,				/* red, (50,120) */
			0x000000FF, 0x50A05078,				/* red, vertex that LOOKS like 5xxx5xxx */
			0x55555555,							/* the real terminator */
			0x68FFFFFF, 0x00640064,				/* 1x1 white dot at (100,100) */
		};
		GPU_ExecWords(shaded, 9);
		checkPx(45, 120, 0x001F, "shaded polyline segment 1");
		checkPx(100, 100, 0x7FFF, "5xxx5xxx vertex did not fake a terminator");
	}

	/* --- isrgb24 display unpack (M7 FMV) ---------------------------------- */
	{
		resetEnv();

		/*	15bpp baseline through the shared unpack.  */
		DISPENV disp;
		SetDefDispEnv(&disp, 8, 16, 320, 240);
		PutDispEnv(&disp);
		g_vram[16][8] = (uint16_t)((0x1F << 10) | (0x08 << 5) | 0x11);
		unsigned char rgb[3];
		GPU_ReadDisplayPixelRGB(0, 0, rgb);
		check(rgb[0] == (0x11 << 3) && rgb[1] == (0x08 << 3) && rgb[2] == 0xF8,
			  "display unpack: 15bpp channels x8");

		/*	24bpp: pixels 0,1 = (R0 G0 B0)(R1 G1 B1) packed little-endian
			into three halfwords: G0R0, R1B0, B1G1.  disp.w is in PIXELS
			(fmv.cpp pre-divides by 3/2); disp.x in halfwords.  */
		disp.isrgb24 = 1;
		PutDispEnv(&disp);
		g_vram[16][8]  = (uint16_t)(0x22 << 8 | 0x11);	/* G0 R0 */
		g_vram[16][9]  = (uint16_t)(0x44 << 8 | 0x33);	/* R1 B0 */
		g_vram[16][10] = (uint16_t)(0x66 << 8 | 0x55);	/* B1 G1 */
		GPU_ReadDisplayPixelRGB(0, 0, rgb);
		check(rgb[0] == 0x11 && rgb[1] == 0x22 && rgb[2] == 0x33,
			  "display unpack: 24bpp pixel 0 (even byte phase)");
		GPU_ReadDisplayPixelRGB(1, 0, rgb);
		check(rgb[0] == 0x44 && rgb[1] == 0x55 && rgb[2] == 0x66,
			  "display unpack: 24bpp pixel 1 (odd byte phase)");

		/*	Leaving 24bpp mode restores the 15bpp path (PutDispEnv copies
			isrgb24 every time - the game's next VidSwapDraw does this).  */
		disp.isrgb24 = 0;
		PutDispEnv(&disp);
		GPU_ReadDisplayPixelRGB(0, 0, rgb);
		check(rgb[2] == ((0x22 >> 2) << 3),
			  "display unpack: isrgb24 clears on the next PutDispEnv");
	}

	if (g_failures)
	{
		std::printf("gpu test FAILED (%d)\n", g_failures);
		return 1;
	}
	std::printf("gpu test PASSED\n");
	return 0;
}
