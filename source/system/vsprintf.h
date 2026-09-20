#ifndef __SYSTEM_VSPRINTF_H__
#define	__SYSTEM_VSPRINTF_H__


#if !defined(mips) && !defined(__mips__)
// PC (any non-MIPS compiler - an ABI matter, so not PSX_MIPS_ASM, which
// PSX_NO_ASM can clear on the PlayStation too): the compiler's own varargs
// (conv_pc.md #33).  The defs below walk the
// stack from &v, which is the i386 convention only - x64 passes the first four
// arguments in registers.  A short is promoted to int on its way through ...
// and must be fetched as one; %p prints the low 32 bits of the pointer
// (number() takes a long - every game pointer lives in the arena, below 4GB).
#include <stdarg.h>
#include <stdint.h>
typedef va_list __va_list;
#define __va_start(ap,v)		va_start(ap,v)
#define __va_arg(ap,t)			va_arg(ap,t)
#define __va_end(ap)			va_end(ap)
#define __va_arg_short(ap)		((short)va_arg(ap,int))
#define __va_arg_ushort(ap)		((unsigned short)va_arg(ap,int))
#define __va_arg_ptr(ap)		((unsigned long)(uintptr_t)va_arg(ap,void *))
#else
// stdarg defs from MSVC
#define _INTSIZEOF(n)   ( (sizeof(n) + sizeof(int) - 1) & ~(sizeof(int) - 1) )
#define __va_start(ap,v)  ( ap = (__va_list)&v + _INTSIZEOF(v) )
#define __va_arg(ap,t)    ( *(t *)((ap += _INTSIZEOF(t)) - _INTSIZEOF(t)) )
#define __va_end(ap)      ( ap = (__va_list)0 )
typedef char *__va_list;
#define __va_arg_short(ap)		__va_arg(ap, short)
#define __va_arg_ushort(ap)		__va_arg(ap, unsigned short)
#define __va_arg_ptr(ap)		(unsigned long) __va_arg(ap, void *)
#endif


extern int __vsprintf(char *buf, const char *fmt, __va_list args);

#endif	/* __SYSTEM_VSPRINTF_H__ */