/*	libpad over the host input (M3).  Port 0 is a permanently-stable
	DualShock whose 34-byte buffer host/input.cpp refreshes every emulated
	vblank (keyboard + first SDL gamepad); port 1 stays disconnected.

	The handshake pads.cpp performs (PadInitShock + ReadController):
	  PadGetState        -> 6 PadStateStable (port 0) / 0 Discon (port 1)
	  PadInfoMode        -> ExID 7 = DualShock (=> IsAnalogue=2), id table
	                        of one entry, also 7
	  PadSetActAlign     -> nonzero, or Pad->Send never latches and
	                        CanVibrate stays false
	  PadSetAct          -> stores the motor buffer pointer for M6's rumble

	PadGetState also pumps: VRamViewer (vid.cpp:461) loops on PadUpdate
	alone - no VSync, so without a pump here the viewer would spin on a
	stale buffer with a frozen window.  A bare Port_Pump fires a vblank only
	when a live capped run is due one, or where a replay's recording says,
	and a nested call is a no-op, so the extra calls from the normal
	per-frame PadUpdate are near-free; the spin rule (host/pump.cpp) keeps
	the viewer alive where bare pumps fire nothing.
*/
#include "stub_log.h"
#include "host/pump.h"

unsigned char *Port_PadBuffer[2];		/* captured for host/input.cpp */
unsigned char *Port_PadMotor[2];		/* captured for M6 rumble */

extern "C" {

void PadInitDirect(unsigned char *pad1, unsigned char *pad2)
{
	Port_PadBuffer[0] = pad1;
	Port_PadBuffer[1] = pad2;
	if (pad1)
	{
		/*	valid idle packet from the first read, even before the SDL
			window exists (headless tools link this too): connected
			DualShock, no buttons (active low), sticks centred  */
		pad1[0] = 0;
		pad1[1] = 0x73;
		pad1[2] = 0xFF;
		pad1[3] = 0xFF;
		pad1[4] = pad1[5] = pad1[6] = pad1[7] = 0x80;
	}
	if (pad2)
		pad2[0] = 1;	/* byte0 != 0: no data (disconnected) */
}

void PadStartCom(void)	{ }
void PadStopCom(void)	{ }

int PadGetState(int port)
{
	Port_Pump();		/* VRamViewer's VSync-less loop lives on this: the spin rule, host/pump.cpp */
	return (port >> 4) == 0 ? 6 : 0;	/* PadStateStable / PadStateDiscon */
}

int PadInfoMode(int port, int term, int id)
{
	(void)port;
	(void)term;
	if (id < 0)
		return 1;		/* InfoModeIdTable size query: one entry */
	return 7;			/* DualShock everywhere it looks */
}

int PadSetMainMode(int port, int offs, int lock)
{
	(void)port; (void)offs; (void)lock;
	return 1;
}

int PadSetAct(int port, unsigned char *data, int len)
{
	(void)len;
	int p = port >> 4;
	if (p == 0 || p == 1)
		Port_PadMotor[p] = data;	/* rumble lands in M6 */
	return 1;
}

int PadSetActAlign(int port, unsigned char *data)
{
	(void)port; (void)data;
	return 1;			/* lets Pad->Send latch and CanVibrate go true */
}

}
