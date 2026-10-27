/*	Shadow of the PSY-Q sys/types.h for the Win32 port.

	The SDK headers (libcd.h, libgte.h, kernel.h, ...) lean on the BSD-style
	u_char/u_short/u_int/u_long typedefs from the vintage sys/types.h.  With
	tools/psyq/include demoted to -idirafter, a plain #include <sys/types.h>
	would find the CRT's header (which lacks them), so this shadow layers the
	PSY-Q-compatible subset ON TOP of the CRT's real header (#include_next
	reaches it, since the psyq dir sits behind the system dirs) - modern
	shim or vendored code keeps off_t/dev_t/stat() working.  The conflicting
	vintage typedefs (time_t as long, dev_t as short, size_t) are
	deliberately NOT replicated.  The PS1 build never sees this file.
*/
#ifndef _PORT_SHADOW_SYS_TYPES_H
#define _PORT_SHADOW_SYS_TYPES_H

#include_next <sys/types.h>	/* the CRT's real header first */
#include <stddef.h>			/* size_t */

#ifndef _UCHAR_T
#define _UCHAR_T
typedef	unsigned char	u_char;
#endif
#ifndef _USHORT_T
#define _USHORT_T
typedef	unsigned short	u_short;
#endif
#ifndef _UINT_T
#define _UINT_T
typedef	unsigned int	u_int;
#endif
#ifndef _ULONG_T
#define _ULONG_T
typedef	unsigned long	u_long;
#endif
#ifndef _SYSIII_USHORT
#define _SYSIII_USHORT
typedef	unsigned short	ushort;
#endif

#ifndef _PORT_CADDR_T
#define _PORT_CADDR_T
typedef	char *	caddr_t;
#endif

#endif
