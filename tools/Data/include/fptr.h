/*	FPTR<T> - a pointer stored in 4 bytes, for the x64 PC build (SBSP_PC64).

	The level (.Lvl) and actor (.SBK) files carry pointer fields: MkLevel /
	MkActor write a file offset into each, 4 bytes wide, and the loader adds
	the load address in place (dstructs.h, the RELOC_PTR sites).  The files
	keep the 32-bit layout the 1999 tools wrote, so on x64 the
	field stays 4 bytes and holds the ADDRESS, not a widened pointer: every
	loaded file lives in the game arena, which port/psyq/api/arena.cpp
	reserves below 1GB, so the address fits and zero-extends back exactly.
	0 stays NULL (MkActor writes PAKSpr=0 for a blank frame).

	No constructors, no destructor, no copy-assignment, one public data
	member: an aggregate POD, so the structs it sits in are still overlaid on
	file data and memcpy'd.  operator=(T*) is not a copy-assignment operator.
	The single conversion to T* is what makes [] + ! if() and argument
	passing work through the built-in operators without ambiguity.

	Only dstructs.h includes this, and only under SBSP_PC64 - the x64 PC
	build, and the x64 builds of MkLevel / MkActor (issue #48), which store
	those offsets through it: the PlayStation build, the 32-bit PC build and
	32-bit tool builds never see it.
*/
#ifndef	__DATA_FPTR_HEADER__
#define	__DATA_FPTR_HEADER__

#if defined(PSX_MIPS_ASM) || !defined(SBSP_PC64)
#error fptr.h is the x64 view of dstructs.h only
#endif

#include <stdint.h>

/* report + exit, a pointer outside the low 4GB: the shim's
   (port/psyq/host/fptr_fail.cpp), or the tools' (port/tools/datatool-src/datatool_abi.cpp) */
extern "C" void	Port_FptrRangeFail(const void *p);

template<class T> struct FPTR
{
	uint32_t	m_addr;

	T			*get() const			{return((T*)(uintptr_t)m_addr);}
	uint32_t	raw() const				{return(m_addr);}
	void		set(T *p)
				{
				if ((uintptr_t)p>>32) Port_FptrRangeFail(p);
				m_addr=(uint32_t)(uintptr_t)p;
				}

	FPTR		&operator=(T *p)		{set(p); return(*this);}
				operator T*() const		{return(get());}
	T			*operator->() const		{return(get());}
};

#endif
