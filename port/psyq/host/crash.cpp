/*	Unhandled-exception filter + watchdog thread (M8 harness).

	  [crash] code=<0x...> addr=<p> scene=<name> vblank=<n> ram=<b> memnodes=<n>
	         (the field set the PS1 build's except.cpp dump prints), then exit 11
	  [watchdog] no vblank progress for <s>s (<scene>, vblank <n>), then exit 12

	Both are armed by Port_RegisterGameGlobals, i.e. only in a real game
	process: the shim-only unit exes can legitimately run for a long time
	without advancing the vblank counter.  The watchdog thread only READS
	Port_VBlankCount; it never touches game state.  SBSP_WATCHDOG=<seconds>
	(default 30, 0 = off) arms it; without it, only scripted runs
	(--uncapped / --pad-file / --exit-after / --pad-script) get the default,
	an interactive session never does.  Off under a debugger.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "host/diag.h"
#include "host/pump.h"

static LONG WINAPI crashFilter(EXCEPTION_POINTERS *ep)
{
	const PortGameGlobals	*g    = Port_GameGlobals();
	EXCEPTION_RECORD		*rec  = ep->ExceptionRecord;
	uintptr_t				addr  = (uintptr_t)rec->ExceptionAddress;
	uintptr_t				base  = (uintptr_t)GetModuleHandle(NULL);

	/*	The exe is relocated at load (ASLR), so also print the address as
		the linker saw it - image base 0x400000 + offset - for addr2line.  */
	fprintf(stderr, "[crash] code=0x%08lX addr=%p (link 0x%08lX) scene=%s vblank=%lu ram=%lu memnodes=%d\n",
			(unsigned long)rec->ExceptionCode, rec->ExceptionAddress,
			(unsigned long)(addr - base + 0x400000ul),
			Port_CurrentScene(), Port_VBlankCount(),
			g->ramUsed ? *g->ramUsed : 0ul,
			g->memNodeCount ? *g->memNodeCount : 0);
	if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
		fprintf(stderr, "[crash] access violation: %s %p\n",
				rec->ExceptionInformation[0] ? "write" : "read",
				(void *)rec->ExceptionInformation[1]);
	Port_Exit(PORT_EXIT_FAULT);
}

extern "C" void Port_CrashInit(void)
{
	SetUnhandledExceptionFilter(crashFilter);
}

/*****************************************************************************/
static DWORD WINAPI watchdogThread(LPVOID arg)
{
	int				limit   = (int)(intptr_t)arg;
	unsigned long	last    = Port_VBlankCount();
	int				stalled = 0;

	for (;;)
	{
		Sleep(1000);
		unsigned long now = Port_VBlankCount();
		if (now != last)
		{
			last = now;
			stalled = 0;
			continue;
		}
		if (++stalled >= limit)
		{
			fprintf(stderr, "[watchdog] no vblank progress for %ds (%s, vblank %lu)\n",
					stalled, Port_CurrentScene(), now);
			Port_Exit(PORT_EXIT_WATCHDOG);
		}
	}
}

/*	Armed only for harness runs: SBSP_WATCHDOG given explicitly, or any of
	the scripted-run flags present.  An interactive session legitimately
	stops pumping for as long as the user holds the title bar (Win32's
	modal move loop runs inside SDL_PollEvent), which must not be a kill.  */
static int harnessRun(void)
{
	static const char *const flags[] = { "SBSP_UNCAPPED", "SBSP_PAD_FILE", "SBSP_EXIT_AFTER", "SBSP_PAD_SCRIPT" };
	for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++)
	{
		const char *e = getenv(flags[i]);
		if (e && *e)
			return 1;
	}
	return 0;
}

extern "C" void Port_WatchdogStart(void)
{
	int limit = 30;
	const char *e = getenv("SBSP_WATCHDOG");
	if (e && *e)
		limit = atoi(e);
	else if (!harnessRun())
		return;
	if (limit <= 0)
		return;
	if (IsDebuggerPresent())
	{
		fprintf(stderr, "[watchdog] debugger present - watchdog off\n");
		return;
	}
	HANDLE h = CreateThread(NULL, 0, watchdogThread, (LPVOID)(intptr_t)limit, 0, NULL);
	if (h)
		CloseHandle(h);
	else
		fprintf(stderr, "[watchdog] CreateThread failed - watchdog off\n");
}
