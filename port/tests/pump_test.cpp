/*	Where a vblank may fire (issue #67), on the shim alone.

	A wait step - Port_PumpIdle, the body of VSync's wait, a paced
	CdReadSync and StGetNext - fires one vblank when it is due.  A bare
	Port_Pump (VSync(-1), DrawSync, PadGetState, the front of every wait) is
	where the modes part:
	  - a live capped run fires a due vblank there too, as the PlayStation's
	    interrupt would, and --record-pad writes each as `# bare <vblank>
	    <k>` (the k-th bare pump since the last wait step);
	  - a scripted run fires one there only where its recording says - so a
	    capped recording replays exactly, capped or uncapped - and a script
	    without `# bare` lines fires none there, wall time owed or not;
	  - any other uncapped run fires none there.
	After PORT_SPIN_PUMPS bare pumps in a row each further bare pump is a
	one-vblank wait (VRamViewer's VSync-less loop), until the game waits
	for itself again.

	The parent only orchestrates.  Each mode is a child process, because the
	pace mode and the pad script are cached on first use:
	  record          capped, live, recording A: bare pumps with wall time
	                  owed fire one each, and A gets their `# bare` lines
	  uncapped        bare pumps fire nothing; the spin rule
	  script          capped, --pad-script and no `# bare`: nothing either
	  replay          A replayed capped, recording B
	  replay          A replayed uncapped, recording C
	then B and C must equal A line for line, `# pace` aside, and
	  handmade        a written recording: each vblank fires at exactly its
	                  bare pump, and one reached through a wait instead is
	                  counted at exit
	Headless: the dummy video driver.
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host/pump.h"		/* Port_Pump, Port_VBlankCount, Port_VBlankBarePump, PORT_SPIN_PUMPS */
#include "host/diag.h"		/* Port_InputAtExit */

extern "C" int	VSync(int mode);
extern "C" void	PadInitDirect(unsigned char *pad1, unsigned char *pad2);

static int			g_failures;
static const char	*g_mode = "parent";

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

static void setEnv(const char *name, const char *value)
{
	char buf[1024];
	std::snprintf(buf, sizeof(buf), "%s=%s", name, value);
	_putenv(buf);
}

/*	seven vblanks of wall time owed: the backlog rebase starts at eight  */
static void oweSevenVblanks(void)
{
	Sleep(7 * 1000 / 60 + 3);
}

/*	The spin rule, where bare pumps fire nothing: PORT_SPIN_PUMPS bare pumps
	in a row fire nothing, every further one is a one-vblank wait, and a
	real wait ends it.  */
static void spinChecks(void)
{
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
}

/*	A hand-written recording (the parent writes it): vblank 2 at bare pump
	5, vblank 3 at bare pump 9, vblank 5 at bare pump 3 after a wait, and
	vblank 7 at bare pump 2 - which this run reaches through a wait instead,
	so the exit check must count it.  */
static void handmadeChecks(void)
{
	char what[96];
	VSync(0);
	expectCount(1, "the first VSync(0)");
	for (int k = 1; k <= 20; k++)
	{
		Port_Pump();
		const unsigned long want = k < 5 ? 1 : k < 9 ? 2 : 3;
		std::snprintf(what, sizeof(what), "after bare pump %d", k);
		expectCount(want, what);
	}
	VSync(0);								/* its front pump is bare pump 1: not recorded */
	expectCount(4, "a VSync(0) fires vblank 4 in its wait");
	for (int k = 1; k <= 5; k++)
	{
		Port_Pump();
		std::snprintf(what, sizeof(what), "after bare pump %d past the wait", k);
		expectCount(k < 3 ? 4 : 5, what);
	}
	VSync(0);
	VSync(0);
	expectCount(7, "two waits: vblank 7 fired in a wait, not at bare pump 2");
	check(Port_InputAtExit() == 1, "the exit check counts the recorded vblank the run reached another way");
}

static int childMain(const char *mode)
{
	g_mode = mode;
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	static unsigned char pad0[34], pad1[34];
	PadInitDirect(pad0, pad1);				/* the recorder runs from the input frame */
	const bool uncapped = std::getenv("SBSP_UNCAPPED") && *std::getenv("SBSP_UNCAPPED");
	check(Port_Uncapped() == (uncapped ? 1 : 0), "the mode is the one asked for");
	if (std::strcmp(mode, "handmade") == 0)
	{
		handmadeChecks();
		SDL_Quit();
		return g_failures ? 1 : 0;
	}

	/*	1. a wait fires one vblank  */
	VSync(0);
	const unsigned long c0 = Port_VBlankCount();
	expectCount(1, "the first VSync(0)");

	/*	2. bare pumps, with wall time owed where there is a wall clock  */
	if (!uncapped)
		oweSevenVblanks();
	if (std::strcmp(mode, "record") == 0)
	{
		/*	live capped: each due vblank fires at the next bare pump, one per
			pump, and says where  */
		Port_Pump();
		expectCount(c0 + 1, "the first bare pump with time owed");
		check(Port_VBlankBarePump() == 1, "it fired at bare pump 1");
		Port_Pump();
		expectCount(c0 + 2, "the second bare pump");
		check(Port_VBlankBarePump() == 2, "it fired at bare pump 2");
		for (int i = 0; i < 98; i++)
			Port_Pump();
		const unsigned long got = Port_VBlankCount();
		check(got >= c0 + 6 && got <= c0 + 8, "100 bare pumps caught up the six to eight owed vblanks");
		check(VSync(-1) == (int)got, "VSync(-1) reads the counter");
	}
	else if (std::strcmp(mode, "replay") == 0)
	{
		for (int i = 0; i < 100; i++)
			Port_Pump();					/* the recording decides; B/C == A proves it */
	}
	else
	{
		/*	uncapped, or scripted without `# bare`: nothing, owed or not  */
		for (int i = 0; i < 100; i++)
			Port_Pump();
		check(VSync(-1) == (int)c0, "VSync(-1) reads the counter without moving it");
		expectCount(c0, "100 bare pumps");
	}

	/*	3. each wait fires exactly one  */
	const unsigned long c2 = Port_VBlankCount();
	VSync(0);
	expectCount(c2 + 1, "one VSync(0) after the bare pumps");
	for (int i = 0; i < 5; i++)
		VSync(0);
	expectCount(c2 + 6, "five more VSync(0)");

	/*	4. the spin rule, where bare pumps fire nothing else  */
	if (std::strcmp(mode, "uncapped") == 0 || std::strcmp(mode, "script") == 0)
		spinChecks();

	if (std::strcmp(mode, "replay") == 0)
		check(Port_InputAtExit() == 0, "every recorded `# bare` vblank fired where it was recorded");

	SDL_Quit();
	return g_failures ? 1 : 0;
}

/*	-------- the parent  */
static int spawnSelf(const char *exe, const char *mode)
{
	char quoted[MAX_PATH + 4];
	std::snprintf(quoted, sizeof(quoted), "\"%s\"", exe);
	intptr_t rc = _spawnl(_P_WAIT, exe, quoted, mode, (const char *)NULL);
	if (rc == -1)
		std::printf("FAIL: _spawnl(%s) failed\n", mode);
	return (int)rc;
}

/*	a recording's lines, `# pace` dropped (the replays differ from A there
	by design), CRLF or LF  */
static std::string linesOf(const char *path, int *bares)
{
	FILE *f = std::fopen(path, "r");
	std::string out;
	*bares = 0;
	if (!f)
		return out;
	char line[512];
	while (std::fgets(line, sizeof(line), f))
	{
		if (std::strncmp(line, "# pace ", 7) == 0)
			continue;
		if (std::strncmp(line, "# bare ", 7) == 0)
			(*bares)++;
		out += line;
	}
	std::fclose(f);
	return out;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		return childMain(argv[1]);

	char exe[MAX_PATH], tmp[MAX_PATH], a[MAX_PATH + 48], b[MAX_PATH + 48], c[MAX_PATH + 48];
	if (!GetModuleFileNameA(NULL, exe, sizeof(exe)) || !GetTempPathA(sizeof(tmp), tmp))
	{
		std::printf("FAIL: GetModuleFileNameA / GetTempPathA\n");
		return 1;
	}
	const unsigned long pid = GetCurrentProcessId();
	std::snprintf(a, sizeof(a), "%ssbsp_pump_test_%lu_A.pad", tmp, pid);
	std::snprintf(b, sizeof(b), "%ssbsp_pump_test_%lu_B.pad", tmp, pid);
	std::snprintf(c, sizeof(c), "%ssbsp_pump_test_%lu_C.pad", tmp, pid);

	static const char *const clear[] =
	{
		"SBSP_FRAME_CRC", "SBSP_EXIT_AFTER", "SBSP_SELFTEST", "SBSP_DUMP_FRAMES",
		"SBSP_DUMP_AUDIO", "SBSP_PACE_LOG", "SBSP_PAD_FILE", "SBSP_PAD_SCRIPT",
		"SBSP_RECORD_PAD", "SBSP_UNCAPPED", "SBSP_SEED", "SBSP_PROMPT_ICONS",
	};
	for (const char *v : clear)
		setEnv(v, "");

	setEnv("SBSP_RECORD_PAD", a);
	check(spawnSelf(exe, "record") == 0, "record: live capped, bare pumps fire due vblanks");
	setEnv("SBSP_RECORD_PAD", "");

	setEnv("SBSP_UNCAPPED", "1");
	check(spawnSelf(exe, "uncapped") == 0, "uncapped: bare pumps fire nothing; the spin rule");
	setEnv("SBSP_UNCAPPED", "");

	setEnv("SBSP_PAD_SCRIPT", "0:0000");
	check(spawnSelf(exe, "script") == 0, "script: a capped scripted run fires nothing at bare pumps");
	setEnv("SBSP_PAD_SCRIPT", "");

	setEnv("SBSP_PAD_FILE", a);
	setEnv("SBSP_RECORD_PAD", b);
	check(spawnSelf(exe, "replay") == 0, "replay capped");
	setEnv("SBSP_RECORD_PAD", c);
	setEnv("SBSP_UNCAPPED", "1");
	check(spawnSelf(exe, "replay") == 0, "replay uncapped");

	int na = 0, nb = 0, nc = 0;
	const std::string la = linesOf(a, &na), lb = linesOf(b, &nb), lc = linesOf(c, &nc);
	check(na >= 6, "A has a `# bare` line for each vblank the bare pumps caught up");
	check(!la.empty() && la == lb, "the capped replay fired every vblank where A did (B == A)");
	check(!la.empty() && la == lc, "the uncapped replay fired every vblank where A did (C == A)");
	if (la != lb || la != lc)
		std::printf("--- A\n%s--- B\n%s--- C\n%s---\n", la.c_str(), lb.c_str(), lc.c_str());
	std::remove(a);
	std::remove(b);
	std::remove(c);

	/*	exact bare-pump counts, and a recorded vblank reached another way  */
	char h[MAX_PATH + 48];
	std::snprintf(h, sizeof(h), "%ssbsp_pump_test_%lu_H.pad", tmp, pid);
	if (FILE *f = std::fopen(h, "w"))
	{
		std::fputs("# bare 2 5\n# bare 3 9\n# bare 5 3\n# bare 7 2\n", f);
		std::fclose(f);
	}
	setEnv("SBSP_RECORD_PAD", "");
	setEnv("SBSP_PAD_FILE", h);				/* SBSP_UNCAPPED=1 still set */
	check(spawnSelf(exe, "handmade") == 0, "handmade: each recorded vblank fires at exactly its bare pump");
	std::remove(h);

	if (g_failures)
	{
		std::printf("pump_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("pump_test: all passed\n");
	return 0;
}
