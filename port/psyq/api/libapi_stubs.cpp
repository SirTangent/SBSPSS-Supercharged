/*	libapi + kernel-adjacent stubs.

	SetSp/GetSp: InitSystem() relocates the MIPS stack pointer with
	SetSp(GetSp()|0x807f0000) - meaningless on PC, so GetSp hands back a
	value for which the OR is harmless and SetSp ignores it.

	EnterCriticalSection/ExitCriticalSection disable PS1 interrupts; the pump
	model is single-threaded so they are no-ops.  The PSY-Q
	EnterCriticalSection shares its name with a Win32 function - distinct
	decorated symbols on i686 (kernel32's is stdcall, _EnterCriticalSection@4),
	one and the same symbol on x64, where the static CRT drags kernel32's
	import member in.  port/include/libapi.h renames the PSY-Q one to
	psyq_sdk_EnterCriticalSection for the game on every PC build; this is
	shim code that does not see that shadow, so it defines the name directly.

	The event/root-counter set drives system/clickcount.cpp's RCnt2 timer
	for real since M2: OpenEvent(RCntCNT2) registers the handler, SetRCnt
	the countdown target, and the pump fires the handler at the programmed
	rate per emulated vblank (RCnt2 counts at sysclock/8 = 4,233,600 Hz;
	the game's 17200 target = ~246 Hz = ~4 ticks per NTSC vblank).
*/
#include "stub_log.h"

/* RCnt2 timer state, ticked by Port_RCnt2Vblank from the pump */
static long		(*g_rcnt2Func)();
static unsigned	g_rcnt2Target;
static int		g_rcnt2Running;
static long long g_rcnt2Accum;		/* counts carried between vblanks */

static const long long RCNT2_HZ = 4233600;	/* sysclock/8 */

/*	Event/counter descriptors are class|index (KERNEL.H): DescRC 0xF2000000 for
	root counters, DescHW 0xF0000000 for hardware sources.  Match the WHOLE
	descriptor - on the low byte alone, RCntCNT2 (0xF2000002), HwGPU
	(0xF0000002) and SwMATH (0xF4000002) are indistinguishable.  */
static const unsigned long RCntCNT2_DESC = 0xF2000002ul;	/* DescRC|0x02 */

extern "C" void Port_RCnt2Vblank(int vblankHz)
{
	if (!g_rcnt2Running || !g_rcnt2Func || g_rcnt2Target == 0)
		return;
	g_rcnt2Accum += RCNT2_HZ / vblankHz;
	long long fires = g_rcnt2Accum / g_rcnt2Target;
	if (fires > 64)
		fires = 64;		/* stall guard - drop excess ticks like the vblank clamp */
	g_rcnt2Accum -= fires * g_rcnt2Target;
	while (fires-- > 0)
		g_rcnt2Func();
}

extern "C" {

long GetSp(void)					{ return 0; }
long SetSp(long newSp)				{ (void)newSp; return 0; }

void psyq_sdk_EnterCriticalSection(void)	{ }
void ExitCriticalSection(void)		{ }

long OpenEvent(unsigned long desc, long spec, long mode, long (*func)())
{
	(void)spec; (void)mode;
	if (desc == RCntCNT2_DESC)
	{
		g_rcnt2Func = func;
		return 2;
	}
	PSYQ_STUB_ONCE();
	return 1;	/* fake event handle */
}
long CloseEvent(long ev)			{ if (ev == 2) g_rcnt2Func = 0; return 1; }
long EnableEvent(long ev)			{ (void)ev; return 1; }
long DisableEvent(long ev)			{ (void)ev; return 1; }

long SetRCnt(unsigned long spec, unsigned short target, long mode)
{
	(void)mode;
	if (spec == RCntCNT2_DESC)
		g_rcnt2Target = target;
	else
		PSYQ_STUB_ONCE();
	return 1;
}
long StartRCnt(unsigned long spec)	{ if (spec == RCntCNT2_DESC) g_rcnt2Running = 1; return 1; }
long StopRCnt(unsigned long spec)	{ if (spec == RCntCNT2_DESC) g_rcnt2Running = 0; return 1; }
long GetRCnt(unsigned long spec)	{ (void)spec; PSYQ_STUB_ONCE(); return 0; }

void FlushCache(void)				{ }

}
