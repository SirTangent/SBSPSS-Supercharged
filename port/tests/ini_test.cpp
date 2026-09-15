/*	Unit test for the M8 shell settings file (port/psyq/host/ini.cpp) and
	the host path helpers (host/hostpath.cpp).

	Checks the parser (comments, blanks, whitespace, unknown keys, empty
	values), the "only if the environment is unset" rule that gives
	argument > environment > ini, that the written
	defaults round-trip through the loader with every non-empty default
	applied, --set's forced override, and that Port_SaveDir honours
	SBSP_SAVE_DIR verbatim.  No SDL, no window: the exe runs from the repo
	root and leaves nothing behind (ini_test_tmp is removed).
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>

extern "C" int Port_IniLoad(const char *path);
extern "C" int Port_IniWriteDefaults(const char *path);
extern "C" int Port_IniSet(const char *key, const char *value, const char *what);
extern "C" int Port_IniKeyCount(void);
extern "C" const char *Port_IniKeyName(int i, const char **env, const char **deflt);
extern "C" int Port_SaveDir(char *dst, size_t n);
extern "C" int Port_ExeDir(char *dst, size_t n);
extern "C" int Port_DirExists(const char *path);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static bool envIs(const char *name, const char *want)
{
	const char *v = getenv(name);
	if (!want)
		return v == NULL;
	return v && strcmp(v, want) == 0;
}

/*	MSVCRT: "NAME=" removes the variable  */
static void clearAll(void)
{
	for (int i = 0; i < Port_IniKeyCount(); i++)
	{
		const char *env;
		Port_IniKeyName(i, &env, NULL);
		char buf[128];
		snprintf(buf, sizeof(buf), "%s=", env);
		_putenv(buf);
	}
}

static void writeFile(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");
	fputs(text, f);
	fclose(f);
}

int main(void)
{
	_mkdir("ini_test_tmp");
	clearAll();

	/*	1. the parser + the precedence rule  */
	_putenv("SBSP_VOLUME=42");						/* "an argument or the environment" */
	writeFile("ini_test_tmp/a.ini",
		"; a comment\n"
		"# another\n"
		"\n"
		"[section headers are tolerated]\n"
		"volume=7\n"
		"  Scale = integer  \r\n"
		"bogus=1\n"
		"key_cross=\n"
		"save_dir=ini_test_tmp\\from_ini\n"
		"no equals sign here\n"
		"pad_deadzone=30\n");
	int applied = Port_IniLoad("ini_test_tmp/a.ini");
	check(applied == 3, "a.ini: exactly scale + save_dir + pad_deadzone applied");
	check(envIs("SBSP_VOLUME", "42"), "environment beats the ini");
	check(envIs("SBSP_SCALE", "integer"), "key is case-insensitive, value trimmed");
	check(envIs("SBSP_PAD_DEADZONE", "30"), "plain key=value");
	check(envIs("SBSP_KEY_CROSS", NULL), "empty value leaves the variable unset");
	check(envIs("SBSP_SAVE_DIR", "ini_test_tmp\\from_ini"),
		  "save_dir is a normal key (the ini itself lives beside the exe)");
	check(getenv("SBSP_BOGUS") == NULL, "unknown keys never export anything");

	/*	2. a second load never overrides what the first exported  */
	writeFile("ini_test_tmp/b.ini", "scale=stretch\n");
	check(Port_IniLoad("ini_test_tmp/b.ini") == 0, "already-set key is not re-applied");
	check(envIs("SBSP_SCALE", "integer"), "first value stands");

	/*	3. --set forces  */
	check(Port_IniSet("scale", "stretch", "test") == 1, "--set known key");
	check(envIs("SBSP_SCALE", "stretch"), "--set overrides");
	check(Port_IniSet("uncapped", "1", "test") == 0, "--set refuses non-ini keys");
	check(getenv("SBSP_UNCAPPED") == NULL, "harness switches have no ini spelling");

	/*	4. missing file  */
	check(Port_IniLoad("ini_test_tmp/missing.ini") == -1, "missing file reports -1");

	/*	5. the defaults round-trip  */
	clearAll();
	check(Port_IniWriteDefaults("ini_test_tmp/defaults.ini") == 1, "defaults written");
	int nonEmpty = 0;
	for (int i = 0; i < Port_IniKeyCount(); i++)
	{
		const char *deflt;
		Port_IniKeyName(i, NULL, &deflt);
		if (*deflt)
			nonEmpty++;
	}
	applied = Port_IniLoad("ini_test_tmp/defaults.ini");
	check(applied == nonEmpty, "every non-empty default applied, no warnings");
	check(envIs("SBSP_WINDOW", "1024x768"), "window default");
	check(envIs("SBSP_KEY_SELECT", "Right Shift"), "a default with a space survives");
	check(envIs("SBSP_LANGUAGE", "english"), "language default");
	check(envIs("SBSP_DATA_DIR", NULL), "empty defaults are written commented out");
	check(Port_IniKeyCount() == 26, "key table has the 12 settings + 14 key bindings");

	/*	6. paths  */
	char dir[512];
	_putenv("SBSP_SAVE_DIR=ini_test_tmp\\saves");
	Port_SaveDir(dir, sizeof(dir));
	check(strcmp(dir, "ini_test_tmp\\saves") == 0, "SBSP_SAVE_DIR is taken verbatim");
	check(Port_DirExists("ini_test_tmp\\saves"), "save dir is created");
	_putenv("SBSP_SAVE_DIR=");
	char exe[512] = "";
	check(Port_ExeDir(exe, sizeof(exe)) == 1 && *exe, "Port_ExeDir answers");
	size_t n = strlen(exe);
	check(n && exe[n - 1] != '\\' && exe[n - 1] != '/', "no trailing separator");
	check(Port_DirExists(exe), "exe dir exists");

	clearAll();
	remove("ini_test_tmp/a.ini");
	remove("ini_test_tmp/b.ini");
	remove("ini_test_tmp/defaults.ini");
	_rmdir("ini_test_tmp\\saves");
	_rmdir("ini_test_tmp");

	if (g_failures)
	{
		std::printf("ini_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("ini_test: all passed\n");
	return 0;
}
