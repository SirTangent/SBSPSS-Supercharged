/*	VLC v3 decoder (M7): DecDCTvlcBuild3/DecDCTvlcSize3/DecDCTvlc3 in C,
	replacing the binary MIPS blob tools/vlc/lib/VLC_BIT.O (which only the
	PSX link uses; it kept its tables in PS1 scratchpad and used the GTE
	as a bit-shifter - none of which applies here).

	Input: one BS frame (header + Huffman bitstream), the concatenated
	2016-byte payloads of a video frame's STR sectors.  Output: the MDEC
	run-level stream mdec.cpp consumes (see mdec_internal.h) - word 0 is
	(0x3800 << 16) | declared-word-count straight from the BS header, and
	the emitted halfwords are padded with 0xFE00 up to that count, exactly
	the buffer the hardware path would have DMAed.

	Formats per psx-spx "CDROM File Video BS Compression":
	- header (8 bytes): u16 mdecSize/4 (rounded up to 0x20 words), u16
	  0x3800, u16 qscale, u16 version (3 for these movies; v2's raw
	  10-bit DC is supported for free).
	- bit order: 16-bit little-endian halfwords, MSB-first within each.
	- AC: the BS variant of the MPEG-1 table ("Huffman codes for AC
	  values BS v1/v2/v3"), decoded by leading-zero count; the trailing s
	  bit negates the 10-bit level; escape 000001 carries a raw 16-bit
	  MDEC halfword.
	- DC v3: MPEG-style differential with separate Cr/Cb and Y size
	  tables ("BS Compression DC Values" - DC v3), THREE predictors (Cr,
	  Cb, one shared Y), diff scaled by 4, wrapped in 10 bits (the
	  "newer decoder" rule); ten 1-bits in place of a Cr DC = end of
	  frame.  v2: raw 10-bit DC, +0x1FF in place of Cr = end.

	DecDCTvlcBuild3's table argument is deliberately ignored (fmv.cpp
	hands a heap DECDCTTAB but the real vlc3 reads scratchpad, and the
	vlc3 call site passes no table at all) - tables live here, statically,
	which also keeps PORT_Scratchpad's FilePosList untouched.
	DecDCTvlcSize3 is recorded but never triggers a partial decode:
	fmv.cpp never resumes one (its retry loop is commented out).
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <sys/types.h>

#include "stub_log.h"
#include "mdec/mdec_internal.h"
#include "cd/str_stream.h"

namespace
{

int g_breakSize;						/* DecDCTvlcSize3, recorded only */

/*	AC code value tables, by leading-zero class (psx-spx lists).  */
const uint16_t AC_LZ1_X1[2] = { 0x0002, 0x0801 };
const uint16_t AC_LZ2_X1[2] = { 0x1001, 0x0C01 };
const uint16_t AC_LZ2_X3[8] =
{
	0x3401, 0x0006, 0x3001, 0x2C01, 0x0C02, 0x0403, 0x0005, 0x2801,
};
const uint16_t AC_LZ3_X2[4] = { 0x1C01, 0x1801, 0x0402, 0x1401 };
const uint16_t AC_LZ4_X2[4] = { 0x0802, 0x2401, 0x0004, 0x2001 };
const uint16_t AC_LZ6_X3[8] =
{
	0x4001, 0x1402, 0x0007, 0x0803, 0x0404, 0x3C01, 0x3801, 0x1002,
};
const uint16_t AC_LZ7_X4[16] =
{
	0x000B, 0x2002, 0x1003, 0x000A, 0x0804, 0x1C02, 0x5401, 0x5001,
	0x0009, 0x4C01, 0x4801, 0x0405, 0x0C03, 0x0008, 0x1802, 0x4401,
};
const uint16_t AC_LZ8_X4[16] =
{
	0x2802, 0x2402, 0x1403, 0x0C04, 0x0805, 0x0407, 0x0406, 0x000F,
	0x000E, 0x000D, 0x000C, 0x6801, 0x6401, 0x6001, 0x5C01, 0x5801,
};
const uint16_t AC_LZ9_X4[16] =
{
	0x001F, 0x001E, 0x001D, 0x001C, 0x001B, 0x001A, 0x0019, 0x0018,
	0x0017, 0x0016, 0x0015, 0x0014, 0x0013, 0x0012, 0x0011, 0x0010,
};
const uint16_t AC_LZ10_X4[16] =
{
	0x0028, 0x0027, 0x0026, 0x0025, 0x0024, 0x0023, 0x0022, 0x0021,
	0x0020, 0x040E, 0x040D, 0x040C, 0x040B, 0x040A, 0x0409, 0x0408,
};
const uint16_t AC_LZ11_X4[16] =
{
	0x0412, 0x0411, 0x0410, 0x040F, 0x1803, 0x4002, 0x3C02, 0x3802,
	0x3402, 0x3002, 0x2C02, 0x7C01, 0x7801, 0x7401, 0x7001, 0x6C01,
};

/*	The bit reader's bound.  A frame handed out by StGetNext ends where
	its ring region ends (StrStream_FrameEnd) - NOT 64KB from the frame
	start, which for a frame late in the game's 64KB StSetRing buffer
	would let a stream with no end code read far past the ring.  A buffer
	the stream engine did not hand out (the unit tests' hand-built frames)
	falls back to one whole ring's worth, 32768 halfwords.  */
const unsigned kMaxBitstreamHalfwords = 32768;

/*	MSB-first bit reader over little-endian halfwords.  Reads past `end`
	yield zero bits and raise `overrun` rather than walking memory: the
	bitstream length is not passed in, and a truncated frame must end the
	decode instead of running off the ring.  Zeros also terminate every
	code path on their own (an all-zero prefix is the unused/invalid code
	in both the AC and DC tables).  */
struct Bits
{
	const uint16_t	*p;
	const uint16_t	*end;
	uint32_t		acc;
	int				n;
	int				overrun;
};

void bitsInit(Bits *b, const uint16_t *p, const uint16_t *end)
{
	b->p = p;
	b->end = end;
	b->acc = 0;
	b->n = 0;
	b->overrun = 0;
}

inline unsigned bitsPeek(Bits *b, int k)
{
	while (b->n < k)
	{
		uint16_t hw = 0;
		if (b->p < b->end)
			hw = *b->p++;
		else
			b->overrun = 1;
		b->acc = (b->acc << 16) | hw;
		b->n += 16;
	}
	return (b->acc >> (b->n - k)) & ((1u << k) - 1u);
}

inline void bitsSkip(Bits *b, int k)
{
	b->n -= k;
}

inline unsigned bitsGet(Bits *b, int k)
{
	unsigned v = bitsPeek(b, k);
	bitsSkip(b, k);
	return v;
}

/*	Negate the signed-10bit level of an MDEC halfword.  */
inline uint16_t negLevel(uint16_t v)
{
	return (uint16_t)((v & 0xFC00u) | ((0x400u - (v & 0x3FFu)) & 0x3FFu));
}

/*	One AC code -> MDEC halfword; MDEC_EOB for EOB; -1 on an invalid code
	(the unused all-zero prefix).  */
int acDecode(Bits *b)
{
	int lz = 0;
	while (bitsGet(b, 1) == 0)
		if (++lz > 11)
			return -1;
	uint16_t v;
	switch (lz)
	{
	case 0:
		if (bitsGet(b, 1) == 0)
			return MDEC_EOB;			/* "10" */
		v = 0x0001;						/* "11"+s */
		break;
	case 1:
		if (bitsGet(b, 1))
			v = 0x0401;					/* "011"+s */
		else
			v = AC_LZ1_X1[bitsGet(b, 1)];
		break;
	case 2:
		if (bitsGet(b, 1))
			v = AC_LZ2_X1[bitsGet(b, 1)];
		else if (bitsGet(b, 1))
			v = 0x0003;					/* "00101"+s */
		else
			v = AC_LZ2_X3[bitsGet(b, 3)];
		break;
	case 3:
		v = AC_LZ3_X2[bitsGet(b, 2)];
		break;
	case 4:
		v = AC_LZ4_X2[bitsGet(b, 2)];
		break;
	case 5:
		return (int)bitsGet(b, 16);		/* escape: raw MDEC halfword */
	case 6:
		v = AC_LZ6_X3[bitsGet(b, 3)];
		break;
	case 7:
		v = AC_LZ7_X4[bitsGet(b, 4)];
		break;
	case 8:
		v = AC_LZ8_X4[bitsGet(b, 4)];
		break;
	case 9:
		v = AC_LZ9_X4[bitsGet(b, 4)];
		break;
	case 10:
		v = AC_LZ10_X4[bitsGet(b, 4)];
		break;
	default:
		v = AC_LZ11_X4[bitsGet(b, 4)];
		break;
	}
	return bitsGet(b, 1) ? negLevel(v) : v;
}

/*	MPEG-style DC differential from k size bits (first is the sign).  */
int dcDiff(Bits *b, int k)
{
	if (k == 0)
		return 0;
	unsigned v = bitsGet(b, k);
	if (v & (1u << (k - 1)))
		return (int)v;					/* positive: 2^(k-1)..2^k-1 */
	return (int)v - (int)((1u << k) - 1u);
}

/*	DC size class, Cr/Cb table: 00=0, 01=1, 10=2, 110=3, 1110=4, ...,
	11111110=8.  Returns -1 on the unused >=9-ones prefixes.  */
int dcSizeChroma(Bits *b)
{
	int ones = 0;
	while (bitsGet(b, 1) == 1)
		if (++ones > 7)
			return -1;
	if (ones == 0)
		return bitsGet(b, 1);			/* "00"->0, "01"->1 */
	return ones + 1;					/* "10"->2, "110"->3, ... */
}

/*	DC size class, Y table: 100=0, 00=1, 01=2, 101=3, 110=4, 1110=5, ...,
	1111110=8.  */
int dcSizeLuma(Bits *b)
{
	if (bitsGet(b, 1) == 0)
		return bitsGet(b, 1) ? 2 : 1;	/* "00"->1, "01"->2 */
	int ones = 1;
	while (bitsGet(b, 1) == 1)
		if (++ones > 6)
			return -1;
	if (ones == 1)
		return bitsGet(b, 1) ? 3 : 0;	/* "100"->0, "101"->3 */
	return ones + 2;					/* "110"->4, "1110"->5, ... */
}

}	/* namespace */

/*****************************************************************************/
extern "C" void DecDCTvlcBuild3(unsigned short *table)
{
	(void)table;						/* tables are internal statics */
}

extern "C" int DecDCTvlcSize3(int breaksize)
{
	int old = g_breakSize;
	g_breakSize = breaksize;			/* recorded; partial decode never
										   happens (fmv.cpp never resumes) */
	return old;
}

extern "C" int DecDCTvlc3(unsigned long *bs, unsigned long *buf)
{
	const uint16_t *hdr = (const uint16_t *)bs;
	unsigned declWords = hdr[0];
	unsigned qscale = hdr[2] & 0x3F;
	unsigned version = hdr[3];

	if (hdr[1] != MDEC_MAGIC)
		PSYQ_LOG_ONCE_KEYED(0, "[vlc3] BS header magic %04X (want 3800) - "
							"decoding anyway\n", hdr[1]);
	if (version != 3 && version != 2)
	{
		PSYQ_LOG_ONCE_KEYED(version, "[vlc3] BS version %u unsupported "
							"(2/3 only) - emitting empty frame\n", version);
		buf[0] = ((unsigned long)MDEC_MAGIC << 16);
		return 0;
	}

	if (declWords > MDEC_MAX_STREAM_WORDS)
	{
		/*	The size is disc data; the output buffer is the game's prim
			pool.  Clamp before it is used as a write bound OR handed on
			in buf[0] (mdec.cpp derives its read limit from it).  */
		PSYQ_LOG_ONCE_KEYED(1, "[vlc3] BS declares %u words (max %u) - "
							"clamped\n", declWords, MDEC_MAX_STREAM_WORDS);
		declWords = MDEC_MAX_STREAM_WORDS;
	}

	uint16_t *out = (uint16_t *)(buf + 1);
	unsigned outMax = declWords * 2;	/* halfwords, the hardware buffer */
	unsigned n = 0;

	const uint16_t *end = (const uint16_t *)StrStream_FrameEnd(bs);
	if (!end)
		end = hdr + 4 + kMaxBitstreamHalfwords;

	Bits b;
	bitsInit(&b, hdr + 4, end);
	int predCr = 0, predCb = 0, predY = 0;
	int blockIdx = 0;					/* 0=Cr 1=Cb 2..5=Y1..Y4 */
	int bad = 0;

	for (;;)
	{
		if (b.overrun)					/* ran off the end: corrupt stream */
		{
			bad = 1;
			break;
		}

		/*	End-of-frame codes stand in place of the next Cr DC.  */
		if (blockIdx == 0)
		{
			if (version == 3 && bitsPeek(&b, 10) == 0x3FF)
				break;
			if (version == 2 && bitsPeek(&b, 10) == 0x1FF)
				break;
		}

		int dc;
		if (version == 2)
		{
			unsigned v = bitsGet(&b, 10);
			dc = (int)((v ^ 0x200u) - 0x200u);
		}
		else
		{
			int size = (blockIdx < 2) ? dcSizeChroma(&b) : dcSizeLuma(&b);
			if (size < 0)
			{
				bad = 2;
				break;
			}
			int *pred = (blockIdx == 0) ? &predCr
					  : (blockIdx == 1) ? &predCb : &predY;
			int v = *pred + dcDiff(&b, size) * 4;
			v = (int)(((unsigned)v & 0x3FFu) ^ 0x200u) - 0x200;	/* 10bit wrap */
			*pred = v;
			dc = v;
		}

		if (n + 2 > outMax)				/* need room for DC + eventual EOB */
		{
			bad = 3;
			break;
		}
		out[n++] = (uint16_t)((qscale << 10) | ((unsigned)dc & 0x3FFu));

		for (;;)
		{
			int v = acDecode(&b);
			if (v < 0)
			{
				bad = 4;
				break;
			}
			if (n >= outMax)
			{
				bad = 3;
				break;
			}
			out[n++] = (uint16_t)v;
			if (v == (int)MDEC_EOB)
				break;
		}
		if (bad)
			break;
		blockIdx = (blockIdx + 1) % 6;
	}

	if (bad)
		PSYQ_LOG_ONCE_KEYED(bad, "[vlc3] bad bitstream (reason %d, block "
							"phase %d) - frame truncated\n", bad, blockIdx);

	while (n < outMax)
		out[n++] = MDEC_EOB;			/* pad to the declared size */
	buf[0] = ((unsigned long)MDEC_MAGIC << 16) | declWords;
	return 0;
}
