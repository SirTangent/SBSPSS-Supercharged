/*	XA streaming engine (M6) - see xa_stream.cpp.  cd.cpp delegates the
	stream-relevant CD commands here; the engine owns the per-vblank sector
	clock, the de-interleave/filter routing, the ADPCM decode into the SPU
	CD-input ring, and the staged sector CdGetSector serves.  */
#ifndef PORT_XA_STREAM_H
#define PORT_XA_STREAM_H

#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <libcd.h>

/*	Silence queued ahead of a speech stream's first audio sector (issue
	#59): 756 frames = 40ms at 18.9kHz, more than the <1 vblank a sector
	can arrive late by being quantized to vblanks (315 frames at 60Hz, 378
	at 50Hz).  What is left over (441 / 378 frames, ~1030 / ~880 output
	frames at 44.1kHz) is the headroom for a playback device pulling a
	period at a time.  So the ring does not run dry mid-line in a
	--dump-audio run (one vblank per render; xa_test streams every slot at
	both rates that way) or at the default device period (~480 frames),
	but an sbsp.ini audio_buffer_frames above ~880 (50Hz) / ~1030 (60Hz)
	can still underrun mid-line in live play.  */
#define XA_PREROLL_FRAMES	756

/*	engine entry points, called from cd.cpp's command dispatch  */
void XaStream_SetFilter(int file, int chan);
void XaStream_GetFilter(int *file, int *chan);	/* str_stream's SF gate */
void XaStream_SetMode(int mode);
void XaStream_ReadS(const CdlLOC *pos);
void XaStream_Pause(void);
void XaStream_Serve(uint32_t *madr, int sizeWords);	/* CdGetSector body */

/*	once per emulated vblank, from the pump (host/pump.cpp)  */
extern "C" void Port_CdVblank(int vblankHz);

/*	The CD ready callback, typed the way the game actually defines it.
	libcd.h's CdlCB types the first argument u_char, but the one handler the
	game registers with CdReadyCallback - sound/cdxa.cpp's XACDReadyCallback;
	fmv.cpp only ever clears it - is `f(int Intr, u_char *result)` cast to
	CdlCB and switches on all 32 bits.  (The read callback, psxboot.cpp's, is
	a genuine CdlCB and cd.cpp keeps it as one.)  MIPS and i686 hand a u_char
	over in a full, zero-extended word, so that works there; the x64 ABI leaves the
	upper bits of the register undefined for a u_char parameter - the handler
	then missed CdlDataReady, never saw a terminator, and speech "played"
	forever (M9: found by the x64 A/B on the gameover_continue route).
	Storing the pointer at the handler's own signature makes every call site
	correct by construction; cd.cpp converts once, where the cast is made.  */
typedef void (*PortCdCB)(int, u_char *);

/*	provided by cd.cpp  */
extern PortCdCB g_cdReadyCallback;
/*	The CdRead data clock (issue #67): ticked by Port_CdVblank once per
	emulated vblank.  Port_CdPaced is SBSP_CD_PACE != 0 (the default; off
	with --no-cd-pace): CdReadSync waits for the emulated drive.  */
extern "C" void Port_CdDataVblank(int vblankHz);
extern "C" int  Port_CdPaced(void);
int Port_CdXaTrackInfo(FILE **fp, long *startLBA, long *sectors);
int Port_CdFileForLBA(long lba, FILE **fp, long *startLBA, long *sectors,
					  int *bytesPerSector, const char **name);

/*	test support: drop the cached track binding and any playing stream
	(used with cd.cpp's Port_CdRebuildDirForTest after re-pointing
	SBSP_DATA_DIR at a synthetic disc)  */
void XaStream_ResetForTest(void);

#endif
