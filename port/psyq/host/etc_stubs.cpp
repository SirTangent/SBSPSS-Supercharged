/*	libetc stubs (VSync/VSyncCallback live in pump.cpp).  The vintage
	libetc.h (tools/psyq/include, behind the system headers via -idirafter)
	supplies the prototypes these definitions must match and the video-mode
	constants - MODE_NTSC / MODE_PAL are preprocessor literals there, the
	SDK has no enum for them.  */
#include <sys/types.h>		/* u_long, for libetc.h */
#include <libetc.h>

#include "stub_log.h"
#include "pump.h"

extern "C" {

int  ResetCallback(void)			{ return 0; }
int  CheckCallback(void)			{ return 0; }
int  RestartCallback(void)			{ return 0; }
int  StopCallback(void)				{ return 0; }

/*	Video mode <-> pump rate.  The mode is not stored: the pump's vblank
	rate IS the mode, so GetVideoMode reports whatever SetVideoMode last
	set (60 -> MODE_NTSC, 50 -> MODE_PAL) and cannot disagree with it.  */
long GetVideoMode(void)				{ return Port_VBlankHz() == 50 ? MODE_PAL : MODE_NTSC; }
long SetVideoMode(long mode)
{
	Port_SetVBlankHz(mode == MODE_PAL ? 50 : 60);
	return mode;
}

void PadInit(int mode)				{ (void)mode; PSYQ_STUB_ONCE(); }
unsigned long PadRead(int id)		{ (void)id; return 0; }
void PadStop(void)					{ }

}
