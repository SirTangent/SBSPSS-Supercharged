/*	sbsp.ini (M8 shell): a config file for the settings a tester touches.

	The file lives beside sbsp.exe - where a tester will look for it, and
	what makes an unpacked folder self-contained - and is written there
	with commented defaults on the first run (args.cpp loadIni; --ini /
	SBSP_INI names a different one).  Every key is the ini spelling of an
	SBSP_* environment variable, and loading it is nothing more than
	_putenv for each key whose variable is not already set - so the
	precedence is

	    command line  >  environment  >  sbsp.ini  >  built-in default

	with zero changes to the consumers, which all getenv() lazily.
	args.cpp calls Port_IniLoad from its priority-101 constructor, after
	its own pre-scan of --save-dir / --ini and BEFORE its env and argument
	passes, so an argument still overrides an ini value.

	The key set is a WHITELIST (kKeys below), shared by the loader and the
	default writer.  Harness switches (SBSP_UNCAPPED, SBSP_EXIT_AFTER,
	SBSP_PAD_FILE, ...) deliberately have no ini spelling: a stray line in
	a tester's ini must never turn an interactive session into a scripted
	one (watchdog armed, pause-on-focus-loss inert).

	Constructor-free, allocation-free (fixed buffers), CRT only: this TU is
	part of the shim archive and rides along into the unit exes.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct IniKey
{
	const char *key;
	const char *env;
	const char *deflt;		/* "" = written commented out */
	const char *comment;
};

/*	Keep the comment to one line: it is what the tester reads.  */
static const IniKey kKeys[] =
{
	{ "window",              "SBSP_WINDOW",              "1024x768", "window size WxH, or 'fullscreen' (borderless); Alt+Enter toggles at run time" },
	{ "scale",               "SBSP_SCALE",               "fit",      "fit = 4:3 letterbox, integer = whole multiples of the 256-line frame at 4:3, stretch = fill the window" },
	{ "vsync",               "SBSP_VSYNC",               "1",        "1 = present with vsync (FIFO); 0 = no vsync (MAILBOX, else IMMEDIATE)" },
	{ "audio_device",        "SBSP_AUDIO_DEVICE",        "",         "part of a playback device name (case-insensitive); empty = the system default" },
	{ "audio_buffer_frames", "SBSP_AUDIO_BUFFER_FRAMES", "0",        "device buffer in sample frames (e.g. 512, 1024); 0 = the driver's default" },
	{ "volume",              "SBSP_VOLUME",              "100",      "master volume 0-100 (applied on the host, the game's own mixer is untouched)" },
	{ "key_up",              "SBSP_KEY_UP",              "Up",       "keyboard bindings: SDL key names (Up, Return, Space, Right Shift, F1, Keypad 0 ...)" },
	{ "key_down",            "SBSP_KEY_DOWN",            "Down",     NULL },
	{ "key_left",            "SBSP_KEY_LEFT",            "Left",     NULL },
	{ "key_right",           "SBSP_KEY_RIGHT",           "Right",    NULL },
	{ "key_cross",           "SBSP_KEY_CROSS",           "Z",        NULL },
	{ "key_circle",          "SBSP_KEY_CIRCLE",          "X",        NULL },
	{ "key_square",          "SBSP_KEY_SQUARE",          "A",        NULL },
	{ "key_triangle",        "SBSP_KEY_TRIANGLE",        "S",        NULL },
	{ "key_start",           "SBSP_KEY_START",           "Return",   NULL },
	{ "key_select",          "SBSP_KEY_SELECT",          "Right Shift", NULL },
	{ "key_l1",              "SBSP_KEY_L1",              "Q",        NULL },
	{ "key_r1",              "SBSP_KEY_R1",              "W",        NULL },
	{ "key_l2",              "SBSP_KEY_L2",              "E",        NULL },
	{ "key_r2",              "SBSP_KEY_R2",              "R",        "(Alt is reserved for host shortcuts and never reaches the game)" },
	{ "prompt_icons",        "SBSP_PROMPT_ICONS",        "auto",     "icons in the button prompts: auto = follow the device in use, keys = always key caps, pad = always the PS1 glyphs" },
	{ "pad_deadzone",        "SBSP_PAD_DEADZONE",        "15",       "analog stick dead zone, percent of full travel (0 = raw)" },
	{ "rumble",              "SBSP_RUMBLE",              "1",        "1 = forward the game's vibration to the gamepad, 0 = never rumble" },
	{ "pause_on_focus_loss", "SBSP_PAUSE_ON_FOCUS_LOSS", "1",        "1 = freeze the game (and its audio) while another window has the focus" },
	{ "language",            "SBSP_LANGUAGE",            "english",  "text language: english swedish dutch italian german - NOTE: only English text exists in the data, the other slots load the same English strings (github issue #37)" },
	{ "data_dir",            "SBSP_DATA_DIR",            "",         "directory holding BIGLUMP.BIN etc.; empty = look in data\\ beside the exe, then the repo's out\\<territory>\\cd" },
	{ "save_dir",            "SBSP_SAVE_DIR",            "",         "directory for card0.mcd; empty = saves\\ beside the exe, else %APPDATA%\\SBSPSS" },
};
#define NUM_KEYS	(int)(sizeof(kKeys) / sizeof(kKeys[0]))

static const IniKey *findKey(const char *key)
{
	for (int i = 0; i < NUM_KEYS; i++)
		if (_stricmp(key, kKeys[i].key) == 0)
			return &kKeys[i];
	return NULL;
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = 0;
	return s;
}

/*	Set a key's variable unconditionally (an argument: --set key=value).
	Returns 1, or 0 for an unknown key (reported).  */
extern "C" int Port_IniSet(const char *key, const char *value, const char *what)
{
	const IniKey *k = findKey(key);
	if (!k)
	{
		fprintf(stderr, "[ini] %s: unknown key '%s' (see sbsp.ini or --help)\n", what, key);
		return 0;
	}
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s=%s", k->env, value);
	_putenv(buf);
	return 1;
}

/*	Load the file: every known key whose environment variable is unset is
	exported.  Returns the number of keys applied, -1 if the file could not
	be opened (not an error - the first run has none).  */
extern "C" int Port_IniLoad(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	char line[1024];
	int  applied = 0, lineNo = 0;
	while (fgets(line, sizeof(line), f))
	{
		lineNo++;
		char *s = trim(line);
		if (!*s || *s == ';' || *s == '#' || *s == '[')
			continue;
		char *eq = strchr(s, '=');
		if (!eq)
		{
			fprintf(stderr, "[ini] %s:%d: not key=value - ignored\n", path, lineNo);
			continue;
		}
		*eq = 0;
		char *key   = trim(s);
		char *value = trim(eq + 1);
		const IniKey *k = findKey(key);
		if (!k)
		{
			fprintf(stderr, "[ini] %s:%d: unknown key '%s' - ignored\n", path, lineNo, key);
			continue;
		}
		if (!*value)
			continue;				/* empty = "use the default"; _putenv("X=") would DELETE it */
		if (getenv(k->env))
			continue;				/* the environment (or an argument) already decided */
		char buf[1024];
		snprintf(buf, sizeof(buf), "%s=%s", k->env, value);
		_putenv(buf);
		applied++;
	}
	fclose(f);
	fprintf(stderr, "[ini] loaded %s (%d key%s applied)\n", path, applied, applied == 1 ? "" : "s");
	return applied;
}

/*	Write the commented defaults.  Empty defaults go out commented, so the
	file documents the key without pinning a value.  */
extern "C" int Port_IniWriteDefaults(const char *path)
{
	FILE *f = fopen(path, "w");
	if (!f)
	{
		fprintf(stderr, "[ini] cannot create %s\n", path);
		return 0;
	}
	fprintf(f,
		"; SpongeBob SquarePants: SuperSponge (PC) - settings.\n"
		"; Written with the defaults on first run; edit and restart the game.\n"
		"; A command-line option or an SBSP_* environment variable overrides\n"
		"; the value here (sbsp.exe --help lists them).  Lines starting with\n"
		"; ';' are comments; an empty value means \"use the default\".\n"
		"\n");
	for (int i = 0; i < NUM_KEYS; i++)
	{
		const IniKey *k = &kKeys[i];
		if (k->comment)
			fprintf(f, "; %s\n", k->comment);
		if (*k->deflt)
			fprintf(f, "%s=%s\n", k->key, k->deflt);
		else
			fprintf(f, ";%s=\n", k->key);
		if (i + 1 < NUM_KEYS && kKeys[i + 1].comment)
			fprintf(f, "\n");
	}
	fclose(f);
	fprintf(stderr, "[ini] wrote defaults to %s\n", path);
	return 1;
}

/*	For tests and the --help text: walk the table.  */
extern "C" int Port_IniKeyCount(void)
{
	return NUM_KEYS;
}

extern "C" const char *Port_IniKeyName(int i, const char **env, const char **deflt)
{
	if (i < 0 || i >= NUM_KEYS)
		return NULL;
	if (env)   *env   = kKeys[i].env;
	if (deflt) *deflt = kKeys[i].deflt;
	return kKeys[i].key;
}
