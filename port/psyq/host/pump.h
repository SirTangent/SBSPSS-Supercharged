/*	Cooperative pump: the single place PS1 "interrupt time" happens on PC.
	Blocking SDK calls (VSync, DrawSync, CdReadSync) call Port_Pump(), which
	advances the emulated vblank counter from the wall clock and fires the
	registered VSyncCallback once per elapsed vblank - reproducing the PS1's
	callback-during-load behaviour single-threaded.
*/
#ifndef PORT_PUMP_H
#define PORT_PUMP_H

#ifdef __cplusplus
extern "C" {
#endif

void			Port_Pump(void);			/* advance vblank clock, fire callbacks */
void			Port_PumpIdle(void);		/* Sleep(1) + Port_Pump - use in wait loops */
unsigned long	Port_VBlankCount(void);
void			Port_SetVBlankHz(int hz);	/* 60 NTSC / 50 PAL (SetVideoMode) */
int				Port_VBlankHz(void);		/* the rate last set: 60 or 50 (GetVideoMode, pace log, XM check) */
double			Port_NowSeconds(void);		/* QPC wall clock, fixed epoch (CD pacing) */
int				Port_Uncapped(void);		/* SBSP_UNCAPPED=1: vblanks are not wall-clock paced */

/*	Pause on focus loss (M8 shell, host/window.cpp).  While paused the pump
	polls the window and delivers NO vblank; on resume it rebases the wall
	clock onto the counter so no catch-up burst follows.  */
int				Host_PausePoll(void);		/* poll events while paused; 1 = still paused */
int				Port_Paused(void);			/* read by the watchdog thread */
double			Host_PausedSeconds(void);	/* total, for [summary] paused= */

#ifdef __cplusplus
}
#endif

#endif
