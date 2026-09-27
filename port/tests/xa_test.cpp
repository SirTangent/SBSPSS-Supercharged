/*	Unit tests for the M6 XA path: the XA-ADPCM sector decoder
	(port/psyq/cd/xa_adpcm.cpp), the streaming engine (xa_stream.cpp)
	driven through the libcd surface exactly as cdxa.cpp drives it over a
	synthetic TRACK1.IXA, and the SPU CD-input mix slice.

	Decoder oracles, three layers:
	  1. hand-checked basics: an all-zero sector decodes to silence;
	     filter-0 blocks are exactly the sign-extended nibble >> shift.
	  2. cross-oracle vs the golden-tested SPU ADPCM decoder: the per-sample
	     arithmetic is the same recurrence, only the bit layout differs - so
	     an XA group carrying the same nibble stream and params as a chain
	     of SPU blocks must decode identically, history and all.
	  3. real data: 4 consecutive channel-1 sectors extracted from
	     Track1.Ixa (xa_fixture_sectors.bin) against ffmpeg's adpcm_xa
	     decode (xa_fixture_golden.pcm).  Regenerate both from the repo
	     root with `py port/tests/make_xa_fixture.py` (wraps the sectors as
	     RIFF CDXA, ffmpeg -i fixture.xa -f s16le xa_fixture_golden.pcm).
	     Layer 3 needs the repo root as cwd; it skips with a note otherwise.
*/
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <direct.h>

#include <sys/types.h>
#include <libcd.h>

#include "cd/xa_adpcm.h"
#include "cd/xa_stream.h"
#include "spu/spu_core.h"

extern "C" void Port_CdRebuildDirForTest(void);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*	build one 128-byte XA sound group: params[8] and, per block, 28 nibbles  */
static void buildGroup(uint8_t *g, const uint8_t params[8],
					   const uint8_t nibbles[8][28])
{
	memset(g, 0, 128);
	for (int b = 0; b < 8; b++)
	{
		g[b < 4 ? b : b + 4] = params[b];		/* + the duplicate copies */
		g[(b < 4 ? b : b + 4) + 4] = params[b];
		for (int i = 0; i < 28; i++)
			g[16 + i * 4 + (b >> 1)] |= (uint8_t)(nibbles[b][i] << ((b & 1) * 4));
	}
}

/*****************************************************************************/
/*	Stream engine over a synthetic disc.

	xa_test_tmp/TRACK1.IXA: 3 groups of 32 raw 2336-byte sectors, laid out
	like the real disc (sector%32 == channel):
	  slot 0          all-zero sector (data -> delivered, Track==0)
	  slot 1          groups 0-1: chan-1 audio, every sample 0x7000
	                  group  2:   chan-1 TERMINATOR (ID 352, Track 1)
	  slot 2          group  1:   chan-2 terminator (delivered, ignored)
	                  groups 0,2: chan-2 audio, every sample -0x7000
	  slots 3-31      audio for chan==slot (the filter must discard them)

	Driven through CdReadyCallback + CdlSetmode/Setfilter/ReadS + pumped
	Port_CdVblank(60), i.e. exactly cdxa.cpp's shape, with the callback
	mimicking XACDReadyCallback: CdGetSector(8 words), test word 3, and
	CdlPause when its own channel's terminator arrives (mid-burst).  */

static int		g_cbDataReady;
static int		g_cbTrack0;
static int		g_cbLastTermTrack = -1;
static uint32_t	g_cbTermWord;
static int		g_cbWatchChan = 1;

static void testReadyCb(u_char intr, u_char *result)
{
	(void)result;
	if (intr != CdlDataReady)
		return;
	uint32_t buf[8];
	CdGetSector(buf, 8);
	g_cbDataReady++;

	unsigned id    = buf[3] & 0xFFFF;
	unsigned track = ((buf[3] >> 16) >> 10) & 31;
	if (id == 0 && track == 0)
		g_cbTrack0++;
	if (id == 352)
	{
		g_cbLastTermTrack = (int)track;
		if ((int)track == g_cbWatchChan)
		{
			g_cbTermWord = buf[3];
			CdControlF(CdlPause, 0);	/* the game's end-of-stream move */
		}
	}
}

static void writeAudioSector(FILE *f, int chan, uint8_t nibble)
{
	uint8_t sec[2336];
	memset(sec, 0, sizeof(sec));
	sec[0] = 1; sec[1] = (uint8_t)chan; sec[2] = 0x64; sec[3] = 0x04;
	memcpy(sec + 4, sec, 4);			/* duplicated subheader */
	/*	params 0 (filter 0, shift 0), data = nibble in both positions of
		every byte -> every sample of every block is nibble<<12  */
	for (int grp = 0; grp < 18; grp++)
		memset(sec + 8 + grp * 128 + 16, nibble | (nibble << 4), 112);
	fwrite(sec, 1, sizeof(sec), f);
}

static void writeTerminatorSector(FILE *f, int chan)
{
	uint8_t sec[2336];
	memset(sec, 0, sizeof(sec));
	sec[0] = 0; sec[1] = (uint8_t)chan; sec[2] = 0x48; sec[3] = 0;
	memcpy(sec + 4, sec, 4);
	sec[8] = 0x60; sec[9] = 0x01;		/* ID 352 */
	unsigned track = 0x8000u | ((unsigned)chan << 10);
	sec[10] = (uint8_t)track;
	sec[11] = (uint8_t)(track >> 8);
	fwrite(sec, 1, sizeof(sec), f);
}

static void writeZeroSector(FILE *f)
{
	uint8_t sec[2336];
	memset(sec, 0, sizeof(sec));
	fwrite(sec, 1, sizeof(sec), f);
}

static int readSAt(long lba)
{
	CdlLOC loc;
	CdIntToPos((int)lba, &loc);
	return CdControlF(CdlReadS, (u_char *)&loc);
}

static void streamEngineTests(void)
{
	_mkdir("xa_test_tmp");
	FILE *f = fopen("xa_test_tmp\\TRACK1.IXA", "wb");
	check(f != NULL, "synthetic disc created");
	if (!f)
		return;
	for (int s = 0; s < 96; s++)
	{
		int g = s / 32, m = s % 32;
		if (m == 0)
			writeZeroSector(f);
		else if (m == 1)
			g < 2 ? writeAudioSector(f, 1, 0x7) : writeTerminatorSector(f, 1);
		else if (m == 2)
			g == 1 ? writeTerminatorSector(f, 2) : writeAudioSector(f, 2, 0x9);
		else
			writeAudioSector(f, m, 0x5);
	}
	fclose(f);

	_putenv("SBSP_DATA_DIR=xa_test_tmp");
	Port_CdRebuildDirForTest();
	XaStream_ResetForTest();
	Spu_CdInClear();

	CdlFILE cf;
	check(CdSearchFile(&cf, (char *)"\\TRACK1.IXA;1") != NULL, "track in the virtual dir");
	long track1 = CdPosToInt(&cf.pos);
	check(cf.size == 96L * 2336, "virtual dir sees 96 raw sectors");

	CdReadyCallback((CdlCB)testReadyCb);
	u_char mode = 0xE8;					/* Speed|RT|SF|Size1, as cdxa.cpp */
	CdControlB(CdlSetmode, &mode, 0);
	u_char filt[4] = { 1, 1, 0, 0 };	/* file 1, chan 1 */
	CdControlF(CdlSetfilter, filt);

	/*	-------- cadence: 2.5 sectors per 60Hz vblank  */
	g_cbDataReady = g_cbTrack0 = 0;
	check(readSAt(track1) == 1, "CdlReadS accepted");
	for (int v = 0; v < 4; v++)
		Port_CdVblank(60);				/* 10 sectors: 0..9 */
	check(Spu_CdInCountForTest() == XA_SECTOR_SAMPLES,
		  "4 vblanks: exactly one audio sector decoded (2.5 sectors/vblank)");
	check(g_cbDataReady == 1 && g_cbTrack0 == 1,
		  "4 vblanks: one Track==0 data delivery (slot 0)");
	for (int v = 4; v < 13; v++)
		Port_CdVblank(60);				/* 32 sectors: 0..31 */
	check(Spu_CdInCountForTest() == XA_SECTOR_SAMPLES,
		  "13 vblanks: still one audio sector (31.5 < 33 delivered)");
	for (int v = 13; v < 14; v++)
		Port_CdVblank(60);				/* 35 sectors: 0..34 */
	check(Spu_CdInCountForTest() == 2 * XA_SECTOR_SAMPLES,
		  "14 vblanks: the group-1 chan-1 sector landed");
	check(g_cbLastTermTrack == 2, "chan-2 terminator delivered and seen");
	check(g_cbDataReady == 3, "zero sectors + other-channel terminator delivered");

	/*	-------- end of stream: own terminator pauses mid-burst  */
	for (int v = 14; v < 30; v++)
		Port_CdVblank(60);
	check(g_cbTermWord == 0x84000160u,
		  "own terminator word3 = ID 352 + chan 1 in Track bits 10-14");
	check(g_cbDataReady == 5, "delivery stopped at the pause (3 zeros + 2 terms)");
	check(Spu_CdInCountForTest() == 0, "pause cleared the CD-input ring");
	int before = g_cbDataReady;
	for (int v = 0; v < 10; v++)
		Port_CdVblank(60);
	check(g_cbDataReady == before, "paused stream delivers nothing");

	/*	-------- Resume shape: ReadS at a later position  */
	g_cbTrack0 = 0;
	check(readSAt(track1 + 32) == 1, "resume ReadS accepted");
	for (int v = 0; v < 2; v++)
		Port_CdVblank(60);				/* 5 sectors: 32..36 */
	check(Spu_CdInCountForTest() == XA_SECTOR_SAMPLES,
		  "resume: group-1 audio decoded from the new position");
	check(g_cbTrack0 == 1, "resume: slot-0 zero sector delivered");
	CdControlF(CdlPause, 0);

	/*	-------- FMV shape: a cleared callback holds the stream in place  */
	CdReadyCallback(NULL);
	check(readSAt(track1) == 1, "ReadS with no callback accepted");
	for (int v = 0; v < 10; v++)
		Port_CdVblank(60);
	check(Spu_CdInCountForTest() == 0, "held stream decodes nothing");
	CdReadyCallback((CdlCB)testReadyCb);
	before = g_cbDataReady;
	for (int v = 0; v < 4; v++)
		Port_CdVblank(60);
	check(Spu_CdInCountForTest() == XA_SECTOR_SAMPLES &&
		  g_cbDataReady == before + 1,
		  "re-registered callback resumes from the held position");
	CdControlF(CdlPause, 0);
	CdReadyCallback(NULL);

	/*	the virtual disc holds TRACK1.IXA open - rebuild against an empty
		dir to release the handle, or the remove below silently fails and
		leaves xa_test_tmp behind  */
	_putenv("SBSP_DATA_DIR=xa_test_tmp\\gone");
	Port_CdRebuildDirForTest();
	XaStream_ResetForTest();
	remove("xa_test_tmp\\TRACK1.IXA");
	check(_rmdir("xa_test_tmp") == 0, "synthetic disc cleaned up");
}

/*****************************************************************************/
/*	The Spu_RenderFrames CD slice: a constant pushed into the ring must come
	out - once the resampler's two taps are filled - as exactly
	master(cdvol(atv(s))) with the mixer's integer arithmetic, and mix-off
	must consume without contributing.  */
static void mixSliceTest(void)
{
	Spu_Lock();
	memset(g_spuVoice, 0, sizeof(SpuVoiceState) * SPU_NVOICES);
	g_spuMasterVolL = g_spuMasterVolR = 0x3FFF;
	g_spuCdVolL = g_spuCdVolR = 0x7FFF;
	g_spuCdMixOn = 1;
	Spu_Unlock();
	Spu_SetCdAtv(128, 0, 0, 128);		/* identity: L->L, R->R */
	Spu_CdInClear();

	const int16_t kIn = 2000;
	static int16_t mono[XA_SECTOR_SAMPLES];
	for (int i = 0; i < XA_SECTOR_SAMPLES; i++)
		mono[i] = kIn;
	Spu_CdInPush(mono, XA_SECTOR_SAMPLES);

	/*	the mixer's own arithmetic, spelled out  */
	int cd       = (kIn * 128 + kIn * 0) >> 7;			/* ATV      */
	int afterVol = (cd * 0x7FFF) >> 15;					/* CD vol   */
	int expected = (afterVol * 0x3FFF) >> 14;			/* master   */

	static int16_t outBuf[512 * 2];
	Spu_RenderFrames(outBuf, 8);		/* warm the prev/cur taps */
	Spu_RenderFrames(outBuf, 512);
	bool flat = true;
	for (int i = 0; i < 512 && flat; i++)
		flat = outBuf[i * 2 + 0] == expected && outBuf[i * 2 + 1] == expected;
	check(flat, "CD slice: constant in -> atv/cdvol/master arithmetic out");

	Spu_Lock();
	g_spuCdMixOn = 0;
	Spu_Unlock();
	unsigned occBefore = Spu_CdInCountForTest();
	Spu_RenderFrames(outBuf, 512);
	bool silent = true;
	for (int i = 0; i < 512 && silent; i++)
		silent = outBuf[i * 2] == 0 && outBuf[i * 2 + 1] == 0;
	check(silent, "CD slice: mix-off contributes nothing");
	check(Spu_CdInCountForTest() < occBefore,
		  "CD slice: mix-off still consumes the ring (no back-up)");

	Spu_Lock();
	g_spuCdMixOn = 0;
	g_spuCdVolL = g_spuCdVolR = 0;
	Spu_Unlock();
	Spu_CdInClear();
}

/*****************************************************************************/
/*	Full-swing CD input: alternating +-30000 steps the resampler's taps by
	60000, and at 18.9kHz the phase reaches 37800, so the interpolation
	product is ~2.27e9 - past a 32-bit `long`, which is what Windows has on
	both ABIs.  Interpolating between the two can never leave [-30000,
	30000]; a wrapped product lands far outside and clips.  */
static void cdSwingTest(void)
{
	Spu_Lock();
	memset(g_spuVoice, 0, sizeof(SpuVoiceState) * SPU_NVOICES);
	g_spuMasterVolL = g_spuMasterVolR = 0x3FFF;
	g_spuCdVolL = g_spuCdVolR = 0x7FFF;
	g_spuCdMixOn = 1;
	Spu_Unlock();
	Spu_SetCdAtv(128, 0, 0, 128);
	Spu_CdInClear();

	static int16_t mono[XA_SECTOR_SAMPLES];
	for (int i = 0; i < XA_SECTOR_SAMPLES; i++)
		mono[i] = (i & 1) ? -30000 : 30000;
	Spu_CdInPush(mono, XA_SECTOR_SAMPLES);

	static int16_t outBuf[4096 * 2];
	Spu_RenderFrames(outBuf, 4096);		/* ~1756 of the 4032 source frames */
	int worst = 0;
	for (int i = 0; i < 4096 * 2; i++)
	{
		int a = outBuf[i] < 0 ? -outBuf[i] : outBuf[i];
		if (a > worst)
			worst = a;
	}
	check(worst <= 30000, "CD slice: full-swing input interpolates without wrapping");
	if (worst > 30000)
		std::printf("  (peak |out| %d)\n", worst);

	Spu_Lock();
	g_spuCdMixOn = 0;
	g_spuCdVolL = g_spuCdVolR = 0;
	Spu_Unlock();
	Spu_CdInClear();
}

/*****************************************************************************/
/*	Stereo CD slice (M7): distinct L/R constants at 37.8kHz through a CROSS
	ATV matrix must come out swapped, with the same vol arithmetic.  */
static void stereoMixSliceTest(void)
{
	Spu_Lock();
	memset(g_spuVoice, 0, sizeof(SpuVoiceState) * SPU_NVOICES);
	g_spuMasterVolL = g_spuMasterVolR = 0x3FFF;
	g_spuCdVolL = g_spuCdVolR = 0x7FFF;
	g_spuCdMixOn = 1;
	Spu_Unlock();
	Spu_SetCdAtv(0, 128, 128, 0);		/* cross: L->R, R->L */
	Spu_CdInClear();
	Spu_CdInSetRate(37800);

	const int16_t kL = 1000, kR = -3000;
	static int16_t pairs[4096 * 2];
	for (int i = 0; i < 4096; i++)
	{
		pairs[i * 2] = kL;
		pairs[i * 2 + 1] = kR;
	}
	Spu_CdInPushStereo(pairs, 4096);

	int cdL      = (kL * 0 + kR * 128) >> 7;			/* = kR */
	int cdR      = (kL * 128 + kR * 0) >> 7;			/* = kL */
	int expectL  = (((cdL * 0x7FFF) >> 15) * 0x3FFF) >> 14;
	int expectR  = (((cdR * 0x7FFF) >> 15) * 0x3FFF) >> 14;

	static int16_t outBuf[512 * 2];
	Spu_RenderFrames(outBuf, 8);		/* warm the taps */
	Spu_RenderFrames(outBuf, 512);
	bool flat = true;
	for (int i = 0; i < 512 && flat; i++)
		flat = outBuf[i * 2 + 0] == expectL && outBuf[i * 2 + 1] == expectR;
	check(flat, "stereo CD slice: 37.8kHz pairs through a cross ATV matrix");

	/*	rate accounting: 512 output frames at 37800/44100 consume ~439
		ring frames (+-1 for phase).  */
	unsigned used = 4096 - Spu_CdInCountForTest();
	unsigned expectUsed = (unsigned)(((int64_t)(8 + 512) * 37800) / 44100);
	check(used >= expectUsed && used <= expectUsed + 1,
		  "stereo CD slice: ring drains at the source rate");

	Spu_Lock();
	g_spuCdMixOn = 0;
	g_spuCdVolL = g_spuCdVolR = 0;
	Spu_Unlock();
	Spu_CdInClear();
}

int main(void)
{
	static uint8_t	user[XA_SECTOR_USER_BYTES];
	static int16_t	out[XA_SECTOR_SAMPLES];
	int32_t			h1, h2;

	/*	-------- 1a: silence in, silence out  */
	memset(user, 0, sizeof(user));
	h1 = h2 = 0;
	XaAdpcm_DecodeSector4bitMono(user, out, &h1, &h2);
	bool allZero = true;
	for (int i = 0; i < XA_SECTOR_SAMPLES; i++)
		allZero = allZero && out[i] == 0;
	check(allZero && h1 == 0 && h2 == 0, "zero sector decodes to silence");

	/*	-------- 1b: filter 0 = pure (nibble<<12 sign-extended) >> shift  */
	{
		uint8_t params[8]     = { 0, 1, 4, 12, 0, 2, 3, 5 };	/* filter 0, varied shifts */
		uint8_t nib[8][28];
		for (int b = 0; b < 8; b++)
			for (int i = 0; i < 28; i++)
				nib[b][i] = (uint8_t)((b * 5 + i) & 0x0F);
		memset(user, 0, sizeof(user));
		buildGroup(user, params, nib);

		h1 = h2 = 0;
		XaAdpcm_DecodeSector4bitMono(user, out, &h1, &h2);
		bool ok = true;
		for (int b = 0; b < 8 && ok; b++)
			for (int i = 0; i < 28; i++)
			{
				int expect = (int16_t)(nib[b][i] << 12) >> params[b];
				if (out[b * 28 + i] != expect)
				{
					std::printf("  block %d sample %d: got %d expect %d\n",
								b, i, out[b * 28 + i], expect);
					ok = false;
					break;
				}
			}
		check(ok, "filter-0 blocks are nibble>>shift exactly");
	}

	/*	-------- 2: cross-oracle vs the SPU ADPCM decoder.
		Feed the identical nibble stream + params through both layouts with
		threaded history; every sample must match.  Covers filters 1-3,
		block-to-block and group-to-group history continuity.  */
	{
		uint8_t params[8];
		uint8_t nib[8][28];
		unsigned seed = 0x2D5;
		for (int b = 0; b < 8; b++)
		{
			int filter = 1 + (b % 3);			/* 1,2,3 */
			int shift  = 2 + (b % 5);
			params[b] = (uint8_t)((filter << 4) | shift);
			for (int i = 0; i < 28; i++)
			{
				seed = seed * 1103515245 + 12345;
				nib[b][i] = (uint8_t)((seed >> 16) & 0x0F);
			}
		}
		/*	the same group in all 18 slots - the SPU chain below then runs
			18x8 blocks, so cross-group history continuity is covered too  */
		for (int grp = 0; grp < 18; grp++)
			buildGroup(user + grp * 128, params, nib);

		h1 = h2 = 0;
		XaAdpcm_DecodeSector4bitMono(user, out, &h1, &h2);

		int16_t spuOut[28];
		int16_t sh1 = 0, sh2 = 0;
		bool ok = true;
		for (int blk = 0; blk < 18 * 8 && ok; blk++)
		{
			int b = blk % 8;
			uint8_t block[16];
			block[0] = params[b];				/* same (filter<<4)|shift byte */
			block[1] = 0;
			memset(block + 2, 0, 14);
			for (int i = 0; i < 28; i++)
				block[2 + (i >> 1)] |= (uint8_t)(nib[b][i] << ((i & 1) * 4));
			SpuAdpcm_DecodeBlock(block, spuOut, &sh1, &sh2);
			for (int i = 0; i < 28; i++)
				if (out[blk * 28 + i] != spuOut[i])
				{
					std::printf("  block %d sample %d: xa %d spu %d\n",
								blk, i, out[blk * 28 + i], spuOut[i]);
					ok = false;
					break;
				}
		}
		check(ok, "XA layout matches the SPU decoder on the same nibble stream");
		check(h1 == sh1 && h2 == sh2, "history matches the SPU decoder");
	}

	/*	-------- 3: real Track1.Ixa sectors vs the ffmpeg golden  */
	FILE *fs = fopen("port/tests/xa_fixture_sectors.bin", "rb");
	FILE *fg = fopen("port/tests/xa_fixture_golden.pcm", "rb");
	if (!fs || !fg)
	{
		std::printf("xa_test: fixtures not found (run from the repo root) - "
					"real-data golden SKIPPED\n");
		if (fs) fclose(fs);
		if (fg) fclose(fg);
	}
	else
	{
		static uint8_t	sector[2336];
		static int16_t	golden[4 * XA_SECTOR_SAMPLES];
		size_t gn = fread(golden, 2, 4 * XA_SECTOR_SAMPLES, fg);
		fclose(fg);
		check(gn == 4 * XA_SECTOR_SAMPLES, "golden pcm is 4 sectors long");

		h1 = h2 = 0;
		int mismatches = 0, maxDiff = 0, firstBad = -1;
		for (int s = 0; s < 4; s++)
		{
			check(fread(sector, 1, 2336, fs) == 2336, "fixture sector read");
			check(sector[2] & 0x04, "fixture sector is XA audio");
			check(sector[3] == 0x04, "fixture coding is mono 18.9kHz 4-bit");
			/*	raw sector = [8-byte subheader][2328 data]; user area first  */
			XaAdpcm_DecodeSector4bitMono(sector + 8, out, &h1, &h2);
			for (int i = 0; i < XA_SECTOR_SAMPLES; i++)
			{
				int d = out[i] - golden[s * XA_SECTOR_SAMPLES + i];
				if (d)
				{
					if (firstBad < 0)
						firstBad = s * XA_SECTOR_SAMPLES + i;
					mismatches++;
					if (d < 0) d = -d;
					if (d > maxDiff) maxDiff = d;
				}
			}
		}
		fclose(fs);
		if (mismatches)
			std::printf("  real-data: %d/%d samples differ from ffmpeg, "
						"max |diff| %d, first at %d\n",
						mismatches, 4 * XA_SECTOR_SAMPLES, maxDiff, firstBad);
		check(mismatches == 0, "real Track1.Ixa sectors decode bit-identical to ffmpeg");
	}

	/*	-------- 3b: stereo decode (M7 STR audio)  */
	{
		static uint8_t	suser[XA_SECTOR_USER_BYTES];
		static int16_t	pairs[2 * XA_SECTOR_PAIRS];
		uint8_t params[8];
		uint8_t nib[8][28];

		/*	filter 0, varied shifts, distinct nibbles per unit: assert the
			L/R interleave places even units at [2i], odd at [2i+1], in
			unit-pair time order.  */
		for (int b = 0; b < 8; b++)
		{
			params[b] = (uint8_t)(b % 5);		/* filter 0, shift 0..4 */
			for (int i = 0; i < 28; i++)
				nib[b][i] = (uint8_t)((b * 3 + i) & 0x0F);
		}
		memset(suser, 0, sizeof(suser));
		buildGroup(suser, params, nib);
		int32_t l1 = 0, l2 = 0, r1 = 0, r2 = 0;
		XaAdpcm_DecodeSector4bitStereo(suser, pairs, &l1, &l2, &r1, &r2);
		bool ok = true;
		for (int u = 0; u < 8 && ok; u++)
			for (int i = 0; i < 28; i++)
			{
				int expect = (int16_t)(nib[u][i] << 12) >> params[u];
				int at = ((u / 2) * 28 + i) * 2 + (u & 1);
				if (pairs[at] != expect)
				{
					std::printf("  stereo unit %d sample %d: got %d expect %d\n",
								u, i, pairs[at], expect);
					ok = false;
					break;
				}
			}
		check(ok, "stereo: filter-0 units land interleaved L/R in pair order");

		/*	channel independence: identical nibbles+params on L and R with a
			history-using filter must produce identical channels.  */
		for (int b = 0; b < 8; b++)
		{
			params[b] = (uint8_t)((2 << 4) | 3);	/* filter 2, shift 3 */
			for (int i = 0; i < 28; i++)
				nib[b][i] = (uint8_t)((7 * (b / 2) + i * 5) & 0x0F);
		}
		for (int b = 0; b < 8; b += 2)
			memcpy(nib[b + 1], nib[b], 28);		/* R mirrors L */
		for (int grp = 0; grp < 18; grp++)
			buildGroup(suser + grp * 128, params, nib);
		l1 = l2 = r1 = r2 = 0;
		XaAdpcm_DecodeSector4bitStereo(suser, pairs, &l1, &l2, &r1, &r2);
		ok = true;
		for (int i = 0; i < XA_SECTOR_PAIRS && ok; i++)
			ok = pairs[i * 2] == pairs[i * 2 + 1];
		check(ok && l1 == r1 && l2 == r2,
			  "stereo: mirrored input decodes to identical channels");
	}

	/*	-------- 3c: real thq.str stereo sectors vs the ffmpeg golden  */
	{
		FILE *ss = fopen("port/tests/xa_stereo_sectors.bin", "rb");
		FILE *sg = fopen("port/tests/xa_stereo_golden.pcm", "rb");
		if (!ss || !sg)
		{
			std::printf("xa_test: stereo fixtures not found (run from the repo "
						"root) - stereo golden SKIPPED\n");
			if (ss) fclose(ss);
			if (sg) fclose(sg);
		}
		else
		{
			static uint8_t	sector[2336];
			static int16_t	pairs[XA_SECTOR_PAIRS * 2];
			static int16_t	golden[2 * XA_SECTOR_PAIRS * 2];
			size_t gn = fread(golden, 2, 2 * XA_SECTOR_PAIRS * 2, sg);
			fclose(sg);
			check(gn == 2 * XA_SECTOR_PAIRS * 2, "stereo golden is 2 sectors");

			int32_t l1 = 0, l2 = 0, r1 = 0, r2 = 0;
			int mismatches = 0;
			for (int s = 0; s < 2; s++)
			{
				check(fread(sector, 1, 2336, ss) == 2336, "stereo sector read");
				check(sector[3] == 0x01, "fixture coding is stereo 37.8kHz 4-bit");
				XaAdpcm_DecodeSector4bitStereo(sector + 8, pairs,
											   &l1, &l2, &r1, &r2);
				for (int i = 0; i < XA_SECTOR_PAIRS * 2; i++)
					if (pairs[i] != golden[s * XA_SECTOR_PAIRS * 2 + i])
						mismatches++;
			}
			fclose(ss);
			if (mismatches)
				std::printf("  stereo real-data: %d samples differ\n", mismatches);
			check(mismatches == 0,
				  "real thq.str stereo sectors decode bit-identical to ffmpeg");
		}
	}

	streamEngineTests();
	mixSliceTest();
	cdSwingTest();
	stereoMixSliceTest();

	if (g_failures)
	{
		std::printf("xa_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("xa_test: all passed\n");
	return 0;
}
