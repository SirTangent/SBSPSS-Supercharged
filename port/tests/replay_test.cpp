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

	A parent and six children, because the pad file is parsed once per
	process and the recorder never closes its file: the parent spawns itself
	as "record" (a virtual pad drives buttons and stick, --seed 4242, A is
	written), checks A line by line, spawns "replay" (A played back, B
	written) and compares B with A, then "wrongseed" (A with the seed + 1:
	must desync), "record-unseeded" (no seed: C must carry no `# seed`) and
	"replay" again on A with its rng fields stripped (an older recording:
	must still replay clean), and "expect-desync" on that copy with an
	epoch's crc doctored (its 3-field epochs must still be checked).  The
	children step vblanks with VSync(0) under SBSP_UNCAPPED=1,
	so Port_VBlankCount advances and Host_VBlank calls Port_InputFrame exactly
	as in the game, and open scenes through Port_SceneEvent at chosen counts.
	Headless: the dummy video driver; the test pumps SDL events itself, as
	Host_VBlank only does that with a window.
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "host/pump.h"		/* Port_VBlankCount */
#include "host/diag.h"		/* Port_SceneEvent, Port_InputAtExit */

extern unsigned char *Port_PadBuffer[2];		/* pads_shim.cpp */
extern "C" void	Port_InputHandleEvent(const void *ev);
extern "C" int	Port_InputPadActive(void);
extern "C" int	Port_PadFileSeed(long *seed);
extern "C" int	Port_BootSeed(long *seed);			/* host/seed.cpp */
extern "C" void	Port_SeedExplicit(long seed);
extern "C" void	PadInitDirect(unsigned char *pad1, unsigned char *pad2);
extern "C" int	VSync(int mode);

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

static void drive(SDL_Joystick *joy, bool replay)
{
	static unsigned char pad0[34], pad1[34];
	PadInitDirect(pad0, pad1);

	char what[128];
	for (unsigned long vb = 1; vb <= RUN_VBLANKS; vb++)
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
		g_fakeRng = (long)(0x015a4e35u * (unsigned long)g_fakeRng + 1u);	/* one draw a frame */
	}
}

/*	-------- the children  */
enum Mode { RECORD, RECORD_UNSEEDED, REPLAY, WRONG_SEED, EXPECT_DESYNC };

static int childMain(Mode mode)
{
	bool replay = mode == REPLAY || mode == WRONG_SEED || mode == EXPECT_DESYNC;
	if (!sdlUp())
		return 1;
	setEnv("SBSP_UNCAPPED", "1");			/* before the first VSync: Port_Uncapped caches */
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
	Port_RegisterGameGlobals(&g_fakeRam, NULL, NULL, NULL, NULL, NULL, NULL, &g_fakeRng);
	drive(joy, replay);
	int bad = Port_InputAtExit();
	if (mode == REPLAY)
		check(bad == 0, "replay: no desync, every entry satisfied");
	if (mode == WRONG_SEED)
		check(bad > 0, "wrong seed: the epoch's rng catches it though picture and RamUsed match");
	if (mode == EXPECT_DESYNC)
		check(bad > 0, "a doctored epoch is reported");
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

/*	A, line by line.  Text mode strips the CRLF the recorder's text-mode
	fopen produced.  */
static void checkRecording(const char *path)
{
	struct Want { const char *text; bool prefix; };
	char abi[32];
	std::snprintf(abi, sizeof(abi), "# abi ptr=%d", (int)sizeof(void *));
	const Want want[] =
	{
		{ "# recorded by sbsp --record-pad", true  },
		{ abi,                              false },
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

static int spawnSelf(const char *exe, const char *mode)
{
	char quoted[MAX_PATH + 4];
	std::snprintf(quoted, sizeof(quoted), "\"%s\"", exe);
	intptr_t rc = _spawnl(_P_WAIT, exe, quoted, mode, (const char *)NULL);
	if (rc == -1)
		std::printf("FAIL: _spawnl(%s) failed\n", mode);
	return (int)rc;
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
	size_t na = 0, nb = 0;
	char *da = slurp(a, &na);
	char *db = slurp(b, &nb);
	check(da != NULL && db != NULL, "both recordings exist");
	if (da && db)
	{
		bool same = na == nb && std::memcmp(da, db, na) == 0;
		check(same, "the replay's recording is byte-identical to the original");
		if (!same)
			std::printf("--- A (%u bytes)\n%s--- B (%u bytes)\n%s---\n",
						(unsigned)na, da, (unsigned)nb, db);
	}
	std::free(da);
	std::free(db);

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
	size_t nc = 0;
	char *dc = slurp(c, &nc);
	check(dc != NULL, "unseeded recording exists");
	if (dc)
	{
		check(std::strstr(dc, "# seed") == NULL, "an unseeded recording carries no `# seed`");
		check(std::strstr(dc, "# epoch 300 ") && std::strstr(dc, " rng="), "its epochs still carry rng");
	}
	std::free(dc);

	/*	6. an older recording - A without its rng fields - still replays
		clean, and its 3-field epochs are really parsed and checked: the same
		copy with the epoch's crc doctored must be reported  */
	char bent[MAX_PATH + 48];
	std::snprintf(bent, sizeof(bent), "%ssbsp_replay_test_%lu_bent.pad", tmp, pid);
	da = slurp(a, &na);
	if (da)
	{
		std::string s(da, na), cut;
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
		check(cut.find(" rng=") == std::string::npos && cut.find("# epoch 300 ") != std::string::npos,
			  "the old-format copy has epochs without rng");
		std::string doctored = cut;
		size_t crc = doctored.find(" crc=", doctored.find("# epoch 300 "));
		if (crc != std::string::npos)
			doctored.replace(crc + 5, 8, doctored.compare(crc + 5, 8, "DEADBEEF") ? "DEADBEEF" : "FEEDFACE");
		for (const auto &out : { std::make_pair(old, &cut), std::make_pair(bent, &doctored) })
		{
			FILE *f = std::fopen(out.first, "wb");
			if (f)
			{
				std::fwrite(out.second->data(), 1, out.second->size(), f);
				std::fclose(f);
			}
		}
	}
	std::free(da);
	setEnv("SBSP_RECORD_PAD", "");
	setEnv("SBSP_PAD_FILE", old);
	rc = spawnSelf(exe, "replay");
	check(rc == 0, "an older recording (no rng fields) replays clean");
	setEnv("SBSP_PAD_FILE", bent);
	rc = spawnSelf(exe, "expect-desync");
	check(rc == 0, "an older recording's 3-field epoch is checked (a doctored crc is caught)");

	std::remove(a);
	std::remove(b);
	std::remove(c);
	std::remove(old);
	std::remove(bent);

	if (g_failures)
	{
		std::printf("replay_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("replay_test: all passed\n");
	return 0;
}
