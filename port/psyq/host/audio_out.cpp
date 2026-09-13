/*	Host audio output (M5): the SDL3 pull path and the deterministic WAV dump.

	Two mutually exclusive modes:

	- Device (normal play): an SDL audio stream on the default playback
	  device; SDL's audio thread calls the get-callback, which renders the
	  requested frames straight out of the SPU mixer under its lock.  The
	  SPU renders continuously from voice state, so drift between the
	  device clock and the emulated vblank clock is absorbed by design -
	  tempo comes solely from XM_Update's call rate.

	- Dump (--dump-audio <file.wav> / SBSP_DUMP_AUDIO): no device at all.
	  Port_AudioVBlank - called once per emulated vblank from the pump,
	  AFTER the vblank callback chain (so that vblank's XM_Update is
	  already applied) - renders exactly 44100/hz frames into the WAV.
	  Sample-exact reproducible output, works headless and on machines
	  with no sound device.  The modes must not combine: every render
	  ADVANCES the mixer, so two consumers would each get half the audio.

	  --no-audio / SBSP_NO_AUDIO=1 skips the device without dumping.

	Init hangs off Host_EnsureVideo (window.cpp), not SpuInit: the unit
	tests drive SpuInit/the mixer directly and must never open a device,
	while the real game always brings the window up first.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include "spu/spu_core.h"
#include "host/wav_writer.h"

namespace
{

const int kMaxChunkFrames = 1024;	/* cap one lock hold / render burst */

SDL_AudioStream *g_stream;
WavWriter g_wav;
int g_wavOpen;
int g_audioUp;

void SDLCALL audioPull(void *userdata, SDL_AudioStream *stream,
					   int additional, int total)
{
	(void)userdata;
	(void)total;
	static int16_t buf[kMaxChunkFrames * 2];
	int frames = additional / 4;		/* s16 stereo */
	while (frames > 0)
	{
		int n = frames > kMaxChunkFrames ? kMaxChunkFrames : frames;
		Spu_RenderFrames(buf, n);
		SDL_PutAudioStreamData(stream, buf, n * 4);
		frames -= n;
	}
}

void closeWavAtExit(void)
{
	if (g_wavOpen)
	{
		Wav_Close(&g_wav);
		g_wavOpen = 0;
	}
}

/*	case-insensitive substring (no strcasestr in the MS CRT)  */
bool containsNoCase(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (_strnicmp(hay, needle, n) == 0)
			return true;
	return false;
}

}	/* namespace */

extern "C" void Host_EnsureAudio(void)
{
	if (g_audioUp)
		return;
	g_audioUp = 1;

	const char *dump = getenv("SBSP_DUMP_AUDIO");
	if (dump && *dump)
	{
		if (Wav_Open(&g_wav, dump, 44100, 2))
		{
			g_wavOpen = 1;
			atexit(closeWavAtExit);
			fprintf(stderr, "[host] audio dump -> %s (no playback device)\n",
					dump);
		}
		else
			fprintf(stderr, "[host] cannot create audio dump '%s'\n", dump);
		return;
	}

	const char *no = getenv("SBSP_NO_AUDIO");
	if (no && *no && *no != '0')
	{
		fprintf(stderr, "[host] audio device disabled (SBSP_NO_AUDIO)\n");
		return;
	}

	/*	SBSP_AUDIO_BUFFER_FRAMES (sbsp.ini audio_buffer_frames, M8 shell):
		the device period, SDL3's hint rather than a spec field; 0/unset
		leaves the driver's choice.  Must precede the open.  */
	const char *frames = getenv("SBSP_AUDIO_BUFFER_FRAMES");
	if (frames && *frames && atoi(frames) > 0)
	{
		SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames);
		fprintf(stderr, "[host] audio buffer: %d frames requested\n", atoi(frames));
	}

	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		fprintf(stderr, "[host] SDL audio init failed: %s\n", SDL_GetError());
		return;
	}

	/*	SBSP_AUDIO_DEVICE (audio_device): part of a playback device's name,
		case-insensitive; no match = the default, with the names listed so
		the tester can copy one.  */
	SDL_AudioDeviceID dev = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
	const char *want = getenv("SBSP_AUDIO_DEVICE");
	if (want && *want)
	{
		int n = 0;
		SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n);
		int found = -1;
		for (int i = 0; ids && i < n && found < 0; i++)
		{
			const char *name = SDL_GetAudioDeviceName(ids[i]);
			if (name && containsNoCase(name, want))
				found = i;
		}
		if (found >= 0)
		{
			dev = ids[found];
			fprintf(stderr, "[host] audio device: %s\n", SDL_GetAudioDeviceName(dev));
		}
		else
		{
			fprintf(stderr, "[host] audio device '%s' not found - using the default; available:\n", want);
			for (int i = 0; ids && i < n; i++)
				fprintf(stderr, "       %s\n", SDL_GetAudioDeviceName(ids[i]));
		}
		SDL_free(ids);
	}

	SDL_AudioSpec spec;
	spec.format = SDL_AUDIO_S16;
	spec.channels = 2;
	spec.freq = 44100;
	g_stream = SDL_OpenAudioDeviceStream(dev, &spec, audioPull, 0);
	if (!g_stream)
	{
		fprintf(stderr, "[host] no audio device: %s\n", SDL_GetError());
		return;
	}

	/*	SBSP_VOLUME (volume): 0-100 on the host stream.  The game's own
		SpuSetCommonMasterVolume keeps driving the SPU mixer underneath.  */
	const char *vol = getenv("SBSP_VOLUME");
	if (vol && *vol)
	{
		char *end;
		long v = strtol(vol, &end, 10);
		if (end == vol || *end || v < 0 || v > 100)
			fprintf(stderr, "[host] bad volume '%s' - want 0-100 - using 100\n", vol);
		else if (v != 100)
		{
			SDL_SetAudioStreamGain(g_stream, (float)v / 100.0f);
			fprintf(stderr, "[host] volume: %ld%%\n", v);
		}
	}
	SDL_ResumeAudioStreamDevice(g_stream);
}

/*	window.cpp, on the pause edges (focus lost / regained, M8 shell).  The
	pump stops delivering vblanks while paused, so XM_Update stops too - but
	the SPU would keep rendering whatever the voices were doing, a drone,
	unless the device stops pulling.  No-op without a device.  */
extern "C" void Host_AudioPause(int on)
{
	if (!g_stream)
		return;
	if (on)
		SDL_PauseAudioStreamDevice(g_stream);
	else
		SDL_ResumeAudioStreamDevice(g_stream);
}

/*	pump.cpp, once per emulated vblank, after the vblank callback chain  */
extern "C" void Port_AudioVBlank(int vblankHz)
{
	if (!g_wavOpen)
	{
		/*	no consumer at all (--no-audio, or no device): nothing renders,
			so the XA feeder's CD-input ring would back up and log an
			overflow ~1.7s into any speech line.  Keep it drained.  */
		if (!g_stream)
			Spu_CdInClear();
		return;
	}
	static int16_t buf[882 * 2];		/* 44100/50 is the larger case */
	int frames = 44100 / (vblankHz > 0 ? vblankHz : 60);
	if (frames > 882)
		frames = 882;
	Spu_RenderFrames(buf, frames);
	Wav_Write(&g_wav, buf, frames);
}
