/*	STR streaming engine (M7) - see str_stream.cpp.  */
#ifndef PORT_STR_STREAM_H
#define PORT_STR_STREAM_H

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
