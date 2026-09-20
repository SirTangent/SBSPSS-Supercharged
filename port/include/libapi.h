/*	Shadow of the PSY-Q libapi.h for the Win32 port.

	The vintage header declares two C functions whose names collide with the
	MinGW CRT at different signatures: rename() (libapi.h:61, the memcard
	filesystem one) and _get_errno() (libapi.h:93).  The game calls neither.
	Rename them out of the way and pass through to the vintage header
	(-idirafter puts tools/psyq/include behind this file on the search path).
	The PS1 build never sees this file.

	x64 (M9): EnterCriticalSection() is also a Win32 function.  On i686 the
	two never met - kernel32's is stdcall, _EnterCriticalSection@4 - but x64
	has one calling convention and no decoration, so the static CRT pulling
	kernel32's import member in makes it a duplicate symbol.  There the PSY-Q
	one is renamed for good, declaration and callers alike (no #undef), and
	psyq/api/libapi_stubs.cpp defines it under the same name.  No TU that
	includes this header calls the Win32 function.
*/
#ifndef _PORT_SHADOW_LIBAPI_H
#define _PORT_SHADOW_LIBAPI_H

#ifdef _WIN64
#define EnterCriticalSection psyq_sdk_EnterCriticalSection
#endif

#define rename     psyq_sdk_rename
#define _get_errno psyq_sdk_get_errno
#include_next <libapi.h>
#undef rename
#undef _get_errno

#endif
