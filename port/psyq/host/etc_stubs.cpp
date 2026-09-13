/*	libetc stubs (VSync/VSyncCallback live in pump.cpp).  */
#include "stub_log.h"
#include "pump.h"

extern "C" {

int  ResetCallback(void)			{ return 0; }
int  CheckCallback(void)			{ return 0; }
int  RestartCallback(void)			{ return 0; }
int  StopCallback(void)				{ return 0; }

/*	Video mode <-> pump rate.  libetc.h: MODE_NTSC 0, MODE_PAL 1 (numeric
	here - this TU carries no PsyQ headers).  The mode is not stored: the
	pump's vblank rate IS the mode, so GetVideoMode reports whatever
	SetVideoMode last set (60 -> NTSC, 50 -> PAL) and cannot disagree with it.  */
long GetVideoMode(void)				{ return Port_VBlankHz() == 50 ? 1 : 0; }
long SetVideoMode(long mode)
{
	Port_SetVBlankHz(mode == 1 ? 50 : 60);
	return mode;
}

void PadInit(int mode)				{ (void)mode; PSYQ_STUB_ONCE(); }
unsigned long PadRead(int id)		{ (void)id; return 0; }
void PadStop(void)					{ }

}
