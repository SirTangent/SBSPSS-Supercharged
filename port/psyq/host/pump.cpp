/*	Emulated vblank clock (QPC-driven) + the libetc VSync surface.

	Game-visible semantics (libetc):
	  VSync(0)   - wait for the next vblank, return the vblank counter
	  VSync(n>0) - wait until n vblanks have passed since the previous VSync
	  VSync(n<0) - return the counter without waiting
	  VSyncCallback(f) - f fires once per vblank (from inside the pump)
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>	/* timeBeginPeriod (link: winmm) */

#include <stdio.h>
#include <stdlib.h>

#include "pump.h"

extern "C" {
int  VSync(int mode);
int  VSyncCallback(void (*f)(void));
}

static void			(*g_vsyncCallback)(void);
static unsigned long	g_vblank;			/* emulated vblank counter */
static unsigned long	g_lastVSyncVblank;
static unsigned long	g_vblankBase;		/* wall count at the last rate change */
static int				g_hz = 60;
static LARGE_INTEGER	g_qpcFreq;
static LARGE_INTEGER	g_qpcBase;		/* vblank epoch - REBASED by Port_SetVBlankHz */
static LARGE_INTEGER	g_qpcOrigin;	/* wall-clock epoch - fixed for the whole run */
static int				g_clockInit;

static void clockInit(void)
{
	if (!g_clockInit)
	{
		QueryPerformanceFrequency(&g_qpcFreq);
		QueryPerformanceCounter(&g_qpcBase);
		g_qpcOrigin = g_qpcBase;
		/*	Sleep(1) in Port_PumpIdle is the granularity of every VSync
			wait; without this it can be ~15ms on Windows' default timer
			resolution, overshooting whole vblanks.  Never matched with
			timeEndPeriod - the process needs it for its entire life.  */
		timeBeginPeriod(1);
		g_clockInit = 1;
	}
}

/*	SBSP_UNCAPPED=1 (--uncapped, M8): emulated time advances only when the
	game WAITS for it.  Each Port_PumpIdle - the body of every blocking
	wait (VSync, DrawSync(0), CdReadSync...) - moves the target one vblank
	ahead of the counter, so that wait iteration delivers exactly one
	callback and returns; non-blocking pumps (VSync(-1), DrawSync(1),
	PadGetState) deliver nothing, exactly as they would between two real
	vblanks.  A game frame therefore costs one vblank, as on a capped host
	keeping full rate (paceLog vbl/frame 1.0), only without the wall-clock
	wait.  (Firing on EVERY pump instead made a frame cost as many vblanks
	as it pumps - 5 - and the simulation diverged from a capped run.)  The
	single-fire block, backlog rebase and re-entrancy guard in Port_Pump are
	untouched: the target never gets more than one ahead.  With
	--no-cd-pace and --no-audio this removes every wall-clock input, so two
	runs with the same --seed are bit-identical.  */
static unsigned long	g_uncappedTarget;

extern "C" int Port_Uncapped(void)
{
	static int uncapped = -1;
	if (uncapped < 0)
	{
		const char *e = getenv("SBSP_UNCAPPED");
		uncapped = (e && *e && *e != '0');
		if (uncapped)
			fprintf(stderr, "[pace] uncapped: vblanks advance one per pump, not by wall clock\n");
	}
	return uncapped;
}

static unsigned long wallVblank(void)
{
	LARGE_INTEGER now;
	clockInit();
	if (Port_Uncapped())
		return g_uncappedTarget;
	QueryPerformanceCounter(&now);
	return g_vblankBase +
		   (unsigned long)(((now.QuadPart - g_qpcBase.QuadPart) * g_hz) / g_qpcFreq.QuadPart);
}

extern "C" void Port_SetVBlankHz(int hz)
{
	/*	rebase the epoch so already-elapsed wall time is not retroactively
		reinterpreted at the new rate (60->50 would stall VSync until the
		wall clock caught back up; 50->60 would burst callbacks)  */
	g_vblankBase = wallVblank();
	QueryPerformanceCounter(&g_qpcBase);
	g_hz = (hz == 50) ? 50 : 60;
}

extern "C" unsigned long Port_VBlankCount(void)
{
	return g_vblank;
}

/*	Wall clock for deadline arithmetic (CD pacing).  It must run off the
	FIXED origin, never off g_qpcBase: Port_SetVBlankHz rebases that epoch at
	the game's SetVideoMode, which would step this clock backwards by however
	long the boot took and freeze every read deadline set before it.  */
extern "C" double Port_NowSeconds(void)
{
	LARGE_INTEGER now;
	clockInit();
	QueryPerformanceCounter(&now);
	return (double)(now.QuadPart - g_qpcOrigin.QuadPart) / (double)g_qpcFreq.QuadPart;
}

/*	Backlog cap.  Pending vblanks drain one per pump call, so the game
	tolerates a short lag; past this the host is simply not keeping up (or
	was stopped dead by a debugger / laptop sleep) and the WALL CLOCK is
	rebased onto the counter - see Port_Pump.  ~133ms at 60Hz.  */
#define MAX_PENDING_VBLANKS 8

/*	SBSP_PACE_LOG=1: every 300 vblanks (5s at 60Hz), print delivered vblanks
	vs VSync(0) waits over the window.  In steady state the game calls
	VSync(0) exactly once per rendered frame (VidSwapDraw), so vbl/vsync0 ~=
	getFramesSinceLast: 1.0 is locked full-rate, ~6 was the M3 frontend
	before the present was decoupled from emulated time.  */
static unsigned long	g_vsync0Count;

static void paceLog(void)
{
	static int				enabled = -1;
	static unsigned long	lastVsync0;
	static double			lastWall;

	if (enabled < 0)
	{
		const char *e = getenv("SBSP_PACE_LOG");
		enabled = (e && *e && *e != '0');
	}
	if (!enabled || (g_vblank % 300) != 0)
		return;

	double now = Port_NowSeconds();
	unsigned long dv = g_vsync0Count - lastVsync0;
	fprintf(stderr, "[pace] vblank=%lu  vsync0=%lu in window (vbl/frame %.2f)  wall %.2fs for 300 vbl\n",
			g_vblank, dv, dv ? 300.0 / (double)dv : 0.0, now - lastWall);
	lastVsync0 = g_vsync0Count;
	lastWall   = now;
}

extern "C" void Port_Pump(void)
{
	extern void Host_VBlank(unsigned long vblankNo);	/* host/window.cpp */
	extern void Port_RCnt2Vblank(int vblankHz);			/* api/libapi_stubs.cpp */
	extern void Port_CdVblank(int vblankHz);			/* cd/xa_stream.cpp */
	extern void Port_AudioVBlank(int vblankHz);			/* host/audio_out.cpp */

	/*	Nested pumps are a complete no-op.  Port_Pump can be reached from
		inside g_vsyncCallback (anything the game's vblank work touches that
		pumps - DrawSync, PadGetState, VSync), and on PS1 that work ran in
		the vblank IRQ handler, which could not observe its own next firing.
		Re-entering here would double-advance FrameCounter/TickCount for one
		real vblank and re-enter LoadingIcon mid-draw.  Skipping the whole
		step (rather than just the callback) keeps the counter and the
		callback in lockstep, so nothing is silently dropped: the pending
		vblank is simply delivered by the next non-nested pump.
		Requirement this places on vblank callbacks: they must not BLOCK on
		the pump (a VSync(n) wait from inside one would never complete).
		Nothing in the tree does - VidVSyncCallback only draws.  */
	static int	inPump;
	if (inPump)
		return;

	unsigned long target = wallVblank();

	/*	Backlog control.  The old code fast-forwarded g_vblank to
		`target - 8`, which advanced the counter WITHOUT firing the
		callbacks for the skipped vblanks: FrameCounter/TickCount
		(source/system/vid.cpp) and the music tick lag wall time forever,
		and every VSync(0) inside the backlog returns for free.  Rebase the
		wall clock onto the counter instead - no callback is ever dropped,
		and a host that cannot sustain 60Hz runs consistently slow rather
		than tearing game time away from the music tempo.  */
	if (target > g_vblank + MAX_PENDING_VBLANKS)
	{
		g_vblankBase = g_vblank + MAX_PENDING_VBLANKS;
		QueryPerformanceCounter(&g_qpcBase);
		target = g_vblankBase;
	}

	/*	AT MOST ONE vblank per pump call.  On PS1 the vblank is an interrupt:
		game code waiting on its side effects always runs between two firings
		and can observe every intermediate state.  StopLoad (vid.cpp:125)
		depends on that - it spins `while(LoadTime) VSync(0)` and LoadTime
		WRAPS to zero, so if one wait iteration ever fires two callbacks the
		zero can be stepped over forever (seen live: the FIFO present paces
		the loop at ~1 real vsync, one pending vblank accumulates per
		iteration, and a catch-up burst here made every VSync(0) fire twice).
		The counter still tracks the wall clock - pending vblanks drain one
		per call through the many pump sites a game frame passes.  */
	if (g_vblank < target)
	{
		inPump = 1;
		g_vblank++;
		if (g_vsyncCallback)
			g_vsyncCallback();		/* game vblank work first (loading icon...) */
		Port_RCnt2Vblank(g_hz);
		Port_CdVblank(g_hz);		/* XA sector clock: decode + deliveries */
		Host_VBlank(g_vblank);		/* ...then events + present + tooling */
		Port_AudioVBlank(g_hz);		/* WAV dump: this vblank's audio, if armed */
		paceLog();
		inPump = 0;
	}
}

/*	Wait-loop body: yield the core for a tick, then pump.  Every blocking SDK
	call spins on some deadline (a vblank number, a CD read completion); doing
	that without the Sleep pins a core at 100% for the whole wait.  */
extern "C" void Port_PumpIdle(void)
{
	if (Port_Uncapped())
		g_uncappedTarget = g_vblank + 1;	/* the wait itself is what passes time */
	else
		Sleep(1);
	Port_Pump();
}

extern "C" int VSync(int mode)
{
	if (mode < 0)
	{
		Port_Pump();
		return (int)g_vblank;
	}

	/*	mode 0: `until` is computed BEFORE any pumping, so one call consumes
		exactly one vblank callback even when a pending vblank has already
		accumulated - the StopLoad invariant (see Port_Pump).  While a
		backlog exists this returns without a real wait, which is exactly
		what catching up means: each return still delivered one callback, so
		game time advanced by one frame.  Port_Pump bounds the backlog at
		MAX_PENDING_VBLANKS, so free returns can never run away.  */
	unsigned long until = (mode == 0) ? g_vblank + 1
									  : g_lastVSyncVblank + (unsigned long)mode;
	if (mode == 0)
		g_vsync0Count++;	/* pace diagnostic - see paceLog */
	Port_Pump();
	while (g_vblank < until)
		Port_PumpIdle();
	g_lastVSyncVblank = g_vblank;
	return (int)g_vblank;
}

extern "C" int VSyncCallback(void (*f)(void))
{
	g_vsyncCallback = f;
	return 0;
}
