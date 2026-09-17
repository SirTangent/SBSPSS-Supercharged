/*	Shadow of the PSY-Q libsn.h for the Win32 port.

	The vintage header defines pollhost()/PSYQpause() as MIPS `break`
	instructions for the SN target-debugger.  On PC, pollhost is a no-op and
	PSYQpause (the ASSERT trap, dbg.cpp:289) becomes a hard trap so failed
	asserts stop in the debugger instead of sailing on.  Everything else
	passes through to the vintage header (-idirafter).  The PS1 build never
	sees this file.
*/
#ifndef _PORT_SHADOW_LIBSN_H
#define _PORT_SHADOW_LIBSN_H

#include_next <libsn.h>

#undef pollhost
#undef PSYQpause
#define pollhost()	((void)0)
#if defined(__GNUC__) || defined(__clang__)
#define PSYQpause()	__builtin_trap()
#else
#define PSYQpause()	__debugbreak()
#endif

#endif
