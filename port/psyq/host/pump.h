/*	Cooperative pump: the single place PS1 "interrupt time" happens on PC.
	Blocking SDK calls (VSync, CdReadSync, StGetNext) wait with
	Port_PumpIdle, which advances the emulated vblank counter - paced by the
	wall clock unless uncapped - and fires the registered VSyncCallback once
	per vblank, reproducing the PS1's callback-during-load behaviour
	single-threaded.  Non-blocking calls (VSync(-1), DrawSync, PadGetState)
	call Port_Pump, which polls and fires a vblank only in a live capped run
	that is due one, or where a replay's recording says one fired (issue #67;
	host/pump.cpp explains the rule and the spin rule).
*/
#ifndef PORT_PUMP_H
#define PORT_PUMP_H

#ifdef __cplusplus
extern "C" {
#endif

void			Port_Pump(void);			/* a bare pump: poll; a vblank only as pump.cpp's rule allows */
void			Port_PumpIdle(void);		/* one wait step: a vblank when one is due */
/*	0 if the latest vblank fired in a wait step, else k: it fired at the k-th
	bare pump since the last one - what `# bare` records (host/input.cpp)  */
unsigned long	Port_VBlankBarePump(void);
int				Port_PumpNested(void);		/* 1 inside a vblank's work, where no wait can advance */
/*	bare pumps in a row after which each further one is a one-vblank wait
	(host/pump.cpp, the spin rule)  */
#define PORT_SPIN_PUMPS	10000
unsigned long	Port_VBlankCount(void);
void			Port_SetVBlankHz(int hz);	/* 60 NTSC / 50 PAL (SetVideoMode) */
int				Port_VBlankHz(void);		/* the rate last set: 60 or 50 (GetVideoMode, pace log, XM check) */
double			Port_NowSeconds(void);		/* QPC wall clock, fixed epoch (pace log, pause, present throttle) */
int				Port_Uncapped(void);		/* SBSP_UNCAPPED=1: vblanks are not wall-clock paced */

/*	--pace-log phase split (M8 perf): wall seconds spent in a phase since the
	last [pace] line.  Callers bracket with Port_NowSeconds() only when
	Port_PaceLogOn(), so an unlogged run never reads the clock for it.  */
enum { PORT_PACE_RASTER, PORT_PACE_PRESENT, PORT_PACE_PHASES };
int				Port_PaceLogOn(void);		/* SBSP_PACE_LOG=1 */
void			Port_PaceAdd(int phase, double seconds);

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
