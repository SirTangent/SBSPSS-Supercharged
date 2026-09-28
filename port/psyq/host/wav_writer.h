/*	Minimal streaming WAV (PCM s16) writer - shared by the mixer tests and
	the host --dump-audio path (M5).  Single-threaded; caller owns pacing.
*/
#ifndef PORT_WAV_WRITER_H
#define PORT_WAV_WRITER_H

#include <stdint.h>
#include <stdio.h>

struct WavWriter
{
	FILE		*file;
	uint32_t	dataBytes;
	int			channels;
	int			sampleRate;
};

/* returns 0 on failure (file not creatable) */
int Wav_Open(WavWriter *w, const char *path, int sampleRate, int channels);
void Wav_Write(WavWriter *w, const int16_t *samples, int nFrames);
/* patch the header to the data so far and flush: a valid file from here on */
void Wav_Sync(WavWriter *w);
void Wav_Close(WavWriter *w);

#endif
