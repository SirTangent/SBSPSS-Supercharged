/*	STR streaming engine (M7): the libcd St* ring surface plus CdRead2,
	feeding fmv.cpp's movie player from the raw-XA .STR host files.

	Model.  strKickCD seeks (CdlSeekL -> StrStream_Seek) then starts
	streaming (CdRead2 -> StrStream_Start).  Sectors are delivered by the
	same emulated-vblank 150/s clock as the XA speech engine
	(StrStream_Vblank, called from Port_CdVblank BEFORE the XA hold check:
	the M6 "hold while the ready callback is NULL" rule must not gate STR,
	because fmv.cpp deliberately clears that callback at both ends).

	Per delivered 2336-byte sector:
	- audio (submode bit 2): the CdlSetfilter file/chan gate applies ONLY
	  when the CdRead2 mode carries CdlModeSF - fmv.cpp streams with
	  Stream|Speed|RT and no SF, so a stale speech filter never mutes a
	  movie; decoded by coding (0x01 stereo 37.8kHz / 0x04 mono 18.9kHz)
	  into the SPU CD-input ring at the matching rate.
	- video (StHEADER id 0160h type 8001h): the 2016-byte payload is
	  assembled into the GAME's StSetRing buffer by frame - a contiguous-
	  with-wrap region of nSectors*2016 bytes per frame, chunk placed at
	  secOffset*2016.  A complete frame joins the ready FIFO with a copy
	  of its StHEADER (fmv.cpp reads frameCount/width/height from it).
	- anything else (zero padding) is skipped.

	StGetNext returns 0 with the oldest ready frame; while streaming with
	nothing ready it BLOCKS on Port_PumpIdle - fmv.cpp's retry loop is
	commented out upstream, so a single "no data" ends the movie; only a
	true end-of-stream (or ~5s of emulated silence, a corrupt-data guard)
	returns 1.  StFreeRing releases frames in FIFO order (fmv.cpp frees
	right after DecDCTvlc3).  If a new frame cannot be placed because the
	ring is still full, the sector clock HOLDS in place - audio pauses
	with it, so A/V cannot drift (production is exactly real-time; the
	game consumes a frame per 2-3.6 vblanks, so the hold is a safety
	valve, not a steady state).

	StCdIntrFlag stays permanently 0: fmv.cpp's out-callback would call
	StCdInterrupt() to drain the CD IRQ re-entrantly; our delivery runs on
	the pump clock instead, so that path is deliberately inert.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <sys/types.h>
#include <libcd.h>

#include "stub_log.h"
#include "cd/str_stream.h"
#include "cd/xa_stream.h"
#include "cd/xa_adpcm.h"
#include "spu/spu_core.h"
#include "host/pump.h"

namespace
{

const int kMaxSlots = 8;				/* frames in flight (32-sector ring
										   holds >= 3 worst-case 9-sector
										   frames; 8 is ample) */

struct FrameSlot
{
	unsigned	offset;					/* byte offset in the game's ring */
	unsigned	bytes;					/* nSectors * 2016 */
	uint32_t	seen;					/* bit per received chunk index */
	unsigned	chunksGot;
	int			ready;
	int			handedOut;
	StHEADER	hdr;
};

/*	the game's ring buffer (StSetRing)  */
uint8_t		*g_ring;
unsigned	g_ringBytes;
int			g_armed;

/*	region allocator + slot FIFO (slots [tail..head) are live)  */
FrameSlot	g_slots[kMaxSlots];
int			g_slotTail, g_slotHead;		/* free-running, masked by kMaxSlots-1 */
unsigned	g_allocHead, g_allocTail;	/* byte cursors into the ring */

/*	stream position  */
long		g_seekLBA = -1;
long		g_mode;						/* CdRead2 mode word */
int			g_streaming;
int			g_ended;
FILE		*g_fp;
long		g_fileStartLBA, g_fileSectors;
const char	*g_fileName;
long		g_curSector;
long		g_acc;						/* fractional sector clock */
int32_t		g_hL1, g_hL2, g_hR1, g_hR2;	/* ADPCM history (stereo/mono) */

int			g_trace = -1;				/* SBSP_STR_LOG */

int trace(void)
{
	if (g_trace < 0)
	{
		const char *e = getenv("SBSP_STR_LOG");
		g_trace = (e && *e && *e != '0');
	}
	return g_trace;
}

int liveSlots(void)
{
	return g_slotHead - g_slotTail;
}

FrameSlot *slotAt(int idx)
{
	return &g_slots[idx & (kMaxSlots - 1)];
}

/*	contiguous-with-wrap region allocator over the game's ring  */
int allocRegion(unsigned bytes, unsigned *off)
{
	if (liveSlots() == 0)
		g_allocHead = g_allocTail = 0;
	if (bytes > g_ringBytes)
		return 0;
	if (g_allocHead >= g_allocTail)
	{
		if (g_ringBytes - g_allocHead >= bytes)
		{
			*off = g_allocHead;
			g_allocHead += bytes;
			return 1;
		}
		if (g_allocTail > bytes)	/* wrap; keep head != tail */
		{
			*off = 0;
			g_allocHead = bytes;
			return 1;
		}
		return 0;
	}
	if (g_allocTail - g_allocHead > bytes)
	{
		*off = g_allocHead;
		g_allocHead += bytes;
		return 1;
	}
	return 0;
}

void resetStream(void)
{
	g_streaming = 0;
	g_ended = 0;
	g_fp = NULL;
	g_curSector = 0;
	g_acc = 0;
	g_hL1 = g_hL2 = g_hR1 = g_hR2 = 0;
}

void resetRing(void)
{
	g_slotTail = g_slotHead = 0;
	g_allocHead = g_allocTail = 0;
	memset(g_slots, 0, sizeof(g_slots));
}

/*	Deliver one sector.  Returns 1 if consumed, 0 on backpressure hold.  */
int deliverNext(void)
{
	static uint8_t sec[2336];
	static int16_t pcm[XA_SECTOR_SAMPLES];		/* mono or stereo pairs */

	if (g_curSector >= g_fileSectors)
	{
		if (!g_ended)
			fprintf(stderr, "[str] %s: end of stream at sector %ld\n",
					g_fileName, g_curSector);
		g_ended = 1;
		g_streaming = 0;
		return 1;
	}

	/*	Peek the sector to decide if it can be consumed at all: a video
		sector that needs a new frame region may have to wait for ring
		space.  Audio and padding always consume.  */
	fseek(g_fp, g_curSector * 2336L, SEEK_SET);
	if (fread(sec, 1, 2336, g_fp) != 2336)
	{
		fprintf(stderr, "[str] %s: short read at sector %ld - ending\n",
				g_fileName, g_curSector);
		g_ended = 1;
		g_streaming = 0;
		return 1;
	}

	int file = sec[0], chan = sec[1], submode = sec[2], coding = sec[3];

	if (submode & 0x04)
	{
		/*	Audio.  SF gate only when the mode asks for it (fmv.cpp's
			CdlModeStream|CdlModeSpeed|CdlModeRT does not).  */
		if (g_mode & CdlModeSF)
		{
			int ff, fc;
			XaStream_GetFilter(&ff, &fc);
			if (file != ff || chan != fc)
			{
				g_curSector++;
				return 1;
			}
		}
		if (coding == 0x01)
		{
			/*	per sector, not latched: Spu_CdInClear() resets the rate to
				18900 and is called from outside this engine (CdlPause, and
				every vblank with no audio consumer)  */
			Spu_CdInSetRate(37800);
			XaAdpcm_DecodeSector4bitStereo(sec + 8, pcm,
										   &g_hL1, &g_hL2, &g_hR1, &g_hR2);
			Spu_CdInPushStereo(pcm, XA_SECTOR_PAIRS);
		}
		else if (coding == 0x04)
		{
			Spu_CdInSetRate(18900);
			XaAdpcm_DecodeSector4bitMono(sec + 8, pcm, &g_hL1, &g_hL2);
			Spu_CdInPush(pcm, XA_SECTOR_SAMPLES);
		}
		else
			PSYQ_LOG_ONCE_KEYED(coding, "[str] unsupported audio coding "
								"0x%02X - silence\n", coding);
		g_curSector++;
		return 1;
	}

	/*	Data sector: STR video chunk, or padding.  */
	const uint8_t *d = sec + 8;
	uint16_t id, type, secOff, nSectors;
	uint32_t frameNo;
	memcpy(&id, d, 2);
	memcpy(&type, d + 2, 2);
	memcpy(&secOff, d + 4, 2);
	memcpy(&nSectors, d + 6, 2);
	memcpy(&frameNo, d + 8, 4);
	if (id != 0x0160 || type != 0x8001)
	{
		g_curSector++;					/* zero padding etc. */
		return 1;
	}
	if (nSectors == 0 || (unsigned)nSectors * 2016u > g_ringBytes)
	{
		PSYQ_LOG_ONCE_KEYED(1, "[str] frame %lu claims %u sectors - "
							"skipping\n", (unsigned long)frameNo, nSectors);
		g_curSector++;
		return 1;
	}

	/*	Frame the chunk belongs to = the newest slot if it matches and is
		still assembling; otherwise open a new one.  */
	FrameSlot *s = NULL;
	if (liveSlots() > 0)
	{
		FrameSlot *newest = slotAt(g_slotHead - 1);
		if (!newest->ready && newest->hdr.frameCount == frameNo)
			s = newest;
		else if (!newest->ready)
		{
			/*	a new frame started before the old one completed: drop the
				incomplete one (lost-sector tolerance).  It is the newest
				allocation, so the region rolls straight back.  */
			fprintf(stderr, "[str] frame %lu incomplete (%u/%u chunks) - "
					"dropped\n", (unsigned long)newest->hdr.frameCount,
					newest->chunksGot, newest->hdr.nSectors);
			g_allocHead = newest->offset;
			g_slotHead--;
		}
	}
	if (!s)
	{
		unsigned off;
		if (liveSlots() >= kMaxSlots ||
			!allocRegion((unsigned)nSectors * 2016u, &off))
		{
			if (trace())
				fprintf(stderr, "[str] ring full - holding at sector %ld\n",
						g_curSector);
			return 0;					/* backpressure: sector not consumed */
		}
		s = slotAt(g_slotHead++);
		memset(s, 0, sizeof(*s));
		s->offset = off;
		s->bytes = (unsigned)nSectors * 2016u;
		memcpy(&s->hdr, d, sizeof(StHEADER) < 32u ? sizeof(StHEADER) : 32u);
	}

	/*	The SLOT's geometry is the authority, never this sector's header: a
		later chunk claiming a bigger nSectors (corrupt STR, bit flip) must
		not write outside the region allocated from the first chunk, and a
		duplicate must not "complete" a frame that still has a hole.  */
	unsigned slotSectors = s->hdr.nSectors;
	unsigned at = (unsigned)secOff * 2016u;
	if (secOff < slotSectors && at + 2016u <= s->bytes)
	{
		memcpy(g_ring + s->offset + at, d + 0x20, 2016);
		if (!(s->seen & (1u << secOff)))
		{
			s->seen |= 1u << secOff;
			s->chunksGot++;
		}
	}
	else
		PSYQ_LOG_ONCE_KEYED(2, "[str] chunk %u (of %u) outside frame %lu's "
							"%u-sector region - dropped\n", secOff, nSectors,
							(unsigned long)s->hdr.frameCount, slotSectors);

	if (s->chunksGot == slotSectors)
	{
		s->ready = 1;
		if (trace())
			fprintf(stderr, "[str] frame %lu ready (%u bytes, %ux%u)\n",
					(unsigned long)s->hdr.frameCount, s->bytes,
					s->hdr.width, s->hdr.height);
	}
	g_curSector++;
	return 1;
}

}	/* namespace */

/*****************************************************************************/
/*	Engine entry points (cd.cpp dispatch + the sector clock).  */

extern "C" void StrStream_Seek(long lba)
{
	g_seekLBA = lba;
}

extern "C" int StrStream_Start(long mode)
{
	g_mode = mode;
	resetStream();
	if (g_seekLBA < 0)
	{
		fprintf(stderr, "[str] CdRead2 with no prior seek - ignored\n");
		return 1;
	}
	FILE *fp;
	long start, sectors;
	int bps;
	const char *name;
	if (!Port_CdFileForLBA(g_seekLBA, &fp, &start, &sectors, &bps, &name))
	{
		fprintf(stderr, "[str] CdRead2: LBA %ld maps to no host file - "
				"stream stays idle\n", g_seekLBA);
		return 1;
	}
	if (bps != 2336)
	{
		fprintf(stderr, "[str] CdRead2: %s has %d-byte sectors (want raw-XA "
				"2336) - stream stays idle\n", name, bps);
		return 1;
	}
	g_fp = fp;
	g_fileStartLBA = start;
	g_fileSectors = sectors;
	g_fileName = name;
	g_curSector = g_seekLBA - start;
	g_streaming = 1;
	if (trace())
		fprintf(stderr, "[str] streaming %s from sector %ld (mode %03lX)\n",
				name, g_curSector, (unsigned long)mode);
	return 1;
}

extern "C" void StrStream_Stop(void)
{
	if (g_streaming && trace())
		fprintf(stderr, "[str] stopped at sector %ld\n", g_curSector);
	g_streaming = 0;
}

extern "C" void StrStream_Vblank(int vblankHz)
{
	if (!g_streaming)
		return;
	g_acc += 150;
	while (g_acc >= vblankHz && g_streaming)
	{
		if (!deliverNext())
		{
			g_acc = 0;					/* hold: clock pauses, no burst later */
			return;
		}
		g_acc -= vblankHz;
	}
}

extern "C" void StrStream_ResetForTest(void)
{
	resetStream();
	resetRing();
	g_armed = 0;
	g_seekLBA = -1;
	g_trace = -1;
}

/*****************************************************************************/
/*	libcd St* surface (LIBCD.H:257-272).  */

/* fmv.cpp polls it (never set); declared there as a plain C++ `extern long`,
   which is the same symbol as an extern "C" one under the Itanium ABI and a
   different one under MSVC's, so this definition has C++ linkage too.  */
long StCdIntrFlag;

extern "C" void StCdInterrupt(void)
{
	/*	IRQ-context drain on hardware; delivery runs on the pump here.  */
}

extern "C" void StSetRing(u_long *ring_addr, u_long ring_size)
{
	g_ring = (uint8_t *)ring_addr;
	g_ringBytes = (unsigned)ring_size * 2048u;
	resetRing();
	g_armed = 1;
}

extern "C" void StClearRing(void)
{
	resetRing();
}

extern "C" void StUnSetRing(void)
{
	resetRing();
	resetStream();
	g_armed = 0;
	g_ring = NULL;
	g_ringBytes = 0;
}

extern "C" void StSetStream(u_long mode, u_long start_frame, u_long end_frame,
							void (*func1)(), void (*func2)())
{
	(void)mode;
	(void)start_frame;
	(void)end_frame;					/* fmv.cpp: (IS_RGB24, 1, ~0, 0, 0) */
	if (func1 || func2)
		PSYQ_LOG_ONCE_KEYED(0, "[str] StSetStream callbacks are not "
							"implemented\n");
}

extern "C" u_long StGetNext(u_long **addr, u_long **header)
{
	if (!g_armed)
		return 1;

	unsigned long waitStart = 0;
	int waiting = 0;					/* vblank 0 is a legal start time,
										   so a 0 sentinel would never arm */
	for (;;)
	{
		/*	oldest not-yet-handed-out ready frame, in FIFO order  */
		for (int i = g_slotTail; i != g_slotHead; i++)
		{
			FrameSlot *s = slotAt(i);
			if (s->handedOut)
				continue;
			if (!s->ready)
				break;					/* older frames complete first */
			s->handedOut = 1;
			*addr = (u_long *)(g_ring + s->offset);
			*header = (u_long *)&s->hdr;
			return 0;
		}
		if (!g_streaming)
			return 1;					/* EOF / stopped: fast no-data */

		/*	Block for the next frame: the pump advances the sector clock.
			A bounded wait (5s of emulated time) turns corrupt data into a
			skipped movie instead of a wedged boot.  */
		if (!waiting)
		{
			waiting = 1;
			waitStart = Port_VBlankCount();
		}
		else if (Port_VBlankCount() - waitStart > 300)
		{
			fprintf(stderr, "[str] no frame within 5s - giving up\n");
			g_streaming = 0;
			return 1;
		}
		Port_PumpIdle();
	}
}

extern "C" u_long StFreeRing(u_long *base)
{
	for (int i = g_slotTail; i != g_slotHead; i++)
	{
		FrameSlot *s = slotAt(i);
		if (!s->handedOut)
			break;
		if ((u_long *)(g_ring + s->offset) != base)
			continue;
		/*	fmv.cpp frees in FIFO order right after DecDCTvlc3; tolerate a
			gap by only advancing the tail past freed leading slots.  */
		s->handedOut = 2;				/* freed */
		while (g_slotTail != g_slotHead && slotAt(g_slotTail)->handedOut == 2)
			g_slotTail++;
		g_allocTail = (liveSlots() > 0) ? slotAt(g_slotTail)->offset
										: g_allocHead;
		return 0;
	}
	PSYQ_LOG_ONCE_KEYED(3, "[str] StFreeRing: unknown frame %p\n",
						(void *)base);
	return 0;
}
