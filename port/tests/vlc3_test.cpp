/*	vlc3_test (M7): the VLC v2/v3 decoder in psyq/mdec/vlc3.cpp.

	Layers:
	  1. Hand-assembled bitstreams: every AC code class, escape, sign
	     negation, EOB; v3 DC for both Huffman tables with the three
	     predictors and 10-bit wrap; v2 raw DC; end-of-frame codes.
	  2. Real-frame pixel layer: THQ.STR frame 1 (fixture) through
	     DecDCTvlc3 + DecDCTin, tolerance-compared against ffmpeg's RGB24
	     decode (the oracle is NOT bit-exact - different IDCT - so this
	     asserts closeness and prints the stats).
	  3. Sweep layer: every video frame of every .str under data/CDData is
	     VLC-decoded into a game-sized (77,120B) buffer, asserting BS
	     version, exactly 1800 blocks per 320x240 frame, monotonic frame
	     numbers, and that the declared MDEC size fits the game's vlcbuf -
	     the empirical retirement of the M7 plan's capacity risk.

	Fixtures: py port/tests/make_str_fixture.py (needs ffmpeg on PATH).
	Layers 2 and 3 skip gracefully when files are absent (run from the
	repo root).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sys/types.h>
#include <libpress.h>

#include "mdec/mdec_internal.h"

extern "C" void DecDCTvlcBuild3(unsigned short *table);
extern "C" int DecDCTvlcSize3(int breaksize);
extern "C" int DecDCTvlc3(unsigned long *bs, unsigned long *buf);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*****************************************************************************/
/*	MSB-first bit assembler over little-endian halfwords, mirroring the
	decoder's reader.  */
struct BitWriter
{
	uint16_t	hw[512];
	int			n;			/* halfwords finished */
	uint32_t	acc;
	int			bits;
};

static void bwInit(BitWriter *w)
{
	memset(w, 0, sizeof(*w));
}

static void bwPut(BitWriter *w, unsigned value, int k)
{
	for (int i = k - 1; i >= 0; i--)
	{
		w->acc = (w->acc << 1) | ((value >> i) & 1u);
		if (++w->bits == 16)
		{
			w->hw[w->n++] = (uint16_t)w->acc;
			w->acc = 0;
			w->bits = 0;
		}
	}
}

static void bwFlush(BitWriter *w)
{
	while (w->bits)
		bwPut(w, 0, 1);
	w->hw[w->n] = 0;		/* slack for the decoder's peek-ahead */
	w->hw[w->n + 1] = 0;
}

/*	Build a BS frame around a payload writer and run DecDCTvlc3.  */
static u_long g_bs[600];
static u_long g_out[19280];				/* 77120 bytes = the game's vlcbuf */

static const uint16_t *runVlc(const BitWriter *w, unsigned qscale,
							  unsigned version, unsigned declWords)
{
	uint16_t *hdr = (uint16_t *)g_bs;
	hdr[0] = (uint16_t)declWords;
	hdr[1] = 0x3800;
	hdr[2] = (uint16_t)qscale;
	hdr[3] = (uint16_t)version;
	memcpy(hdr + 4, w->hw, (size_t)(w->n + 2) * 2);
	memset(g_out, 0, sizeof(g_out));
	DecDCTvlc3(g_bs, g_out);
	return (const uint16_t *)(g_out + 1);
}

/*	v3 DC writers: emit the size code then the MPEG-style size bits.  */
static void putDcChroma(BitWriter *w, int diff4)
{
	static const struct { unsigned code; int len; } SZ[9] =
	{
		{ 0x0, 2 }, { 0x1, 2 }, { 0x2, 2 }, { 0x6, 3 }, { 0xE, 4 },
		{ 0x1E, 5 }, { 0x3E, 6 }, { 0x7E, 7 }, { 0xFE, 8 },
	};
	int diff = diff4, size = 0, mag = diff < 0 ? -diff : diff;
	while (mag >> size)
		size++;
	bwPut(w, SZ[size].code, SZ[size].len);
	if (size)
		bwPut(w, diff >= 0 ? (unsigned)diff
							: (unsigned)(diff + (1 << size) - 1), size);
}

static void putDcLuma(BitWriter *w, int diff4)
{
	static const struct { unsigned code; int len; } SZ[9] =
	{
		{ 0x4, 3 }, { 0x0, 2 }, { 0x1, 2 }, { 0x5, 3 }, { 0x6, 3 },
		{ 0xE, 4 }, { 0x1E, 5 }, { 0x3E, 6 }, { 0x7E, 7 },
	};
	int diff = diff4, size = 0, mag = diff < 0 ? -diff : diff;
	while (mag >> size)
		size++;
	bwPut(w, SZ[size].code, SZ[size].len);
	if (size)
		bwPut(w, diff >= 0 ? (unsigned)diff
							: (unsigned)(diff + (1 << size) - 1), size);
}

static void testHandAssembled(void)
{
	BitWriter w;

	/*	v2: one full macroblock of DC-only blocks with distinct raw DCs,
		then EOF (0x1FF in place of Cr).  */
	bwInit(&w);
	for (int b = 0; b < 6; b++)
	{
		bwPut(&w, (unsigned)(100 + b) & 0x3FF, 10);	/* raw DC */
		bwPut(&w, 0x2, 2);							/* EOB "10" */
	}
	bwPut(&w, 0x1FF, 10);
	bwFlush(&w);
	const uint16_t *out = runVlc(&w, 2, 2, 32);
	{
		bool ok = true;
		for (int b = 0; b < 6; b++)
		{
			ok = ok && out[b * 2] == ((2u << 10) | (unsigned)(100 + b));
			ok = ok && out[b * 2 + 1] == MDEC_EOB;
		}
		check(ok, "v2: raw DCs + EOBs come through");
		check(((const uint16_t *)(g_out + 1))[12] == MDEC_EOB
			  && (g_out[0] & 0xFFFF) == 32
			  && (g_out[0] >> 16) == 0x3800,
			  "v2: header word + FE00 padding to declared size");
	}

	/*	v3 DC: chroma and luma tables, predictors, negative diffs, wrap.
		MB1: Cr diff +8, Cb diff -4, Y diffs +16, +4, -8, 0
		MB2: Cr diff -12 (pred carries), Y1 diff +4 (pred = Y4 of MB1).  */
	bwInit(&w);
	putDcChroma(&w, 8);   bwPut(&w, 0x2, 2);
	putDcChroma(&w, -4);  bwPut(&w, 0x2, 2);
	putDcLuma(&w, 16);    bwPut(&w, 0x2, 2);
	putDcLuma(&w, 4);     bwPut(&w, 0x2, 2);
	putDcLuma(&w, -8);    bwPut(&w, 0x2, 2);
	putDcLuma(&w, 0);     bwPut(&w, 0x2, 2);
	putDcChroma(&w, -12); bwPut(&w, 0x2, 2);
	putDcChroma(&w, 0);   bwPut(&w, 0x2, 2);
	putDcLuma(&w, 4);     bwPut(&w, 0x2, 2);
	for (int i = 0; i < 3; i++)
	{
		putDcLuma(&w, 0);
		bwPut(&w, 0x2, 2);
	}
	bwPut(&w, 0x3FF, 10);				/* EOF */
	bwFlush(&w);
	out = runVlc(&w, 1, 3, 32);
	{
		/*	Diffs scale by 4: Cr 32; Cb -16; Y 64,80,48,48; then MB2
			Cr 32-48=-16, Cb -16, Y1 48+16=64, Y 64,64,64.  */
		static const int expect[12] =
		{
			32, -16, 64, 80, 48, 48,	/* MB1: Cr Cb Y1 Y2 Y3 Y4 */
			-16, -16, 64, 64, 64, 64,	/* MB2 */
		};
		bool ok = true;
		for (int i = 0; i < 12; i++)
			ok = ok && out[i * 2] == ((1u << 10) | ((unsigned)expect[i] & 0x3FF));
		check(ok, "v3: DC predictors across both tables");
	}

	/*	v3 DC 10-bit wrap: two +255*4 luma diffs from 0 wrap negative.  */
	bwInit(&w);
	putDcChroma(&w, 0); bwPut(&w, 0x2, 2);
	putDcChroma(&w, 0); bwPut(&w, 0x2, 2);
	putDcLuma(&w, 255); bwPut(&w, 0x2, 2);	/* DC 1020 */
	putDcLuma(&w, 255); bwPut(&w, 0x2, 2);	/* 2040 wraps to -8 */
	putDcLuma(&w, 0);   bwPut(&w, 0x2, 2);
	putDcLuma(&w, 0);   bwPut(&w, 0x2, 2);
	bwPut(&w, 0x3FF, 10);
	bwFlush(&w);
	out = runVlc(&w, 1, 3, 32);
	check(out[2 * 2] == ((1u << 10) | (1020u & 0x3FF)),
		  "v3: +255*4 diff reaches DC 1020");
	check(out[3 * 2] == ((1u << 10) | ((unsigned)-8 & 0x3FF)),
		  "v3: DC wraps in 10 bits (2040 -> -8)");

	/*	AC codes: one block exercising each class, plus escape and sign.
		Emitted after a DC of 0 (chroma table, diff 0).  */
	bwInit(&w);
	putDcChroma(&w, 0);
	bwPut(&w, 0x3, 2); bwPut(&w, 0, 1);			/* "11"+s0    -> 0x0001 */
	bwPut(&w, 0x3, 3); bwPut(&w, 1, 1);			/* "011"+s1   -> neg 0x0401 */
	bwPut(&w, 0x2, 3); bwPut(&w, 1, 1); bwPut(&w, 0, 1);	/* "010"+x1+s0  -> 0x0801 */
	bwPut(&w, 0x3, 4); bwPut(&w, 0, 1); bwPut(&w, 0, 1);	/* "0011"+x0+s0 -> 0x1001 */
	bwPut(&w, 0x5, 5); bwPut(&w, 0, 1);			/* "00101"+s0 -> 0x0003 */
	bwPut(&w, 0x4, 5); bwPut(&w, 5, 3); bwPut(&w, 1, 1);	/* "00100"+x5+s1 -> neg 0x0403 */
	bwPut(&w, 0x1, 4); bwPut(&w, 2, 2); bwPut(&w, 0, 1);	/* "0001"+x2+s0 -> 0x0402 */
	bwPut(&w, 0x1, 5); bwPut(&w, 1, 2); bwPut(&w, 0, 1);	/* "00001"+x1+s0 -> 0x2401 */
	bwPut(&w, 0x1, 7); bwPut(&w, 3, 3); bwPut(&w, 0, 1);	/* "0000001"+x3+s0 -> 0x0803 */
	bwPut(&w, 0x1, 8); bwPut(&w, 0, 4); bwPut(&w, 1, 1);	/* "00000001"+x0+s1 -> neg 0x000B */
	bwPut(&w, 0x1, 9); bwPut(&w, 7, 4); bwPut(&w, 0, 1);	/* "000000001"+x7+s0 -> 0x000F */
	bwPut(&w, 0x1, 10); bwPut(&w, 15, 4); bwPut(&w, 0, 1);	/* -> 0x0010 */
	bwPut(&w, 0x1, 11); bwPut(&w, 9, 4); bwPut(&w, 0, 1);	/* -> 0x040E */
	bwPut(&w, 0x1, 12); bwPut(&w, 4, 4); bwPut(&w, 0, 1);	/* -> 0x1803 */
	bwPut(&w, 0x1, 6); bwPut(&w, 0xABCDu >> 4, 12);			/* escape "000001".. */
	bwPut(&w, 0xABCDu & 0xF, 4);
	bwPut(&w, 0x2, 2);							/* EOB */
	/*	EOF is only legal in place of a Cr DC, so finish the macroblock:
		Cb + 4 Y blocks, all DC 0 with empty AC lists.  */
	putDcChroma(&w, 0); bwPut(&w, 0x2, 2);
	for (int i = 0; i < 4; i++)
	{
		putDcLuma(&w, 0);
		bwPut(&w, 0x2, 2);
	}
	bwPut(&w, 0x3FF, 10);						/* EOF */
	bwFlush(&w);
	out = runVlc(&w, 1, 3, 64);
	{
		static const uint16_t expect[] =
		{
			(1u << 10) | 0,				/* DC */
			0x0001,
			(uint16_t)((0x0401 & 0xFC00) | ((0x400 - 1) & 0x3FF)),
			0x0801,
			0x1001,
			0x0003,
			(uint16_t)((0x0403 & 0xFC00) | ((0x400 - 3) & 0x3FF)),
			0x0402,
			0x2401,
			0x0803,
			(uint16_t)((0x000B & 0xFC00) | ((0x400 - 0xB) & 0x3FF)),
			0x000F,
			0x0010,
			0x040E,
			0x1803,
			0xABCD,
			MDEC_EOB,
			(1u << 10) | 0, MDEC_EOB,	/* Cb */
			(1u << 10) | 0, MDEC_EOB,	/* Y1..Y4 */
			(1u << 10) | 0, MDEC_EOB,
			(1u << 10) | 0, MDEC_EOB,
			(1u << 10) | 0, MDEC_EOB,
		};
		bool ok = true;
		for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++)
			ok = ok && out[i] == expect[i];
		check(ok, "v3: AC code classes, signs, escape");
	}
}

/*****************************************************************************/
/*	Layer 2: THQ frame 1 vs the ffmpeg golden.  */

static u_long g_frameBs[3000];			/* up to ~12KB of BS data */
static uint8_t g_pixels[300 * MDEC_MB_BYTES_24BPP];
static uint8_t g_golden[320 * 240 * 3];

static void testRealFrame(void)
{
	FILE *fb = fopen("port/tests/str_frame1.bin", "rb");
	FILE *fg = fopen("port/tests/str_frame1_golden.rgb", "rb");
	if (!fb || !fg)
	{
		printf("vlc3_test: frame fixtures not found (run from the repo root, "
			   "py port/tests/make_str_fixture.py) - pixel layer SKIPPED\n");
		if (fb) fclose(fb);
		if (fg) fclose(fg);
		return;
	}
	char magic[4];
	uint16_t wh[2];
	uint32_t sz[2];
	bool ok = fread(magic, 1, 4, fb) == 4 && memcmp(magic, "STF1", 4) == 0
		   && fread(wh, 2, 2, fb) == 2 && fread(sz, 4, 2, fb) == 2
		   && wh[0] == 320 && wh[1] == 240
		   && sz[1] <= sizeof(g_frameBs)
		   && fread(g_frameBs, 1, sz[1], fb) == sz[1]
		   && fread(g_golden, 1, sizeof(g_golden), fg) == sizeof(g_golden);
	fclose(fb);
	fclose(fg);
	if (!ok)
	{
		check(false, "frame fixture reads");
		return;
	}

	DecDCTvlcBuild3(NULL);
	DecDCTvlcSize3(320 * 240);
	DecDCTvlc3(g_frameBs, g_out);
	check((g_out[0] >> 16) == 0x3800, "frame: vlc output header");

	DecDCTReset(0);
	DecDCToutCallback(0);
	DecDCTin(g_out, 3);
	check(Mdec_FrameBytesForTest() == (int)sizeof(g_pixels),
		  "frame: 300 macroblocks decoded");
	DecDCTout((u_long *)g_pixels, (int)sizeof(g_pixels) / 4);

	/*	Our stream is 16px column strips of 16x16 MBs; golden is raster.  */
	long maxD = 0, sumD = 0, over2 = 0;
	for (int mb = 0; mb < 300; mb++)
	{
		int col = mb / 15, row = mb % 15;
		for (int py = 0; py < 16; py++)
			for (int px = 0; px < 16; px++)
				for (int ch = 0; ch < 3; ch++)
				{
					int a = g_pixels[mb * 768 + (py * 16 + px) * 3 + ch];
					int b = g_golden[((row * 16 + py) * 320 + col * 16 + px) * 3 + ch];
					int d = a > b ? a - b : b - a;
					if (d > maxD) maxD = d;
					sumD += d;
					if (d > 2) over2++;
				}
	}
	printf("vlc3_test: frame 1 vs ffmpeg: maxDelta %ld, meanAbs %.3f, "
		   ">2 count %ld of %d\n", maxD, (double)sumD / (320 * 240 * 3),
		   over2, 320 * 240 * 3);
	check(maxD <= 16, "frame: within tolerance of the ffmpeg oracle");
}

/*****************************************************************************/
/*	Layer 3: sweep every frame of every movie (VLC only - no IDCT).  */

static void sweepMovie(const char *path, long *framesOut)
{
	FILE *f = fopen(path, "rb");
	if (!f)
	{
		printf("vlc3_test: %s absent - skipped\n", path);
		return;
	}
	static uint8_t sec[2336];
	static uint8_t bs[24 * 2016 + 16];
	long frames = 0, badFrames = 0, curFrame = -1, lastFrame = 0;
	unsigned nSectors = 0, gotChunks = 0, bsLen = 0;
	long maxDecl = 0;
	int versions = 0;

	for (;;)
	{
		if (fread(sec, 1, 2336, f) != 2336)
			break;
		if (sec[2] & 0x04)
			continue;					/* audio */
		const uint8_t *d = sec + 8;
		uint16_t id, type, secOff, secCnt;
		uint32_t frameNo;
		memcpy(&id, d, 2);
		memcpy(&type, d + 2, 2);
		memcpy(&secOff, d + 4, 2);
		memcpy(&secCnt, d + 6, 2);
		memcpy(&frameNo, d + 8, 4);
		if (id != 0x0160 || type != 0x8001)
			continue;					/* zero pad etc */
		if ((long)frameNo != curFrame)
		{
			curFrame = (long)frameNo;
			nSectors = secCnt;
			gotChunks = 0;
			bsLen = 0;
		}
		if ((unsigned)secOff * 2016 + 2016 <= sizeof(bs))
		{
			memcpy(bs + (unsigned)secOff * 2016, d + 0x20, 2016);
			gotChunks++;
			if ((unsigned)(secOff + 1) * 2016 > bsLen)
				bsLen = (unsigned)(secOff + 1) * 2016;
		}
		if (gotChunks != nSectors)
			continue;

		/*	Whole frame assembled - VLC it into the game-sized buffer.  */
		const uint16_t *hdr = (const uint16_t *)bs;
		unsigned decl = hdr[0], ver = hdr[3];
		versions |= 1 << (ver < 8 ? ver : 7);
		if ((long)decl > maxDecl)
			maxDecl = (long)decl;
		if ((decl + 1) * 4 > 77120)
			badFrames += 1000000;		/* would not fit the prim-pool bufs */
		memset(bs + bsLen, 0, 8);		/* peek-ahead slack */
		DecDCTvlc3((unsigned long *)bs, g_out);

		/*	Count blocks in the run-level output.  */
		const uint16_t *o = (const uint16_t *)(g_out + 1);
		unsigned total = (unsigned)(g_out[0] & 0xFFFF) * 2;
		long blocks = 0;
		for (unsigned i = 0; i < total;)
		{
			if (o[i] == MDEC_EOB)
			{
				i++;
				continue;
			}
			i++;						/* DC */
			while (i < total && o[i] != MDEC_EOB)
				i++;
			i++;						/* EOB */
			blocks++;
		}
		if (blocks != 1800)
			badFrames++;
		if ((long)frameNo < lastFrame)
			badFrames++;
		lastFrame = (long)frameNo;
		frames++;
		gotChunks = 0;					/* don't re-run on stray chunks */
	}
	fclose(f);
	printf("vlc3_test: %s: %ld frames, versions mask %02X, max declared "
		   "words %ld (worst buffer need %ld of 77120)\n",
		   path, frames, versions, maxDecl, (maxDecl + 1) * 4);
	check(badFrames == 0, path);
	*framesOut += frames;
}

/*****************************************************************************/
int main(void)
{
	testHandAssembled();
	testRealFrame();

	long frames = 0;
	sweepMovie("data/CDData/thq.str", &frames);
	sweepMovie("data/CDData/climax.str", &frames);
	sweepMovie("data/CDData/intro.str", &frames);
	sweepMovie("data/CDData/demo.str", &frames);
	if (frames)
		printf("vlc3_test: sweep total %ld frames\n", frames);

	if (g_failures)
	{
		printf("vlc3_test: %d FAILURES\n", g_failures);
		return 1;
	}
	printf("vlc3_test: all passed\n");
	return 0;
}
