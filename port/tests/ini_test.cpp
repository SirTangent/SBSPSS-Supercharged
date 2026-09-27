/*	Unit test for the M8 shell settings file (port/psyq/host/ini.cpp) and
	the host path helpers (host/hostpath.cpp).

	Checks the parser (comments, blanks, whitespace, unknown keys, empty
	values), the "only if the environment is unset" rule that gives
	argument > environment > ini, that the written
	defaults round-trip through the loader with every non-empty default
	applied, --set's forced override, and that Port_SaveDir honours
	SBSP_SAVE_DIR verbatim.  No SDL, no window: the exe runs from the repo
	root and leaves nothing behind (ini_test_tmp is removed).

	UTF-8 paths (issue #62): the exe's manifest makes the process code page
	UTF-8, so a save directory named in Polish and Japanese is created under
	its real wide name, and a copy of this exe run from inside it finds its
	own directory under that name (the child mode `exedir`).  The name is
	spelled in \x escapes so the source stays ASCII.
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
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

/*	"Lukasz_" with a Polish L-stroke (U+0141), then "Nihon" in kanji
	(U+65E5 U+672C): no single-byte code page spells both.  */
static const char		kUtf8Name[] = "\xC5\x81ukasz_\xE6\x97\xA5\xE6\x9C\xAC";
static const wchar_t	kWideName[] = L"Łukasz_日本";

/*	child mode: does Port_ExeDir end in the directory this copy runs from?  */
static int exeDirChild(void)
{
	char dir[1024];
	if (!Port_ExeDir(dir, sizeof(dir)))
	{
		std::printf("FAIL: child: Port_ExeDir answered nothing\n");
		return 1;
	}
	size_t n = strlen(dir), k = strlen(kUtf8Name);
	if (n <= k || dir[n - k - 1] != '\\' || strcmp(dir + n - k, kUtf8Name) != 0)
	{
		std::printf("FAIL: child: Port_ExeDir = '%s'\n", dir);
		return 1;
	}
	return 0;
}

/*	copy this exe (and SDL3.dll, if it sits beside it - the clang-cl
	build) into dirW and run it there in `exedir` mode; returns its code  */
static int runCopyIn(const wchar_t *dirW, const char *dirUtf8)
{
	wchar_t self[1024], src[1024], dst[1024];
	DWORD n = GetModuleFileNameW(NULL, self, 1024);
	if (!n || n >= 1024)
		return -1;
	wchar_t *slash = wcsrchr(self, L'\\');
	if (!slash)
		return -1;
	swprintf(dst, 1024, L"%ls\\ini_test.exe", dirW);
	if (!CopyFileW(self, dst, FALSE))
		return -1;
	*slash = 0;
	swprintf(src, 1024, L"%ls\\SDL3.dll", self);
	if (GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
	{
		swprintf(dst, 1024, L"%ls\\SDL3.dll", dirW);
		CopyFileW(src, dst, FALSE);
	}

	/*	the narrow spawn is part of the proof: it only finds the copy if
		the UTF-8 name survives the CRT's code-page conversion  */
	char exe[1024], quoted[1100];
	std::snprintf(exe, sizeof(exe), "%s\\ini_test.exe", dirUtf8);
	std::snprintf(quoted, sizeof(quoted), "\"%s\"", exe);
	intptr_t rc = _spawnl(_P_WAIT, exe, quoted, "exedir", (const char *)NULL);

	for (int tries = 0; tries < 50; tries++)		/* the image can stay mapped a moment */
	{
		swprintf(dst, 1024, L"%ls\\ini_test.exe", dirW);
		BOOL gone = DeleteFileW(dst) || GetLastError() == ERROR_FILE_NOT_FOUND;
		swprintf(dst, 1024, L"%ls\\SDL3.dll", dirW);
		gone = (DeleteFileW(dst) || GetLastError() == ERROR_FILE_NOT_FOUND) && gone;
		if (gone)
			break;
		Sleep(20);
	}
	return (int)rc;
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "exedir") == 0)
		return exeDirChild();

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
	check(Port_IniKeyCount() == 27, "key table has the 13 settings + 14 key bindings");

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

	/*	7. UTF-8 paths (issue #62) - see the header  */
	check(GetACP() == 65001, "the manifest makes the process code page UTF-8 (65001)");
	char env[256], utf8Dir[256];
	std::snprintf(utf8Dir, sizeof(utf8Dir), "ini_test_tmp\\%s", kUtf8Name);
	std::snprintf(env, sizeof(env), "SBSP_SAVE_DIR=%s", utf8Dir);
	_putenv(env);
	Port_SaveDir(dir, sizeof(dir));
	_putenv("SBSP_SAVE_DIR=");
	check(strcmp(dir, utf8Dir) == 0, "a UTF-8 SBSP_SAVE_DIR is taken verbatim");
	bool found = false;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(L"ini_test_tmp\\*", &fd);
	if (h != INVALID_HANDLE_VALUE)
	{
		do
			found |= wcscmp(fd.cFileName, kWideName) == 0;
		while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	check(found, "the save directory exists under its real wide name");
	wchar_t wideDir[256];
	swprintf(wideDir, 256, L"ini_test_tmp\\%ls", kWideName);
	if (found)
		check(runCopyIn(wideDir, utf8Dir) == 0, "a copy run from that directory finds it by name (Port_ExeDir)");
	RemoveDirectoryW(wideDir);

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
