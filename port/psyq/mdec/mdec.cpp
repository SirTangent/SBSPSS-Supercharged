/*	libpress MDEC decoder (M7): DecDCTReset/in/out/outCallback in C,
	replacing the hardware MDEC + DMA0/DMA1 path for FMV playback.

	Model.  DecDCTin decodes the WHOLE frame synchronously (run-level
	stream -> dequant -> IDCT -> 24bpp RGB) into an internal linear stream
	of 16x16 macroblocks; DecDCTout copies successive slices of that
	stream and fires the out-callback.  fmv.cpp's callback re-enters
	DecDCTout for the next 16px column slice, so the callback dispatch is
	a trampoline (pending flag + flat loop), never 20-deep recursion.
	Because macroblocks are decoded in bitstream order (16px column
	strips) and each slice is exactly one macroblock wide, the linear
	stream IS the slice layout - no reordering anywhere.

	Everything follows psx-spx "Macroblock Decoder (MDEC)" - MDEC
	Decompression: rl_decode_block, the real_idct_core (not the fast one:
	bit-stable output is what makes frame CRCs and emulator comparisons
	meaningful), and yuv_to_rgb.  Two knowingly inexact spots, both noted
	inline: the IDCT rounding shifts (psx-spx: "or so?") and the
	yuv_to_rgb fixed-point width (psx-spx: "unknown").  The regression
	contract is committed CRCs + tolerance-vs-ffmpeg, per the M7 plan.

	Only mode 3 (24bpp, what IS_RGB24 fmv.cpp sends) is implemented; any
	other mode logs once and yields an empty frame (black, loud in logs).
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <sys/types.h>
#include <libpress.h>

#include "stub_log.h"
#include "mdec/mdec_internal.h"

namespace
{

/*	zigzag[raster] = stream index (psx-spx zigzag table).  */
const uint8_t ZIGZAG[64] =
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

/*	Sony's default quantization matrix (raster order): the MPEG-1 intra
	matrix with [0]=2, used for BOTH luma and chroma - what DecDCTReset(0)
	programs on hardware (jpsxdec PlayStation1_STR_format.txt 2.3.3).  */
const uint8_t DEFAULT_IQ[64] =
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

/*	MDEC(3) scale table (raster order, S1.14) - the standard JPEG cosine
	constants every game uses.  real_idct_core reads scaletable[x + z*8].  */
const int16_t SCALETABLE[64] =
{
	(int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82,
	(int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82, (int16_t)0x5A82,
	(int16_t)0x7D8A, (int16_t)0x6A6D, (int16_t)0x471C, (int16_t)0x18F8,
	(int16_t)0xE707, (int16_t)0xB8E3, (int16_t)0x9592, (int16_t)0x8275,
	(int16_t)0x7641, (int16_t)0x30FB, (int16_t)0xCF04, (int16_t)0x89BE,
	(int16_t)0x89BE, (int16_t)0xCF04, (int16_t)0x30FB, (int16_t)0x7641,
	(int16_t)0x6A6D, (int16_t)0xE707, (int16_t)0x8275, (int16_t)0xB8E3,
	(int16_t)0x471C, (int16_t)0x7D8A, (int16_t)0x18F8, (int16_t)0x9592,
	(int16_t)0x5A82, (int16_t)0xA57D, (int16_t)0xA57D, (int16_t)0x5A82,
	(int16_t)0x5A82, (int16_t)0xA57D, (int16_t)0xA57D, (int16_t)0x5A82,
	(int16_t)0x471C, (int16_t)0x8275, (int16_t)0x18F8, (int16_t)0x6A6D,
	(int16_t)0x9592, (int16_t)0xE707, (int16_t)0x7D8A, (int16_t)0xB8E3,
	(int16_t)0x30FB, (int16_t)0x89BE, (int16_t)0x7641, (int16_t)0xCF04,
	(int16_t)0xCF04, (int16_t)0x7641, (int16_t)0x89BE, (int16_t)0x30FB,
	(int16_t)0x18F8, (int16_t)0xB8E3, (int16_t)0x6A6D, (int16_t)0x8275,
	(int16_t)0x7D8A, (int16_t)0x9592, (int16_t)0x471C, (int16_t)0xE707,
};

uint8_t	g_zagzig[64];					/* stream index -> raster index */
uint8_t	g_iqY[64], g_iqC[64];			/* quant tables, STREAM order */
int		g_tablesReady;

uint8_t	g_frame[MDEC_MAX_MB * MDEC_MB_BYTES_24BPP];
int		g_frameBytes;					/* decoded bytes in g_frame */
int		g_cursor;						/* DecDCTout read position */

void	(*g_outCallback)(void);
int		g_outPending;
int		g_inDispatch;

void initTables(void)
{
	if (g_tablesReady)
		return;
	for (int i = 0; i < 64; i++)
		g_zagzig[ZIGZAG[i]] = (uint8_t)i;
	for (int k = 0; k < 64; k++)
		g_iqY[k] = g_iqC[k] = DEFAULT_IQ[g_zagzig[k]];
	g_tablesReady = 1;
}

inline int sign10(unsigned v)
{
	return (int)((v ^ 0x200u) - 0x200u);
}

inline int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

}	/* namespace */

/*****************************************************************************/
/*	rl_decode_block per psx-spx: DC without qscale/8, AC as
	(level*qt[k]*qscale+4)/8, both saturated to signed 11 bits; qscale==0
	bypasses the table (level*2, linear order, no zigzag).  */
extern "C" int Mdec_RlDecodeBlock(int16_t blk[64], const uint16_t **srcp,
								  const uint16_t *end, const uint8_t *qt)
{
	const uint16_t *src = *srcp;
	unsigned n;

	do
	{
		if (src >= end)
		{
			*srcp = src;
			return -1;					/* only padding remained */
		}
		n = *src++;
	} while (n == MDEC_EOB);

	memset(blk, 0, 64 * sizeof(int16_t));

	int qscale = (n >> 10) & 0x3F;
	int k = 0;
	int32_t val = sign10(n & 0x3FF) * qt[0];
	for (;;)
	{
		if (qscale == 0)
			val = sign10(n & 0x3FF) * 2;
		val = clampi(val, -0x400, 0x3FF);
		if (qscale > 0)
			blk[g_zagzig[k]] = (int16_t)val;
		else
			blk[k] = (int16_t)val;
		if (src >= end)
			break;						/* truncated stream: implicit EOB */
		n = *src++;
		k += ((n >> 10) & 0x3F) + 1;
		if (k > 63)
			break;						/* EOB (run 63 overshoots) or done */
		val = (sign10(n & 0x3FF) * qt[k] * qscale + 4) / 8;
	}
	*srcp = src;
	return 0;
}

/*****************************************************************************/
/*	real_idct_core: dst[x+y*8] = sum(src[y+z*8] * scaletable[x+z*8]) over z,
	two passes with src/dst swapped.  psx-spx says the hardware keeps the
	upper 13 bits of the scale entries (>>3 here) and rounds the sum with
	"(sum+0FFFh)/2000h ... or so"; we use arithmetic shifts for both - the
	one spot the hardware is documented as not-perfectly-known.  */
extern "C" void Mdec_Idct(int16_t blk[64])
{
	int32_t a[64], b[64];
	const int32_t *src;
	int32_t *dst;

	for (int i = 0; i < 64; i++)
		a[i] = blk[i];
	for (int pass = 0; pass < 2; pass++)
	{
		src = pass ? b : a;
		dst = pass ? a : b;
		for (int x = 0; x < 8; x++)
		{
			for (int y = 0; y < 8; y++)
			{
				int64_t sum = 0;
				for (int z = 0; z < 8; z++)
					sum += (int64_t)src[y + z * 8] * (SCALETABLE[x + z * 8] >> 3);
				dst[x + y * 8] = (int32_t)((sum + 0xFFF) >> 13);
			}
		}
	}
	for (int i = 0; i < 64; i++)
		blk[i] = (int16_t)a[i];
}

/*****************************************************************************/
/*	yuv_to_rgb, 24bpp unsigned: chroma sampled 2x2-nearest, contributions
	R=1.402*Cr, G=-0.3437*Cb-0.7143*Cr, B=1.772*Cb added to Y and clamped
	to -128..127, then +128.  psx-spx leaves the exact fixed-point width
	unknown; 16.16 truncating is our pick (validated by tolerance tests).  */
extern "C" void Mdec_YuvToRgb24(const int16_t crBlk[64], const int16_t cbBlk[64],
								const int16_t yBlk[64], int xx, int yy,
								uint8_t rgb[16 * 16 * 3])
{
	for (int y = 0; y < 8; y++)
	{
		for (int x = 0; x < 8; x++)
		{
			int cr = crBlk[((x + xx) >> 1) + ((y + yy) >> 1) * 8];
			int cb = cbBlk[((x + xx) >> 1) + ((y + yy) >> 1) * 8];
			int rc = (91881 * cr) >> 16;
			int gc = (-22525 * cb - 46816 * cr) >> 16;
			int bc = (116130 * cb) >> 16;
			int Y = yBlk[x + y * 8];
			uint8_t *p = rgb + ((y + yy) * 16 + (x + xx)) * 3;
			p[0] = (uint8_t)(clampi(Y + rc, -128, 127) + 128);
			p[1] = (uint8_t)(clampi(Y + gc, -128, 127) + 128);
			p[2] = (uint8_t)(clampi(Y + bc, -128, 127) + 128);
		}
	}
}

/*****************************************************************************/
/*	Public libpress surface.  */

extern "C" void DecDCTReset(int mode)
{
	(void)mode;							/* game only ever passes 0 */
	g_tablesReady = 0;					/* reload default IQ tables */
	initTables();
	g_frameBytes = 0;
	g_cursor = 0;
	g_outPending = 0;
}

extern "C" int DecDCToutCallback(void (*func)(void))
{
	g_outCallback = func;
	return 0;
}

extern "C" void DecDCTin(u_long *buf, int mode)
{
	initTables();
	g_frameBytes = 0;
	g_cursor = 0;

	if (mode != 3)
	{
		PSYQ_LOG_ONCE_KEYED(mode, "[mdec] DecDCTin mode %d unimplemented "
							"(only 3 = 24bpp) - serving black\n", mode);
		return;
	}

	uint32_t w0 = (uint32_t)buf[0];
	if ((w0 >> 16) != MDEC_MAGIC)
		PSYQ_LOG_ONCE_KEYED(0, "[mdec] DecDCTin header %08lX lacks 3800 "
							"magic - decoding anyway\n", (unsigned long)w0);
	uint32_t words = w0 & 0xFFFF;
	if (words > MDEC_MAX_STREAM_WORDS)
	{
		PSYQ_LOG_ONCE_KEYED(4, "[mdec] stream declares %lu words (max %d) - "
							"clamped\n", (unsigned long)words,
							MDEC_MAX_STREAM_WORDS);
		words = MDEC_MAX_STREAM_WORDS;
	}
	const uint16_t *src = (const uint16_t *)(buf + 1);
	const uint16_t *end = src + words * 2;

	int16_t blk[6][64];
	for (;;)
	{
		if (g_frameBytes + MDEC_MB_BYTES_24BPP > (int)sizeof(g_frame))
		{
			PSYQ_LOG_ONCE_KEYED(1, "[mdec] frame exceeds %d macroblocks - "
								"truncating\n", MDEC_MAX_MB);
			break;
		}
		int got = 0;
		for (int b = 0; b < 6; b++)
		{
			if (Mdec_RlDecodeBlock(blk[b], &src, end,
								   b < 2 ? g_iqC : g_iqY) < 0)
				break;
			Mdec_Idct(blk[b]);
			got++;
		}
		if (got == 0)
			break;						/* clean end of stream */
		if (got < 6)
		{
			PSYQ_LOG_ONCE_KEYED(2, "[mdec] stream ended mid-macroblock "
								"(%d/6 blocks) - dropping it\n", got);
			break;
		}
		uint8_t *mb = g_frame + g_frameBytes;
		Mdec_YuvToRgb24(blk[0], blk[1], blk[2], 0, 0, mb);
		Mdec_YuvToRgb24(blk[0], blk[1], blk[3], 8, 0, mb);
		Mdec_YuvToRgb24(blk[0], blk[1], blk[4], 0, 8, mb);
		Mdec_YuvToRgb24(blk[0], blk[1], blk[5], 8, 8, mb);
		g_frameBytes += MDEC_MB_BYTES_24BPP;
	}
}

/*	The most one DecDCTout can ask for: a 16-pixel column slice of 24bpp
	at the full 512 VRAM lines.  fmv.cpp sizes its request from the STR
	header's height (disc data) into a buffer sized for 240 lines, and the
	real MDEC's DMA would stall rather than write what it never decoded -
	so an absurd height must not become an absurd write (issue #60).  */
#define MDEC_MAX_OUT_BYTES	(16 * 3 * 512)

/*	Copy the next slice of the decoded stream, then run the completion
	callback through the trampoline: the callback's own DecDCTout call
	(fmv.cpp strCallback) only copies and re-arms the pending flag, so the
	whole 20-slice chain runs iteratively at fixed stack depth.  */
extern "C" void DecDCTout(u_long *buf, int size)
{
	uint8_t *dst = (uint8_t *)buf;
	int bytes;
	if (size <= 0)
		bytes = 0;
	else if (size > MDEC_MAX_OUT_BYTES / 4)
	{
		PSYQ_LOG_ONCE_KEYED(5, "[mdec] DecDCTout of %d words - capped at "
							"%d bytes\n", size, MDEC_MAX_OUT_BYTES);
		bytes = MDEC_MAX_OUT_BYTES;
	}
	else
		bytes = size * 4;
	int avail = g_frameBytes - g_cursor;
	int n = bytes < avail ? bytes : (avail > 0 ? avail : 0);

	memcpy(dst, g_frame + g_cursor, (size_t)n);
	if (n < bytes)
	{
		memset(dst + n, 0, (size_t)(bytes - n));
		PSYQ_LOG_ONCE_KEYED(3, "[mdec] DecDCTout past the decoded frame "
							"(%d of %d bytes) - zero fill\n", n, bytes);
	}
	g_cursor += n;

	g_outPending = 1;
	if (!g_inDispatch)
	{
		g_inDispatch = 1;
		while (g_outPending && g_outCallback)
		{
			g_outPending = 0;
			g_outCallback();
		}
		g_outPending = 0;
		g_inDispatch = 0;
	}
}

/*****************************************************************************/
/*	Test hooks.  */

extern "C" const uint8_t *Mdec_IqZigzagYForTest(void)
{
	initTables();
	return g_iqY;
}

extern "C" int Mdec_FrameBytesForTest(void)
{
	return g_frameBytes;
}
