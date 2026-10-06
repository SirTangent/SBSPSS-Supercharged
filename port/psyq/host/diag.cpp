/*	M8 harness diagnostics - see diag.h.

	  [scene] <name> vblank=<n>                       GameState opened a scene;
	                                                  FMA scripts add FMA:<script>
	  [assert] <expr> at <file>:<line> (<scene>, vblank <n>)
	  [summary] exit=<code> vblanks=<n> scene=<name> asserts=<n> ...
	            render=<GPU_RENDER_REVISION> blind_epochs=<n>

	Exit codes: 0 clean, 10 assert, 11 fault, 12 watchdog, 13 oracle/replay.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>				/* _close - SBSP_SELFTEST=invalid-param */
#include <exception>		/* std::terminate - SBSP_SELFTEST=terminate */

#include "system/types.h"
#include "system/asmport.h"		/* PORT_Scratchpad + guard */
#include "host/diag.h"
#include "host/pump.h"
#include "gpu/gpu_core.h"		/* GPU_RENDER_REVISION, in [summary] */

extern "C" unsigned long GPU_PrimPoolPeak(void);	/* gpu/gp0.cpp */
void VkPresent_SelfTestLose(void);						/* vk/vk_present.cpp */

namespace
{

/*	NO C++ containers / delete in shim code that links into the game exe:
	mem/memory.cpp replaces the global operator delete (-> MemFree on the
	game arena) but supplies only a two-argument operator new, so a shim
	`new` lands in the CRT heap and its `delete` trashes the game heap
	("Memory guard trashed").  malloc/realloc/free only.  */
struct SceneRec
{
	char			name[48];
	unsigned long	*opens;		/* vblank of each open, in order */
	int				count, cap;
};

enum { MAX_SCENES = 32 };
SceneRec				g_scenes[MAX_SCENES];
int						g_sceneCount;
unsigned long			g_lastOpenVblank;
char					g_currentScene[64] = "boot";	/* plain chars: the watchdog
														   thread reads it unlocked */
unsigned long			g_assertCount;
PortGameGlobals			g_globals;		/* all NULL until Port_RegisterGameGlobals */
unsigned long			g_peakRam;
int						g_peakNodes;

SceneRec *findScene(const char *name)
{
	for (int i = 0; i < g_sceneCount; i++)
		if (strcmp(g_scenes[i].name, name) == 0)
			return &g_scenes[i];
	return NULL;
}

int envFlag(const char *name)
{
	const char *e = getenv(name);
	return e && *e && *e != '0';
}

/*	SBSP_SELFTEST=stack-overflow: unbounded recursion the optimizer cannot
	see through (the call goes through a volatile pointer, and the frame is
	still live after it, so it is no tail call either).  */
void recurse(volatile char *prev);
void (*volatile g_recurse)(volatile char *) = recurse;

void recurse(volatile char *prev)
{
	volatile char frame[1024];
	frame[0] = prev ? prev[0] : 1;
	g_recurse(frame);
	frame[1] = frame[0];
}

/*	SBSP_SELFTEST=abort-in-hook: a fault-safe exit hook that aborts, so the
	abort's own Port_Exit meets a second abort() - SIGABRT must still be
	armed, and the second report is an ordinary second Port_Exit caller.  */
void abortHook(int code)
{
	(void)code;
	abort();
}

/*	SBSP_SELFTEST=<mode>@<vblank>: exercise one exit path on purpose so
	run_tier.py can prove the exit codes (10/11/12) and their log lines
	without a throwaway build.  Modes: assert, fault, hang, and the CRT
	terminations host/crash.cpp routes to exit 11 (issue #62): abort,
	abort-in-hook, terminate, invalid-param, stack-overflow.  */
void selfTest(void)
{
	static int			parsed;
	static char			mode[16];
	static unsigned long	at;

	if (!parsed)
	{
		parsed = 1;
		const char *e = getenv("SBSP_SELFTEST");
		if (e && *e)
		{
			char *end;
			const char *sep = strchr(e, '@');
			size_t n = sep ? (size_t)(sep - e) : strlen(e);
			snprintf(mode, sizeof(mode), "%.*s", (int)(n < sizeof(mode) - 1 ? n : sizeof(mode) - 1), e);
			at = sep ? strtoul(sep + 1, &end, 10) : 1;
			fprintf(stderr, "[selftest] %s at vblank %lu\n", mode, at);
		}
	}
	if (!mode[0] || Port_VBlankCount() < at)
		return;
	if (strcmp(mode, "assert") == 0)
		Port_Assert("SBSP_SELFTEST", __FILE__, __LINE__);
	else if (strcmp(mode, "fault") == 0)
		*(volatile int *)0 = 0;
	else if (strcmp(mode, "hang") == 0)
		for (;;)
			Sleep(1);		/* never pumps: only the watchdog thread can end this */
	else if (strcmp(mode, "abort") == 0)
		abort();
	else if (strcmp(mode, "abort-in-hook") == 0)
	{
		Port_OnExit(abortHook, 1);
		abort();
	}
	else if (strcmp(mode, "terminate") == 0)
		std::terminate();
	else if (strcmp(mode, "invalid-param") == 0)
	{
		/*	fd -1: a CRT that validates calls the invalid-parameter handler;
			one that does not just returns -1 (EBADF) and says so here,
			which run_tier reports as a SKIP rather than a pass  */
		_close(-1);
		fprintf(stderr, "[selftest] invalid-param returned - this CRT did not report it\n");
	}
	else if (strcmp(mode, "stack-overflow") == 0)
		g_recurse(NULL);
	else if (strcmp(mode, "vklost") == 0)
		VkPresent_SelfTestLose();	/* the presenter reads its next fence as DEVICE_LOST (#63) */
	else
		fprintf(stderr, "[selftest] unknown mode '%s' - ignored\n", mode);
	mode[0] = 0;
}

/*	subEvent: a second name for the scene that just opened (FMA:<script>),
	recorded at that open's vblank so entries anchored to either name mean
	the same occurrence, and never counted as a new open of its own.  */
void noteOpen(const char *name, int subEvent)
{
	unsigned long vb = subEvent ? g_lastOpenVblank : Port_VBlankCount();
	SceneRec *r = findScene(name);
	if (!r && g_sceneCount < MAX_SCENES)
	{
		r = &g_scenes[g_sceneCount++];
		snprintf(r->name, sizeof(r->name), "%s", name);
	}
	if (r)
	{
		if (r->count == r->cap)
		{
			r->cap   = r->cap ? r->cap * 2 : 8;
			r->opens = (unsigned long *)realloc(r->opens, r->cap * sizeof(unsigned long));
		}
		r->opens[r->count++] = vb;
	}
	if (!subEvent)
		g_lastOpenVblank = vb;
	snprintf(g_currentScene, sizeof(g_currentScene), "%s", name);
	fprintf(stderr, "[scene] %s vblank=%lu\n", name, vb);
	fflush(stderr);
}

}

/*****************************************************************************/
extern "C" void Port_SceneEvent(const char *name)
{
	noteOpen(name ? name : "?", 0);
}

extern "C" void Port_FmaEvent(int script)
{
	/* index = CFmaScene::FMA_SCRIPT_NUMBER (source/fma/fma.h) */
	static const char *const names[] =
	{
		"FMA:INTRO",       "FMA:CH1FINISHED", "FMA:CH2FINISHED", "FMA:CH3FINISHED",
		"FMA:CH4FINISHED", "FMA:CH5FINISHED", "FMA:PLANKTON",    "FMA:PARTY",
	};
	char buf[32];
	if (script >= 0 && script < (int)(sizeof(names) / sizeof(names[0])))
		noteOpen(names[script], 1);
	else
	{
		snprintf(buf, sizeof(buf), "FMA:%d", script);
		noteOpen(buf, 1);
	}
}

extern "C" const char *Port_CurrentScene(void)
{
	return g_currentScene;
}

/*****************************************************************************/
extern "C" void Port_RegisterGameGlobals(unsigned long *ramUsed, int *memNodeCount,
										 int *invincibleSponge,
										 unsigned char **currPrim, unsigned char **endPrim,
										 unsigned char **primListStart, unsigned char **primListEnd,
										 long *randomSeed)
{
	g_globals.ramUsed          = ramUsed;
	g_globals.memNodeCount     = memNodeCount;
	g_globals.invincibleSponge = invincibleSponge;
	g_globals.currPrim         = currPrim;
	g_globals.endPrim          = endPrim;
	g_globals.primListStart    = primListStart;
	g_globals.primListEnd      = primListEnd;
	g_globals.randomSeed       = randomSeed;

	/*	--invincible: the DEBUG pause menu's toggle (player.cpp
		invincibleSponge, read by CPlayer::takeDamage) exists in both
		variants; only the menu is DEBUG-only.  */
	if (invincibleSponge && envFlag("SBSP_INVINCIBLE"))
	{
		*invincibleSponge = 1;
		fprintf(stderr, "[args] invincible: on\n");
	}

	Port_CrashInit();
	Port_WatchdogStart();
}

extern "C" const PortGameGlobals *Port_GameGlobals(void)
{
	return &g_globals;
}

/*****************************************************************************/
/*	Mirrors gp0.cpp's primPoolWatch: high-water logging behind an env flag,
	one-shot warnings at 87.5% of a hard cap.  MemNodeCount's cap is LListLen
	(256, mem/memory.h) - the game's own `ASSERT(MemNodeCount<LListLen)` is
	DEBUG-only and post-hoc.  */
extern "C" void Port_MemWatch(void)
{
	static int	logging = -1;
	static int	warnedNodes, warnedPad;

	if (logging < 0)
		logging = envFlag("SBSP_MEM_LOG");

	if (g_globals.ramUsed && *g_globals.ramUsed > g_peakRam)
	{
		g_peakRam = *g_globals.ramUsed;
		if (logging)
			fprintf(stderr, "[mem] RamUsed high-water %lu bytes (%s, vblank %lu)\n",
					g_peakRam, g_currentScene, Port_VBlankCount());
	}

	if (g_globals.memNodeCount)
	{
		int n = *g_globals.memNodeCount;
		if (n > g_peakNodes)
			g_peakNodes = n;
		if (!warnedNodes && n >= 256 - 256 / 8)
		{
			warnedNodes = 1;
			fprintf(stderr, "[mem] WARNING: MemNodeCount at %d/256 (%s, vblank %lu) - "
							"raise LListLen (source/mem/memory.h) before it overruns\n",
					n, g_currentScene, Port_VBlankCount());
		}
	}

	if (!warnedPad)
	{
		for (int i = 0; i < PORT_SCRATCHPAD_GUARD; i++)
		{
			if (PORT_Scratchpad[1024 + i] != PORT_SCRATCHPAD_GUARD_BYTE)
			{
				warnedPad = 1;
				fprintf(stderr, "[mem] LEAK scratchpad overrun: guard byte %d = 0x%02X "
								"(%s, vblank %lu)\n",
						i, PORT_Scratchpad[1024 + i], g_currentScene,
						Port_VBlankCount());
				break;
			}
		}
	}
	fflush(stderr);
	selfTest();
}

extern "C" int Port_SceneOpenCount(const char *name)
{
	SceneRec *r = findScene(name);
	return r ? r->count : 0;
}

extern "C" unsigned long Port_LastSceneOpenVblank(void)
{
	return g_lastOpenVblank;
}

extern "C" int Port_SceneOpenVblank(const char *name, int nth, unsigned long *vblank)
{
	SceneRec *r = findScene(name);
	if (!r || nth < 1 || nth > r->count)
		return 0;
	*vblank = r->opens[nth - 1];
	return 1;
}

/*****************************************************************************/
extern "C" void Port_Assert(const char *expr, const char *file, int line)
{
	g_assertCount++;
	fprintf(stderr, "[assert] %s at %s:%d (%s, vblank %lu)\n",
			expr, file, line, g_currentScene, Port_VBlankCount());
	fflush(stderr);
	if (!envFlag("SBSP_ASSERT_CONTINUE"))
		Port_Exit(PORT_EXIT_ASSERT);
}

/*****************************************************************************/
/*	One stderr line that bypasses the CRT (issue #62): formatted on the
	stack and written to the handle in one WriteFile, with the "\r\n" the
	text-mode stream would have produced, so the bytes match fprintf's.
	fprintf would wait forever on a stream lock that a faulting thread (or
	the thread the watchdog interrupted) holds mid-printf.  */
extern "C" void Port_StderrRaw(const char *fmt, ...)
{
	char	line[384];
	va_list	ap;
	va_start(ap, fmt);
	int		n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	if (n < 0)
		n = 0;
	if (n > (int)sizeof(line) - 3)
		n = (int)sizeof(line) - 3;
	line[n++] = '\r';
	line[n++] = '\n';
	DWORD written;
	HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
	if (h && h != INVALID_HANDLE_VALUE)
		WriteFile(h, line, (DWORD)n, &written, NULL);
}

/*****************************************************************************/
/*	Exit hooks (issue #62): what used to be atexit's job - the WAV dump's
	header, the rumble stop - for a process that only ever leaves through
	_exit.  A fixed table and no allocation: a hook may be registered from a
	static-init path, and Port_Exit may be running on a faulting thread.  */
namespace
{

struct ExitHook
{
	void			(*volatile fn)(int code);	/* written last: the slot is live */
	int				faultSafe;
	DWORD			thread;						/* the registering thread */
};

enum { MAX_EXIT_HOOKS = 8 };
ExitHook		g_exitHooks[MAX_EXIT_HOOKS];
volatile LONG	g_exitHookClaims;

/*	[summary]: the contract line run_tier.py parses.  On a fault or a
	watchdog kill it goes out through Port_StderrRaw, past the CRT stream
	lock the faulting thread (or, for the watchdog, whichever thread was
	mid-printf) may hold.  */
void writeSummary(int code, int lockFree)
{
	char	line[384];
	int		n = snprintf(line, sizeof(line),
						 "[summary] exit=%d vblanks=%lu scene=%s asserts=%lu "
						 "peak_ram=%lu peak_memnodes=%d/256 peak_prim=%lu paused=%.1f "
						 "render=%d blind_epochs=%d",
						 code, Port_VBlankCount(), g_currentScene, g_assertCount,
						 g_peakRam, g_peakNodes, GPU_PrimPoolPeak(), Host_PausedSeconds(),
						 GPU_RENDER_REVISION, Port_InputBlindEpochs());
	if (n < 0)
		n = 0;
	if (n > (int)sizeof(line) - 1)
		n = (int)sizeof(line) - 1;
	if (lockFree)
	{
		Port_StderrRaw("%.*s", n, line);
		return;
	}
	fprintf(stderr, "%.*s\n", n, line);
	fflush(stderr);
}

}

extern "C" void Port_OnExit(void (*fn)(int code), int faultSafe)
{
	LONG slot = InterlockedIncrement(&g_exitHookClaims) - 1;
	if (slot >= MAX_EXIT_HOOKS)
	{
		fprintf(stderr, "[diag] more than %d exit hooks - one will not run\n", MAX_EXIT_HOOKS);
		return;
	}
	g_exitHooks[slot].faultSafe = faultSafe;
	g_exitHooks[slot].thread    = GetCurrentThreadId();
	MemoryBarrier();				/* the fields before the pointer that publishes them */
	g_exitHooks[slot].fn        = fn;
}

extern "C" PORT_NORETURN void Port_Exit(int code)
{
	static volatile LONG	entered;
	static volatile LONG	ownerCode;
	static volatile DWORD	ownerThread;
	const DWORD				self = GetCurrentThreadId();

	/*	A second caller never decides the exit code: the first owns it and
		the [summary] that states it.  On the owner's own thread this is a
		re-fault inside a hook or the summary - nothing left to wait for, so
		go now.  On another thread (the watchdog, a fault on SDL's audio
		thread) the owner is probably still writing: give it 5s, then leave
		with its code anyway - a hook stuck on a lock must not hang the
		process.  (ownerThread still 0 means the owner is between its CAS
		and the store below, necessarily on another thread.)  */
	if (InterlockedCompareExchange(&entered, 1, 0) != 0)
	{
		if (ownerThread != self)
			Sleep(5000);
		_exit((int)ownerCode);
	}
	ownerCode   = code;
	ownerThread = self;
	MemoryBarrier();

	/*	The game's own printf text is stdout, block-buffered whenever it is
		redirected (the tester zip's stdout.txt, run_tier's pipe), and _exit
		never flushes it: the last lines before an assert or a scripted exit
		were lost.  Not on a fault or a watchdog kill - the faulting thread
		may hold the CRT stream lock mid-printf and the watchdog is another
		thread altogether, so flushing there could hang the exit; for those
		the per-vblank flush in Host_VBlank bounds the loss to one frame.  */
	const int crashing = (code == PORT_EXIT_FAULT || code == PORT_EXIT_WATCHDOG);
	if (!crashing)
		fflush(stdout);

	/*	The contract line first, then the hooks: a hook that hangs or
		re-faults can no longer cost the run its [summary].  */
	writeSummary(code, crashing);

	LONG n = g_exitHookClaims;
	if (n > MAX_EXIT_HOOKS)
		n = MAX_EXIT_HOOKS;
	for (LONG i = n - 1; i >= 0; i--)
	{
		const ExitHook	*h  = &g_exitHooks[i];
		void			(*fn)(int) = h->fn;
		if (!fn || h->thread != self)
			continue;				/* not live yet, or another thread's state */
		if (code == PORT_EXIT_FAULT && !h->faultSafe)
			continue;
		fn(code);
	}
	_exit(code);
}
