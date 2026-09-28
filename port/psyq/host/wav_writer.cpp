/*	Minimal streaming WAV writer (M5).  Writes a placeholder RIFF header,
	streams s16 PCM, and patches the two size fields on Wav_Sync / close.
*/
#include <string.h>

#include "host/wav_writer.h"

int Wav_Open(WavWriter *w, const char *path, int sampleRate, int channels)
{
	memset(w, 0, sizeof(*w));
	w->file = fopen(path, "wb");
	if (!w->file)
		return 0;
	w->channels = channels;
	w->sampleRate = sampleRate;

	uint8_t hdr[44];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr + 0, "RIFF", 4);				/* [4..7] riff size, patched */
	memcpy(hdr + 8, "WAVEfmt ", 8);
	uint32_t fmtSize = 16;
	uint16_t fmtTag = 1;					/* PCM */
	uint16_t nCh = (uint16_t)channels;
	uint32_t rate = (uint32_t)sampleRate;
	uint32_t byteRate = rate * nCh * 2;
	uint16_t blockAlign = (uint16_t)(nCh * 2);
	uint16_t bits = 16;
	memcpy(hdr + 16, &fmtSize, 4);
	memcpy(hdr + 20, &fmtTag, 2);
	memcpy(hdr + 22, &nCh, 2);
	memcpy(hdr + 24, &rate, 4);
	memcpy(hdr + 28, &byteRate, 4);
	memcpy(hdr + 32, &blockAlign, 2);
	memcpy(hdr + 34, &bits, 2);
	memcpy(hdr + 36, "data", 4);			/* [40..43] data size, patched */
	fwrite(hdr, 1, sizeof(hdr), w->file);
	return 1;
}

void Wav_Write(WavWriter *w, const int16_t *samples, int nFrames)
{
	if (!w->file || nFrames <= 0)
		return;
	size_t n = (size_t)nFrames * w->channels;
	fwrite(samples, sizeof(int16_t), n, w->file);
	w->dataBytes += (uint32_t)(n * sizeof(int16_t));
}

/*	Patch both size fields to what has been written so far and put the
	whole lot in the OS's hands.  The first fseek flushes the PCM before
	the header claims it, so the header never counts bytes the file lacks:
	whatever ends the process afterwards - even TerminateProcess - leaves a
	valid WAV of everything written up to the last sync.  */
void Wav_Sync(WavWriter *w)
{
	if (!w->file)
		return;
	uint32_t riffSize = 36 + w->dataBytes;
	fseek(w->file, 4, SEEK_SET);
	fwrite(&riffSize, 4, 1, w->file);
	fseek(w->file, 40, SEEK_SET);
	fwrite(&w->dataBytes, 4, 1, w->file);
	fseek(w->file, 0, SEEK_END);
	fflush(w->file);
}

void Wav_Close(WavWriter *w)
{
	if (!w->file)
		return;
	Wav_Sync(w);
	fclose(w->file);
	w->file = 0;
}
