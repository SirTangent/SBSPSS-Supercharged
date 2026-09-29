/*	Unit tests for the SPU voice mixer (port/psyq/spu/spu_core.cpp).
	Property checks over a synthetic looping square-wave sample: silence
	before key-on, pan sidedness, pitch halving doubles the waveform period,
	ADSR release decays to silence and frees the voice, one-shot samples
	(LOOP_END without REPEAT) self-mute, master volume scales the output,
	an overdriven mix clips (clamp, then master) instead of wrapping, and
	rendering is deterministic for identical programmed state.
*/
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <libspu.h>

#include "spu/spu_core.h"

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*	±28672 square wave, period 8 samples, as two looping ADPCM blocks
	(filter 0, shift 0: nibble 0x7 = +28672, 0x9 = -28672)  */
static void writeSquareSample(uint32_t addr)
{
	static const uint8_t b0Data[14] = { 0x77, 0x77, 0x99, 0x99, 0x77, 0x77,
										0x99, 0x99, 0x77, 0x77, 0x99, 0x99,
										0x77, 0x77 };
	static const uint8_t b1Data[14] = { 0x99, 0x99, 0x77, 0x77, 0x99, 0x99,
										0x77, 0x77, 0x99, 0x99, 0x77, 0x77,
										0x99, 0x99 };
	uint8_t *p = &g_spuRam[addr];
	p[0] = 0x00;
	p[1] = SPU_ADPCM_LOOP_START;
	memcpy(p + 2, b0Data, 14);
	p[16] = 0x00;
	p[17] = SPU_ADPCM_LOOP_END | SPU_ADPCM_LOOP_REPEAT;
	memcpy(p + 18, b1Data, 14);
}

/*	single one-shot block: 28 samples of +28672, END without REPEAT  */
static void writeOneShotSample(uint32_t addr)
{
	uint8_t *p = &g_spuRam[addr];
	p[0] = 0x00;
	p[1] = SPU_ADPCM_LOOP_END;
	memset(p + 2, 0x77, 14);
}

static void resetSpu(void)
{
	memset(g_spuRam, 0, SPU_RAM_SIZE);
	memset(g_spuVoice, 0, sizeof(SpuVoiceState) * SPU_NVOICES);
	g_spuMasterVolL = 0x3FFF;
	g_spuMasterVolR = 0x3FFF;
}

/*	program + key a voice with an effectively instant envelope
	(attack shift 0 step +7, sustain level max, sustain pinned)  */
static void keyVoice(int i, uint32_t addr, int pitch, int volL, int volR)
{
	SpuVoiceState &v = g_spuVoice[i];
	memset(&v, 0, sizeof(v));
	v.startAddr = addr;
	v.repeatAddr = addr;
	v.pitch = (uint16_t)pitch;
	v.volL = (int16_t)volL;
	v.volR = (int16_t)volR;
	v.adsr1 = 0x000F;
	v.adsr2 = 0x0000;
	Spu_KeyOn(i);
}

static int countTransitions(const int16_t *stereo, int nFrames, int ch)
{
	int n = 0;
	int prev = 0;
	for (int i = 0; i < nFrames; i++)
	{
		int s = stereo[i * 2 + ch];
		if (s == 0)
			continue;
		if (prev != 0 && ((s > 0) != (prev > 0)))
			n++;
		prev = s;
	}
	return n;
}

static int peakAbs(const int16_t *stereo, int nFrames, int ch)
{
	int peak = 0;
	for (int i = 0; i < nFrames; i++)
	{
		int a = std::abs((int)stereo[i * 2 + ch]);
		if (a > peak)
			peak = a;
	}
	return peak;
}

int main()
{
	static int16_t buf[8192 * 2];
	static int16_t buf2[8192 * 2];

	/* --- all voices off: silence ------------------------------------------ */
	{
		resetSpu();
		memset(buf, 0x5A, sizeof(buf));
		Spu_RenderFrames(buf, 256);
		bool silent = true;
		for (int i = 0; i < 256 * 2; i++)
			silent = silent && buf[i] == 0;
		check(silent, "no keyed voices -> silence");
	}

	/* --- keyed voice: sound on the panned side only ----------------------- */
	{
		resetSpu();
		writeSquareSample(0x1000);
		keyVoice(0, 0x1000, 0x1000, 0x3FFF, 0);
		Spu_RenderFrames(buf, 1024);
		check(peakAbs(buf, 1024, 0) > 20000,
			  "hard-left voice: strong left signal");
		check(peakAbs(buf, 1024, 1) == 0, "hard-left voice: right silent");
		check(g_spuVoice[0].envPhase == SPU_ENV_SUSTAIN,
			  "instant envelope reached sustain");
	}

	/* --- pitch halving doubles the period --------------------------------- */
	{
		resetSpu();
		writeSquareSample(0x1000);
		keyVoice(0, 0x1000, 0x1000, 0x3FFF, 0x3FFF);
		Spu_RenderFrames(buf, 64);						/* warm-up */
		Spu_RenderFrames(buf, 4096);
		int tFull = countTransitions(buf, 4096, 0);

		keyVoice(0, 0x1000, 0x0800, 0x3FFF, 0x3FFF);
		Spu_RenderFrames(buf, 64);
		Spu_RenderFrames(buf, 4096);
		int tHalf = countTransitions(buf, 4096, 0);

		/*	period 8 at 0x1000 -> ~1024 transitions in 4096 frames  */
		check(tFull > 920 && tFull < 1130,
			  "pitch 0x1000: ~2 transitions per 8 samples");
		check(tHalf > 460 && tHalf < 565, "pitch 0x0800: half the transitions");
		if (!(tFull > 920 && tFull < 1130 && tHalf > 460 && tHalf < 565))
			std::printf("  (transitions: full=%d half=%d)\n", tFull, tHalf);
	}

	/* --- key-off: release ramps to silence and frees the voice ------------ */
	{
		resetSpu();
		writeSquareSample(0x1000);
		keyVoice(0, 0x1000, 0x1000, 0x3FFF, 0x3FFF);
		Spu_RenderFrames(buf, 256);
		g_spuVoice[0].adsr2 = 0x0005;	/* linear release, shift 5: -512/sample */
		Spu_KeyOff(0);
		Spu_RenderFrames(buf, 256);
		check(peakAbs(buf, 32, 0) > 0, "release: audible tail after key-off");
		bool tailSilent = true;
		for (int i = 128; i < 256; i++)
			tailSilent = tailSilent && buf[i * 2] == 0 && buf[i * 2 + 1] == 0;
		check(tailSilent, "release: silent within ~64 samples at shift 5");
		check(g_spuVoice[0].envPhase == SPU_ENV_OFF,
			  "release: voice freed at level 0");
	}

	/* --- one-shot sample (END without REPEAT) self-mutes ------------------ */
	{
		resetSpu();
		writeOneShotSample(0x2000);
		keyVoice(0, 0x2000, 0x1000, 0x3FFF, 0x3FFF);
		Spu_RenderFrames(buf, 128);
		check(peakAbs(buf, 32, 0) > 0, "one-shot: audible while block plays");
		bool tailSilent = true;
		for (int i = 40; i < 128; i++)
			tailSilent = tailSilent && buf[i * 2] == 0;
		check(tailSilent, "one-shot: silent after the 28-sample block");
		check(g_spuVoice[0].envPhase == SPU_ENV_OFF, "one-shot: voice freed");
		check(g_spuVoice[0].endx == 1, "one-shot: ENDX latched");
	}

	/* --- master volume scales the mix ------------------------------------- */
	{
		resetSpu();
		writeSquareSample(0x1000);
		keyVoice(0, 0x1000, 0x1000, 0x3FFF, 0x3FFF);
		Spu_RenderFrames(buf, 512);
		int peakFull = peakAbs(buf, 512, 0);

		keyVoice(0, 0x1000, 0x1000, 0x3FFF, 0x3FFF);
		g_spuMasterVolL = g_spuMasterVolR = 0x2000;
		Spu_RenderFrames(buf, 512);
		int peakHalf = peakAbs(buf, 512, 0);
		check(peakHalf > peakFull * 2 / 5 && peakHalf < peakFull * 3 / 5,
			  "master volume 0x2000 halves the peak");
	}

	/* --- an overdriven mix clips to the rail of its own sign --------------- */
	{
		/*	Six in-phase copies of the square at volume 0x3FFF sum to ~171k
			before the master volume.  Clamping only after the master
			multiply let 171k * 0x3FFF wrap an int onto the opposite rail;
			the hardware clamps first, so every frame must sit at the rail
			matching one voice's sign, scaled by the master: 32767 * 0x3FFF
			>> 14 = 32765 and -32768 * 0x3FFF >> 14 = -32766.  The square's
			dominant gaussian tap keeps a lone voice above ~20000, so six of
			them clip on every frame, transitions included.  */
		resetSpu();
		writeSquareSample(0x1000);
		keyVoice(0, 0x1000, 0x1000, 0x3FFF, 0x3FFF);
		Spu_RenderFrames(buf2, 64);					/* envelope + taps settle */
		Spu_RenderFrames(buf2, 1024);				/* one voice's signs */

		resetSpu();
		writeSquareSample(0x1000);
		for (int i = 0; i < 6; i++)
			keyVoice(i, 0x1000, 0x1000, 0x3FFF, 0x3FFF);
		Spu_RenderFrames(buf, 64);
		Spu_RenderFrames(buf, 1024);
		int bad = 0, firstBad = -1;
		for (int i = 0; i < 1024 * 2; i++)
		{
			int want = buf2[i] > 0 ? 32765 : -32766;
			if (buf2[i] == 0 || buf[i] != want)
			{
				if (firstBad < 0)
					firstBad = i;
				bad++;
			}
		}
		check(bad == 0, "six in-phase voices: every sample at its own-sign rail");
		if (bad)
			std::printf("  (%d samples off the rail, first [%d] = %d, one voice %d)\n",
						bad, firstBad, buf[firstBad], buf2[firstBad]);

		/*	master 0x2000 halves the clipped s16, not the 85k sum  */
		resetSpu();
		writeSquareSample(0x1000);
		for (int i = 0; i < 3; i++)
			keyVoice(i, 0x1000, 0x1000, 0x3FFF, 0x3FFF);
		g_spuMasterVolL = g_spuMasterVolR = 0x2000;
		Spu_RenderFrames(buf, 64);
		Spu_RenderFrames(buf, 1024);
		bad = 0;
		for (int i = 0; i < 1024 * 2; i++)
			if (buf[i] != (buf2[i] > 0 ? 16383 : -16384))
				bad++;
		check(bad == 0, "three clipped voices at master 0x2000 give +16383/-16384");
		if (bad)
			std::printf("  (%d samples off, peak %d)\n", bad, peakAbs(buf, 1024, 0));
	}

	/* --- determinism: identical state renders identical output ------------ */
	{
		resetSpu();
		writeSquareSample(0x1000);
		keyVoice(0, 0x1000, 0x0C00, 0x3FFF, 0x2000);
		keyVoice(1, 0x1000, 0x1000, 0x1000, 0x3FFF);
		Spu_RenderFrames(buf, 2048);

		keyVoice(0, 0x1000, 0x0C00, 0x3FFF, 0x2000);
		keyVoice(1, 0x1000, 0x1000, 0x1000, 0x3FFF);
		Spu_RenderFrames(buf2, 2048);
		check(memcmp(buf, buf2, 2048 * 2 * sizeof(int16_t)) == 0,
			  "re-keyed identical state renders bit-identical audio");
	}

	/* --- libspu API: transfer set + voice attr + keys --------------------- */
	{
		resetSpu();
		SpuInit();
		/*	stage the square-wave pair in host memory so SpuWrite is what
			puts it into sound RAM  */
		static uint8_t sample[32];
		writeSquareSample(0x7000);
		memcpy(sample, &g_spuRam[0x7000], 32);
		memset(&g_spuRam[0x7000], 0, 32);
		check(SpuSetTransferStartAddr(0x3000) == 0x3000,
			  "SpuSetTransferStartAddr echoes the address");
		check(SpuWrite(sample, 32) == 32, "SpuWrite reports full transfer");
		check(SpuIsTransferCompleted(SPU_TRANSFER_PEEK) == 1,
			  "SpuIsTransferCompleted: synchronous");
		check(memcmp(&g_spuRam[0x3000], sample, 32) == 0,
			  "SpuWrite landed at the transfer address");

		SpuSetCommonMasterVolume(0x3FFF, 0x3FFF);
		SpuVoiceAttr attr;
		memset(&attr, 0, sizeof(attr));
		attr.voice = SPU_VOICECH(2);
		attr.mask = SPU_VOICE_VOLL | SPU_VOICE_VOLR | SPU_VOICE_PITCH |
					SPU_VOICE_WDSA | SPU_VOICE_LSAX |
					SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_ADSR2;
		attr.volume.left = 0x3FFF;
		attr.volume.right = 0x3FFF;
		attr.pitch = 0x1000;
		attr.addr = 0x3000;
		attr.loop_addr = 0x3000;
		attr.adsr1 = 0x000F;
		attr.adsr2 = 0x0000;
		SpuSetVoiceAttr(&attr);
		SpuSetKey(SPU_ON, SPU_VOICECH(2));
		Spu_RenderFrames(buf, 256);
		check(peakAbs(buf, 256, 0) > 20000, "SpuSetVoiceAttr+SpuSetKey: audio");

		char keys[SPU_NVOICES];
		SpuGetAllKeysStatus(keys);
		check(keys[2] == SPU_ON && keys[0] == SPU_OFF,
			  "SpuGetAllKeysStatus reflects the keyed voice");
		SpuSetKey(SPU_OFF, SPU_VOICECH(2));
		Spu_RenderFrames(buf, 64);
		SpuGetAllKeysStatus(keys);
		check(keys[2] == SPU_OFF, "instant release reads back SPU_OFF");
	}

	/* --- SpuMalloc: first-fit, coalescing, the VAB-swap pattern ----------- */
	{
		SpuInit();
		static char table[SPU_MALLOC_RECSIZ * (16 + 1)];
		check(SpuInitMalloc(16, table) == 16, "SpuInitMalloc returns num");

		long a = SpuMalloc(0x20000);			/* two "VABs" */
		long b = SpuMalloc(0x22000);
		check(a >= 0x1010 && (a & 15) == 0, "first block: valid aligned addr");
		check(b >= a + 0x20000, "second block placed after the first");

		/*	the defragSpuMemory pattern: close everything, re-upload larger  */
		SpuFree((unsigned long)a);
		SpuFree((unsigned long)b);
		long c = SpuMalloc(0x40000);
		check(c == a, "close-all/re-upload-all reuses the space (coalesced)");

		/*	hole reuse: free the first of two, a smaller alloc drops in  */
		long d = SpuMalloc(0x8000);
		SpuFree((unsigned long)c);
		long e = SpuMalloc(0x10000);
		check(e == c, "freed hole satisfies a smaller allocation first-fit");

		/*	exhaustion fails cleanly  */
		long big = SpuMalloc(0x80000);
		check(big == -1, "over-large allocation returns -1");
		SpuFree((unsigned long)d);
		SpuFree((unsigned long)e);
		long whole = SpuMalloc(SPU_RAM_SIZE - 0x1010 - 16);
		check(whole >= 0, "after freeing everything one huge block fits");
	}

	if (g_failures)
	{
		std::printf("spu test FAILED (%d)\n", g_failures);
		return 1;
	}
	std::printf("spu test PASSED\n");
	return 0;
}
