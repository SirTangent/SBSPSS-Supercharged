/*	Host paths (M8 shell): where the exe is, where the saves live.

	Two layouts are served by the same rules.  A developer tree runs from
	the repo root with data under out/<T>/cd and saves in %APPDATA%\SBSPSS;
	a tester zip is flat - sbsp.exe, data\, saves\ side by side - and the
	exe finds both by looking next to itself:

	  data   SBSP_DATA_DIR (verbatim) > <exe>\data > out/<T>/cd >
	         out/<T>/<V>/version/CD          (cd.cpp resolveDataRoot)
	  saves  SBSP_SAVE_DIR (verbatim) > <exe>\saves (if the directory
	         exists) > %APPDATA%\SBSPSS       (Port_SaveDir, below)

	Everything here is stateless and constructor-free: this TU is pulled
	into unit exes (mcrd_test re-points SBSP_SAVE_DIR between opens), and
	args.cpp calls it from its priority-101 constructor, before main().
	Win32 + CRT only - no SDL (SDL_GetBasePath needs an initialised SDL).
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>

/*	Directory holding the running executable, no trailing separator.
	Returns 0 (dst untouched) if Windows cannot say or it does not fit.  */
extern "C" int Port_ExeDir(char *dst, size_t n)
{
	char path[MAX_PATH];
	DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (len == 0 || len >= MAX_PATH)
		return 0;
	char *bs = strrchr(path, '\\');
	char *fs = strrchr(path, '/');
	char *cut = (fs > bs) ? fs : bs;
	if (!cut)
		return 0;
	*cut = 0;
	if (strlen(path) >= n)
		return 0;
	strcpy(dst, path);
	return 1;
}

extern "C" int Port_FileExists(const char *path)
{
	struct _stat st;
	return _stat(path, &st) == 0 && !(st.st_mode & _S_IFDIR);
}

extern "C" int Port_DirExists(const char *path)
{
	struct _stat st;
	return _stat(path, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
}

/*	mkdir -p: create every segment of a path (moved here from
	mcrd_card.cpp, the port's first mkdir).  Drive roots ("C:") and empty
	segments are skipped; failures are ignored - the caller finds out when
	it opens a file there.  */
extern "C" void Port_MkdirChain(const char *dir)
{
	char part[512];
	size_t n = 0;
	for (const char *p = dir; ; p++)
	{
		if (*p && *p != '\\' && *p != '/')
		{
			if (n < sizeof(part) - 1)
				part[n++] = *p;
			continue;
		}
		part[n] = 0;
		if (n && !(n == 2 && part[1] == ':'))
			_mkdir(part);
		if (!*p)
			break;
		if (n < sizeof(part) - 1)
			part[n++] = '\\';
	}
}

/*	The directory holding card0.mcd and sbsp.ini, created if needed.
	SBSP_SAVE_DIR is taken verbatim (relative paths included - the tests
	rely on it); otherwise a saves\ directory beside the exe wins (the
	tester-zip layout, portable), else %APPDATA%\SBSPSS.  */
extern "C" int Port_SaveDir(char *dst, size_t n)
{
	const char *env = getenv("SBSP_SAVE_DIR");
	if (env)
		snprintf(dst, n, "%s", env);
	else
	{
		char exe[512];
		int portable = 0;
		if (Port_ExeDir(exe, sizeof(exe)))
		{
			snprintf(dst, n, "%s\\saves", exe);
			portable = Port_DirExists(dst);
		}
		if (!portable)
		{
			const char *appdata = getenv("APPDATA");
			snprintf(dst, n, "%s\\SBSPSS", appdata ? appdata : ".");
		}
	}
	Port_MkdirChain(dst);
	return 1;
}
