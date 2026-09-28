/*	Crash reporting + watchdog thread (M8 harness, issue #62).

	  [crash] code=<0x...> addr=<p> rva=<0x...> link=<p> scene=<name> vblank=<n> ram=<b> memnodes=<n>
	         an unhandled SEH exception (the field set the PS1 build's
	         except.cpp dump prints); rva/link are "-" outside the exe
	  [crash] kind=abort|terminate|invalid-parameter|purecall scene=... vblank=... ram=... memnodes=...
	         a CRT termination that never raises an SEH exception
	  (either, then exit 11)
	  [watchdog] no vblank progress for <s>s (<scene>, vblank <n>), then exit 12

	`link` is the address as the linker laid it out - the exe file's
	preferred ImageBase + rva - so it goes straight into addr2line /
	llvm-symbolizer on an ASLR-relocated run: 0x400000-based on the 32-bit
	builds, 0x140000000-based on x64.

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
#include <string.h>
#include <signal.h>
#include <exception>			/* std::set_terminate */

#include "host/diag.h"
#include "host/pump.h"

/*	The exe's preferred ImageBase and SizeOfImage, read from the FILE at
	Port_CrashInit.  Not from the mapped header: the loader rewrites its
	ImageBase to the actual load address when ASLR relocates the image,
	which is exactly the number `link` must not be.  0 = unknown.  */
static uintptr_t	g_linkBase;
static DWORD		g_imageSize;

static void readLinkBase(void)
{
	wchar_t	path[1024];
	DWORD	n = GetModuleFileNameW(NULL, path, sizeof(path) / sizeof(path[0]));
	if (n == 0 || n >= sizeof(path) / sizeof(path[0]))
		return;
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
						   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return;
	static unsigned char	b[4096];	/* static: Port_CrashInit may run on a small stack */
	DWORD					got = 0;
	BOOL					ok = ReadFile(h, b, sizeof(b), &got, NULL);
	CloseHandle(h);
	if (!ok || got < 0x40 || b[0] != 'M' || b[1] != 'Z')
		return;

	DWORD pe;
	memcpy(&pe, b + 0x3C, 4);					/* e_lfanew */
	const DWORD opt = pe + 24;					/* "PE\0\0" + IMAGE_FILE_HEADER */
	if (pe > got || got - pe < 24 + 60 || memcmp(b + pe, "PE\0\0", 4) != 0)
		return;
	WORD magic;
	memcpy(&magic, b + opt, 2);
	memcpy(&g_imageSize, b + opt + 56, 4);		/* SizeOfImage: same offset in both */
	if (magic == 0x10B)							/* PE32: 32-bit ImageBase at +28 */
	{
		DWORD base;
		memcpy(&base, b + opt + 28, 4);
		g_linkBase = base;
	}
	else if (magic == 0x20B)					/* PE32+: 64-bit ImageBase at +24 */
	{
		unsigned long long base;
		memcpy(&base, b + opt + 24, 8);
		g_linkBase = (uintptr_t)base;
	}
	if (!g_linkBase)
		g_imageSize = 0;
}

static LONG WINAPI crashFilter(EXCEPTION_POINTERS *ep)
{
	/*	A fault inside this report (a stack overflow re-faulting while it
		formats) re-enters the filter on the same thread: skip straight to
		the exit, whose summary needs no CRT lock on this path.  The report
		lines take none either (Port_StderrRaw): the faulting thread may
		have been mid-printf, holding the stderr stream lock.  */
	static volatile DWORD	reporting;
	const DWORD				self = GetCurrentThreadId();
	if (reporting == self)
		Port_Exit(PORT_EXIT_FAULT);
	reporting = self;

	const PortGameGlobals	*g    = Port_GameGlobals();
	EXCEPTION_RECORD		*rec  = ep->ExceptionRecord;
	uintptr_t				addr  = (uintptr_t)rec->ExceptionAddress;
	uintptr_t				base  = (uintptr_t)GetModuleHandle(NULL);

	/*	The exe is relocated at load (ASLR), so also print the offset into
		the image and the address the linker gave it (see g_linkBase), for
		addr2line.  Outside the exe (SDL3.dll, the CRT, ntdll) neither
		means anything.  */
	char where[64];
	if (g_imageSize && addr >= base && addr - base < g_imageSize)
		snprintf(where, sizeof(where), "rva=0x%lX link=%p",
				 (unsigned long)(addr - base), (void *)(g_linkBase + (addr - base)));
	else
		snprintf(where, sizeof(where), "rva=- link=-");
	Port_StderrRaw("[crash] code=0x%08lX addr=%p %s scene=%s vblank=%lu ram=%lu memnodes=%d",
				   (unsigned long)rec->ExceptionCode, rec->ExceptionAddress, where,
				   Port_CurrentScene(), Port_VBlankCount(),
				   g->ramUsed ? *g->ramUsed : 0ul,
				   g->memNodeCount ? *g->memNodeCount : 0);
	if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
		Port_StderrRaw("[crash] access violation: %s %p",
					   rec->ExceptionInformation[0] ? "write" : "read",
					   (void *)rec->ExceptionInformation[1]);
	Port_Exit(PORT_EXIT_FAULT);
}

/*	The CRT's own ways to end a process, none of which raises an SEH
	exception the filter would see: abort() (and std::terminate's default,
	which is abort), an invalid CRT argument, a pure virtual call.  Each
	reports like a fault and leaves with the same exit code, 11 - and, like
	one, past the stderr stream lock: abort() can come from any thread,
	mid-printf on another.  */
PORT_NORETURN static void crashReport(const char *kind)
{
	const PortGameGlobals *g = Port_GameGlobals();
	Port_StderrRaw("[crash] kind=%s scene=%s vblank=%lu ram=%lu memnodes=%d",
				   kind, Port_CurrentScene(), Port_VBlankCount(),
				   g->ramUsed ? *g->ramUsed : 0ul,
				   g->memNodeCount ? *g->memNodeCount : 0);
	Port_Exit(PORT_EXIT_FAULT);
}

static void __cdecl onSigabrt(int sig)
{
	/*	Both CRTs reset SIGABRT to SIG_DFL before calling the handler, so
		re-arm first: a second abort() - another thread, or a fault-safe
		exit hook this report's Port_Exit runs - must land here again and
		become an ordinary second Port_Exit caller.  At SIG_DFL it would
		end the process with the CRT's own exit code 3 while [summary]
		says 11.  */
	signal(SIGABRT, onSigabrt);
	(void)sig;
	crashReport("abort");
}

static void onTerminate()
{
	crashReport("terminate");
}

static void __cdecl onInvalidParameter(const wchar_t *expr, const wchar_t *func,
									   const wchar_t *file, unsigned int line, uintptr_t reserved)
{
	(void)expr; (void)func; (void)file; (void)line; (void)reserved;	/* all NULL in a release CRT */
	crashReport("invalid-parameter");
}

#if defined(_MSC_VER)
static void __cdecl onPurecall(void)
{
	crashReport("purecall");
}
#endif

extern "C" void Port_CrashInit(void)
{
	readLinkBase();
	SetUnhandledExceptionFilter(crashFilter);

	/*	Both CRTs (MinGW's msvcrt.dll, clang-cl's static UCRT).  SIGABRT is
		process-wide; the MSVC terminate handler is per-thread, but a thread
		without one calls abort(), which lands in SIGABRT anyway.  */
	signal(SIGABRT, onSigabrt);
	std::set_terminate(onTerminate);
	_set_invalid_parameter_handler(onInvalidParameter);
#if defined(_MSC_VER)
	/*	msvcrt.dll has neither.  The UCRT's abort() raises SIGABRT before
		it looks at the abort-behavior flags, so onSigabrt sees every
		abort() while it is armed; the flags only decide what happens when
		no handler is installed (the instant between the CRT's reset and
		onSigabrt's re-arm).  Clearing _CALL_REPORTFAULT makes that an
		_exit(3) instead of a __fastfail to Windows Error Reporting that
		prints nothing.  _WRITE_ABORT_MSG's "abort() has been called"
		report exists only in the debug CRT, and these builds link the
		release one (/MT), so clearing it changes nothing here.  */
	_set_purecall_handler(onPurecall);
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

	/*	Room for the filter to run on after EXCEPTION_STACK_OVERFLOW: the
		guard page is gone by then, and formatting the report needs more
		than the few KB Windows leaves.  This thread only (it is the game's
		main thread, Port_RegisterGameGlobals); SDL's audio thread keeps the
		default.  */
	ULONG guarantee = 64 * 1024;
	if (!SetThreadStackGuarantee(&guarantee))
		fprintf(stderr, "[crash] SetThreadStackGuarantee failed (%lu)\n", (unsigned long)GetLastError());
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
		if (now != last || Port_Paused())	/* paused (focus lost): not a stall */
		{
			last = now;
			stalled = 0;
			continue;
		}
		if (++stalled >= limit)
		{
			/*	past the stderr lock: the stuck thread may hold it  */
			Port_StderrRaw("[watchdog] no vblank progress for %ds (%s, vblank %lu)",
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

/*	The same predicate for the rest of the shell (M8 shell): a scripted run
	neither pauses on focus loss nor writes a default sbsp.ini.  Not cached
	- args.cpp asks before its own _putenv pass has necessarily finished.  */
extern "C" int Port_HarnessRun(void)
{
	return harnessRun();
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
