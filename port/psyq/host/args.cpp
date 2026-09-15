/*	Command-line arguments (M4 dev tooling).

	The game owns main() with no parameters, so the MinGW CRT globals
	__argc/__argv are read instead - from an early-priority constructor,
	because parts of the shim consume their configuration from static
	initialisers (cd.cpp's CdBoot reads SBSP_DATA_DIR while building the
	virtual disc directory, before main() runs).

	  --level C-L | N   boot straight into a level, skipping the frontend
	                    (needs the Port_BootLevel hook in system/main.cpp).
	                    C-L: chapter 1-5, level 1-5 (L=5 is that chapter's
	                    bonus level; 6-N also addresses bonus level N).
	                    N: raw LvlTable index 0..24.
	                    Env equivalent: SBSP_BOOT_LEVEL (same formats).
	  --seed <n>        fixed setRndSeed value instead of the boot tick count
	                    (Port_BootSeed hook, M8).  Env: SBSP_SEED.
	  --invincible      SBSP_INVINCIBLE=1: the DEBUG pause menu's
	                    invincibleSponge, set at Port_RegisterGameGlobals (M8).
	  --language <l>    text language for the boot-time
	                    TranslationDatabase::loadLanguage (Port_Language hook
	                    in system/main.cpp, M8 EUR).  Either a name or the
	                    locale/textdbase.h enum index it stands for:
	                    english=0 swedish=1 dutch=2 italian=3 german=4
	                    (kLanguageNames below IS that enum, in order).
	                    Env: SBSP_LANGUAGE.  The shipped data carries
	                    English text in every language slot (the four other
	                    translation sources are stubs), so this proves the
	                    load path rather than changing what is displayed -
	                    real localization is github issue #37.

	The rest are aliases for the SBSP_* environment variables - the argument
	just sets the variable (overriding an inherited one), and the existing
	consumers stay env-only:

	  --data-dir <path>     SBSP_DATA_DIR
	  --pad-script <s>      SBSP_PAD_SCRIPT
	  --dump-frames <list>  SBSP_DUMP_FRAMES
	  --dump-dir <path>     SBSP_DUMP_DIR
	  --exit-after <n>      SBSP_EXIT_AFTER
	  --dump-audio <wav>    SBSP_DUMP_AUDIO (M5: deterministic mixer dump,
	                        disables the playback device)
	  --save-dir <path>     SBSP_SAVE_DIR (M6: memory-card image directory,
	                        default %APPDATA%\SBSPSS)
	  --pad-file <path>     SBSP_PAD_FILE   (M8: see host/input.cpp)
	  --record-pad <path>   SBSP_RECORD_PAD (M8)
	  --frame-crc           SBSP_FRAME_CRC=1 (M8: [frame] line per vblank)
	  --no-cd-pace          SBSP_CD_PACE=0
	  --no-audio            SBSP_NO_AUDIO=1
	  --pace-log            SBSP_PACE_LOG=1
	  --uncapped            SBSP_UNCAPPED=1 (M8: host/pump.cpp - emulated time
	                        passes only while the game waits; implies
	                        SBSP_CD_PACE=0; with --no-audio --seed the run is
	                        deterministic and faster than real time)
	  --assert-continue     SBSP_ASSERT_CONTINUE=1 (M8 shell)
	  --mem-log             SBSP_MEM_LOG=1 (M8 shell)

	Settings (M8 shell, host/ini.cpp): sbsp.ini beside the exe holds the
	tester-facing knobs, each the ini spelling of an SBSP_* variable, and
	is written with commented defaults on first run.  Precedence is
	argument > environment > ini (loadIni below).  Argument twins:
	  --ini <path>          SBSP_INI (where the file is)
	  --window <WxH|fullscreen>  SBSP_WINDOW
	  --scale <fit|integer|stretch>  SBSP_SCALE
	  --vsync <0|1>         SBSP_VSYNC
	  --volume <0-100>      SBSP_VOLUME
	  --set key=value       any ini key (SBSP_<KEY>), e.g. --set key_cross=Space

	Both "--flag value" and "--flag=value" spellings work.  Unknown
	arguments warn and are ignored (the run continues).  --help prints
	this surface and exits.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int	g_bootLevel = -1;		/* -1 = normal boot (frontend) */
static long	g_seed;
static int	g_seedSet;
static int	g_language = -1;		/* -1 = the game's default (ENGLISH) */

/*	--language / SBSP_LANGUAGE value -> TranslationDatabase::loadLanguage()
	argument.  The game's language type is the anonymous enum in
	source/locale/textdbase.h - ENGLISH=0 SWEDISH=1 DUTCH=2 ITALIAN=3
	GERMAN=4 (NUM_OF_LANGUAGES=5) - and this table is that enum in order,
	so a name resolves to its index here and an index is passed through as
	is.  The user may give either form ("german" or "4"); names are
	case-insensitive.  Keep the table in enum order if textdbase.h ever
	changes - it is the only place the shim spells the mapping out.  */
static const char *const kLanguageNames[] =
	{ "english", "swedish", "dutch", "italian", "german" };
#define NUM_LANGUAGE_NAMES	(int)(sizeof(kLanguageNames) / sizeof(kLanguageNames[0]))

/*	"C-L" (chapter-level) or a raw LvlTable index.  Returns 0..24, or -1 on
	a malformed/out-of-range value.  Index math mirrors LvlTable's layout
	(source/level/level.cpp:139): 5 rows per chapter, row 5 = the chapter's
	bonus (kelp-world) level; rows 25/26 are FMA scenes, not bootable.  */
static int parseLevel(const char *s)
{
	char *end;
	long a = strtol(s, &end, 10);
	if (end == s)
		return -1;
	if (*end == '-')
	{
		long b = strtol(end + 1, &end, 10);
		if (*end)
			return -1;
		if (a >= 1 && a <= 5 && b >= 1 && b <= 5)
			return (int)((a - 1) * 5 + (b - 1));
		if (a == 6 && b >= 1 && b <= 5)		/* kelp world: 6-N = bonus N */
			return (int)((b - 1) * 5 + 4);
		return -1;
	}
	if (*end)
		return -1;
	return (a >= 0 && a <= 24) ? (int)a : -1;
}

static void parseSeed(const char *s, const char *what)
{
	char *end;
	long v = strtol(s, &end, 0);
	if (end == s || *end)
	{
		fprintf(stderr, "[args] bad %s '%s' - using the boot tick count\n", what, s);
		return;
	}
	g_seed = v;
	g_seedSet = 1;
}

/*	A language name (case-insensitive) or its textdbase.h enum index -
	see kLanguageNames.  Anything else is reported with the accepted set.  */
static void parseLanguage(const char *s, const char *what)
{
	char *end;
	long v = strtol(s, &end, 10);
	if (end != s && !*end && v >= 0 && v < NUM_LANGUAGE_NAMES)
	{
		g_language = (int)v;
		return;
	}
	for (int i = 0; i < NUM_LANGUAGE_NAMES; i++)
	{
		if (_stricmp(s, kLanguageNames[i]) == 0)
		{
			g_language = i;
			return;
		}
	}
	fprintf(stderr, "[args] bad %s '%s' - want", what, s);
	for (int i = 0; i < NUM_LANGUAGE_NAMES; i++)
		fprintf(stderr, "%s %s=%d", i ? "," : "", kLanguageNames[i], i);
	fprintf(stderr, " - using english\n");
}

static int uncappedRequested(int argc, char **argv)
{
	const char *e = getenv("SBSP_UNCAPPED");
	if (e && *e && *e != '0')
		return 1;
	for (int i = 1; i < argc; i++)
		if (strcmp(argv[i], "--uncapped") == 0)
			return 1;
	return 0;
}

static void usage(void)
{
	fprintf(stderr,
		"sbsp [options]\n"
		"  --level C-L | N       boot straight into a level (chapter 1-5,\n"
		"                        level 1-5; L=5 = bonus; or LvlTable index 0-24)\n"
		"  --seed <n>            fixed random seed        (SBSP_SEED)\n"
		"  --invincible          player takes no damage   (SBSP_INVINCIBLE=1)\n"
		"  --language <l>        boot text language       (SBSP_LANGUAGE)\n"
		"                        a name or its locale/textdbase.h enum index:\n"
		"                        english=0 swedish=1 dutch=2 italian=3 german=4\n"
		"                        NOTE: only English text exists in the data - every\n"
		"                        other slot loads the same English strings (issue #37)\n"
		"  --data-dir <path>     CD data directory        (SBSP_DATA_DIR)\n"
		"  --pad-script <s>      scripted input           (SBSP_PAD_SCRIPT)\n"
		"  --pad-file <path>     scripted input from file (SBSP_PAD_FILE)\n"
		"                        lines: <vblank>:<hex> | <Scene>#<n>+<off>:<hex>\n"
		"                        hex mask: START=0800 SELECT=0100 UP=1000 RIGHT=2000\n"
		"                        DOWN=4000 LEFT=8000 CROSS=0040 CIRCLE=0020 SQUARE=0080\n"
		"                        TRIANGLE=0010 L1=0004 R1=0008 L2=0001 R2=0002\n"
		"  --record-pad <path>   write the applied input  (SBSP_RECORD_PAD)\n"
		"                        in --pad-file form, with # epoch desync markers\n"
		"  --frame-crc           [frame] <vbl> crc= line  (SBSP_FRAME_CRC=1)\n"
		"  --dump-frames <list>  BMP dump vblanks         (SBSP_DUMP_FRAMES)\n"
		"  --dump-dir <path>     where dumps go           (SBSP_DUMP_DIR)\n"
		"  --exit-after <n>      clean exit at vblank n   (SBSP_EXIT_AFTER)\n"
		"  --dump-audio <wav>    mixer audio to WAV       (SBSP_DUMP_AUDIO)\n"
		"  --save-dir <path>     memory-card directory    (SBSP_SAVE_DIR)\n"
		"  --no-cd-pace          instant loads            (SBSP_CD_PACE=0)\n"
		"  --no-audio            no playback device       (SBSP_NO_AUDIO=1)\n"
		"  --pace-log            frame-pacing stderr log  (SBSP_PACE_LOG=1)\n"
		"  --uncapped            vblanks not wall-paced   (SBSP_UNCAPPED=1)\n"
		"                        (implies --no-cd-pace; + --no-audio --seed: deterministic)\n"
		"  --assert-continue     log asserts, keep going  (SBSP_ASSERT_CONTINUE=1)\n"
		"  --mem-log             RamUsed high-water log   (SBSP_MEM_LOG=1)\n"
		"Settings (sbsp.ini beside the exe, written with defaults on first run;\n"
		"argument > environment > ini):\n"
		"  --ini <path>          settings file            (SBSP_INI)\n"
		"  --window WxH|fullscreen  window size / borderless fullscreen (SBSP_WINDOW)\n"
		"  --scale fit|integer|stretch  viewport scaling  (SBSP_SCALE)\n"
		"  --vsync 0|1           present with vsync       (SBSP_VSYNC)\n"
		"  --volume 0-100        master volume            (SBSP_VOLUME)\n"
		"  --set key=value       any ini key: audio_device audio_buffer_frames\n"
		"                        key_<button> pad_deadzone rumble pause_on_focus_loss\n"
		"                        language data_dir       (SBSP_<KEY>)\n"
		"  Alt+Enter toggles fullscreen; the game pauses while another window has focus\n"
		"Env only: SBSP_PRIM_LOG=1 (prim-pool high-water log),\n"
		"          SBSP_WATCHDOG=<s> (exit 12 after s seconds without a vblank; 30, 0=off),\n"
		"          SBSP_SELFTEST=assert|fault|hang@<vblank> (exercise an exit path)\n"
		"Exit codes: 0 clean, 10 assert, 11 fault, 12 watchdog, 13 replay/oracle\n");
}

/*	If argv[*i] names this option, set *matched and return its value:
	"--name=value", or "--name value" (advancing *i past the value).
	Returns NULL - with *matched already set, so the caller reports nothing
	further - when the option IS present but its value is missing or is
	itself an option.  Accepting the latter meant "--data-dir --level 1-2"
	silently set SBSP_DATA_DIR=--level and surfaced only much later, as an
	abort() inside CdRead.  */
static const char *argValue(const char *name, int *i, int argc, char **argv,
							int *matched)
{
	const char *a = argv[*i];
	size_t n = strlen(name);

	if (strncmp(a, name, n) != 0 || (a[n] != '\0' && a[n] != '='))
		return NULL;
	*matched = 1;
	if (a[n] == '=')
		return a + n + 1;
	if (*i + 1 < argc && strncmp(argv[*i + 1], "--", 2) != 0)
		return argv[++*i];
	fprintf(stderr, "[args] %s needs a value - ignored\n", name);
	return NULL;
}

/*	host/hostpath.cpp, host/ini.cpp, host/crash.cpp - declared here rather
	than through a header because this TU carries no include set (see
	port/CMakeLists.txt psyq_args).  */
extern "C" int Port_SaveDir(char *dst, size_t n);
extern "C" int Port_ExeDir(char *dst, size_t n);
extern "C" int Port_FileExists(const char *path);
extern "C" int Port_IniLoad(const char *path);
extern "C" int Port_IniWriteDefaults(const char *path);
extern "C" int Port_IniSet(const char *key, const char *value, const char *what);
extern "C" int Port_HarnessRun(void);

/*	sbsp.ini (M8 shell): the lowest tier of `argument > environment > ini`.
	Runs AFTER the argument pass, so every --flag has already exported its
	variable and the loader's "only if unset" rule sees arguments and the
	inherited environment alike; the three parsed-into-globals options
	(--level/--seed/--language) read their variables after this returns.
	Location: --ini / SBSP_INI, else <exe dir>\sbsp.ini - where a tester
	will look, and what makes an unpacked folder self-contained.  An older
	one beside card0.mcd is still read when there is none there.  Defaults
	are written only for an interactive run (a scripted one must leave no
	files behind), and in the save directory instead when the exe's own is
	not writable.  */
static void loadIni(void)
{
	char exeDir[512], saveDir[512], path[600], legacy[600];

	const char *explicitPath = getenv("SBSP_INI");
	if (explicitPath && *explicitPath)
	{
		if (Port_IniLoad(explicitPath) < 0)
			fprintf(stderr, "[ini] cannot open %s\n", explicitPath);
		return;
	}

	int haveExe = Port_ExeDir(exeDir, sizeof(exeDir));
	if (haveExe)
		snprintf(path, sizeof(path), "%s\\sbsp.ini", exeDir);

	/*	Beside card0.mcd is where this file first lived; it is still READ
		when there is none beside the exe, so an edited one is never
		silently ignored - but a new one is always written next to the exe,
		where a settings file belongs and where the tester will look.  */
	Port_SaveDir(saveDir, sizeof(saveDir));
	snprintf(legacy, sizeof(legacy), "%s\\sbsp.ini", saveDir);

	if (haveExe && Port_IniLoad(path) >= 0)
	{
		if (Port_FileExists(legacy))
			fprintf(stderr, "[ini] note: %s also exists and was NOT read - "
							"the one beside the exe wins; delete the other\n", legacy);
		return;
	}
	if (Port_IniLoad(legacy) >= 0)
		return;

	/*	None yet.  A scripted run leaves no files behind.  */
	if (Port_HarnessRun())
		return;
	if (haveExe && Port_IniWriteDefaults(path))
	{
		Port_IniLoad(path);
		return;
	}
	/*	the exe directory is not writable (an install under Program Files):
		fall back to the save directory, which always is  */
	if (Port_IniWriteDefaults(legacy))
		Port_IniLoad(legacy);
}

/*	Priority 101 (0-100 are reserved): runs before every normal-priority
	static constructor in the program, so the env aliases are in place
	before any consumer - including cd.cpp's CdBoot - reads them.  */
__attribute__((constructor(101)))
static void parseArgs(void)
{
	static const struct { const char *arg; const char *env; } aliases[] =
	{
		{ "--data-dir",    "SBSP_DATA_DIR"    },
		{ "--pad-script",  "SBSP_PAD_SCRIPT"  },
		{ "--dump-frames", "SBSP_DUMP_FRAMES" },
		{ "--dump-dir",    "SBSP_DUMP_DIR"    },
		{ "--exit-after",  "SBSP_EXIT_AFTER"  },
		{ "--dump-audio",  "SBSP_DUMP_AUDIO"  },
		{ "--save-dir",    "SBSP_SAVE_DIR"    },
		{ "--pad-file",    "SBSP_PAD_FILE"    },
		{ "--record-pad",  "SBSP_RECORD_PAD"  },
		{ "--ini",         "SBSP_INI"         },	/* M8 shell: the settings file */
		{ "--window",      "SBSP_WINDOW"      },
		{ "--scale",       "SBSP_SCALE"       },
		{ "--vsync",       "SBSP_VSYNC"       },
		{ "--volume",      "SBSP_VOLUME"      },
	};
	static const struct { const char *arg; const char *setting; } switches[] =
	{
		{ "--no-cd-pace",      "SBSP_CD_PACE=0"         },
		{ "--pace-log",        "SBSP_PACE_LOG=1"        },
		{ "--no-audio",        "SBSP_NO_AUDIO=1"        },
		{ "--invincible",      "SBSP_INVINCIBLE=1"      },
		{ "--frame-crc",       "SBSP_FRAME_CRC=1"       },
		{ "--assert-continue", "SBSP_ASSERT_CONTINUE=1" },	/* M8 shell: argv twins of */
		{ "--mem-log",         "SBSP_MEM_LOG=1"         },	/* the env-only harness knobs */
	};
	int levelFromArg = 0, languageFromArg = 0;

	if (uncappedRequested(__argc, __argv))
	{
		/*	The CD read deadline (cd.cpp) is wall-clock; under --uncapped a
			paced load would spin through vblanks at CPU speed.  */
		_putenv("SBSP_UNCAPPED=1");
		_putenv("SBSP_CD_PACE=0");
		fprintf(stderr, "[args] uncapped: CD pacing off (SBSP_CD_PACE=0)\n");
	}

	for (int i = 1; i < __argc; i++)
	{
		const char *v;
		if (strcmp(__argv[i], "--help") == 0 || strcmp(__argv[i], "-h") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(__argv[i], "--uncapped") == 0)
			continue;		/* handled up front - see uncappedRequested */
		int matched = 0;
		for (int s = 0; s < (int)(sizeof(switches) / sizeof(switches[0])); s++)
		{
			if (strcmp(__argv[i], switches[s].arg) == 0)
			{
				_putenv(switches[s].setting);
				matched = 1;
				break;
			}
		}
		if (matched)
			continue;
		if ((v = argValue("--level", &i, __argc, __argv, &matched)) != NULL)
		{
			g_bootLevel = parseLevel(v);
			levelFromArg = 1;
			if (g_bootLevel < 0)
				fprintf(stderr, "[args] bad --level '%s' - booting normally\n", v);
		}
		if (!matched && (v = argValue("--seed", &i, __argc, __argv, &matched)) != NULL)
			parseSeed(v, "--seed");
		if (!matched && (v = argValue("--language", &i, __argc, __argv, &matched)) != NULL)
		{
			parseLanguage(v, "--language");
			languageFromArg = 1;
		}
		if (!matched && (v = argValue("--set", &i, __argc, __argv, &matched)) != NULL)
		{
			/*	--set key=value: any sbsp.ini key from the command line  */
			char kv[1024];
			snprintf(kv, sizeof(kv), "%s", v);
			char *eq = strchr(kv, '=');
			if (!eq || eq == kv)
				fprintf(stderr, "[args] --set wants key=value, got '%s' - ignored\n", v);
			else
			{
				*eq = 0;
				Port_IniSet(kv, eq + 1, "--set");
			}
		}
		for (int a = 0; !matched && a < (int)(sizeof(aliases) / sizeof(aliases[0])); a++)
		{
			if ((v = argValue(aliases[a].arg, &i, __argc, __argv, &matched)) != NULL)
			{
				char buf[1024];
				snprintf(buf, sizeof(buf), "%s=%s", aliases[a].env, v);
				_putenv(buf);
			}
		}
		if (!matched)
			fprintf(stderr, "[args] unknown argument '%s' (see --help) - ignored\n",
					__argv[i]);
	}

	loadIni();

	/*	The environment (now including what the ini exported) for the three
		options that are parsed into globals, unless an argument decided.  */
	const char *e = getenv("SBSP_BOOT_LEVEL");
	if (!levelFromArg && e && *e)
	{
		g_bootLevel = parseLevel(e);
		if (g_bootLevel < 0)
			fprintf(stderr, "[args] bad SBSP_BOOT_LEVEL '%s' - booting normally\n", e);
	}
	e = getenv("SBSP_SEED");
	if (!g_seedSet && e && *e)
		parseSeed(e, "SBSP_SEED");
	e = getenv("SBSP_LANGUAGE");
	if (!languageFromArg && e && *e)
		parseLanguage(e, "SBSP_LANGUAGE");

	if (g_bootLevel >= 0)
		fprintf(stderr, "[args] boot level: LvlTable[%d] (chapter %d level %d)\n",
				g_bootLevel, g_bootLevel / 5 + 1, g_bootLevel % 5 + 1);
	if (g_language > 0)		/* the ini's default is english, the game's own default: say nothing */
		fprintf(stderr, "[args] language: %s (%d)\n", kLanguageNames[g_language], g_language);
}

/*	Hook read by system/main.cpp's scene select (PC build only): the
	LvlTable index to boot into, or -1 for the normal frontend boot.  */
extern "C" int Port_BootLevel(void)
{
	return g_bootLevel;
}

/*	Hook read by system/main.cpp's InitSystem: 1 and the seed when --seed /
	SBSP_SEED was given, else 0 (the game keeps setRndSeed(VidGetTickCount())).  */
extern "C" int Port_BootSeed(long *seed)
{
	if (g_seedSet)
		*seed = g_seed;
	return g_seedSet;
}

/*	Hook read by system/main.cpp's InitSystem (M8 EUR): the language enum
	value to hand TranslationDatabase::loadLanguage, or `deflt` (the game's
	ENGLISH) when no --language / SBSP_LANGUAGE was given.  Boot is the only
	loadLanguage the game ever reaches: the save restore's copy runs under
	`if(!isLoaded())`, which is never true after this one.  */
extern "C" int Port_Language(int deflt)
{
	return g_language >= 0 ? g_language : deflt;
}
