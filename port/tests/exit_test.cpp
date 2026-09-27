/*	Unit test for the host exit path (port/psyq/host/diag.cpp, issue #62):
	Port_Exit's owner rule and the Port_OnExit hooks.

	Every case ends a process, so each runs in a child - this exe again,
	with a mode argument - whose stderr is redirected to a file named by
	EXIT_TEST_STDERR (both the CRT stream and the Win32 handle, since the
	fault path writes [summary] to the handle directly).  The parent checks
	the child's exit code and what reached that file:

	  hooks     hooks run newest first, after [summary]; exit 10
	  fault     on PORT_EXIT_FAULT only the faultSafe hook runs, and the
	            lock-free [summary] still arrives, CRLF-terminated; exit 11
	  thread    a hook registered on main does not run when a worker thread
	            exits the process
	  blocked   the owner (a worker) blocks inside a hook while main calls
	            Port_Exit(12): main waits its bounded 5s and exits with the
	            owner's 10; [summary] appears once, saying 10
	  reenter   a hook that calls Port_Exit(11) still exits 10
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host/diag.h"

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*****************************************************************************/
/*	child side  */

static void say(const char *text)
{
	std::fprintf(stderr, "[exit_test] %s\n", text);
	std::fflush(stderr);
}

static void hookA(int code)		{ (void)code; say("hook A"); }
static void hookB(int code)		{ (void)code; say("hook B"); }
static void hookMain(int code)	{ (void)code; say("hook main"); }
static void hookReenter(int code)
{
	(void)code;
	say("hook reenter");
	Port_Exit(PORT_EXIT_FAULT);
}

static HANDLE g_inHook;

static void hookBlock(int code)
{
	(void)code;
	say("hook blocking");
	SetEvent(g_inHook);
	Sleep(INFINITE);
}

static DWORD WINAPI workerExit(LPVOID arg)
{
	(void)arg;
	Port_Exit(PORT_EXIT_CLEAN);
}

static DWORD WINAPI workerOwner(LPVOID arg)
{
	(void)arg;
	Port_OnExit(hookBlock, 1);
	Port_Exit(PORT_EXIT_ASSERT);
}

static int childMain(const char *mode)
{
	const char *path = std::getenv("EXIT_TEST_STDERR");
	if (path && *path)
	{
		if (!std::freopen(path, "w", stderr))
			return 2;
		std::setvbuf(stderr, NULL, _IONBF, 0);
		SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
	}

	if (std::strcmp(mode, "hooks") == 0)
	{
		Port_OnExit(hookA, 0);
		Port_OnExit(hookB, 1);
		Port_Exit(PORT_EXIT_ASSERT);
	}
	if (std::strcmp(mode, "fault") == 0)
	{
		Port_OnExit(hookA, 0);
		Port_OnExit(hookB, 1);
		Port_Exit(PORT_EXIT_FAULT);
	}
	if (std::strcmp(mode, "thread") == 0)
	{
		Port_OnExit(hookMain, 1);
		HANDLE t = CreateThread(NULL, 0, workerExit, NULL, 0, NULL);
		if (t)
			WaitForSingleObject(t, INFINITE);
		say("worker returned");
		return 3;
	}
	if (std::strcmp(mode, "blocked") == 0)
	{
		g_inHook = CreateEventA(NULL, TRUE, FALSE, NULL);
		HANDLE t = CreateThread(NULL, 0, workerOwner, NULL, 0, NULL);
		if (!t || WaitForSingleObject(g_inHook, 20000) != WAIT_OBJECT_0)
			return 4;
		Port_Exit(PORT_EXIT_WATCHDOG);
	}
	if (std::strcmp(mode, "reenter") == 0)
	{
		Port_OnExit(hookReenter, 1);
		Port_Exit(PORT_EXIT_ASSERT);
	}
	return 5;
}

/*****************************************************************************/
/*	parent side  */

static char g_exe[MAX_PATH], g_log[MAX_PATH + 64];
static char g_text[16384];

static int spawnSelf(const char *mode, DWORD *ms)
{
	char quoted[MAX_PATH + 4];
	std::snprintf(quoted, sizeof(quoted), "\"%s\"", g_exe);
	std::remove(g_log);
	DWORD t0 = GetTickCount();
	intptr_t rc = _spawnl(_P_WAIT, g_exe, quoted, mode, (const char *)NULL);
	if (ms)
		*ms = GetTickCount() - t0;
	if (rc == -1)
		std::printf("FAIL: _spawnl(%s) failed\n", mode);

	g_text[0] = 0;
	FILE *f = std::fopen(g_log, "rb");
	if (f)
	{
		size_t n = std::fread(g_text, 1, sizeof(g_text) - 1, f);
		g_text[n] = 0;
		std::fclose(f);
	}
	return (int)rc;
}

static int count(const char *needle)
{
	int n = 0;
	for (const char *p = g_text; (p = std::strstr(p, needle)) != NULL; p += std::strlen(needle))
		n++;
	return n;
}

static const char *at(const char *needle)
{
	const char *p = std::strstr(g_text, needle);
	return p ? p : g_text + std::strlen(g_text) + 1;	/* absent sorts last */
}

static void report(const char *mode)
{
	if (g_failures)
		std::printf("--- %s child stderr ---\n%s--- end ---\n", mode, g_text);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		return childMain(argv[1]);

	char tmp[MAX_PATH];
	if (!GetModuleFileNameA(NULL, g_exe, sizeof(g_exe)) || !GetTempPathA(sizeof(tmp), tmp))
	{
		std::printf("FAIL: GetModuleFileNameA / GetTempPathA\n");
		return 1;
	}
	std::snprintf(g_log, sizeof(g_log), "%ssbsp_exit_test_%lu.txt", tmp, GetCurrentProcessId());
	char env[MAX_PATH + 96];
	std::snprintf(env, sizeof(env), "EXIT_TEST_STDERR=%s", g_log);
	_putenv(env);

	int before, rc;
	DWORD ms;

	/*	1. hooks run newest first, after the contract line  */
	before = g_failures;
	rc = spawnSelf("hooks", NULL);
	check(rc == PORT_EXIT_ASSERT, "hooks: exit code 10");
	check(count("[summary] exit=10 ") == 1, "hooks: one [summary] saying exit=10");
	check(count("hook A") == 1 && count("hook B") == 1, "hooks: both hooks ran once");
	check(at("hook B") < at("hook A"), "hooks: newest first");
	check(at("[summary]") < at("hook B"), "hooks: [summary] before any hook");
	if (g_failures != before)
		report("hooks");

	/*	2. a fault runs only the faultSafe hooks; the summary bypasses the CRT  */
	before = g_failures;
	rc = spawnSelf("fault", NULL);
	check(rc == PORT_EXIT_FAULT, "fault: exit code 11");
	check(count("[summary] exit=11 ") == 1, "fault: one [summary] saying exit=11");
	check(std::strstr(g_text, "paused=0.0\r\n") != NULL, "fault: lock-free [summary] ends in CRLF like the stream's");
	check(count("hook B") == 1, "fault: the faultSafe hook ran");
	check(count("hook A") == 0, "fault: the other hook did not");
	if (g_failures != before)
		report("fault");

	/*	3. another thread's exit runs none of main's hooks  */
	before = g_failures;
	rc = spawnSelf("thread", NULL);
	check(rc == PORT_EXIT_CLEAN, "thread: exit code 0");
	check(count("[summary] exit=0 ") == 1, "thread: one [summary]");
	check(count("hook main") == 0, "thread: main's hook did not run on the worker");
	check(count("worker returned") == 0, "thread: the worker's Port_Exit ended the process");
	if (g_failures != before)
		report("thread");

	/*	4. an owner stuck in a hook: the second caller waits, bounded, and
		the owner's code wins  */
	before = g_failures;
	rc = spawnSelf("blocked", &ms);
	check(rc == PORT_EXIT_ASSERT, "blocked: exit code is the owner's 10, not main's 12");
	check(count("[summary]") == 1 && count("[summary] exit=10 ") == 1, "blocked: [summary] once, saying 10");
	check(count("hook blocking") == 1, "blocked: the owner reached its hook");
	check(ms >= 4000 && ms < 20000, "blocked: the second caller waited its ~5s and no longer");
	if (g_failures != before)
	{
		std::printf("blocked: child took %lu ms\n", (unsigned long)ms);
		report("blocked");
	}

	/*	5. re-entering from a hook: out at once, with the owner's code  */
	before = g_failures;
	rc = spawnSelf("reenter", &ms);
	check(rc == PORT_EXIT_ASSERT, "reenter: exit code 10, not the hook's 11");
	check(count("[summary]") == 1, "reenter: [summary] once");
	check(count("hook reenter") == 1, "reenter: the hook ran once");
	check(ms < 4000, "reenter: no wait on the owner's own thread");
	if (g_failures != before)
		report("reenter");

	std::remove(g_log);
	if (g_failures)
	{
		std::printf("exit_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("exit_test: all passed\n");
	return 0;
}
