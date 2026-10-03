/*	M8 harness diagnostics (host/diag.cpp): process exit with a [summary]
	line, [scene] epochs, [assert] routing.  Every line is stderr, one per
	event, tagged so port/tests/run_tier.py can grep it.
*/
#ifndef PORT_DIAG_H
#define PORT_DIAG_H

#include "compiler.h"		/* PORT_NORETURN */

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	PORT_EXIT_CLEAN    = 0,
	PORT_EXIT_ASSERT   = 10,
	PORT_EXIT_FAULT    = 11,
	PORT_EXIT_WATCHDOG = 12,
	PORT_EXIT_ORACLE   = 13,	/* replay/oracle mismatch */
};

/*	host/input.cpp: judges a scripted run as it ends; nonzero = the run
	failed its own script.  complete = 1 at the scripted exit
	(SBSP_EXIT_AFTER): unsatisfied pad-file entries, unfired bare-pump
	vblanks, desyncs, and a replay that compared nothing - no epoch
	reached, or every one reached blind.  complete = 0 when the user
	closes the window: what has not been reached yet is not a failure, so
	only the epochs reached so far are judged - their desyncs, and all of
	them blind.  */
int		Port_InputAtExit(int complete);

/*	host/input.cpp: how many epochs this replay reached that compared
	nothing (no rng, ram skipped, crc skipped) - [summary]'s blind_epochs,
	which run_tier requires to be 0  */
int		Port_InputBlindEpochs(void);

/*	The one way out of the process: prints [summary], runs the exit hooks
	(Port_OnExit), then _exit(code).  _exit, not exit: the game never shuts
	down on PS1, so its static destructors were never designed to run (one
	traps) - and _exit runs no atexit handler either, hence the hooks.

	The first caller owns the exit: its code is the process exit code and
	the one in [summary], whoever else calls in.  A second call on the SAME
	thread (a fault inside a hook or the summary) exits at once with the
	owner's code; one from ANOTHER thread (the watchdog, a fault on SDL's
	audio thread) waits up to 5s for the owner to finish, then exits with
	the owner's code anyway.  On a fault or a watchdog kill the [summary]
	line goes straight to the stderr handle, past the CRT stream lock that
	the faulting thread may hold.  */
PORT_NORETURN void Port_Exit(int code);

/*	Register fn to run when Port_Exit ends the process: the WAV dump's
	close, the rumble stop.  Hooks run newest first, after [summary] is
	out, and ONLY on the thread that registered them - host state belongs
	to the thread that drives it, so a watchdog or audio-thread exit runs
	none.  On a fault (PORT_EXIT_FAULT) only the faultSafe ones run.
	Allocation-free: at most 8, more are refused with a warning.  */
void	Port_OnExit(void (*fn)(int code), int faultSafe);

/*	printf one line to stderr past the CRT stream lock: formatted on the
	stack (at most 381 chars), written with one WriteFile, "\r\n" appended
	- the bytes fprintf to text-mode stderr would write.  For the fault and
	watchdog paths only, where another thread may hold that lock; anywhere
	else it could interleave with buffered fprintf output.  */
void	Port_StderrRaw(const char *fmt, ...);

/*	Game-side hooks; the game sees these through source/system/asmport.h.  */
void	Port_SceneEvent(const char *sceneName);
void	Port_FmaEvent(int fmaScript);
void	Port_Assert(const char *expr, const char *file, int line);

/*	Game globals the shim's watches read through.  Registered once from
	system/main.cpp; every pointer stays NULL in the shim-only unit exes,
	which is how the watches know there is no game code linked.  */
struct PortGameGlobals
{
	unsigned long	*ramUsed;			/* MainRam.RamUsed   (mem/memory.h)   */
	int				*memNodeCount;		/* MemNodeCount      (mem/memory.cpp) */
	int				*invincibleSponge;	/* player/player.cpp                  */
	unsigned char	**currPrim;			/* gfx/prim.cpp                       */
	unsigned char	**endPrim;
	unsigned char	**primListStart;
	unsigned char	**primListEnd;
	long			*randomSeed;		/* s_randomSeed      (utils/utils.cpp): the
										   RNG state, in every `# epoch` line */
};
void	Port_RegisterGameGlobals(unsigned long *ramUsed, int *memNodeCount,
								 int *invincibleSponge,
								 unsigned char **currPrim, unsigned char **endPrim,
								 unsigned char **primListStart, unsigned char **primListEnd,
								 long *randomSeed);
const struct PortGameGlobals *Port_GameGlobals(void);

/*	Once per vblank (Host_VBlank): RamUsed high-water (SBSP_MEM_LOG=1),
	MemNodeCount vs its 256 cap, scratchpad guard bytes ([mem] LEAK), and
	the SBSP_SELFTEST=<mode>@<vblank> exit-path self-test (diag.cpp selfTest
	lists the modes).  */
void	Port_MemWatch(void);

/*	host/crash.cpp - armed by Port_RegisterGameGlobals  */
void	Port_CrashInit(void);
void	Port_WatchdogStart(void);

const char		*Port_CurrentScene(void);
int				Port_SceneOpenCount(const char *sceneName);
/*	vblank of the nth (1-based) open of a scene; 0 = not opened that often  */
int				Port_SceneOpenVblank(const char *sceneName, int nth, unsigned long *vblank);
/*	vblank of the most recent scene open of any kind (0 before the first)  */
unsigned long	Port_LastSceneOpenVblank(void);

#ifdef __cplusplus
}
#endif

#endif
