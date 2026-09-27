/*	str_test (M7): the STR streaming engine (psyq/cd/str_stream.cpp)
	over a synthetic movie, driven through the real CD dispatch exactly as
	fmv.cpp drives it: StSetRing/StSetStream, CdlSeekL, CdRead2, pumped
	Port_CdVblank(60), StGetNext/StFreeRing, CdlPause, StUnSetRing.

	str_test_tmp/THQ.STR layout (raw 2336-byte sectors):
	  frame 1: 3 video chunks          then 1 audio sector (coding 0x01)
	  frame 2: 3 video chunks          then 1 audio sector
	  frame 3: 9 video chunks (the INTRO worst case)
	  frame 4: 3 video chunks
	  2 zero-pad sectors (no STR header - must be skipped)
	Video payload byte i of (frame f, chunk c) = (f*31 + c*7 + i) & 0xFF.
	Audio sectors are all-zero nibbles on file 1 chan 1 - they decode to
	silence but their PAIR COUNT in the SPU CD ring proves routing/rate.

	str_test_tmp/CLIMAX.STR is one 1-chunk frame whose payload is a BS v2
	stream with no end-of-frame code (issue #60).

	Covers: arming + streaming via the real dispatch, frame assembly
	order/content/StHEADER copies, the 9-chunk frame, ring backpressure
	(frames held un-freed until delivery stalls, then resumes on free),
	the CdlModeSF filter gate both ways, stereo pairs at 37.8kHz, EOF,
	CdlPause mid-stream, StUnSetRing + full re-arm (the THQ->CLIMAX
	shape), StrStream_FrameEnd, and DecDCTvlc3 on an unterminated frame
	that ends at a guard page.  The XA speech engine's own coverage is
	xa_test.
*/
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>
#include <stdlib.h>

#include <sys/types.h>
#include <libcd.h>

#include "cd/xa_stream.h"
#include "cd/str_stream.h"
#include "cd/xa_adpcm.h"
#include "spu/spu_core.h"

extern "C" void Port_CdRebuildDirForTest(void);
extern "C" int DecDCTvlc3(unsigned long *bs, unsigned long *buf);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*****************************************************************************/
static void writeVideoSector(FILE *f, unsigned frameNo, unsigned chunk,
							 unsigned nChunks, const uint8_t *payload = NULL)
{
	uint8_t sec[2336];
	memset(sec, 0, sizeof(sec));
	sec[0] = 0; sec[1] = 1; sec[2] = 0x48; sec[3] = 0;	/* file 0 chan 1 data */
	memcpy(sec + 4, sec, 4);
	uint8_t *d = sec + 8;
	d[0] = 0x60; d[1] = 0x01;			/* StStatus 0160h */
	d[2] = 0x01; d[3] = 0x80;			/* StType 8001h */
	d[4] = (uint8_t)chunk; d[5] = (uint8_t)(chunk >> 8);
	d[6] = (uint8_t)nChunks; d[7] = (uint8_t)(nChunks >> 8);
	memcpy(d + 8, &frameNo, 4);
	uint32_t frameSize = nChunks * 2016u;
	memcpy(d + 12, &frameSize, 4);
	d[0x10] = 320 & 0xFF; d[0x11] = 320 >> 8;
	d[0x12] = 240 & 0xFF; d[0x13] = 240 >> 8;
	for (int i = 0; i < 2016; i++)
		d[0x20 + i] = (uint8_t)(frameNo * 31 + chunk * 7 + i);
	if (payload)
		memcpy(d + 0x20, payload, 2016);
	fwrite(sec, 1, sizeof(sec), f);
}

static void writeAudioSector(FILE *f, int file, int chan)
{
	uint8_t sec[2336];
	memset(sec, 0, sizeof(sec));
	sec[0] = (uint8_t)file; sec[1] = (uint8_t)chan;
	sec[2] = 0x64; sec[3] = 0x01;		/* audio, stereo 37.8kHz 4-bit */
	memcpy(sec + 4, sec, 4);
	fwrite(sec, 1, sizeof(sec), f);
}

static void writeZeroSector(FILE *f)
{
	uint8_t sec[2336];
	memset(sec, 0, sizeof(sec));
	fwrite(sec, 1, sizeof(sec), f);
}

static void buildDisc(void)
{
	_mkdir("str_test_tmp");
	FILE *f = fopen("str_test_tmp\\THQ.STR", "wb");
	check(f != NULL, "synthetic movie created");
	if (!f)
		return;
	for (unsigned c = 0; c < 3; c++)
		writeVideoSector(f, 1, c, 3);
	writeAudioSector(f, 1, 1);
	for (unsigned c = 0; c < 3; c++)
		writeVideoSector(f, 2, c, 3);
	writeAudioSector(f, 1, 1);
	for (unsigned c = 0; c < 9; c++)
		writeVideoSector(f, 3, c, 9);
	for (unsigned c = 0; c < 3; c++)
		writeVideoSector(f, 4, c, 3);
	writeZeroSector(f);
	writeZeroSector(f);
	fclose(f);

	/*	CLIMAX.STR: a BS v2 frame (max declared size, qscale 1) of DC-0 +
		EOB blocks - 12 bits each, "000000000010", MSB-first in
		little-endian halfwords, so three halfwords hold four blocks - and
		no end-of-frame code anywhere: 1004 payload halfwords are 1338
		whole blocks and 8 bits of a 1339th.  */
	uint16_t bs[1008];
	bs[0] = 19279;
	bs[1] = 0x3800;
	bs[2] = 1;
	bs[3] = 2;
	static const uint16_t kFour[3] = { 0x0020, 0x0200, 0x2002 };
	for (int i = 0; i < 1004; i++)
		bs[4 + i] = kFour[i % 3];
	f = fopen("str_test_tmp\\CLIMAX.STR", "wb");
	check(f != NULL, "unterminated movie created");
	if (!f)
		return;
	writeVideoSector(f, 1, 0, 1, (const uint8_t *)bs);
	writeZeroSector(f);
	fclose(f);
}

static bool frameContentOk(const uint8_t *p, unsigned frameNo, unsigned nChunks)
{
	for (unsigned c = 0; c < nChunks; c++)
		for (int i = 0; i < 2016; i += 97)	/* stride sample, cheap + dense */
			if (p[c * 2016 + i] != (uint8_t)(frameNo * 31 + c * 7 + i))
				return false;
	return true;
}

static long g_movieLBA;

static void kick(long mode)
{
	CdlLOC loc;
	CdIntToPos((int)g_movieLBA, &loc);
	check(CdControl(CdlSeekL, (u_char *)&loc, 0) != 0, "CdlSeekL accepted");
	check(CdRead2(mode) != 0, "CdRead2 reports streaming");
}

static void pump(int vblanks)
{
	for (int i = 0; i < vblanks; i++)
		Port_CdVblank(60);
}

/*****************************************************************************/
int main(void)
{
	static u_long ring[32 * 512];		/* the game's 32-sector ring */

	buildDisc();
	_putenv("SBSP_DATA_DIR=str_test_tmp");
	Port_CdRebuildDirForTest();
	XaStream_ResetForTest();
	StrStream_ResetForTest();
	Spu_CdInClear();

	CdlFILE cf;
	CdlFILE *found = CdSearchFile(&cf, (char *)"\\THQ.STR;1");
	check(found != NULL, "movie in the virtual dir");
	if (!found)
		return 1;
	g_movieLBA = CdPosToInt(&cf.pos);

	u_long *addr = 0, *hdr = 0;

	/*	-------- 1: basic streaming, frame order, content, headers  */
	StSetRing(ring, 32);
	StSetStream(1, 1, 0xFFFFFFFFu, 0, 0);
	kick(CdlModeStream | CdlModeSpeed | CdlModeRT);		/* fmv.cpp's mode */

	pump(2);							/* 5 sectors: frame 1 + its audio */
	check(StGetNext(&addr, &hdr) == 0, "frame 1 ready");
	if (addr && hdr)
	{
		const StHEADER *h = (const StHEADER *)hdr;
		check(h->frameCount == 1 && h->nSectors == 3
			  && h->width == 320 && h->height == 240,
			  "frame 1 StHEADER fields");
		check(frameContentOk((const uint8_t *)addr, 1, 3), "frame 1 content");
		check(StrStream_FrameEnd(addr) == (const uint8_t *)addr + 3 * 2016,
			  "FrameEnd: frame 1 ends after its 3 chunks");
		check(StrStream_FrameEnd((const uint8_t *)addr + 2) == NULL,
			  "FrameEnd: only a frame's start pointer is known");
	}
	check(Spu_CdInCount() == XA_SECTOR_PAIRS,
		  "audio sector became 2016 stereo pairs (no SF: filter ignored)");
	StFreeRing(addr);
	check(StrStream_FrameEnd(addr) == NULL, "FrameEnd: a freed frame is unknown");

	pump(2);
	check(StGetNext(&addr, &hdr) == 0, "frame 2 ready");
	if (hdr)
		check(((const StHEADER *)hdr)->frameCount == 2, "frame 2 in order");
	StFreeRing(addr);

	pump(4);							/* the 9-chunk frame */
	check(StGetNext(&addr, &hdr) == 0, "9-chunk frame 3 ready");
	if (addr && hdr)
	{
		check(((const StHEADER *)hdr)->nSectors == 9, "frame 3 header");
		check(frameContentOk((const uint8_t *)addr, 3, 9), "frame 3 content");
		check(StrStream_FrameEnd(addr) == (const uint8_t *)addr + 9 * 2016,
			  "FrameEnd: the 9-chunk frame ends after 9 chunks");
	}
	StFreeRing(addr);

	pump(2);
	check(StGetNext(&addr, &hdr) == 0, "frame 4 ready");
	StFreeRing(addr);

	/*	-------- 2: EOF - pad sectors skipped, then a fast "no data"  */
	pump(4);
	check(StGetNext(&addr, &hdr) == 1, "end of stream returns 1");

	/*	-------- 3: backpressure - re-arm, deliver everything with nothing
		freed: 3+3+9+3 chunks = 36,288B fit the 64KB ring, so all four
		frames go ready; a fresh re-run with a tiny 4-sector (8KB) ring
		must stall after frame 1 (frame 2 does not fit alongside), then
		resume when frame 1 is freed.  */
	StUnSetRing();
	StrStream_ResetForTest();
	Spu_CdInClear();
	StSetRing(ring, 4);					/* 8192B: one 6048B frame at a time */
	StSetStream(1, 1, 0xFFFFFFFFu, 0, 0);
	kick(CdlModeStream | CdlModeSpeed | CdlModeRT);
	pump(8);							/* try to deliver well past frame 2 */
	check(StGetNext(&addr, &hdr) == 0, "small ring: frame 1 ready");
	unsigned audioBefore = Spu_CdInCount();
	pump(8);							/* held: no new sectors may flow */
	check(Spu_CdInCount() == audioBefore,
		  "small ring: held stream also pauses audio (A/V lockstep)");
	StFreeRing(addr);
	pump(8);							/* resume: frame 2 assembles now */
	check(StGetNext(&addr, &hdr) == 0, "small ring: frame 2 after free");
	if (hdr)
		check(((const StHEADER *)hdr)->frameCount == 2,
			  "small ring: resumed in order");
	StFreeRing(addr);

	/*	-------- 4: CdlPause mid-stream  */
	StUnSetRing();
	StrStream_ResetForTest();
	Spu_CdInClear();
	StSetRing(ring, 32);
	StSetStream(1, 1, 0xFFFFFFFFu, 0, 0);
	kick(CdlModeStream | CdlModeSpeed | CdlModeRT);
	pump(2);
	CdControlB(CdlPause, 0, 0);
	check(Spu_CdInCount() == 0, "CdlPause flushed the CD-input ring");
	pump(4);
	unsigned after = Spu_CdInCount();
	check(after == 0, "paused stream delivers nothing");

	/*	-------- 5: the SF gate - with CdlModeSF and a non-matching speech
		filter the movie audio is dropped; with a matching one it plays  */
	StUnSetRing();
	StrStream_ResetForTest();
	Spu_CdInClear();
	CdlFILTER flt;
	flt.file = 1;
	flt.chan = 5;						/* stale speech channel */
	CdControlB(CdlSetfilter, (u_char *)&flt, 0);
	StSetRing(ring, 32);
	StSetStream(1, 1, 0xFFFFFFFFu, 0, 0);
	kick(CdlModeStream | CdlModeSpeed | CdlModeRT | CdlModeSF);
	pump(4);
	check(Spu_CdInCount() == 0,
		  "SF + mismatched filter drops movie audio");
	StrStream_ResetForTest();
	Spu_CdInClear();
	flt.chan = 1;
	CdControlB(CdlSetfilter, (u_char *)&flt, 0);
	StSetRing(ring, 32);
	kick(CdlModeStream | CdlModeSpeed | CdlModeRT | CdlModeSF);
	pump(4);
	check(Spu_CdInCount() > 0, "SF + matching filter passes audio");

	/*	-------- 6: full re-arm (the THQ -> CLIMAX shape)  */
	StUnSetRing();
	StrStream_ResetForTest();
	Spu_CdInClear();
	StSetRing(ring, 32);
	StSetStream(1, 1, 0xFFFFFFFFu, 0, 0);
	kick(CdlModeStream | CdlModeSpeed | CdlModeRT);
	pump(2);
	check(StGetNext(&addr, &hdr) == 0
		  && ((const StHEADER *)hdr)->frameCount == 1,
		  "fresh re-arm streams frame 1 again");
	StFreeRing(addr);
	StUnSetRing();
	check(StrStream_FrameEnd(addr) == NULL, "FrameEnd: nothing after StUnSetRing");

	/*	-------- 7: an unterminated BS frame ending at a guard page.  The
		bit reader is bounded by the frame's own ring region, so the decode
		stops at the frame's last byte; the old 64KB-from-the-frame bound
		read on into the PAGE_NOACCESS page and faulted.  */
	{
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		size_t page = si.dwPageSize;
		uint8_t *base = (uint8_t *)VirtualAlloc(NULL, 2 * page,
												MEM_RESERVE | MEM_COMMIT,
												PAGE_READWRITE);
		check(base != NULL, "guarded ring allocated");
		if (base)
		{
			DWORD old;
			VirtualProtect(base + page, page, PAGE_NOACCESS, &old);
			/*	a 1-sector ring whose one 2016-byte frame region ends
				exactly at the guard (the engine writes nothing past it)  */
			u_long *gring = (u_long *)(base + page - 2016);

			CdlFILE cf2;
			check(CdSearchFile(&cf2, (char *)"\\CLIMAX.STR;1") != NULL,
				  "unterminated movie in the virtual dir");
			g_movieLBA = CdPosToInt(&cf2.pos);
			StrStream_ResetForTest();
			Spu_CdInClear();
			StSetRing(gring, 1);
			StSetStream(1, 1, 0xFFFFFFFFu, 0, 0);
			kick(CdlModeStream | CdlModeSpeed | CdlModeRT);
			pump(2);
			check(StGetNext(&addr, &hdr) == 0, "unterminated frame ready");
			if (addr == gring)
			{
				check(StrStream_FrameEnd(addr) == base + page,
					  "FrameEnd: a 1-chunk frame ends at the guard");
				static unsigned long out[19280];
				DecDCTvlc3((unsigned long *)addr, out);
				const uint16_t *hw = (const uint16_t *)(out + 1);
				check(hw[0] == 0x0400 && hw[1] == 0xFE00,
					  "unterminated frame: first block decoded");
				check(hw[2 * 1337] == 0x0400 && hw[2 * 1337 + 1] == 0xFE00,
					  "unterminated frame: decoded to its last whole block");
			}
			else
				check(false, "unterminated frame placed at the ring start");
			StFreeRing(addr);
			StUnSetRing();
			VirtualFree(base, 0, MEM_RELEASE);
		}
	}

	if (g_failures)
	{
		std::printf("str_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("str_test: all passed\n");
	return 0;
}
