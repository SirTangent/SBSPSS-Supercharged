/*	XMPlayer reimplementation - shared internal state (M5).

	SCEE's XMPLAY.LIB shipped binary-only; this player is written from
	tools/mod/Calls.txt + XMPLAY.H + the game's call sites.  The "file
	header" (XM_ID) and "song" structures whose sizes XM_GetFileHeaderSize /
	XM_GetSongSize report are defined here - the game allocates them and
	hands the storage over via XM_SetFileHeaderAddress/XM_SetSongAddress.

	The .PXM format (XM2PSX.EXE output) is a FastTracker II .XM whose
	version word (offset 0x3A) is 0xDDBA, whose pattern data is repacked
	sparse ([channel byte][standard XM packed slot]... 0xFF ends each row),
	and whose sample payloads are stripped into the .VB (headerless SPU
	ADPCM bodies indexed by the .VH's 256-entry size table, 8-byte units).
	The PXM buffer stays resident in game memory for the life of the mod
	slot, so the module keeps pointers into it instead of copying.
*/
#ifndef PORT_XM_STATE_H
#define PORT_XM_STATE_H

#include <stdint.h>

#define XM_MAX_HEADER_SLOTS		8	/* "max 8" per Calls.txt (game uses 2) */
#define XM_MAX_SONG_SLOTS		24	/* "max 24" (game registers all 24) */
#define XM_MAX_VABS				8	/* XM_VABInit returns 0..7 */
#define XM_MAX_PATTERNS			256
#define XM_MAX_INSTRUMENTS		128
#define XM_MAX_VAGS				256	/* the .VH size table has 256 entries */
#define XM_MAX_MODULE_CHANNELS	24

#define XM_PXM_VERSION			0xDDBA
#define XM_FT2_VERSION			0x0104

struct XmPatternRef
{
	const uint8_t	*data;		/* sparse rows, in the resident PXM */
	uint16_t		rows;
	uint16_t		packedSize;
};

struct XmInstrumentRef
{
	const uint8_t	*hdr;		/* XM instrument header in the PXM */
	const uint8_t	*sampleHdr;	/* first 40-byte sample header (0 if none) */
	uint16_t		numSamples;
	uint16_t		vagBase;	/* VAG index of this instrument's sample 0 */
};

/*	the "file header" structure - one per InitXMData'd module (XM_ID)  */
struct XmModule
{
	int				inUse;
	const uint8_t	*base;		/* the resident PXM buffer */
	int				numChannels;
	int				songLength;	/* pattern order table length */
	int				restartPos;
	int				numPatterns;
	int				numInstruments;
	int				linearFreq;	/* header flags bit 0 */
	int				defSpeed;
	int				defBPM;
	int				panType;	/* XM_UseXMPanning / XM_UseS3MPanning */
	const uint8_t	*orderTable;
	XmPatternRef	pat[XM_MAX_PATTERNS];
	XmInstrumentRef	ins[XM_MAX_INSTRUMENTS];
};

/*	per-channel sequencer state (commit "XM sequencer" fills the engine)  */
struct XmChannelState
{
	uint8_t		note;			/* 1..96 */
	uint8_t		instr;			/* 1-based instrument */
	int16_t		volume;			/* 0..64 */
	int16_t		pan;			/* 0..255 */
	int32_t		period;			/* current (with vibrato) */
	int32_t		basePeriod;		/* without vibrato */
	int32_t		targetPeriod;	/* tone portamento goal */
	int32_t		finetune;

	uint8_t		effect, param;
	uint8_t		memPortaUp, memPortaDown, memTonePorta, memVolSlide;
	uint8_t		memFinePortaUp, memFinePortaDown;
	uint8_t		memEFineVolUp, memEFineVolDown;
	uint8_t		memPanSlide, memRetrig, memSampleOfs;
	uint8_t		vibPos, vibSpeed, vibDepth, vibWave;
	uint8_t		tremPos, tremSpeed, tremDepth, tremWave;
	uint8_t		retrigTick;
	uint8_t		patLoopRow, patLoopCount;
	uint8_t		delayedNote, delayTick;
	uint8_t		rowVolCol;		/* this row's volume-column byte */

	int			volEnvPos, panEnvPos;
	int			autoVibPos, autoVibSweep;	/* instrument vibrato, own clock */
	int			fadeout;		/* 65536 down to 0 */
	uint8_t		keyOff;
	uint8_t		active;			/* channel has a sounding sample */
	int			vagIndex;

	/* last programmed SPU register values (write-through cache) */
	uint16_t	spuPitch;
	int16_t		spuVolL, spuVolR;
	uint8_t		spuKeyed;
};

/*	the "song" structure - one per XM_SetSongAddress'd slot (Song_ID)  */
struct XmSongState
{
	int				inUse;
	int				vabId;
	int				xmId;
	int				firstCh;
	int				numCh;
	int				loop;			/* XM_Loop / XM_NoLoop */
	int				playMask;
	int				songType;		/* XM_Music / XM_SFX */
	int				sfxPattern;		/* SFX: pattern; Music: start position */

	int				songPos, row, tick;
	int				speed, bpm;
	int				tickAccumHz;	/* fractional-tick accumulator */
	int				patDelay;
	int				pendingBreakRow;	/* -1 = none */
	int				pendingJumpPos;		/* -1 = none */

	int				status;			/* XM_STOPPED / XM_PLAYING / XM_PAUSED */
	int				finished;		/* nonzero once a NoLoop song/SFX ends */
	int				masterVol;		/* 0..128 */
	int				masterPan;		/* -128..127 */

	const XmModule	*mod;
	XmChannelState	ch[XM_MAX_MODULE_CHANNELS];
};

struct XmVab
{
	int			inUse;
	uint32_t	spuBase;		/* one SpuMalloc'd region for the whole VB */
	uint32_t	spuBytes;
	int			numVags;
	uint32_t	vagAddr[XM_MAX_VAGS];	/* SPU byte address per VAG index */
	uint32_t	vagBytes[XM_MAX_VAGS];
};

/* registries (xm_data.cpp) */
extern XmModule *g_xmHeaderSlot[XM_MAX_HEADER_SLOTS];
extern int g_xmHeaderCount;
extern XmSongState *g_xmSongSlot[XM_MAX_SONG_SLOTS];
extern int g_xmSongCount;
extern XmVab g_xmVab[XM_MAX_VABS];
extern int g_xmTickHz;			/* 60 (XM_NTSC) or 50 (XM_PAL) */
extern int g_xmStereo;			/* XM_SetStereo/XM_SetMono */
extern "C" int XM_TickClockMismatch(void);	/* g_xmTickHz != Port_VBlankHz() - warned by XM_OnceOffInit (M8) */

#endif
