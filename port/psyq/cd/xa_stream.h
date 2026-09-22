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

/*	engine entry points, called from cd.cpp's command dispatch  */
void XaStream_SetFilter(int file, int chan);
void XaStream_GetFilter(int *file, int *chan);	/* str_stream's SF gate */
void XaStream_SetMode(int mode);
void XaStream_ReadS(const CdlLOC *pos);
void XaStream_Pause(void);
void XaStream_Serve(uint32_t *madr, int sizeWords);	/* CdGetSector body */

/*	once per emulated vblank, from Port_Pump  */
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
int Port_CdXaTrackInfo(FILE **fp, long *startLBA, long *sectors);
int Port_CdFileForLBA(long lba, FILE **fp, long *startLBA, long *sectors,
					  int *bytesPerSector, const char **name);

/*	test support: drop the cached track binding and any playing stream
	(used with cd.cpp's Port_CdRebuildDirForTest after re-pointing
	SBSP_DATA_DIR at a synthetic disc)  */
void XaStream_ResetForTest(void);

#endif
