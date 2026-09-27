/*	Emulated time is waited time (issue #67), on the shim alone.

	A vblank fires only in a wait step - Port_PumpIdle, the body of VSync's
	wait, a paced CdReadSync and StGetNext - capped or uncapped.  A bare
	Port_Pump (VSync(-1), DrawSync, PadGetState) fires none, even when a
	capped run has fallen behind the wall clock, so a capped run and an
	uncapped one see the same vblanks at the same points of the game and a
	capped recording replays uncapped.  After PORT_SPIN_PUMPS bare pumps in a
	row each further bare pump is a one-vblank wait (VRamViewer's VSync-less
	loop), until the game waits for itself again.

	The parent runs the checks capped, then spawns itself as "uncapped" to
	run them again under SBSP_UNCAPPED=1: the mode is cached on first use,
	so each needs its own process.  Headless: the dummy video driver.
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "host/pump.h"		/* Port_Pump, Port_VBlankCount, PORT_SPIN_PUMPS */

extern "C" int	VSync(int mode);

static int		g_failures;
static const char	*g_mode = "capped";

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL (%s): %s\n", g_mode, what);
		g_failures++;
	}
}

static void expectCount(unsigned long want, const char *what)
{
	char msg[160];
	const unsigned long got = Port_VBlankCount();
	std::snprintf(msg, sizeof(msg), "%s: vblank %lu, want %lu", what, got, want);
	check(got == want, msg);
}

static int runChecks(bool capped)
{
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	static const char *const clear[] =
	{
		"SBSP_FRAME_CRC=", "SBSP_EXIT_AFTER=", "SBSP_SELFTEST=", "SBSP_DUMP_FRAMES=",
		"SBSP_DUMP_AUDIO=", "SBSP_PACE_LOG=", "SBSP_PAD_FILE=", "SBSP_PAD_SCRIPT=",
		"SBSP_RECORD_PAD=",
	};
	for (const char *c : clear)
		_putenv(c);
	_putenv(capped ? "SBSP_UNCAPPED=" : "SBSP_UNCAPPED=1");	/* before the first pump */
	check(Port_Uncapped() == (capped ? 0 : 1), "the mode is the one asked for");

	/*	1. a wait fires one vblank  */
	VSync(0);
	const unsigned long c0 = Port_VBlankCount();
	check(c0 >= 1, "VSync(0) fires a vblank");

	/*	2. bare pumps fire none - capped, not even with seven vblanks of wall
		time owed (the rebase starts at eight)  */
	if (capped)
		Sleep(7 * 1000 / 60 + 3);
	for (int i = 0; i < 100; i++)
		Port_Pump();
	check(VSync(-1) == (int)c0, "VSync(-1) reads the counter without moving it");
	expectCount(c0, "100 bare pumps, the wall clock ahead");

	/*	3. each wait fires exactly one, owed time or not: a capped run that
		fell behind catches up through its waits  */
	VSync(0);
	expectCount(c0 + 1, "one VSync(0) after the bare pumps");
	for (int i = 0; i < 5; i++)
		VSync(0);
	expectCount(c0 + 6, "five more VSync(0)");

	/*	4. the spin rule: PORT_SPIN_PUMPS bare pumps in a row fire nothing,
		every further one is a one-vblank wait, and a real wait ends it  */
	VSync(0);
	const unsigned long c1 = Port_VBlankCount();
	for (int i = 0; i < PORT_SPIN_PUMPS; i++)
		Port_Pump();
	expectCount(c1, "PORT_SPIN_PUMPS bare pumps in a row");
	for (int i = 0; i < 3; i++)
		Port_Pump();
	expectCount(c1 + 3, "three bare pumps past the spin limit, one vblank each");
	VSync(0);
	expectCount(c1 + 4, "VSync(0) inside a spin");
	Port_Pump();
	expectCount(c1 + 4, "a bare pump after a real wait fires nothing again");

	SDL_Quit();
	return g_failures ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "uncapped") == 0)
	{
		g_mode = "uncapped";
		return runChecks(false);
	}

	const int capped = runChecks(true);

	char exe[MAX_PATH], quoted[MAX_PATH + 4];
	int uncapped = 1;
	if (GetModuleFileNameA(NULL, exe, sizeof(exe)))
	{
		std::snprintf(quoted, sizeof(quoted), "\"%s\"", exe);
		intptr_t rc = _spawnl(_P_WAIT, exe, quoted, "uncapped", (const char *)NULL);
		uncapped = rc == 0 ? 0 : 1;
		if (rc == -1)
			std::printf("FAIL: _spawnl(uncapped) failed\n");
	}
	else
		std::printf("FAIL: GetModuleFileNameA\n");

	if (capped || uncapped)
	{
		std::printf("pump_test: failed (%s%s%s)\n", capped ? "capped" : "",
					capped && uncapped ? ", " : "", uncapped ? "uncapped" : "");
		return 1;
	}
	std::printf("pump_test: all passed\n");
	return 0;
}
