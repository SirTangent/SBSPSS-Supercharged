/*	Pad recording -> replay round trip (issue #58), on the shim alone: the
	things a tester's session.pad has to carry for a replay to reproduce it,
	proven by recording a replay of the recording and requiring the two files
	to be byte-identical.

	  - the left stick, folded into the mask by the game's own rule
	    (host/input.cpp stickFold) - a pad session used to have no movement
	    in it at all
	  - a button still held when a scene opens, pressed again in the new
	    scene's terms (a scene open releases every entry on replay)
	  - the header: `# seed` when the recording was given one (host/seed.cpp
	    - a replay without --seed then runs the same RNG; with none given the
	    game seeds itself identically every boot), `# pace`, `# loads`
	    (issue #67), and the `# prompt` device switches
	  - a scripted run ignores the live devices: the replaying process holds
	    SQUARE and the stick hard left the whole time, and none of it may
	    reach the packet, the prompts or the recording
	  - an epoch carries the game's RNG state: a replay forced onto the wrong
	    seed must report a desync even though its picture and RamUsed match

	A parent and nine children, because the pad file is parsed once per
	process and the recorder never closes its file: the parent spawns itself
	as "record" (a virtual pad drives buttons and stick, --seed 4242, A is
	written), checks A line by line, spawns "replay" (A played back, B
	written) and compares B with A, then "wrongseed" (A with the seed + 1:
	must desync), "record-unseeded" (no seed: C must carry no `# seed`) and
	"replay" again on A with its rng fields stripped (an older recording:
	must still replay clean), and "expect-desync" on that copy with an
	epoch's crc doctored (its 3-field epochs must still be checked), then
	three copies of A about the build type (issue #67): `# build` flipped
	and the epoch's ram doctored ("replay": ram is not compared across
	builds), the ram doctored alone ("expect-desync": it is compared on the
	same build) and an unknown `# build` word (refused at boot, exit 13), and
	a doctored crc under a reported pause menu, which only a replay on
	the other build type may overlook, and only while the menu is recent and
	the scene has not changed (six children), and last the renderer revision
	(issue #60): a recording without `# render`, or naming another revision,
	replays clean with its crc doctored but is still caught on a doctored ram
	or rng, a `# render` with blanks or a ` #` comment after this exe's
	number is read as it, and one that is not exactly one non-negative
	number is refused at boot (thirteen children), and last what the epochs
	compare: an old-format recording (no rng, no `# render`) replayed across
	ABIs compares nothing and is refused, one with rng says at boot that it
	compares rng only, the boot line names the set in four more cases, and
	a replay whose every epoch is left comparing nothing - its RNG never
	registered, or the pause menu up across build types - is refused at exit
	(eight children, their stderr captured), and a replay stopped before
	its first epoch: refused at the scripted exit, not when closed like a
	window (two children).  The children step vblanks with VSync(0) under SBSP_UNCAPPED=1,
	so Port_VBlankCount advances and Host_VBlank calls Port_InputFrame exactly
	as in the game, and open scenes through Port_SceneEvent at chosen counts.
	Headless: the dummy video driver; the test pumps SDL events itself, as
	Host_VBlank only does that with a window.
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host/pump.h"		/* Port_VBlankCount */
#include "gpu/gpu_core.h"	/* GPU_RENDER_REVISION - the `# render` line */
#include "host/diag.h"		/* Port_SceneEvent, Port_InputAtExit */

extern unsigned char *Port_PadBuffer[2];		/* pads_shim.cpp */
extern "C" void	Port_InputHandleEvent(const void *ev);
extern "C" int	Port_InputPadActive(void);
extern "C" int	Port_PadFileSeed(long *seed);
extern "C" int	Port_BootSeed(long *seed);			/* host/seed.cpp */
extern "C" void	Port_SeedExplicit(long seed);
extern "C" void	Port_PauseMenuDrawn(int drawn);	/* host/input.cpp, from game.cpp */
extern "C" void	PadInitDirect(unsigned char *pad1, unsigned char *pad2);
extern "C" int	VSync(int mode);
extern char		INF_Version[];				/* api/info.cpp: "DEBUG" or "FINAL", per tree */

/*	what --record-pad writes after `# build` for this build  */
static const char *thisBuild(void)
{
	return _stricmp(INF_Version, "FINAL") == 0 ? "final" : "debug";
}

static int g_failures;

/*	The game's RNG, stood in for: seeded from Port_BootSeed as main.cpp does
	and stepped once a frame with utils.h's getRndSeed formula, registered so
	every `# epoch` carries it.  RamUsed stays 0 and the display CRC is a
	constant here, so only this can tell two runs apart at an epoch - which
	is exactly the case the wrong-seed child tests.  */
static long				g_fakeRng;
static unsigned long	g_fakeRam;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*	forward every pending SDL event to the shim's handler, as Host_VBlank
	does when there is a window  */
static void pumpEvents(void)
{
	SDL_Event ev;
	SDL_PumpEvents();
	while (SDL_PollEvent(&ev))
		Port_InputHandleEvent(&ev);
}

/*	active-high 16-bit (Button1<<8)|Button2 word recovered from the
	active-low packet bytes  */
static unsigned packetMask(const unsigned char *buf)
{
	return ((unsigned)(unsigned char)~buf[2] << 8) | (unsigned char)~buf[3];
}

static void setEnv(const char *name, const char *value)
{
	char buf[1024];
	std::snprintf(buf, sizeof(buf), "%s=%s", name, value);
	_putenv(buf);
}

/*	Nothing ambient may shape the run: the parent clears these before it
	spawns, and the children inherit the result.  */
static void clearEnv(void)
{
	static const char *const vars[] =
	{
		"SBSP_FRAME_CRC", "SBSP_EXIT_AFTER", "SBSP_SELFTEST", "SBSP_DUMP_FRAMES",
		"SBSP_DUMP_DIR", "SBSP_SEED", "SBSP_PROMPT_ICONS", "SBSP_PAD_DEADZONE",
		"SBSP_MEM_LOG", "SBSP_PACE_LOG", "SBSP_RUMBLE", "SBSP_PAD_SCRIPT",
		"SBSP_PAD_FILE", "SBSP_RECORD_PAD", "SBSP_CD_PACE",
	};
	for (const char *v : vars)
		setEnv(v, "");
}

static bool sdlUp(void)
{
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
	{
		std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
		return false;
	}
	return true;
}

static SDL_Joystick *attachPad(void)
{
	SDL_VirtualJoystickDesc desc;
	SDL_INIT_INTERFACE(&desc);
	desc.type     = SDL_JOYSTICK_TYPE_GAMEPAD;
	desc.naxes    = SDL_GAMEPAD_AXIS_COUNT;
	desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	desc.name     = "SBSP virtual pad";
	SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
	check(id != 0, "SDL_AttachVirtualJoystick");
	pumpEvents();					/* GAMEPAD_ADDED: the shim opens it and the pad owns the prompts */
	SDL_Joystick *joy = SDL_GetJoystickFromID(id);
	check(joy != NULL, "shim opened the virtual gamepad on GAMEPAD_ADDED");
	return joy;
}

/*	The schedule both children share.  Scenes open AFTER the vblank named,
	i.e. between pumps, as the game's GameState::think does; the recorder
	first sees each one on the next frame, so the re-pressed stick lands as
	Game#1+1.  */
enum { RUN_VBLANKS = 320 };

/*	The pause menu (issue #67), as game.cpp reports it: once per frame the
	Game scene renders, here after each vblank's VSync.  REPLAY_TEST_PAUSE
	"<first>-<last>" draws it in the frames rendered after those vblanks;
	REPLAY_TEST_MAP_AT <vb> opens a "Map" scene after that vblank, which
	has no pause menu and so reports nothing from then on.  */
static unsigned long g_pauseFirst = 1, g_pauseLast = 0, g_mapAt = 0;

/*	REPLAY_TEST_VBLANKS <n> ends the run after n vblanks instead of
	RUN_VBLANKS, and REPLAY_TEST_CLOSE=1 ends it the way closing the window
	does (Port_InputAtExit(0)) rather than as --exit-after does  */
static unsigned long g_runVblanks = RUN_VBLANKS;
static int			 g_closeEarly;

static void readPauseEnv(void)
{
	const char *p = std::getenv("REPLAY_TEST_PAUSE");
	if (p && *p)
		std::sscanf(p, "%lu-%lu", &g_pauseFirst, &g_pauseLast);
	const char *m = std::getenv("REPLAY_TEST_MAP_AT");
	if (m && *m)
		g_mapAt = std::strtoul(m, NULL, 10);
	const char *v = std::getenv("REPLAY_TEST_VBLANKS");
	if (v && *v)
		g_runVblanks = std::strtoul(v, NULL, 10);
	const char *c = std::getenv("REPLAY_TEST_CLOSE");
	g_closeEarly = c && *c == '1';
}

static void drive(SDL_Joystick *joy, bool replay)
{
	static unsigned char pad0[34], pad1[34];
	PadInitDirect(pad0, pad1);

	char what[128];
	for (unsigned long vb = 1; vb <= g_runVblanks; vb++)
	{
		if (replay)
		{
			/* live input a scripted run has to ignore */
			SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_WEST, true);
			SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, -32768);
		}
		else
		{
			SDL_SetJoystickVirtualButton(joy, SDL_GAMEPAD_BUTTON_SOUTH, vb >= 20 && vb <= 25);
			SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, (vb >= 30 && vb <= 69) ? 32767 : 0);
		}
		pumpEvents();
		VSync(0);
		if (Port_VBlankCount() != vb)
		{
			std::snprintf(what, sizeof(what), "VSync(0) delivered exactly one vblank (count %lu at %lu)",
						  Port_VBlankCount(), vb);
			check(false, what);
			return;
		}

		unsigned m    = packetMask(pad0);
		unsigned want = (vb >= 20 && vb <= 25) ? 0x0040 : (vb >= 30 && vb <= 69) ? 0x2000 : 0;
		if (m != want)
		{
			std::snprintf(what, sizeof(what), "%s vblank %lu: mask %04X, want %04X",
						  replay ? "replay" : "record", vb, m, want);
			check(false, what);
		}
		if (!replay && vb == 30)
			check(pad0[6] == 255, "record: the stick byte passes through beside the fold");
		if (replay)
		{
			if (pad0[6] != 0x80 || pad0[7] != 0x80)
			{
				std::snprintf(what, sizeof(what), "replay vblank %lu: sticks centred despite the live stick", vb);
				check(false, what);
			}
			if (Port_InputPadActive() != 1)
			{
				std::snprintf(what, sizeof(what), "replay vblank %lu: `# prompt 1 pad` in force", vb);
				check(false, what);
			}
		}

		if (vb == 10)
			Port_SceneEvent("FrontEnd");
		if (vb == 50)
			Port_SceneEvent("Game");
		if (g_mapAt && vb == g_mapAt)
			Port_SceneEvent("Map");
		if (vb >= 50 && (!g_mapAt || vb < g_mapAt))
			Port_PauseMenuDrawn(vb >= g_pauseFirst && vb <= g_pauseLast);
		g_fakeRng = (long)(0x015a4e35u * (unsigned long)g_fakeRng + 1u);	/* one draw a frame */
	}
}

/*	-------- the children  */
enum Mode { RECORD, RECORD_UNSEEDED, REPLAY, WRONG_SEED, EXPECT_DESYNC, REPLAY_NO_RNG };

static int childMain(Mode mode)
{
	bool replay = mode == REPLAY || mode == WRONG_SEED || mode == EXPECT_DESYNC || mode == REPLAY_NO_RNG;
	if (!sdlUp())
		return 1;
	setEnv("SBSP_UNCAPPED", "1");			/* before the first VSync: Port_Uncapped caches */
	readPauseEnv();
	SDL_Joystick *joy = attachPad();
	if (!joy)
		return 1;
	long seed = 0;
	check(Port_PadFileSeed(&seed) == (replay ? 1 : 0),
		  replay ? "replay: the recording carries a seed" : "record: no recording, no seed to inherit");
	if (mode == RECORD)
		Port_SeedExplicit(4242);				/* what --seed 4242 does: `# seed 4242` */
	if (mode == WRONG_SEED)
		Port_SeedExplicit(seed + 1);			/* what --seed <other> does */
	/*	main.cpp: setRndSeed(Port_BootSeed(&seed) ? seed : VidGetTickCount()),
		and VidGetTickCount() is 0 at that point on every boot  */
	g_fakeRng = Port_BootSeed(&seed) ? seed : 0;
	/*	"replay-norng" registers no RNG, as the shim-only unit exes do: its
		epochs' rng has nothing live to be held against  */
	Port_RegisterGameGlobals(&g_fakeRam, NULL, NULL, NULL, NULL, NULL, NULL,
							 mode == REPLAY_NO_RNG ? NULL : &g_fakeRng);
	drive(joy, replay);
	int bad = Port_InputAtExit(g_closeEarly ? 0 : 1);
	if (mode == REPLAY)
		check(bad == 0, "replay: no desync, every entry satisfied");
	if (mode == WRONG_SEED)
		check(bad > 0, "wrong seed: the epoch's rng catches it though picture and RamUsed match");
	if (mode == EXPECT_DESYNC)
		check(bad > 0, "a doctored epoch is reported");
	if (mode == REPLAY_NO_RNG)
		check(bad > 0, "epochs that compared nothing refuse the replay at exit");
	SDL_Quit();
	return g_failures ? 1 : 0;
}

/*	-------- the parent  */
static char *slurp(const char *path, size_t *n)
{
	FILE *f = std::fopen(path, "rb");
	if (!f)
		return NULL;
	std::fseek(f, 0, SEEK_END);
	long len = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	char *d = (char *)std::malloc((size_t)len + 1);
	*n = std::fread(d, 1, (size_t)len, f);
	d[*n] = 0;
	std::fclose(f);
	return d;
}

/*	-------- recording edits the cases below share  */

/*	the whole file, "" when it cannot be read  */
static std::string readText(const char *path)
{
	size_t n = 0;
	char *d = slurp(path, &n);
	std::string s = d ? std::string(d, n) : std::string();
	std::free(d);
	return s;
}

static void writeText(const char *path, const std::string &s)
{
	FILE *f = std::fopen(path, "wb");
	if (!f)
	{
		std::printf("FAIL: cannot write %s\n", path);
		g_failures++;
		return;
	}
	std::fwrite(s.data(), 1, s.size(), f);
	std::fclose(f);
}

/*	s with every epoch's ` rng=<hex>` removed: what a recording made before
	issue #58 looks like  */
static std::string stripRng(const std::string &s)
{
	std::string cut;
	for (size_t i = 0; i < s.size();)
	{
		if (s.compare(i, 5, " rng=") == 0)
		{
			i += 5;
			while (i < s.size() && std::isxdigit((unsigned char)s[i]))
				i++;
			continue;
		}
		cut += s[i++];
	}
	return cut;
}

/*	this exe's `# render N` line in s replaced by `line`, or removed with
	its newline when `line` is NULL (a recording from before issue #60);
	false when s has no such line  */
static bool setRenderLine(std::string &s, const char *line)
{
	char mine[32];
	std::snprintf(mine, sizeof(mine), "# render %d", GPU_RENDER_REVISION);
	const size_t rl = s.find(mine);
	if (rl == std::string::npos)
		return false;
	if (line)
		s.replace(rl, std::strlen(mine), line);
	else
	{
		const size_t eol = s.find('\n', rl);
		s.erase(rl, (eol == std::string::npos ? s.size() : eol + 1) - rl);
	}
	return true;
}

/*	the `# build` line naming this build, and the one naming the other  */
static std::string buildLine(bool other)
{
	const bool isFinal = std::strcmp(thisBuild(), "final") == 0;
	return std::string("# build ") + ((isFinal != other) ? "final" : "debug");
}

/*	A, line by line.  Text mode strips the CRLF the recorder's text-mode
	fopen produced.  */
static void checkRecording(const char *path)
{
	struct Want { const char *text; bool prefix; };
	char abi[32], render[32];
	std::snprintf(abi, sizeof(abi), "# abi ptr=%d", (int)sizeof(void *));
	std::snprintf(render, sizeof(render), "# render %d", GPU_RENDER_REVISION);
	const std::string build = buildLine(false);
	const Want want[] =
	{
		{ "# recorded by sbsp --record-pad", true  },
		{ abi,                              false },
		{ build.c_str(),                    false },	/* issue #67 */
		{ render,                           false },	/* issue #60 */
		{ "# seed 4242",                    false },	/* the recording was given one */
		{ "# pace uncapped",                false },
		{ "# loads paced",                  false },	/* uncapped no longer means instant loads (#67) */
		{ "# prompt 1 pad",                 false },	/* a pad plugged in before the first frame */
		{ "# scene FrontEnd#1 vblank=10",   false },
		{ "FrontEnd#1+10:0040",             false },	/* CROSS from vblank 20 */
		{ "FrontEnd#1+16:0000",             false },	/* released at 26 */
		{ "FrontEnd#1+20:2000",             false },	/* the stick, as RIGHT, from 30 */
		{ "# scene Game#1 vblank=50",       false },
		{ "Game#1+1:2000",                  false },	/* still held: pressed again in Game's terms */
		{ "Game#1+20:0000",                 false },	/* released at 70 */
		{ "# epoch 300 ram=0 crc=",         true  },	/* ... rng=<the fake RNG>, checked below */
	};
	const int nWant = (int)(sizeof(want) / sizeof(want[0]));

	FILE *f = std::fopen(path, "r");
	check(f != NULL, "recording A exists");
	if (!f)
		return;
	char line[512], what[640];
	int  i = 0;
	while (std::fgets(line, sizeof(line), f))
	{
		size_t n = std::strlen(line);
		while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (i >= nWant)
		{
			std::snprintf(what, sizeof(what), "recording A line %d unexpected: %s", i + 1, line);
			check(false, what);
			i++;
			continue;
		}
		bool ok = want[i].prefix ? std::strncmp(line, want[i].text, std::strlen(want[i].text)) == 0
								 : std::strcmp(line, want[i].text) == 0;
		if (ok && std::strncmp(line, "# epoch ", 8) == 0)
			ok = std::strstr(line, " rng=") != NULL;	/* the game registered its RNG */
		if (!ok)
		{
			std::snprintf(what, sizeof(what), "recording A line %d: got '%s', want '%s%s'",
						  i + 1, line, want[i].text, want[i].prefix ? "..." : "");
			check(false, what);
		}
		i++;
	}
	std::fclose(f);
	if (i < nWant)
	{
		std::snprintf(what, sizeof(what), "recording A ends after line %d; expected %d lines", i, nWant);
		check(false, what);
	}
}

/*	Run this exe as the child `mode` and return its exit code (-1 when it
	could not start).  With `log`, the child's stderr goes there, is echoed
	to ours and lands in *err with the child's text-mode CRLF made LF ("" when
	unreadable).  */
static int spawnSelf(const char *exe, const char *mode, const char *log = NULL,
					 std::string *err = NULL)
{
	HANDLE h = INVALID_HANDLE_VALUE;
	if (log)
	{
		SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
		h = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
						FILE_ATTRIBUTE_NORMAL, NULL);
		if (h == INVALID_HANDLE_VALUE)
		{
			std::printf("FAIL: cannot create %s\n", log);
			return -1;
		}
	}
	char cmd[MAX_PATH * 2];
	std::snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, mode);
	STARTUPINFOA si = {};
	si.cb         = sizeof(si);
	si.dwFlags    = STARTF_USESTDHANDLES;
	si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError  = log ? h : GetStdHandle(STD_ERROR_HANDLE);
	PROCESS_INFORMATION pi = {};
	std::fflush(stdout);
	std::fflush(stderr);
	int rc = -1;
	if (CreateProcessA(exe, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
	{
		WaitForSingleObject(pi.hProcess, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(pi.hProcess, &code);
		rc = (int)code;
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}
	else
		std::printf("FAIL: CreateProcess(%s) failed\n", mode);
	if (!log)
		return rc;
	CloseHandle(h);
	size_t n = 0;
	char *text = slurp(log, &n);
	if (err)
		err->clear();
	if (text)
	{
		std::fputs(text, stderr);
		if (err)
			for (const char *r = text; *r; r++)
				if (*r != '\r')
					*err += *r;
		std::free(text);
	}
	return rc;
}

int main(int argc, char **argv)
{
	if (argc > 1 && std::strcmp(argv[1], "record") == 0)
		return childMain(RECORD);
	if (argc > 1 && std::strcmp(argv[1], "replay") == 0)
		return childMain(REPLAY);
	if (argc > 1 && std::strcmp(argv[1], "wrongseed") == 0)
		return childMain(WRONG_SEED);
	if (argc > 1 && std::strcmp(argv[1], "record-unseeded") == 0)
		return childMain(RECORD_UNSEEDED);
	if (argc > 1 && std::strcmp(argv[1], "expect-desync") == 0)
		return childMain(EXPECT_DESYNC);
	if (argc > 1 && std::strcmp(argv[1], "replay-norng") == 0)
		return childMain(REPLAY_NO_RNG);

	char exe[MAX_PATH], tmp[MAX_PATH], a[MAX_PATH + 48], b[MAX_PATH + 48];
	if (!GetModuleFileNameA(NULL, exe, sizeof(exe)) || !GetTempPathA(sizeof(tmp), tmp))
	{
		std::printf("FAIL: GetModuleFileNameA / GetTempPathA\n");
		return 1;
	}
	unsigned long pid = GetCurrentProcessId();
	std::snprintf(a, sizeof(a), "%ssbsp_replay_test_%lu_A.pad", tmp, pid);
	std::snprintf(b, sizeof(b), "%ssbsp_replay_test_%lu_B.pad", tmp, pid);
	std::remove(a);
	std::remove(b);

	clearEnv();

	/*	0. no --seed and no recording: the hook leaves the seed to the game
		(which seeds the same way every boot), and --record-pad writes no
		`# seed` for such a run  */
	long untouched = 99;
	check(Port_BootSeed(&untouched) == 0 && untouched == 99,
		  "no seed given, none recorded: Port_BootSeed answers 0 and leaves the value alone");

	/*	1. record A: the virtual pad plays  */
	setEnv("SBSP_RECORD_PAD", a);
	int rc = spawnSelf(exe, "record");
	check(rc == 0, "record child exited 0");
	checkRecording(a);

	/*	2. replay A, recording B  */
	setEnv("SBSP_PAD_FILE", a);
	setEnv("SBSP_RECORD_PAD", b);
	rc = spawnSelf(exe, "replay");
	check(rc == 0, "replay child exited 0 (no desync, no leaked input)");

	/*	3. B == A, byte for byte: same seed, same prompt device, same
		entries, same epochs  */
	const std::string recA = readText(a);		/* every edited copy below starts from it */
	{
		const std::string recB = readText(b);
		check(!recA.empty() && !recB.empty(), "both recordings exist");
		if (!recA.empty() && !recB.empty() && recA != recB)
		{
			check(false, "the replay's recording is byte-identical to the original");
			std::printf("--- A (%u bytes)\n%s--- B (%u bytes)\n%s---\n",
						(unsigned)recA.size(), recA.c_str(), (unsigned)recB.size(), recB.c_str());
		}
	}

	/*	4. replay A with the wrong seed and record nothing: same input, same
		picture, same RamUsed - only the RNG differs, and the epoch must still
		report it (a real wrong-seed replay matched every CRC-and-RAM epoch on
		three levels in five, issue #58)  */
	setEnv("SBSP_RECORD_PAD", "");
	rc = spawnSelf(exe, "wrongseed");
	check(rc == 0, "wrong-seed child exited 0 (it saw the desync it expected)");

	/*	5. record C with no seed at all: the game seeds itself the same way
		every boot, so there is no `# seed` to write - but the epochs still
		carry the RNG  */
	char c[MAX_PATH + 48], old[MAX_PATH + 48];
	std::snprintf(c, sizeof(c), "%ssbsp_replay_test_%lu_C.pad", tmp, pid);
	std::snprintf(old, sizeof(old), "%ssbsp_replay_test_%lu_old.pad", tmp, pid);
	setEnv("SBSP_PAD_FILE", "");
	setEnv("SBSP_RECORD_PAD", c);
	rc = spawnSelf(exe, "record-unseeded");
	check(rc == 0, "unseeded record child exited 0");
	{
		const std::string recC = readText(c);
		check(!recC.empty(), "unseeded recording exists");
		if (!recC.empty())
		{
			check(recC.find("# seed") == std::string::npos, "an unseeded recording carries no `# seed`");
			check(recC.find("# epoch 300 ") != std::string::npos && recC.find(" rng=") != std::string::npos,
				  "its epochs still carry rng");
		}
	}

	/*	6. an older recording - A without its rng fields - still replays
		clean, and its 3-field epochs are really parsed and checked: the same
		copy with the epoch's crc doctored must be reported  */
	char bent[MAX_PATH + 48];
	std::snprintf(bent, sizeof(bent), "%ssbsp_replay_test_%lu_bent.pad", tmp, pid);
	{
		const std::string cut = stripRng(recA);
		check(cut.find(" rng=") == std::string::npos && cut.find("# epoch 300 ") != std::string::npos,
			  "the old-format copy has epochs without rng");
		std::string doctored = cut;
		size_t crc = doctored.find(" crc=", doctored.find("# epoch 300 "));
		if (crc != std::string::npos)
			doctored.replace(crc + 5, 8, doctored.compare(crc + 5, 8, "DEADBEEF") ? "DEADBEEF" : "FEEDFACE");
		writeText(old, cut);
		writeText(bent, doctored);
	}
	setEnv("SBSP_RECORD_PAD", "");
	setEnv("SBSP_PAD_FILE", old);
	rc = spawnSelf(exe, "replay");
	check(rc == 0, "an older recording (no rng fields) replays clean");
	setEnv("SBSP_PAD_FILE", bent);
	rc = spawnSelf(exe, "expect-desync");
	check(rc == 0, "an older recording's 3-field epoch is checked (a doctored crc is caught)");

	/*	7. the build type (issue #67): a DEBUG heap block carries guard words,
		so RamUsed differs between DEBUG and FINAL and a recording made on the
		other build compares the CRC and rng alone - a doctored ram then
		replays clean - while on this build the same doctored ram is caught.
		An unknown `# build` word is refused at boot, like a bad `# abi`.  */
	char xbuild[MAX_PATH + 48], ramd[MAX_PATH + 48], badb[MAX_PATH + 48];
	std::snprintf(xbuild, sizeof(xbuild), "%ssbsp_replay_test_%lu_xbuild.pad", tmp, pid);
	std::snprintf(ramd, sizeof(ramd), "%ssbsp_replay_test_%lu_ram.pad", tmp, pid);
	std::snprintf(badb, sizeof(badb), "%ssbsp_replay_test_%lu_badbuild.pad", tmp, pid);
	{
		const std::string s = recA;
		const std::string mine = buildLine(false), other = buildLine(true);
		const size_t bl = s.find(mine);
		const size_t rl = s.find(" ram=0 ", s.find("# epoch 300 "));
		check(bl != std::string::npos && rl != std::string::npos,
			  "A carries this build's `# build` line and an epoch at 300 with ram=0");
		if (bl != std::string::npos && rl != std::string::npos)
		{
			std::string ramOnly = s;					/* same length edits: offsets stay valid */
			ramOnly.replace(rl, 7, " ram=1 ");
			std::string cross = ramOnly;
			cross.replace(bl, mine.size(), other);
			std::string bad = s;
			bad.replace(bl, mine.size(), "# build release");
			writeText(xbuild, cross);
			writeText(ramd, ramOnly);
			writeText(badb, bad);
		}
	}
	setEnv("SBSP_PAD_FILE", xbuild);
	rc = spawnSelf(exe, "replay");
	check(rc == 0, "a recording from the other build type: ram is not compared (a doctored ram replays clean)");
	setEnv("SBSP_PAD_FILE", ramd);
	rc = spawnSelf(exe, "expect-desync");
	check(rc == 0, "a recording from this build type: a doctored ram is caught");
	setEnv("SBSP_PAD_FILE", badb);
	rc = spawnSelf(exe, "replay");
	check(rc == 13, "an unknown `# build` word is refused at boot (exit 13)");

	/*	8. the pause menu (issue #67): DEBUG draws one more line in it, so
		across build types an epoch's crc is not compared while the menu is
		in any of the last three frames rendered - and only then.  A copy
		with the epoch at 300's crc doctored stands in for that picture.  */
	char xcrc[MAX_PATH + 48], scrc[MAX_PATH + 48];
	std::snprintf(xcrc, sizeof(xcrc), "%ssbsp_replay_test_%lu_xcrc.pad", tmp, pid);
	std::snprintf(scrc, sizeof(scrc), "%ssbsp_replay_test_%lu_crc.pad", tmp, pid);
	{
		const std::string s = recA;
		const std::string mine = buildLine(false), other = buildLine(true);
		const size_t bl = s.find(mine);
		const size_t cl = s.find(" crc=", s.find("# epoch 300 "));
		check(bl != std::string::npos && cl != std::string::npos,
			  "A carries this build's `# build` line and an epoch at 300 with a crc");
		if (bl != std::string::npos && cl != std::string::npos)
		{
			std::string same = s;						/* same length edits: offsets stay valid */
			same.replace(cl + 5, 8, s.compare(cl + 5, 8, "DEADBEEF") ? "DEADBEEF" : "FEEDFACE");
			std::string cross = same;
			cross.replace(bl, mine.size(), other);
			writeText(xcrc, cross);
			writeText(scrc, same);
		}
	}
	struct PauseCase { const char *file, *pause, *mapAt, *mode, *what; };
	const PauseCase pauseCases[] =
	{
		{ xcrc, "290-299", "",    "replay",
		  "other build, the pause menu up at the epoch: crc is not compared" },
		{ xcrc, "290-297", "",    "replay",
		  "other build, the menu last drawn two frames before the latest: still not compared" },
		{ xcrc, "290-296", "",    "expect-desync",
		  "other build, the menu gone for three frames: crc is compared again" },
		{ xcrc, "",        "",    "expect-desync",
		  "other build, no pause menu: crc is compared" },
		{ xcrc, "290-298", "299", "expect-desync",
		  "other build, a scene opened since the menu was drawn: crc is compared" },
		{ scrc, "290-299", "",    "expect-desync",
		  "same build, the pause menu up: crc is compared" },
	};
	for (const PauseCase &pc : pauseCases)
	{
		setEnv("SBSP_PAD_FILE", pc.file);
		setEnv("REPLAY_TEST_PAUSE", pc.pause);
		setEnv("REPLAY_TEST_MAP_AT", pc.mapAt);
		rc = spawnSelf(exe, pc.mode);
		check(rc == 0, pc.what);
	}
	setEnv("REPLAY_TEST_PAUSE", "");
	setEnv("REPLAY_TEST_MAP_AT", "");

	/*	9. the renderer revision (issue #60): a fidelity fix redraws the same
		game state differently, so a recording made before it - no `# render`
		line - or by another revision does not compare crc; ram and rng still
		are.  A doctored crc stands in for the changed picture.  */
	{
		char other[32], spaced[32], noted[64];
		std::snprintf(other, sizeof(other), "# render %d", GPU_RENDER_REVISION + 1);
		std::snprintf(spaced, sizeof(spaced), "# render  %d  ", GPU_RENDER_REVISION);
		std::snprintf(noted, sizeof(noted), "# render %d   # made on another exe", GPU_RENDER_REVISION);
		struct RenderCase { const char *tag, *renderLine; char field; const char *mode, *what; };
		const RenderCase renderCases[] =
		{
			{ "rnone",  NULL,          'c', "replay",
			  "no `# render` line (an older recording): crc is not compared" },
			{ "rother", other,         'c', "replay",
			  "another renderer revision: crc is not compared" },
			{ "rram",   NULL,          'r', "expect-desync",
			  "no `# render` line: a doctored ram is still caught" },
			{ "rrng",   NULL,          'n', "expect-desync",
			  "no `# render` line: a doctored rng is still caught" },
			{ "rspace", spaced,        'c', "expect-desync",
			  "`# render` with blanks around this exe's revision is read as it: crc is compared" },
			{ "rnote",  noted,         'c', "expect-desync",
			  "`# render` with a ` #` comment after this exe's revision is read as it: crc is compared" },
			/*	`# render` takes exactly one non-negative number: anything else
				used to fall through as a comment and leave revision 0  */
			{ "rbad",   "# render -1",    0, "replay", NULL },
			{ "rbare",  "# render",       0, "replay", NULL },
			{ "rword",  "# render one",   0, "replay", NULL },
			{ "rdash",  "# render -",     0, "replay", NULL },
			{ "rtwo",   "# render 1 2",   0, "replay", NULL },
			{ "rtail",  "# render 1x",    0, "replay", NULL },
			{ "rglue",  "# render 1#x",   0, "replay", NULL },	/* a comment needs the blank */
		};
		for (const RenderCase &rc9 : renderCases)
		{
			char path[MAX_PATH + 48];
			std::snprintf(path, sizeof(path), "%ssbsp_replay_test_%lu_%s.pad", tmp, pid, rc9.tag);
			std::string s = recA;
			const size_t ep = s.find("# epoch 300 ");
			check(ep != std::string::npos, "A carries an epoch at 300");
			if (ep == std::string::npos)
				continue;
			if (rc9.field)
			{
				const char *key = rc9.field == 'c' ? " crc=" : rc9.field == 'r' ? " ram=" : " rng=";
				size_t at = s.find(key, ep);
				check(at != std::string::npos, "A's epoch at 300 carries the field to doctor");
				if (at == std::string::npos)
					continue;
				at += 5;
				if (rc9.field == 'r')
					s.replace(at, 1, "1");					/* ram=0 -> ram=1 */
				else
					s.replace(at, 8, s.compare(at, 8, "DEADBEEF") ? "DEADBEEF" : "FEEDFACE");
			}
			check(setRenderLine(s, rc9.renderLine), "A carries this exe's `# render` line");
			writeText(path, s);
			setEnv("SBSP_PAD_FILE", path);
			rc = spawnSelf(exe, rc9.mode);
			if (rc9.what)
				check(rc == 0, rc9.what);
			else
			{
				char what[96];
				std::snprintf(what, sizeof(what), "`%s' is refused at boot (exit 13)", rc9.renderLine);
				check(rc == 13, what);
			}
			std::remove(path);
		}
	}

	/*	10. what the epochs compare (review of #75).  The skips stack: an
		old-format recording (no rng, no `# render`) replayed across ABIs
		would compare no ram, no crc and no rng, and exit 0 whatever the run
		did - it is refused at boot instead.  Flipping `# abi ptr=` stands in
		for the other exe: it is exactly the header the ram skip reads.  With
		rng the same replay passes, and the boot line names what is
		compared.  What only shows per epoch is counted at the end: an RNG
		the game never registered compares no rng, and across build types
		an epoch under the pause menu compares no crc - a replay made only
		of epochs left with nothing is refused at exit.  */
	{
		char abiMine[32], abiOther[32], renderOther[32];
		std::snprintf(abiMine, sizeof(abiMine), "# abi ptr=%d", (int)sizeof(void *));
		std::snprintf(abiOther, sizeof(abiOther), "# abi ptr=%d", sizeof(void *) == 8 ? 4 : 8);
		std::snprintf(renderOther, sizeof(renderOther), "# render %d", GPU_RENDER_REVISION + 1);
		struct CompareCase
		{
			const char *tag;
			bool		stripRng, dropRender, flipAbi, otherRender, flipBuild;
			const char	*pause, *mode;
			int			wantRc;
			const char	*wantText, *what;
		};
		const CompareCase cases[] =
		{
			{ "blind",   true,  true,  true,  false, false, "", "replay", 13,
			  "epochs compare nothing on this exe (render revision differs, ram skipped cross-ABI, "
			  "no rng recorded)",
			  "an old-format recording across ABIs compares nothing: refused" },
			{ "rngonly", false, false, true,  true,  false, "", "replay", 0,
			  "[input] epochs: comparing rng only\n",
			  "rng recorded, another revision, across ABIs: replays clean, comparing rng only" },
			{ "rngram",  false, true,  false, false, false, "", "replay", 0,
			  "[input] epochs: comparing rng, ram\n",
			  "no `# render`, same ABI: comparing rng, ram" },
			{ "ramonly", true,  true,  false, false, false, "", "replay", 0,
			  "[input] epochs: comparing ram only\n",
			  "old format on the same ABI: comparing ram only" },
			{ "all",     false, false, false, false, false, "", "replay", 0,
			  "[input] epochs: comparing rng, ram, crc\n",
			  "this exe's own recording: comparing rng, ram, crc" },
			{ "norng",   false, false, true,  true,  false, "", "replay-norng", 0,
			  "[replay] all 1 epochs reached compared nothing on this exe",
			  "rng recorded but no RNG registered, another revision, across ABIs: refused at exit" },
			{ "crconly", true,  false, false, false, true,  "", "replay", 0,
			  "[input] epochs: comparing crc only; crc not while the pause menu is up\n",
			  "old format from the other build: the boot line says crc is not compared under the menu" },
			{ "paused",  true,  false, false, false, true,  "290-299", "expect-desync", 0,
			  "compared nothing: ram cross-build, crc cross-build pause menu, no rng recorded",
			  "old format from the other build, every epoch under the menu: refused at exit" },
		};
		for (const CompareCase &cc : cases)
		{
			char path[MAX_PATH + 48], log[MAX_PATH + 48];
			std::snprintf(path, sizeof(path), "%ssbsp_replay_test_%lu_%s.pad", tmp, pid, cc.tag);
			std::snprintf(log, sizeof(log), "%ssbsp_replay_test_%lu_%s.log", tmp, pid, cc.tag);
			std::string s = recA;
			if (cc.dropRender || cc.otherRender)
				check(setRenderLine(s, cc.dropRender ? NULL : renderOther),
					  "A carries this exe's `# render` line");
			if (cc.flipAbi)
			{
				const size_t al = s.find(abiMine);
				check(al != std::string::npos, "A carries this exe's `# abi` line");
				if (al != std::string::npos)
					s.replace(al, std::strlen(abiMine), abiOther);
			}
			if (cc.flipBuild)
			{
				const std::string mine = buildLine(false);
				const size_t bl = s.find(mine);
				check(bl != std::string::npos, "A carries this build's `# build` line");
				if (bl != std::string::npos)
					s.replace(bl, mine.size(), buildLine(true));
			}
			if (cc.stripRng)
				s = stripRng(s);
			writeText(path, s);
			setEnv("SBSP_PAD_FILE", path);
			setEnv("REPLAY_TEST_PAUSE", cc.pause);
			std::string err;
			const int rc10 = spawnSelf(exe, cc.mode, log, &err);
			char what[256];
			std::snprintf(what, sizeof(what), "%s (exit %d, want %d)", cc.what, rc10, cc.wantRc);
			check(rc10 == cc.wantRc && err.find(cc.wantText) != std::string::npos, what);
			std::remove(path);
			std::remove(log);
		}
		setEnv("REPLAY_TEST_PAUSE", "");
	}

	/*	11. a replay that stops before the recording's first epoch (review
		of #77): at --exit-after it compared nothing and is refused; closed
		like a window, what was not reached yet is no failure  */
	{
		struct EarlyCase { const char *close, *mode, *wantText, *what; };
		const EarlyCase cases[] =
		{
			{ "",  "expect-desync",
			  "[replay] none of the recording's 1 epochs was reached (the first is at vblank 300)",
			  "--exit-after before the first epoch: refused" },
			{ "1", "replay", "",
			  "closed before the first epoch: not a failure" },
		};
		char log[MAX_PATH + 48];
		std::snprintf(log, sizeof(log), "%ssbsp_replay_test_%lu_early.log", tmp, pid);
		setEnv("SBSP_PAD_FILE", a);
		setEnv("REPLAY_TEST_VBLANKS", "250");
		for (const EarlyCase &ec : cases)
		{
			setEnv("REPLAY_TEST_CLOSE", ec.close);
			std::string err;
			rc = spawnSelf(exe, ec.mode, log, &err);
			check(rc == 0 && err.find(ec.wantText) != std::string::npos, ec.what);
			std::remove(log);
		}
		setEnv("REPLAY_TEST_VBLANKS", "");
		setEnv("REPLAY_TEST_CLOSE", "");
	}

	std::remove(a);
	std::remove(b);
	std::remove(c);
	std::remove(old);
	std::remove(bent);
	std::remove(xbuild);
	std::remove(ramd);
	std::remove(badb);
	std::remove(xcrc);
	std::remove(scrc);

	if (g_failures)
	{
		std::printf("replay_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("replay_test: all passed\n");
	return 0;
}
