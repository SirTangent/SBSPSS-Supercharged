/*	Software PS1 SPU - voice mixer (M5).

	Renders the 24 ADPCM voices at 44.1kHz stereo from the register file in
	spu_core.h: per-voice pitch stepping with the hardware's 4-point gaussian
	interpolation (the 512-entry psx-spx table; index = fraction bits 4..11),
	the ADSR envelope evaluated from the raw adsr1/adsr2 register halves
	(cycles = 1 << max(0,shift-11), step = base << max(0,11-shift), the
	exponential quirks), plain L/R volume (0x3FFF = unity), 32-bit
	accumulation, clamp to s16, common master volume.

	Threading: everything here runs under one mutex.  Spu_RenderFrames locks
	internally (it is the audio thread's entry point); all register mutation
	from the game thread goes through Spu_Lock/Spu_Unlock.
*/
#include <stdio.h>
#include <string.h>
#include <mutex>

#include "spu/spu_core.h"

uint8_t g_spuRam[SPU_RAM_SIZE];
SpuVoiceState g_spuVoice[SPU_NVOICES];
int16_t g_spuMasterVolL;
int16_t g_spuMasterVolR;
int16_t g_spuCdVolL;
int16_t g_spuCdVolR;
int g_spuCdMixOn;

namespace
{

std::mutex g_spuMutex;

/*	CD-input ring (M6, stereo since M7): interleaved L,R frames from the
	XA/STR engines.  Speech pushes 18.9kHz mono (duplicated onto both
	channels - arithmetic-identical to the old mono ring); FMV pushes
	37.8kHz stereo pairs and sets the rate.  32768 frames = ~1.7s of
	headroom for speech, ~0.87s for movies, against real-time sector
	lumps.  Free-running unsigned head/tail counting FRAMES; power-of-two
	mask.  All access under the mutex.  */
const unsigned kCdRingSize = 32768;				/* frames (pairs) */
int16_t		g_cdRing[kCdRingSize * 2];
const unsigned kCdRingMask = kCdRingSize - 1;
unsigned	g_cdHead, g_cdTail;					/* frame counters */
uint8_t		g_cdAtv[4] = { 128, 0, 0, 128 };	/* identity: L->L, R->R */
int			g_cdRate = 18900;					/* source frames per second */
unsigned	g_cdPhase;							/* resampler, 0..44099 */
int			g_cdPrevL, g_cdCurL;				/* interpolation taps */
int			g_cdPrevR, g_cdCurR;
int			g_cdOverflowLogged;

/*	psx-spx "Gauss Interpolation Table" - kept in its 16-per-row source
	layout for diffing against the reference.  The four taps sum to ~0x7F7F
	(255/256 of unity), a Nuttall-family cosine window, not a true gaussian. */
const int16_t kGauss[512] = {
	-0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
	0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007, 0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
	0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018, 0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025,
	0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038, 0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050,
	0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F, 0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
	0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7, 0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101,
	0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148, 0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C,
	0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200, 0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
	0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9, 0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392,
	0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441, 0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506,
	0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4, 0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
	0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF, 0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E,
	0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C, 0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8,
	0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63, 0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
	0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB, 0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
	0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4, 0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
	0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B, 0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
	0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37, 0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
	0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389, 0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
	0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E, 0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
	0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D, 0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
	0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509, 0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
	0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00, 0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
	0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0, 0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
	0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651, 0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
	0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F, 0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
	0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7, 0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
	0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397, 0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
	0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684, 0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
	0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886, 0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
	0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F, 0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3,
};

inline uint32_t wrapAddr(uint32_t a)
{
	return a & (SPU_RAM_SIZE - 1);
}

inline int blockFlags(uint32_t blockAddr)
{
	return g_spuRam[wrapAddr(blockAddr + 1)];
}

void decodeCurrentBlock(SpuVoiceState &v)
{
	v.curAddr = wrapAddr(v.curAddr) & ~15u;
	if (blockFlags(v.curAddr) & SPU_ADPCM_LOOP_START)
		v.repeatAddr = v.curAddr;
	SpuAdpcm_DecodeBlock(&g_spuRam[v.curAddr], v.block, &v.hist1, &v.hist2);
}

/*	consume one source sample into the gaussian shift register, advancing to
	the next block (with loop-flag handling) when this one is exhausted  */
void advanceOneSample(SpuVoiceState &v)
{
	v.s3 = v.s2;
	v.s2 = v.s1;
	v.s1 = v.s0;
	v.s0 = v.block[v.blockIdx++];
	if (v.blockIdx < 28)
		return;
	v.blockIdx = 0;

	int flags = blockFlags(v.curAddr);
	if (flags & SPU_ADPCM_LOOP_END)
	{
		v.endx = 1;
		v.curAddr = v.repeatAddr & ~15u;
		if (!(flags & SPU_ADPCM_LOOP_REPEAT))
		{
			/* one-shot end: hardware forces release with level 0 */
			v.envPhase = SPU_ENV_RELEASE;
			v.envLevel = 0;
		}
	}
	else
	{
		v.curAddr = wrapAddr(v.curAddr + 16);
	}
	decodeCurrentBlock(v);
}

/*	one 44.1kHz envelope step.  psx-spx: a phase's (shift, step-base, mode)
	turn into  cycles = 1 << max(0, shift-11),  step = base << max(0,
	11-shift); exponential increase quadruples the wait above 0x6000;
	exponential decrease scales the step by level/0x8000 (arithmetic shift,
	so a negative step never rounds to zero and release always terminates). */
void envTick(SpuVoiceState &v)
{
	if (v.envPhase == SPU_ENV_OFF)
		return;
	if (--v.envCounter > 0)
		return;

	int shift, stepBase;
	int exponential, decreasing;
	switch (v.envPhase)
	{
	case SPU_ENV_ATTACK:
		shift = (v.adsr1 >> 10) & 0x1F;
		stepBase = 7 - ((v.adsr1 >> 8) & 3);
		exponential = (v.adsr1 >> 15) & 1;
		decreasing = 0;
		break;
	case SPU_ENV_DECAY:
		shift = (v.adsr1 >> 4) & 0x0F;
		stepBase = -8;
		exponential = 1;
		decreasing = 1;
		break;
	case SPU_ENV_SUSTAIN:
		shift = (v.adsr2 >> 8) & 0x1F;
		decreasing = (v.adsr2 >> 14) & 1;
		stepBase = decreasing ? -8 + ((v.adsr2 >> 6) & 3)
							  : 7 - ((v.adsr2 >> 6) & 3);
		exponential = (v.adsr2 >> 15) & 1;
		break;
	default: /* SPU_ENV_RELEASE */
		shift = v.adsr2 & 0x1F;
		stepBase = -8;
		exponential = (v.adsr2 >> 5) & 1;
		decreasing = 1;
		break;
	}

	int cycles = 1 << (shift > 11 ? shift - 11 : 0);
	int step = stepBase << (shift < 11 ? 11 - shift : 0);
	if (exponential && !decreasing && v.envLevel > 0x6000)
		cycles *= 4;
	if (exponential && decreasing)
		step = (step * v.envLevel) >> 15;

	v.envLevel += step;
	if (v.envLevel < 0)
		v.envLevel = 0;
	if (v.envLevel > 0x7FFF)
		v.envLevel = 0x7FFF;

	switch (v.envPhase)
	{
	case SPU_ENV_ATTACK:
		if (v.envLevel >= 0x7FFF)
			v.envPhase = SPU_ENV_DECAY;
		break;
	case SPU_ENV_DECAY:
	{
		int sustainLevel = (((v.adsr1 & 0x0F) + 1) << 11);
		if (v.envLevel <= sustainLevel)
			v.envPhase = SPU_ENV_SUSTAIN;
		break;
	}
	case SPU_ENV_RELEASE:
		if (v.envLevel == 0)
			v.envPhase = SPU_ENV_OFF;
		break;
	}

	v.envCounter = cycles;
}

inline int clamp16(int s)
{
	if (s > 32767)
		return 32767;
	if (s < -32768)
		return -32768;
	return s;
}

}	/* namespace */

void Spu_Lock(void)   { g_spuMutex.lock(); }
void Spu_Unlock(void) { g_spuMutex.unlock(); }

namespace
{
/*	Common push body; caller holds the mutex.  */
inline int cdPushFrame(int16_t l, int16_t r)
{
	if (g_cdHead - g_cdTail >= kCdRingMask + 1)
	{
		/*	cannot happen under normal pacing (the producer is real-time);
			a diagnosis line beats silently eating the overrun  */
		if (!g_cdOverflowLogged)
		{
			fprintf(stderr, "[spu] CD-input ring overflow - dropping\n");
			g_cdOverflowLogged = 1;
		}
		return 0;
	}
	unsigned at = (g_cdHead++ & (kCdRingMask)) * 2;
	g_cdRing[at] = l;
	g_cdRing[at + 1] = r;
	return 1;
}
}	/* namespace */

void Spu_CdInPush(const int16_t *mono, int n)
{
	std::lock_guard<std::mutex> lock(g_spuMutex);
	for (int i = 0; i < n; i++)
		if (!cdPushFrame(mono[i], mono[i]))
			return;
}

void Spu_CdInPushStereo(const int16_t *pairs, int nPairs)
{
	std::lock_guard<std::mutex> lock(g_spuMutex);
	for (int i = 0; i < nPairs; i++)
		if (!cdPushFrame(pairs[i * 2], pairs[i * 2 + 1]))
			return;
}

void Spu_CdInSetRate(int hz)
{
	std::lock_guard<std::mutex> lock(g_spuMutex);
	g_cdRate = hz;
}

void Spu_CdInClear(void)
{
	std::lock_guard<std::mutex> lock(g_spuMutex);
	g_cdHead = g_cdTail = 0;
	g_cdPhase = 0;
	g_cdPrevL = g_cdCurL = 0;
	g_cdPrevR = g_cdCurR = 0;
	g_cdRate = 18900;
}

unsigned Spu_CdInCountForTest(void)
{
	std::lock_guard<std::mutex> lock(g_spuMutex);
	return g_cdHead - g_cdTail;
}

void Spu_SetCdAtv(uint8_t v0, uint8_t v1, uint8_t v2, uint8_t v3)
{
	std::lock_guard<std::mutex> lock(g_spuMutex);
	g_cdAtv[0] = v0;
	g_cdAtv[1] = v1;
	g_cdAtv[2] = v2;
	g_cdAtv[3] = v3;
}

void Spu_KeyOn(int voice)
{
	SpuVoiceState &v = g_spuVoice[voice];
	v.curAddr = v.startAddr & ~15u;
	v.blockIdx = 0;
	v.pitchFrac = 0;
	v.hist1 = v.hist2 = 0;
	v.s0 = v.s1 = v.s2 = v.s3 = 0;
	v.envPhase = SPU_ENV_ATTACK;
	v.envLevel = 0;
	v.envCounter = 1;
	v.endx = 0;
	decodeCurrentBlock(v);
}

void Spu_KeyOff(int voice)
{
	SpuVoiceState &v = g_spuVoice[voice];
	if (v.envPhase != SPU_ENV_OFF)
	{
		v.envPhase = SPU_ENV_RELEASE;
		v.envCounter = 1;
	}
}

void Spu_RenderFrames(int16_t *stereoOut, int nFrames)
{
	std::lock_guard<std::mutex> lock(g_spuMutex);

	for (int f = 0; f < nFrames; f++)
	{
		int sumL = 0;
		int sumR = 0;

		for (int i = 0; i < SPU_NVOICES; i++)
		{
			SpuVoiceState &v = g_spuVoice[i];
			if (v.envPhase == SPU_ENV_OFF)
				continue;

			uint32_t step = v.pitch;
			if (step > 0x4000)
				step = 0x4000;
			v.pitchFrac += step;
			while (v.pitchFrac >= 0x1000)
			{
				v.pitchFrac -= 0x1000;
				advanceOneSample(v);
			}

			int gi = (v.pitchFrac >> 4) & 0xFF;
			int s = (kGauss[0x0FF - gi] * v.s3) >> 15;
			s += (kGauss[0x1FF - gi] * v.s2) >> 15;
			s += (kGauss[0x100 + gi] * v.s1) >> 15;
			s += (kGauss[0x000 + gi] * v.s0) >> 15;

			envTick(v);
			s = (s * v.envLevel) >> 15;

			sumL += (s * v.volL) >> 14;
			sumR += (s * v.volR) >> 14;
		}

		/*	CD input: consume at the source rate (18.9kHz speech, 37.8kHz
			STR) regardless of the mix gate (the CD keeps playing whether
			or not the SPU mixes it, and a muted stream must not back up
			the ring), linear-interpolated per channel up to 44.1kHz.  Mono
			pushes duplicate onto both channels, keeping this arithmetic-
			identical to the M6 mono ring; then the CdMix ATV matrix (128 =
			unity per side - the game's all-127 sums L+R onto both outputs,
			~2x for duplicated mono, as on hardware) and the common CD
			volume (0x7FFF ~ unity), summed BEFORE the master multiply like
			the real SPU.  */
		g_cdPhase += (unsigned)g_cdRate;
		while (g_cdPhase >= 44100)
		{
			g_cdPhase -= 44100;
			g_cdPrevL = g_cdCurL;
			g_cdPrevR = g_cdCurR;
			if (g_cdTail != g_cdHead)
			{
				unsigned at = (g_cdTail++ & (kCdRingMask)) * 2;
				g_cdCurL = g_cdRing[at];
				g_cdCurR = g_cdRing[at + 1];
			}
			else
				g_cdCurL = g_cdCurR = 0;
		}
		if (g_spuCdMixOn)
		{
			/*	64-bit: the tap difference reaches +-65535 and the phase
				44099, a product that overflows `long` on Windows (32 bits on
				both ABIs) - a full-swing step would wrap into a click  */
			int sL = g_cdPrevL + (int)((g_cdCurL - g_cdPrevL) * (int64_t)g_cdPhase / 44100);
			int sR = g_cdPrevR + (int)((g_cdCurR - g_cdPrevR) * (int64_t)g_cdPhase / 44100);
			int cdL = (sL * g_cdAtv[0] + sR * g_cdAtv[2]) >> 7;
			int cdR = (sL * g_cdAtv[1] + sR * g_cdAtv[3]) >> 7;
			/*	64-bit for the volume step: cdL/cdR reach +-130,555 with the
				ATV matrix at full scale, which times a 0x7FFF CD volume
				overflows a 32-bit int.  The game's own 127/32000 programming
				lands inside the range but within 1% of it, and
				Spu_SetCdAtv/SpuSetCommonAttr accept the full register range. */
			sumL += (int)(((int64_t)cdL * g_spuCdVolL) >> 15);
			sumR += (int)(((int64_t)cdR * g_spuCdVolR) >> 15);
		}

		/*	clamp BEFORE the master volume - the hardware's order, and
			DuckStation's: 24 voices plus the CD term can sum to ~920k, which
			times 0x3FFF overflows an int and wraps to the opposite rail.
			Clamped to s16 first, the master step is a 16x16 product.  */
		sumL = (clamp16(sumL) * g_spuMasterVolL) >> 14;
		sumR = (clamp16(sumR) * g_spuMasterVolR) >> 14;
		stereoOut[f * 2 + 0] = (int16_t)clamp16(sumL);
		stereoOut[f * 2 + 1] = (int16_t)clamp16(sumR);
	}
}
