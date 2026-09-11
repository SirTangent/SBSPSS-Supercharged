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
double			Port_NowSeconds(void);		/* QPC wall clock, fixed epoch (CD pacing) */
int				Port_Uncapped(void);		/* SBSP_UNCAPPED=1: vblanks are not wall-clock paced */

#ifdef __cplusplus
}
#endif

#endif
