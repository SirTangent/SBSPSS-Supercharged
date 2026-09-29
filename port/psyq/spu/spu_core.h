/*	Software PS1 SPU - shared internal state (M5).

	Sound RAM is the real thing: a flat 512KB byte array addressed exactly as
	the hardware is (the game and the XM player only ever pass integer SPU
	addresses around, so no game-visible pointer ever aliases this).  The
	voice register file mirrors what SpuSetVoiceAttr / the XM player program.
	Unlike the GPU, this state is shared with the SDL audio thread - the
	mixer (spu_core.cpp) owns the lock: every mutation of voices/RAM/master
	volume must happen between Spu_Lock()/Spu_Unlock(), and Spu_RenderFrames
	takes the lock itself.
*/
#ifndef PORT_SPU_CORE_H
#define PORT_SPU_CORE_H

#include <stdint.h>

#define SPU_RAM_SIZE	(512 * 1024)
#define SPU_NVOICES		24

extern uint8_t g_spuRam[SPU_RAM_SIZE];

/* envelope phases (envPhase) */
enum
{
	SPU_ENV_OFF = 0,
	SPU_ENV_ATTACK,
	SPU_ENV_DECAY,
	SPU_ENV_SUSTAIN,
	SPU_ENV_RELEASE,
};

struct SpuVoiceState
{
	/* programmed registers */
	uint32_t	startAddr;		/* byte address in g_spuRam (16-byte blocks) */
	uint32_t	repeatAddr;		/* loop address (flag bit 2 rewrites it) */
	uint16_t	pitch;			/* 0x1000 = 44100 Hz, hardware max 0x4000 */
	int16_t		volL, volR;		/* 0..0x3FFF plain volume - sweep never used */
	uint16_t	adsr1, adsr2;	/* raw hardware ADSR register halves */

	/* runtime (owned by the mixer, valid only under the lock) */
	int			envPhase;
	int			envLevel;		/* 0..0x7FFF */
	int			envCounter;		/* samples until the next envelope step */
	uint32_t	curAddr;		/* current 16-byte block */
	int			blockIdx;		/* next sample to consume, 0..27 */
	uint32_t	pitchFrac;		/* 12-bit fractional sample position */
	int16_t		block[28];		/* decoded current block */
	int16_t		hist1, hist2;	/* ADPCM filter history */
	int16_t		s0, s1, s2, s3;	/* gaussian taps, s0 newest */
	int			endx;			/* reached a LOOP_END block since key-on */
};

extern SpuVoiceState g_spuVoice[SPU_NVOICES];

/*	common (master) volume, 0x3FFF = unity - written by
	SpuSetCommonMasterVolume under the lock  */
extern int16_t g_spuMasterVolL, g_spuMasterVolR;

/*	CD-input bus (XA speech / FMV audio).  The XA engine (cd/xa_stream.cpp)
	pushes decoded 18.9kHz mono into a ring; Spu_RenderFrames resamples it
	7/3 to 44.1kHz, routes it through the CdMix ATV matrix, scales by these
	volumes (0x7FFF ~ unity) and sums it in before the master multiply.
	Mix-off gates only the contribution - the ring is consumed either way,
	like the hardware (the CD keeps playing whether or not the SPU mixes
	it), so a muted stream cannot back up the ring.  */
extern int16_t g_spuCdVolL, g_spuCdVolR;
extern int g_spuCdMixOn;

void Spu_CdInPush(const int16_t *mono, int n);	/* takes the lock itself;
												   mono = both channels */
void Spu_CdInPushStereo(const int16_t *pairs, int nPairs);	/* L,R frames (M7 STR) */
void Spu_CdInSetRate(int hz);					/* source rate: 18900 (default,
												   speech) or 37800 (STR).
												   Spu_CdInClear resets it. */
void Spu_CdInClear(void);						/* also resets the resampler */
unsigned Spu_CdInCount(void);					/* frames queued (XA pre-roll,
												   xa_test, str_test) */
void Spu_SetCdAtv(uint8_t v0, uint8_t v1, uint8_t v2, uint8_t v3);
												/* CdlATV: v0 L->L, v1 L->R,
												   v2 R->L, v3 R->R; 128=unity */

/*	spu_adpcm.cpp: decode one 16-byte SPU ADPCM block into 28 PCM samples.
	byte 0 = shift (low nibble; 13..15 act as 9) | filter (high nibble, 0..4),
	byte 1 = loop flags, bytes 2..15 = 4-bit samples, low nibble first.
	hist1/hist2 are the two previous output samples and carry across
	consecutive blocks of one voice.  Pure function - no SPU state.  */
void SpuAdpcm_DecodeBlock(const uint8_t *block, int16_t *out28,
						  int16_t *hist1, int16_t *hist2);

/*	loop flags in block byte 1  */
#define SPU_ADPCM_LOOP_END		1	/* jump to repeatAddr after this block */
#define SPU_ADPCM_LOOP_REPEAT	2	/* with END: keep playing; without: mute */
#define SPU_ADPCM_LOOP_START	4	/* set repeatAddr to this block */

/*	spu_core.cpp: the mixer.  Key-on latches startAddr and restarts the
	envelope from ATTACK; key-off enters RELEASE; a released voice frees
	itself (envPhase -> SPU_ENV_OFF) when its envelope reaches zero.
	Callers must hold the lock for KeyOn/KeyOff and any direct state pokes;
	RenderFrames locks internally and renders interleaved stereo s16.  */
void Spu_Lock(void);
void Spu_Unlock(void);
void Spu_KeyOn(int voice);
void Spu_KeyOff(int voice);
void Spu_RenderFrames(int16_t *stereoOut, int nFrames);

#endif
