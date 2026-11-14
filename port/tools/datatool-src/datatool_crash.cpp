/*	datatool_crash.cpp - linked into the rebuilt MkActor and MkLevel (issue #48).

	A crash in one of the 1999 converters in the middle of a data build is
	otherwise a bare "Segmentation fault" from make.  This reports the
	exception and the stack as offsets into the exe; the exe's PDB turns them
	into source lines:
		llvm-symbolizer --obj=MkLevel.exe 0x<imagebase + offset>
	(the image base is printed alongside).  Then the process exits 125.
*/
#include <stdio.h>
#include <windows.h>

static LONG WINAPI datatool_crash(EXCEPTION_POINTERS *e)
{
	HMODULE		exe = GetModuleHandleA(NULL);
	ULONG_PTR	base = (ULONG_PTR)exe;
	char		name[MAX_PATH];
	GetModuleFileNameA(exe, name, sizeof(name));
	fprintf(stderr, "FATAL: exception 0x%08lx at %s+0x%llx (image base 0x%llx)\n",
	        e->ExceptionRecord->ExceptionCode, name,
	        (unsigned long long)((ULONG_PTR)e->ExceptionRecord->ExceptionAddress - base),
	        (unsigned long long)base);

	void	*frames[48];
	USHORT	n = RtlCaptureStackBackTrace(0, 48, frames, NULL);
	for (USHORT i = 0; i < n; i++)
	{
		HMODULE mod = NULL;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                   (LPCSTR)frames[i], &mod);
		if (mod == exe)
			fprintf(stderr, "  exe+0x%llx\n", (unsigned long long)((ULONG_PTR)frames[i] - base));
	}
	fflush(stderr);
	ExitProcess(125);
	return EXCEPTION_EXECUTE_HANDLER;
}

static const int datatool_crash_installed = (SetUnhandledExceptionFilter(datatool_crash), 0);
