/*	STR streaming engine (M7) - see str_stream.cpp.  */
#ifndef PORT_STR_STREAM_H
#define PORT_STR_STREAM_H

/*	Silence queued ahead of a movie's first audio sector (issue #71): 1512
	stereo frames = 40ms at 37.8kHz, as long as the XA pre-roll (a mono
	18.9kHz stream gets 756).  A movie's audio is one sector in eight, 2016
	frames every 3.2 vblanks at 60Hz (2.67 at 50Hz), and each sector lands
	on a whole vblank, up to 0.8 (60Hz) / 0.67 (50Hz) of a vblank later
	than the first one did: up to 504 frames short without a cushion (the
	shipped movies' slot: none at 60Hz, 252 at 50Hz).  What is left over -
	at least 1008 frames, and for the shipped movies 1512 at 60Hz / 1260 at
	50Hz (~1760 / ~1470 output frames at 44.1kHz) - is the headroom for a
	playback device pulling a period at a time: the ring does not run dry
	in a --dump-audio run or at the default device period (~480 frames),
	but an sbsp.ini audio_buffer_frames above ~1470 (50Hz) / ~1760 (60Hz)
	can still underrun mid-movie in live play.  The delay moves the sound
	toward the picture: fmv.cpp shows a frame two vblanks after the next one
	is whole, so the sound led the picture by 33-120ms and now leads it by
	-7 to 80ms.  */
#define STR_PREROLL_FRAMES	1512

#ifdef __cplusplus
extern "C" {
#endif

void StrStream_Seek(long lba);			/* CdlSeekL records the position */
int  StrStream_Start(long mode);		/* CdRead2: begin streaming there */
void StrStream_Stop(void);				/* CdlPause: freeze delivery */
void StrStream_Vblank(int vblankHz);	/* sector clock (xa_stream.cpp) */
void StrStream_ResetForTest(void);

/*	One past the last byte of the frame StGetNext handed out at `frame`
	(its ring region is contiguous: nSectors * 2016 bytes), or NULL when
	`frame` is not a handed-out, unfreed frame.  The BS bit reader's bound
	(vlc3.cpp) - disc data without an end code must stop there, not run
	past the game's ring.  */
const void *StrStream_FrameEnd(const void *frame);

#ifdef __cplusplus
}
#endif
#endif
