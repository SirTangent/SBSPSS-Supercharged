/*	XA streaming engine (M6): real sector delivery for CXAStream's speech,
	replacing the M4 end-immediately terminator hack.

	Model.  CdlReadS starts a linear walk over TRACK1.IXA's raw 2336-byte
	sectors from the given position, clocked by EMULATED vblanks at the
	double-speed 150 sectors/s (2.5 sectors per 60Hz vblank via a fractional
	accumulator, exactly 3 per 50Hz PAL vblank) - never by the wall clock, so the M5 --dump-audio
	determinism contract extends to XA.  SBSP_CD_PACE only governs CdRead
	data loads; real-time audio cannot be delivered "instantly".

	Routing per sector, matching the hardware's RT+SF mode:
	  - audio sectors (submode bit 2) matching the CdlSetfilter file/chan
	    are decoded (xa_adpcm.cpp) into the SPU CD-input ring and are NEVER
	    seen by the CPU; non-matching audio is discarded.  The disc's
	    1-in-32 interleave makes the matching channel exactly real-time
	    18.9kHz mono.
	  - non-audio sectors (zero pads, the ID-352 terminators) are delivered
	    to the CPU unconditionally: staged for CdGetSector and announced
	    through the registered ready callback as CdlDataReady.  That serves
	    every game-side need at once: the own-channel terminator ends the
	    stream (cdxa.cpp:81-89), the slot-0 zero sectors tick its
	    CurrentSector += 32 pause/resume position (Track==0), and other
	    channels' terminators are ignored by its ID/Track test.

	The ready callback runs INSIDE the delivery (pump/vblank context) and
	issues CdlPause + CdGetSector - both non-blocking - so the playing flag
	is re-checked after every delivery: the end-of-stream pause lands
	mid-burst.  While FMV has the callback cleared (fmv.cpp:186,267) the
	stream holds in place rather than dropping sectors into nowhere.

	CdRead coexistence: deliberately fully independent.  The virtual disc
	has no shared drive head - a mid-speech BIGLUMP load must neither wedge
	isSpeechPlaying() nor corrupt the load, and only the game's own
	Pause/Stop/end logic should end a stream.
*/
#include <stdlib.h>
#include <string.h>

#include "xa_stream.h"
#include "xa_adpcm.h"
#include "str_stream.h"
#include "spu/spu_core.h"

namespace
{

int		g_filterFile = 1;
int		g_filterChan = -1;
int		g_mode;						/* last CdlSetmode byte, recorded only */
int		g_playing;
long	g_curSector;				/* next sector index within TRACK1.IXA */
long	g_acc;						/* fractional sector clock, units of hz */
int32_t	g_h1, g_h2;					/* ADPCM history for the current stream */

uint8_t	g_staged[4 + 2336];			/* Size1 view: header + subheader + data */
int		g_stagedValid;

FILE	*g_fp;
long	g_startLBA, g_sectors;
int		g_bound;					/* 0 not yet, 1 ok, -1 file absent */

int		g_endLogged;
int		g_codingLogged;
int		g_trace = -1;				/* SBSP_XA_LOG */

int trace(void)
{
	if (g_trace < 0)
	{
		const char *e = getenv("SBSP_XA_LOG");
		g_trace = (e && *e && *e != '0');
	}
	return g_trace;
}

int bindTrack(void)
{
	if (!g_bound)
		g_bound = Port_CdXaTrackInfo(&g_fp, &g_startLBA, &g_sectors) ? 1 : -1;
	return g_bound > 0;
}

void deliverNext(void)
{
	if (g_curSector >= g_sectors)
	{
		if (!g_endLogged)
		{
			fprintf(stderr, "[xa] stream ran past the end of TRACK1.IXA "
							"(chan %d) - pausing\n", g_filterChan);
			g_endLogged = 1;
		}
		g_playing = 0;
		return;
	}

	uint8_t sec[2336];
	fseek(g_fp, g_curSector * 2336L, SEEK_SET);
	if (fread(sec, 1, sizeof(sec), g_fp) != sizeof(sec))
	{
		fprintf(stderr, "[xa] short read at sector %ld - pausing\n", g_curSector);
		g_playing = 0;
		return;
	}
	long thisLBA = g_startLBA + g_curSector;
	g_curSector++;

	int file    = sec[0];
	int chan    = sec[1];
	int submode = sec[2];
	int coding  = sec[3];

	if (submode & 0x04)
	{
		/*	RT audio: the filter selects what reaches the ADPCM decoder;
			everything else on the disc's other 31 slots is discarded  */
		if (file == g_filterFile && chan == g_filterChan)
		{
			if (coding != 0x04)
			{
				if (!g_codingLogged)
				{
					fprintf(stderr, "[xa] unsupported coding 0x%02X (only mono "
									"18.9kHz 4-bit is implemented) - silence\n",
							coding);
					g_codingLogged = 1;
				}
			}
			else
			{
				static int16_t pcm[XA_SECTOR_SAMPLES];
				XaAdpcm_DecodeSector4bitMono(sec + 8, pcm, &g_h1, &g_h2);
				Spu_CdInPush(pcm, XA_SECTOR_SAMPLES);
			}
		}
		return;
	}

	/*	data sector -> CPU: stage the Size1 (2340-byte) view and announce it.
		Word 3 of what CdGetSector serves = user-data bytes 0-3, exactly the
		ID + Track halfwords XACDReadyCallback tests.  */
	CdlLOC loc;
	CdIntToPos((int)thisLBA, &loc);
	g_staged[0] = loc.minute;
	g_staged[1] = loc.second;
	g_staged[2] = loc.sector;
	g_staged[3] = 2;						/* mode 2 */
	memcpy(g_staged + 4, sec, 2336);
	g_stagedValid = 1;

	if (trace() && sec[8] == 0x60 && sec[9] == 0x01)
		fprintf(stderr, "[xa] terminator sector %ld (chan %d) delivered\n",
				thisLBA - g_startLBA, (sec[10] | (sec[11] << 8)) >> 10 & 31);

	static u_char result[8];
	g_cdReadyCallback(CdlDataReady, result);
	g_stagedValid = 0;						/* served only during the callback */
}

}	/* namespace */

/*****************************************************************************/

void XaStream_ResetForTest(void)
{
	g_playing = 0;
	g_bound   = 0;
	g_fp      = NULL;
	g_endLogged = g_codingLogged = 0;
}

void XaStream_SetFilter(int file, int chan)
{
	g_filterFile = file;
	g_filterChan = chan;
}

void XaStream_GetFilter(int *file, int *chan)
{
	*file = g_filterFile;
	*chan = g_filterChan;
}

void XaStream_SetMode(int mode)
{
	g_mode = mode;		/* 0xE8 (Speed|RT|SF|Size1) on the XA path; data
						   loads set their own modes - nothing to act on */
}

void XaStream_ReadS(const CdlLOC *pos)
{
	int lba = CdPosToInt((CdlLOC *)pos);

	if (!bindTrack())
	{
		fprintf(stderr, "[xa] CdlReadS but TRACK1.IXA is absent - "
						"rebuild the data (port/build-data.cmd)\n");
		g_playing = 0;
		return;
	}
	long s = lba - g_startLBA;
	if (s < 0 || s >= g_sectors)
	{
		fprintf(stderr, "[xa] CdlReadS out of range: LBA %d (track %ld..%ld) "
						"- ignored\n", lba, g_startLBA, g_startLBA + g_sectors);
		g_playing = 0;
		return;
	}

	if (trace())
		fprintf(stderr, "[xa] stream start: sector %ld chan %d\n", s, g_filterChan);

	g_curSector = s;
	g_acc       = 0;
	g_h1 = g_h2 = 0;
	g_endLogged = 0;
	g_playing   = 1;
}

void XaStream_Pause(void)
{
	/*	hardware pause cuts the ADPCM feed immediately; the <=213ms already
		decoded into the ring would otherwise linger past a menu pause.
		Resume re-reads from the game's own coarse 32-sector position, so
		the discarded tail stays inside its position slack.  */
	if (g_playing && trace())
		fprintf(stderr, "[xa] pause at sector %ld\n", g_curSector);
	g_playing = 0;
	Spu_CdInClear();
}

void XaStream_Serve(uint32_t *madr, int sizeWords)
{
	uint8_t *dst = (uint8_t *)madr;
	int bytes = sizeWords * 4;
	memset(dst, 0, bytes);
	if (g_stagedValid)
		memcpy(dst, g_staged,
			   bytes < (int)sizeof(g_staged) ? bytes : (int)sizeof(g_staged));
}

/*	Once per emulated vblank, from Port_Pump (pump.cpp) - BEFORE
	Port_AudioVBlank, so a dump-mode vblank's sectors are decoded before its
	audio frames render (the determinism contract).  150 sectors/s against
	the vblank rate: at 60Hz the accumulator delivers 2-3 sectors per call,
	at 50Hz (EUR) exactly 3.  */
extern "C" void Port_CdVblank(int vblankHz)
{
	/*	The STR engine ticks FIRST and unconditionally: the hold below
		exists because FMV clears the ready callback, and it must not gate
		the movie stream itself (str_stream.cpp, M7).  */
	StrStream_Vblank(vblankHz);

	if (!g_playing || !g_cdReadyCallback)
		return;					/* callback cleared (FMV): hold in place */

	g_acc += 150;
	while (g_acc >= vblankHz && g_playing && g_cdReadyCallback)
	{
		g_acc -= vblankHz;
		deliverNext();			/* may clear g_playing via the callback's own
								   CdlPause, or (FMV) drop the registration -
								   both re-checked before the next delivery of
								   this 2-3 sector burst */
	}
}
