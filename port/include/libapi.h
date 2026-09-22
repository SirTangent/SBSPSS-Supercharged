/*	Shadow of the PSY-Q libapi.h for the Win32 port.

	The vintage header declares three C functions whose names collide with
	the Windows toolchains at different signatures.  rename() (libapi.h:61,
	the memcard filesystem one) and _get_errno() (libapi.h:93) clash with
	the MinGW CRT; the game calls neither, so they are renamed for the
	duration of the vintage header and passed through (-idirafter puts
	tools/psyq/include behind this file on the search path).  The PS1 build
	never sees this file.

	EnterCriticalSection() (disable interrupts, no arguments) clashes with
	kernel32.  On i686 the two never met - kernel32's is stdcall,
	_EnterCriticalSection@4 - but x64 has one calling convention and no
	decoration, so the static CRT pulling kernel32's import member in made
	it a duplicate symbol (M9).  The PSY-Q one is renamed for good,
	declaration and callers alike (fileio.cpp, psxboot.cpp, clickcount.cpp;
	no #undef), and psyq/api/libapi_stubs.cpp defines that name directly.
	The rename is unconditional: keying it on a predicate here and another
	in the stub (SBSP_PC64 is derived from the pointer size, _WIN64 from
	the compiler) would let the declaration and the definition disagree
	and leave an unresolved symbol; on i686 the rename is merely harmless.
	A shim TU that wants PSY-Q's no-op calls it by its psyq_sdk_ name, and
	one that includes this header must not also use Win32's
	EnterCriticalSection(LPCRITICAL_SECTION) - the macro would rewrite it.
*/
#ifndef _PORT_SHADOW_LIBAPI_H
#define _PORT_SHADOW_LIBAPI_H

#define EnterCriticalSection psyq_sdk_EnterCriticalSection
#define rename     psyq_sdk_rename
#define _get_errno psyq_sdk_get_errno
#include_next <libapi.h>
#undef rename
#undef _get_errno

#endif
