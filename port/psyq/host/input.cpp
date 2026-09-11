/*	SDL3 keyboard + gamepad -> PS1 controller packet (M3).

	Once per emulated vblank (Host_VBlank), Port_InputFrame rebuilds the
	34-byte pad buffer that PadInitDirect captured (pads_shim.cpp) in the
	PS1 wire format the game's ReadController parses:
	  [0]=0 connected, [1]=0x73 analog controller (type 7, 3 halfwords),
	  [2]/[3] active-low buttons, [4..7] RightX,RightY,LeftX,LeftY.

	Keyboard (RetroArch's default RetroPad layout - the de-facto standard,
	user-confirmed): arrows=D-pad, Z=Cross, X=Circle, A=Square, S=Triangle,
	Enter=Start, RShift=Select, Q=L1, W=R1, E=L2, R=R2.  Sticks centred.
	The first connected SDL gamepad ORs in on top (south/east/west/north =
	Cross/Circle/Square/Triangle, triggers = L2/R2, sticks pass through).
	Port 1 stays disconnected.

	Test tooling: SBSP_PAD_SCRIPT="vblank:HEXmask[,vblank:HEXmask...]"
	injects buttons for automated runs.  Each entry applies from its vblank
	until the next one comes due (by vblank, not by listing order); the
	mask is the active-HIGH 16-bit
	(Button1<<8)|Button2 hardware word (LIBETC.H order), e.g. START=0800,
	CROSS=0040, SELECT=0100, DOWN=4000.

	SBSP_PAD_FILE=<path> (M8) is the same idea from a file, one entry per
	line, unbounded, with `#` comments and a scene-relative form that
	survives load-time drift:
	    1500:0800                 absolute vblank
	    Map#2+30:2000             30 vblanks after the 2nd open of "Map"
	    FMA:INTRO#1+10:0800       (FMA scripts use the [scene] FMA:<name>)
	    # epoch 3000 ram=123456 crc=89ABCDEF
	A scene open releases every button: an entry is in force only if it
	came due at or after the most recent scene open (for a scene-relative
	entry that also means its anchor occurrence is the current scene).
	That is what a route author means by "during the 2nd Map", it lets a
	boot-time button pulse train anchored to FrontEnd#1 stop by itself when
	the first level opens, and it keeps an absolute press recorded before
	the first scene from outliving its scene-relative release.  Within the
	entries in force, the latest one that has come due wins.

	The `# epoch` markers come from SBSP_RECORD_PAD=<path>, which writes
	the applied mask in the scene-relative form plus one marker every 300
	vblanks; replaying such a file re-checks them and reports "[replay]
	desync".  A malformed line, a desync, or a scene reference the run
	never reached (reported at exit) makes the process exit 13.
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "host/diag.h"
#include "gpu/gpu_core.h"

extern unsigned char *Port_PadBuffer[2];	/* pads_shim.cpp */
extern unsigned char *Port_PadMotor[2];		/* pads_shim.cpp - PadSetAct buffer */

static SDL_Gamepad	*g_gamepad;

/*	Rumble (M6): last values armed on the device, so a steady game state
	does not spam the driver every vblank, plus the small motor's smoothed
	level (see rumbleFrame).  */
static Uint16			g_rumbleLow, g_rumbleHigh;
static unsigned long	g_rumbleArmVblank;
static int				g_smallLevel;

/*	button masks in the (Button1<<8)|Button2 word (active-high here;
	inverted at packet-build time)  */
#define BTN_SELECT	0x0100
#define BTN_START	0x0800
#define BTN_UP		0x1000
#define BTN_RIGHT	0x2000
#define BTN_DOWN	0x4000
#define BTN_LEFT	0x8000
#define BTN_L2		0x0001
#define BTN_R2		0x0002
#define BTN_L1		0x0004
#define BTN_R1		0x0008
#define BTN_TRI		0x0010
#define BTN_CIRCLE	0x0020
#define BTN_CROSS	0x0040
#define BTN_SQUARE	0x0080

/*****************************************************************************/
/*	Scripted input: SBSP_PAD_SCRIPT (env) + SBSP_PAD_FILE (file) entries.  */

struct PadEntry
{
	unsigned long	vblank;			/* absolute; valid once `resolved` */
	unsigned		mask;
	int				resolved;
	unsigned long	anchor;			/* open vblank of the anchor occurrence */
	char			scene[48];		/* scene-relative entries only */
	int				nth;
	unsigned long	offset;
	int				line;			/* pad-file line, 0 = SBSP_PAD_SCRIPT */
};

struct EpochCheck
{
	unsigned long	vblank;
	unsigned long	ram;
	uint32_t		crc;
	int				line;
};

/*	malloc/realloc, never a C++ container: see the note in host/diag.cpp
	(the game replaces the global operator delete).  */
static PadEntry		*g_entries;
static int			g_entryCount, g_entryCap;
static EpochCheck	*g_epochs;
static int			g_epochCount, g_epochCap;
static int			g_scriptParsed;
static int			g_desyncs;

static void addEntry(const PadEntry &e)
{
	if (g_entryCount == g_entryCap)
	{
		g_entryCap = g_entryCap ? g_entryCap * 2 : 64;
		g_entries  = (PadEntry *)realloc(g_entries, g_entryCap * sizeof(PadEntry));
	}
	g_entries[g_entryCount++] = e;
}

static void addEpoch(const EpochCheck &ep)
{
	if (g_epochCount == g_epochCap)
	{
		g_epochCap = g_epochCap ? g_epochCap * 2 : 16;
		g_epochs   = (EpochCheck *)realloc(g_epochs, g_epochCap * sizeof(EpochCheck));
	}
	g_epochs[g_epochCount++] = ep;
}

static void scriptParse(void)
{
	const char *s = getenv("SBSP_PAD_SCRIPT");
	int n = 0;
	if (!s)
		return;
	while (*s)
	{
		char *end;
		unsigned long vb = strtoul(s, &end, 10);
		if (end == s || *end != ':')
			break;
		unsigned mask = (unsigned)strtoul(end + 1, &end, 16);
		PadEntry e = {};
		e.vblank   = vb;
		e.mask     = mask & 0xFFFF;
		e.resolved = 1;
		addEntry(e);
		n++;
		if (*end != ',')
			break;
		s = end + 1;
	}
	if (n)
		fprintf(stderr, "[input] SBSP_PAD_SCRIPT: %d entries\n", n);
}

static void padFileFail(const char *path, int line, const char *why)
{
	fprintf(stderr, "[replay] pad-file %s line %d: %s - aborting\n", path, line, why);
	Port_Exit(PORT_EXIT_ORACLE);
}

/*	One pad-file line, comments already stripped, whitespace-trimmed.  */
static int parsePadEntry(const char *tok, PadEntry *e, const char **why)
{
	const char *hash  = strchr(tok, '#');
	const char *colon = strrchr(tok, ':');
	char *end;

	if (!colon)
	{
		*why = "expected ':' before the hex mask";
		return 0;
	}
	unsigned mask = (unsigned)strtoul(colon + 1, &end, 16);
	if (end == colon + 1 || *end)
	{
		*why = "bad hex mask";
		return 0;
	}
	e->mask = mask & 0xFFFF;

	if (!hash)
	{
		e->vblank = strtoul(tok, &end, 10);
		if (end == tok || end != colon)
		{
			*why = "bad vblank number";
			return 0;
		}
		e->resolved = 1;
		return 1;
	}

	size_t n = (size_t)(hash - tok);
	if (n == 0 || n >= sizeof(e->scene))
	{
		*why = "bad scene name";
		return 0;
	}
	memcpy(e->scene, tok, n);
	e->scene[n] = 0;
	e->nth = (int)strtol(hash + 1, &end, 10);
	if (end == hash + 1 || e->nth < 1 || *end != '+')
	{
		*why = "expected Scene#<n>+<offset>";
		return 0;
	}
	e->offset = strtoul(end + 1, &end, 10);
	if (end != colon)
	{
		*why = "bad offset";
		return 0;
	}
	e->resolved = 0;
	return 1;
}

static void padFileParse(void)
{
	const char *path = getenv("SBSP_PAD_FILE");
	if (!path || !*path)
		return;
	FILE *f = fopen(path, "r");
	if (!f)
		padFileFail(path, 0, "cannot open");

	char buf[512];
	int  line = 0, entries = 0;
	while (fgets(buf, sizeof(buf), f))
	{
		line++;
		char *s = buf;
		while (*s == ' ' || *s == '\t')
			s++;

		/*	`# epoch <vblank> ram=<n> crc=<hex>` is data; every other
			comment (a leading `#`, or ` #` after an entry) is dropped.  */
		if (*s == '#')
		{
			EpochCheck ep = {};
			if (sscanf(s, "# epoch %lu ram=%lu crc=%x", &ep.vblank, &ep.ram, &ep.crc) == 3)
			{
				ep.line = line;
				addEpoch(ep);
			}
			continue;
		}
		for (char *c = s + 1; *c; c++)		/* first '#' preceded by a blank */
			if (*c == '#' && (c[-1] == ' ' || c[-1] == '\t'))
			{
				*c = 0;
				break;
			}
		char *e = s + strlen(s);
		while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
			*--e = 0;
		if (!*s)
			continue;

		PadEntry	ent = {};
		const char	*why = "";
		if (!parsePadEntry(s, &ent, &why))
		{
			fclose(f);
			padFileFail(path, line, why);
		}
		ent.line = line;
		addEntry(ent);
		entries++;
	}
	fclose(f);
	fprintf(stderr, "[input] SBSP_PAD_FILE %s: %d entries, %d epoch checks\n",
			path, entries, g_epochCount);
}

static void scriptsParse(void)
{
	if (g_scriptParsed)
		return;
	g_scriptParsed = 1;
	scriptParse();
	padFileParse();
}

/*	Scene-relative entries become absolute the vblank their scene open
	happens; until then they are simply not due.  */
static void resolveEntries(void)
{
	for (int i = 0; i < g_entryCount; i++)
	{
		PadEntry		&e = g_entries[i];
		unsigned long	vb;
		if (!e.resolved && Port_SceneOpenVblank(e.scene, e.nth, &vb))
		{
			e.vblank   = vb + e.offset;
			e.anchor   = vb;
			e.resolved = 1;
		}
	}
}

/*	The entry in force is the LATEST one that has come due - selected by
	vblank, never by position in the list.  (Keeping the last list-order
	match instead made "600:0000,300:0800" hold START forever: at vblank 700
	both are due and the later-listed, earlier-timed entry won.  Scripts are
	normally written in ascending order, but nothing enforces that and a
	silently mistimed automated run is expensive to diagnose.)  */
static unsigned scriptMask(unsigned long vblank)
{
	unsigned		mask = 0;
	int				found = 0;
	unsigned long	best = 0;
	unsigned long	current = Port_LastSceneOpenVblank();

	for (int i = 0; i < g_entryCount; i++)
	{
		const PadEntry &e = g_entries[i];
		if (!e.resolved || vblank < e.vblank)
			continue;
		if (e.vblank < current || (e.scene[0] && e.anchor != current))
			continue;					/* released by a later scene open */
		if (!found || e.vblank >= best)
		{
			best  = e.vblank;
			mask  = e.mask;
			found = 1;
		}
	}
	return mask;
}

static void epochCheck(unsigned long vblank)
{
	for (int i = 0; i < g_epochCount; i++)
	{
		const EpochCheck &ep = g_epochs[i];
		if (ep.vblank != vblank)
			continue;
		const PortGameGlobals *g = Port_GameGlobals();
		unsigned long ram = g->ramUsed ? *g->ramUsed : 0;
		uint32_t      crc = GPU_DisplayCRC32(NULL);
		if (ram != ep.ram || crc != ep.crc)
		{
			g_desyncs++;
			fprintf(stderr, "[replay] desync at vblank %lu (line %d): ram %lu vs %lu, crc %08X vs %08X\n",
					vblank, ep.line, ram, ep.ram, crc, ep.crc);
		}
	}
}

/*	Called from Port_Exit: unreached scene references and desyncs turn a
	clean exit into 13 - a route that quietly never pressed half its
	buttons must not pass.  */
extern "C" int Port_InputAtExit(void)
{
	int bad = g_desyncs;
	for (int i = 0; i < g_entryCount; i++)
	{
		const PadEntry &e = g_entries[i];
		if (!e.resolved)
		{
			fprintf(stderr, "[replay] unsatisfied line %d: %s#%d never opened\n",
					e.line, e.scene, e.nth);
			bad++;
		}
	}
	return bad;
}

/*****************************************************************************/
/*	SBSP_RECORD_PAD=<path>: the applied mask, on change, in the scene-
	relative grammar above, plus `# scene` markers at each open and an
	`# epoch` line every 300 vblanks.  Flushed per line so a crash still
	leaves a usable file.  */
static FILE			*g_rec;
static int			g_recTried;
static unsigned		g_recLastMask;
static char			g_recScene[48];
static int			g_recNth;

static void recordFrame(unsigned long vblank, unsigned mask)
{
	if (!g_recTried)
	{
		g_recTried = 1;
		const char *path = getenv("SBSP_RECORD_PAD");
		if (path && *path)
		{
			g_rec = fopen(path, "w");
			if (g_rec)
				fprintf(g_rec, "# recorded by sbsp --record-pad (mask: START=0800 SELECT=0100 "
							   "UP=1000 RIGHT=2000 DOWN=4000 LEFT=8000 CROSS=0040 "
							   "CIRCLE=0020 SQUARE=0080 TRIANGLE=0010 L1=0004 R1=0008 L2=0001 R2=0002)\n");
			else
				fprintf(stderr, "[input] SBSP_RECORD_PAD: cannot write %s\n", path);
		}
	}
	if (!g_rec)
		return;

	const char		*scene = Port_CurrentScene();
	int				nth    = Port_SceneOpenCount(scene);
	unsigned long	open   = 0;
	int				inScene = nth > 0 && Port_SceneOpenVblank(scene, nth, &open);

	if (inScene && (nth != g_recNth || strcmp(scene, g_recScene) != 0))
	{
		snprintf(g_recScene, sizeof(g_recScene), "%s", scene);
		g_recNth = nth;
		fprintf(g_rec, "# scene %s#%d vblank=%lu\n", scene, nth, open);
	}
	if (mask != g_recLastMask)
	{
		g_recLastMask = mask;
		if (inScene)
			fprintf(g_rec, "%s#%d+%lu:%04X\n", scene, nth, vblank - open, mask);
		else
			fprintf(g_rec, "%lu:%04X\n", vblank, mask);
	}
	if (vblank % 300 == 0)
	{
		const PortGameGlobals *g = Port_GameGlobals();
		fprintf(g_rec, "# epoch %lu ram=%lu crc=%08X\n", vblank,
				g->ramUsed ? *g->ramUsed : 0ul, GPU_DisplayCRC32(NULL));
	}
	fflush(g_rec);
}

/*****************************************************************************/
extern "C" void Port_InputHandleEvent(const void *evv)
{
	const SDL_Event *ev = (const SDL_Event *)evv;
	if (ev->type == SDL_EVENT_GAMEPAD_ADDED && !g_gamepad)
	{
		g_gamepad = SDL_OpenGamepad(ev->gdevice.which);
		if (g_gamepad)
			fprintf(stderr, "[input] gamepad connected: %s\n",
					SDL_GetGamepadName(g_gamepad));
		g_rumbleLow = g_rumbleHigh = 0;		/* fresh device: re-arm from scratch */
		g_smallLevel = 0;
	}
	else if (ev->type == SDL_EVENT_GAMEPAD_REMOVED && g_gamepad &&
			 ev->gdevice.which == SDL_GetGamepadID(g_gamepad))
	{
		SDL_CloseGamepad(g_gamepad);
		g_gamepad = NULL;
		g_rumbleLow = g_rumbleHigh = 0;
		g_smallLevel = 0;
		fprintf(stderr, "[input] gamepad disconnected\n");
	}
}

/*****************************************************************************/
/*	Port_InputFrame runs on every emulated vblank, including before
	Host_EnsureVideo has created the window and on hosts where it never
	will (the SBSP_PAD_SCRIPT path deliberately does not depend on SDL).
	Neither reader may be called before SDL_Init.  */
static bool sdlInputUp(void)
{
	return SDL_WasInit(SDL_INIT_VIDEO) != 0;
}

static unsigned keyboardMask(void)
{
	if (!sdlInputUp())
		return 0;

	const bool *k = SDL_GetKeyboardState(NULL);
	unsigned m = 0;
	if (!k)
		return 0;
	if (k[SDL_SCANCODE_UP])		m |= BTN_UP;
	if (k[SDL_SCANCODE_DOWN])	m |= BTN_DOWN;
	if (k[SDL_SCANCODE_LEFT])	m |= BTN_LEFT;
	if (k[SDL_SCANCODE_RIGHT])	m |= BTN_RIGHT;
	if (k[SDL_SCANCODE_Z])		m |= BTN_CROSS;
	if (k[SDL_SCANCODE_X])		m |= BTN_CIRCLE;
	if (k[SDL_SCANCODE_A])		m |= BTN_SQUARE;
	if (k[SDL_SCANCODE_S])		m |= BTN_TRI;
	if (k[SDL_SCANCODE_RETURN])	m |= BTN_START;
	if (k[SDL_SCANCODE_RSHIFT])	m |= BTN_SELECT;
	if (k[SDL_SCANCODE_Q])		m |= BTN_L1;
	if (k[SDL_SCANCODE_W])		m |= BTN_R1;
	if (k[SDL_SCANCODE_E])		m |= BTN_L2;
	if (k[SDL_SCANCODE_R])		m |= BTN_R2;
	return m;
}

static unsigned gamepadMask(void)
{
	SDL_Gamepad *p = sdlInputUp() ? g_gamepad : NULL;
	unsigned m = 0;
	if (!p)
		return 0;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_UP))		m |= BTN_UP;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_DOWN))		m |= BTN_DOWN;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_LEFT))		m |= BTN_LEFT;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))		m |= BTN_RIGHT;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_SOUTH))			m |= BTN_CROSS;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_EAST))			m |= BTN_CIRCLE;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_WEST))			m |= BTN_SQUARE;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_NORTH))			m |= BTN_TRI;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_START))			m |= BTN_START;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_BACK))			m |= BTN_SELECT;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))	m |= BTN_L1;
	if (SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER))	m |= BTN_R1;
	if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > 8192) m |= BTN_L2;
	if (SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) m |= BTN_R2;
	return m;
}

static unsigned char stickByte(SDL_Gamepad *p, SDL_GamepadAxis axis)
{
	if (!p || !sdlInputUp())
		return 0x80;			/* centred */
	int v = (SDL_GetGamepadAxis(p, axis) >> 8) + 128;	/* -32768..32767 -> 0..255 */
	return (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/*****************************************************************************/
/*	Rumble (M6): forward the actuator bytes the game writes into the
	PadSetAct buffer (pads.cpp ReadController) to the SDL gamepad.
	Layout per PadAlign {0,1,...}: byte 0 = small motor on/off, byte 1 =
	big motor 0-255.  The big motor maps to SDL's low-frequency (heavy)
	rumble, the small one to high-frequency.

	The small motor needs smoothing.  The game derives it as
	(intensity & 1) from the SAME summed envelope that drives the big motor
	(pads.cpp:271, and vibe.cpp:130 clamps only the top end), so the bit is
	effectively a coin flip that changes almost every frame throughout any
	vibration.  Driving SDL's high-frequency motor straight off it would
	slam between silence and full scale at ~30Hz - which no physical small
	motor can follow, and which would defeat the re-arm limiter below by
	making the value "change" on nearly every vblank.  A first-order decay
	toward the bit approximates the motor inertia that does the averaging on
	real hardware, and a deadband keeps the residual wobble (and the big
	motor's own per-frame envelope steps) from re-arming constantly.

	SDL_RumbleGamepad is armed with a 100ms window and re-armed every 3
	vblanks while nonzero (50-60ms, comfortably inside the window), or
	immediately on a meaningful change.  A transition to zero sends one
	explicit stop so rumble ends when the game says so, not when the window
	runs out.  */
static void rumbleFrame(unsigned long vblank)
{
	/*	The decay rate and the deadband are a pair: a bit alternating every
		frame settles into a steady oscillation of about +-1000 around the
		half-scale mean, which has to stay INSIDE the deadband or the
		re-arm limiter is defeated exactly as it was before the smoothing. */
	static const int kDeadband = 0x1000;		/* ~6% of full scale */

	if (!g_gamepad)
		return;

	const unsigned char *m = Port_PadMotor[0];
	Uint16 low = 0;
	int    bit = 0;
	if (m)
	{
		low = (Uint16)((m[1] << 8) | m[1]);		/* 0..255 -> 0..0xFFFF */
		bit = m[0] & 1;
	}

	if (!low && !bit)
		g_smallLevel = 0;						/* game says stop: no tail */
	else
		g_smallLevel += ((bit ? 0xFFFF : 0) - g_smallLevel) >> 4;
	Uint16 high = (Uint16)(g_smallLevel < 0 ? 0 : g_smallLevel);

	bool zero     = !low && !high;
	bool wasZero  = !g_rumbleLow && !g_rumbleHigh;
	int  dLow     = (int)low  - (int)g_rumbleLow;
	int  dHigh    = (int)high - (int)g_rumbleHigh;
	bool changed  = (zero != wasZero) ||
					(dLow  > kDeadband || dLow  < -kDeadband) ||
					(dHigh > kDeadband || dHigh < -kDeadband);
	bool rearm    = !zero && (vblank - g_rumbleArmVblank >= 3);
	if (!changed && !rearm)
		return;

	/*	stop = duration 0 with zero magnitudes; active = 100ms window  */
	SDL_RumbleGamepad(g_gamepad, low, high, zero ? 0 : 100);
	g_rumbleLow       = low;
	g_rumbleHigh      = high;
	g_rumbleArmVblank = vblank;
}

extern "C" void Port_InputFrame(unsigned long vblank)
{
	rumbleFrame(vblank);

	unsigned char *buf = Port_PadBuffer[0];
	if (!buf)
		return;

	scriptsParse();
	resolveEntries();
	unsigned mask = keyboardMask() | gamepadMask() | scriptMask(vblank);
	epochCheck(vblank);
	recordFrame(vblank, mask);

	buf[0] = 0;								/* connected, data valid */
	buf[1] = 0x73;							/* analog controller, 3 halfwords */
	buf[2] = (unsigned char)~(mask >> 8);	/* active low */
	buf[3] = (unsigned char)~mask;
	buf[4] = stickByte(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
	buf[5] = stickByte(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
	buf[6] = stickByte(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
	buf[7] = stickByte(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY);
}
