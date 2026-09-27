/*	mdec_test (M7): the libpress MDEC decoder core in psyq/mdec/mdec.cpp.

	Layers:
	  1. Run-level dequant (Mdec_RlDecodeBlock): DC scaling, AC formula,
	     zigzag placement, qscale==0 linear mode, EOB/padding handling.
	  2. IDCT (Mdec_Idct): DC flatness plus golden vectors from
	     make_mdec_golden.py (independent psx-spx transcription).
	  3. yuv_to_rgb (Mdec_YuvToRgb24): full-macroblock golden, byte order.
	  4. DecDCTin/DecDCTout/DecDCToutCallback: end-to-end stream decode,
	     fmv.cpp-style callback chaining through the trampoline (flat stack).
	  5. DecDCTout bounds: a request sized from a corrupt STR height is
	     capped at one full-height slice, against a guard page.

	Golden fixture is loaded repo-root-relative with a graceful skip, like
	xa_test.  Regenerate with:  py port/tests/make_mdec_golden.py
*/
#include <windows.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <sys/types.h>
#include <libpress.h>

#include "mdec/mdec_internal.h"

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*	Same tables the implementation transcribed from psx-spx - duplicated
	here so a table typo on either side fails loudly.  */
static const uint8_t ZIGZAG[64] =
{
	 0,  1,  5,  6, 14, 15, 27, 28,
	 2,  4,  7, 13, 16, 26, 29, 42,
	 3,  8, 12, 17, 25, 30, 41, 43,
	 9, 11, 18, 24, 31, 40, 44, 53,
	10, 19, 23, 32, 39, 45, 52, 54,
	20, 22, 33, 38, 46, 51, 55, 60,
	21, 34, 37, 47, 50, 56, 59, 61,
	35, 36, 48, 49, 57, 58, 62, 63,
};

static const uint8_t QUANT_RASTER[64] =
{
	 2, 16, 19, 22, 26, 27, 29, 34,
	16, 16, 22, 24, 27, 29, 34, 37,
	19, 22, 26, 27, 29, 34, 34, 38,
	22, 22, 26, 27, 29, 34, 37, 40,
	22, 26, 27, 29, 32, 35, 40, 48,
	26, 27, 29, 32, 35, 40, 48, 58,
	26, 27, 29, 34, 38, 46, 56, 69,
	27, 29, 35, 38, 46, 56, 69, 83,
};

static uint8_t zagzig[64];

/*****************************************************************************/
static void testIqTable(void)
{
	for (int i = 0; i < 64; i++)
		zagzig[ZIGZAG[i]] = (uint8_t)i;
	const uint8_t *iq = Mdec_IqZigzagYForTest();
	bool ok = true;
	for (int k = 0; k < 64; k++)
		ok = ok && iq[k] == QUANT_RASTER[zagzig[k]];
	check(ok, "default IQ table is the PSX matrix in zigzag order");
}

/*****************************************************************************/
static void testRlDecode(void)
{
	const uint8_t *iq = Mdec_IqZigzagYForTest();
	int16_t blk[64];

	/*	DC only: qscale 2, DC 5 -> blk[0] = 5*iq[0] = 10, rest zero.  */
	{
		const uint16_t stream[] = { (2u << 10) | 5u, MDEC_EOB };
		const uint16_t *p = stream, *end = stream + 2;
		check(Mdec_RlDecodeBlock(blk, &p, end, iq) == 0, "rl: DC-only decodes");
		check(p == end, "rl: DC-only consumes both halfwords");
		check(blk[0] == 5 * iq[0], "rl: DC = DC*qt[0] (no qscale/8)");
		bool rest = true;
		for (int i = 1; i < 64; i++)
			rest = rest && blk[i] == 0;
		check(rest, "rl: DC-only leaves ACs zero");
	}

	/*	Negative DC sign extension: 10bit 0x3FF = -1.  */
	{
		const uint16_t stream[] = { (1u << 10) | 0x3FFu, MDEC_EOB };
		const uint16_t *p = stream, *end = stream + 2;
		Mdec_RlDecodeBlock(blk, &p, end, iq);
		check(blk[0] == -1 * iq[0], "rl: DC sign-extends from 10 bits");
	}

	/*	One AC with run 2 at qscale 4: stream index k = 0+2+1 = 3, raster
		position zagzig[3]; value = (level*qt[3]*4+4)/8.  */
	{
		const int level = 20, qscale = 4;
		const uint16_t stream[] =
		{
			(uint16_t)((qscale << 10) | 0u),
			(uint16_t)((2u << 10) | (unsigned)level),
			MDEC_EOB,
		};
		const uint16_t *p = stream, *end = stream + 3;
		Mdec_RlDecodeBlock(blk, &p, end, iq);
		int expect = (level * iq[3] * qscale + 4) / 8;
		check(blk[zagzig[3]] == expect, "rl: AC formula + zigzag placement");
		check(blk[0] == 0, "rl: zero DC stays zero");
	}

	/*	Saturation: DC -512 * qt[0]=2 = -1024 exactly at the -0x400 rail;
		big AC saturates to +0x3FF.  (Note qscale 63 with DC 0x200 would
		encode as 0xFE00 = the EOB code itself - unencodable, and ignored
		as padding by hardware and shim alike - so use qscale 62.)  */
	{
		const uint16_t stream[] =
		{
			(uint16_t)((62u << 10) | 0x200u),		/* DC -512, qscale 62 */
			(uint16_t)((0u << 10) | 0x1FFu),		/* AC +511 */
			MDEC_EOB,
		};
		const uint16_t *p = stream, *end = stream + 3;
		Mdec_RlDecodeBlock(blk, &p, end, iq);
		check(blk[0] == -0x400, "rl: DC saturates at -0x400");
		check(blk[zagzig[1]] == 0x3FF, "rl: AC saturates at +0x3FF");
	}

	/*	qscale==0: value = level*2, LINEAR placement (no zigzag).  */
	{
		const uint16_t stream[] =
		{
			(uint16_t)((0u << 10) | 7u),			/* DC 7, qscale 0 */
			(uint16_t)((4u << 10) | 0x3F0u),		/* run 4 -> k=5, -16 */
			MDEC_EOB,
		};
		const uint16_t *p = stream, *end = stream + 3;
		Mdec_RlDecodeBlock(blk, &p, end, iq);
		check(blk[0] == 14, "rl: qscale 0 DC = DC*2");
		check(blk[5] == -32, "rl: qscale 0 stores linearly");
	}

	/*	Leading FE00 padding is skipped; end-of-stream returns -1.  */
	{
		const uint16_t stream[] = { MDEC_EOB, MDEC_EOB, (1u << 10) | 3u, MDEC_EOB };
		const uint16_t *p = stream, *end = stream + 4;
		check(Mdec_RlDecodeBlock(blk, &p, end, iq) == 0, "rl: skips FE00 padding");
		check(blk[0] == 3 * iq[0], "rl: decodes after padding");
		check(Mdec_RlDecodeBlock(blk, &p, end, iq) == -1,
			  "rl: pure padding tail reports end of stream");
	}
}

/*****************************************************************************/
static FILE *g_golden;

static bool goldenRead(void *dst, size_t n, const char *what)
{
	if (!g_golden || fread(dst, 1, n, g_golden) != n)
	{
		printf("mdec_test: golden fixture short read (%s)\n", what);
		return false;
	}
	return true;
}

static void testIdct(void)
{
	int16_t blk[64];

	/*	DC-only input must come out flat (all 64 outputs equal).  */
	memset(blk, 0, sizeof(blk));
	blk[0] = 64;
	Mdec_Idct(blk);
	bool flat = true;
	for (int i = 1; i < 64; i++)
		flat = flat && blk[i] == blk[0];
	check(flat, "idct: DC-only input decodes flat");

	if (!g_golden)
		return;
	int16_t golden[64];

	memset(blk, 0, sizeof(blk));
	blk[0] = 64;
	Mdec_Idct(blk);
	if (goldenRead(golden, sizeof(golden), "idct dc"))
		check(memcmp(blk, golden, sizeof(golden)) == 0, "idct: DC golden vector");

	memset(blk, 0, sizeof(blk));
	blk[9] = 100;
	Mdec_Idct(blk);
	if (goldenRead(golden, sizeof(golden), "idct impulse"))
		check(memcmp(blk, golden, sizeof(golden)) == 0, "idct: AC impulse golden vector");
}

/*****************************************************************************/
static void pseudoBlocks(int16_t cr[64], int16_t cb[64], int16_t ys[4][64])
{
	for (int i = 0; i < 64; i++)
	{
		cr[i] = (int16_t)(((i * 7) % 61) - 30);
		cb[i] = (int16_t)(((i * 13) % 91) - 45);
		for (int q = 0; q < 4; q++)
			ys[q][i] = (int16_t)(((i * 5 + q * 17) % 255) - 128);
	}
}

static void testYuv(void)
{
	int16_t cr[64], cb[64], ys[4][64];
	uint8_t rgb[16 * 16 * 3];
	pseudoBlocks(cr, cb, ys);
	Mdec_YuvToRgb24(cr, cb, ys[0], 0, 0, rgb);
	Mdec_YuvToRgb24(cr, cb, ys[1], 8, 0, rgb);
	Mdec_YuvToRgb24(cr, cb, ys[2], 0, 8, rgb);
	Mdec_YuvToRgb24(cr, cb, ys[3], 8, 8, rgb);

	/*	Spot-check byte order at (0,0): R,G,B ascending.  */
	{
		int Y = ys[0][0], crv = cr[0], cbv = cb[0];
		int rc = (91881 * crv) >> 16;
		int r = Y + rc;
		r = (r < -128 ? -128 : (r > 127 ? 127 : r)) + 128;
		check(rgb[0] == (uint8_t)r, "yuv: byte 0 is R");
		int bc = (116130 * cbv) >> 16;
		int b = Y + bc;
		b = (b < -128 ? -128 : (b > 127 ? 127 : b)) + 128;
		check(rgb[2] == (uint8_t)b, "yuv: byte 2 is B");
	}

	if (!g_golden)
		return;
	uint8_t golden[16 * 16 * 3];
	if (goldenRead(golden, sizeof(golden), "yuv mb"))
		check(memcmp(rgb, golden, sizeof(golden)) == 0, "yuv: macroblock golden");
}

/*****************************************************************************/
/*	fmv.cpp-style DecDCTout chaining: the callback re-enters DecDCTout for
	the next slice.  Assert flat stack via the trampoline, correct data,
	and exact cursor accounting over a 2-macroblock frame.  */

static u_long g_stream[1 + 8 * 6 + 16];		/* header + 6 blocks * small */
static uint8_t g_slice[MDEC_MB_BYTES_24BPP];
static uint8_t g_got[2 * MDEC_MB_BYTES_24BPP];
static int g_slices, g_depth, g_maxDepth, g_done;

extern "C" void mdecTestCallback(void)
{
	g_depth++;
	if (g_depth > g_maxDepth)
		g_maxDepth = g_depth;
	memcpy(g_got + g_slices * MDEC_MB_BYTES_24BPP, g_slice, MDEC_MB_BYTES_24BPP);
	g_slices++;
	if (g_slices < 2)
		DecDCTout((u_long *)g_slice, MDEC_MB_BYTES_24BPP / 4);
	else
		g_done = 1;
	g_depth--;
}

static void testPipeline(void)
{
	/*	Build a 2-macroblock stream: every block DC-only (qscale 1), all
		four Y blocks of an MB sharing one DC so each MB decodes to a flat
		color, distinct between the two MBs.  */
	uint16_t *hw = (uint16_t *)(g_stream + 1);
	int n = 0;
	for (int mb = 0; mb < 2; mb++)
		for (int b = 0; b < 6; b++)
		{
			unsigned dc = (b < 2) ? (unsigned)(8 + b) : (unsigned)(24 + mb * 16);
			hw[n++] = (uint16_t)((1u << 10) | dc);
			hw[n++] = MDEC_EOB;
		}
	if (n & 1)
		hw[n++] = MDEC_EOB;
	g_stream[0] = ((u_long)MDEC_MAGIC << 16) | (u_long)(n / 2);

	DecDCTReset(0);
	DecDCToutCallback(mdecTestCallback);
	DecDCTin(g_stream, 3);
	check(Mdec_FrameBytesForTest() == 2 * MDEC_MB_BYTES_24BPP,
		  "pipeline: 2 macroblocks decoded");

	g_slices = 0;
	g_depth = 0;
	g_maxDepth = 0;
	g_done = 0;
	DecDCTout((u_long *)g_slice, MDEC_MB_BYTES_24BPP / 4);
	check(g_done == 1, "pipeline: callback chain ran to completion");
	check(g_slices == 2, "pipeline: callback fired once per DecDCTout");
	check(g_maxDepth == 1, "pipeline: trampoline keeps the chain flat");

	/*	Each MB must be flat and the two MBs distinct.  */
	bool flatA = true, flatB = true;
	for (int i = 3; i < MDEC_MB_BYTES_24BPP; i += 3)
	{
		flatA = flatA && g_got[i] == g_got[0];
		flatB = flatB && g_got[MDEC_MB_BYTES_24BPP + i] == g_got[MDEC_MB_BYTES_24BPP];
	}
	check(flatA && flatB, "pipeline: DC-only macroblocks decode flat");
	check(g_got[0] != g_got[MDEC_MB_BYTES_24BPP],
		  "pipeline: distinct DCs give distinct macroblocks");

	/*	Reading past the frame zero-fills (log-once) and doesn't wedge.  */
	memset(g_slice, 0xAA, sizeof(g_slice));
	DecDCToutCallback(0);
	DecDCTout((u_long *)g_slice, MDEC_MB_BYTES_24BPP / 4);
	bool zeros = true;
	for (int i = 0; i < MDEC_MB_BYTES_24BPP; i++)
		zeros = zeros && g_slice[i] == 0;
	check(zeros, "pipeline: DecDCTout past the frame serves zeros");
}

/*****************************************************************************/
/*	DecDCTout's request comes from fmv.cpp as 24*height/2 words, height
	straight from the STR frame header.  Whatever it asks for, one call
	writes at most a 16-pixel 24bpp slice of 512 lines (24,576 bytes), and
	a size <= 0 writes nothing; the completion callback fires either way.
	The buffer ends at a PAGE_NOACCESS page, so an overrun faults.  */

static int g_outFired;

extern "C" void mdecCountCallback(void)
{
	g_outFired++;
}

static void testOutBounds(void)
{
	const int kCap = 16 * 3 * 512;
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	size_t page = si.dwPageSize;
	size_t span = (kCap + page - 1) / page * page;
	uint8_t *base = (uint8_t *)VirtualAlloc(NULL, span + page,
											MEM_RESERVE | MEM_COMMIT,
											PAGE_READWRITE);
	check(base != NULL, "out bounds: guarded buffer allocated");
	if (!base)
		return;
	DWORD old;
	VirtualProtect(base + span, page, PAGE_NOACCESS, &old);
	uint8_t *buf = base + span - kCap;		/* last byte abuts the guard */

	DecDCTReset(0);
	DecDCToutCallback(mdecCountCallback);
	DecDCTin(g_stream, 3);					/* testPipeline's 2 MBs */

	g_outFired = 0;
	memset(buf, 0xAA, kCap);
	DecDCTout((u_long *)buf, 0);
	DecDCTout((u_long *)buf, -8);
	check(g_outFired == 2, "out bounds: size <= 0 still completes");
	check(buf[0] == 0xAA && buf[kCap - 1] == 0xAA,
		  "out bounds: size <= 0 writes nothing");

	/*	a 1 MB-word request (a header height of ~87,000 lines)  */
	DecDCTout((u_long *)buf, 1 << 20);
	check(g_outFired == 3, "out bounds: a huge request completes");
	check(buf[kCap - 1] == 0, "out bounds: capped at one full slice");

	DecDCToutCallback(0);
	VirtualFree(base, 0, MEM_RELEASE);
}

/*****************************************************************************/
int main(void)
{
	g_golden = fopen("port/tests/mdec_golden.bin", "rb");
	if (g_golden)
	{
		char magic[4];
		if (fread(magic, 1, 4, g_golden) != 4 || memcmp(magic, "MDG1", 4))
		{
			printf("mdec_test: bad golden magic - golden layers SKIPPED\n");
			fclose(g_golden);
			g_golden = NULL;
		}
	}
	else
		printf("mdec_test: port/tests/mdec_golden.bin not found (run "
					"from the repo root) - golden layers SKIPPED\n");

	testIqTable();
	testRlDecode();
	testIdct();
	testYuv();
	testPipeline();
	testOutBounds();

	if (g_golden)
		fclose(g_golden);
	if (g_failures)
	{
		printf("mdec_test: %d FAILURES\n", g_failures);
		return 1;
	}
	printf("mdec_test: all passed\n");
	return 0;
}
