/**************************/
/*** Memory Alloc Stuff ***/
/**************************/

#ifndef	__MEMORY_HEADER__
#define	__MEMORY_HEADER__


#ifndef _GLOBAL_HEADER_
#include "system\global.h"
#endif


/*****************************************************************************/
// Define if you want to debug memory
#ifdef __USER_paul__
	#ifdef	__VERSION_DEBUG__
	#define	__DEBUG_MEM__
	#endif
#endif

/*****************************************************************************/
// Allocation granularity: every block is a multiple of MEM_ALIGN, behind a
// MEM_ALIGN-byte header holding its length, and (DEBUG) MEM_NUM_GUARDS ints
// of guard either side.  4 on the PlayStation and the 32-bit PC build.  The
// x64 PC build (SBSP_PC64, conv_pc.md #34) needs 16 - pointers are 8 bytes and
// the compiler assumes operator new returns 16-aligned storage - and 4 guard
// ints, so that header + head guard is still a multiple of 16.
#if defined(SBSP_PC64)
#define	MEM_ALIGN		(16)
#define	MEM_NUM_GUARDS	(4)
#else
#define	MEM_ALIGN		(4)
#define	MEM_NUM_GUARDS	(2)
#endif
#define	MEM_ROUND(n)	(((n)+(MEM_ALIGN-1))&~(u32)(MEM_ALIGN-1))

// The header MemAllocate puts in front of the pointer it hands back: the
// length word (MEM_ALIGN bytes) and, in DEBUG, the head guards.  One
// definition for all three readers - MemAllocate sizing the block, MemFree
// walking back to the length word, dumpDebugMem (__DEBUG_MEM__) reading it -
// so they cannot drift.  Walked in bytes: stepping back a count of u32s is
// only the same address while MEM_ALIGN is 4.
//
// These live here rather than in memory.cpp deliberately: that file's line
// numbering is load-bearing.  The PS1 build bakes __LINE__ into its ASSERTs
// and Spongey.cpe is held to byte-identity, so adding or removing a line in
// memory.cpp moves every assert below it.  A header cannot do that.
#ifdef __VERSION_DEBUG__		// == memory.cpp's USE_MEM_GUARDS
#define	MEM_BLOCK_HDR	(MEM_ALIGN + MEM_NUM_GUARDS*sizeof(int))
#else
#define	MEM_BLOCK_HDR	(MEM_ALIGN)
#endif
#define	MEM_BLOCK_LEN(p)	(*(u32 *)((char *)(p) - MEM_BLOCK_HDR))

/*****************************************************************************/
#define LListLen		(256)

/*****************************************************************************/
typedef struct
	{
	char		*Addr;
	u32 		Len;
	u16			Prev;
	u16			Next;
	}sLLNode;

typedef	struct
	{
	u32			TotalRam;
	u32			RamUsed;

	u16			Head;
	u16			Tail;
	u16			SP;

	u16			Stack[LListLen];

	sLLNode		Nodes[LListLen];
	} sLList;

/*****************************************************************************/

char *	MemAllocate( u32 Size, char const *Name, char const * File, int LineNumber);

void	MemInit();
void  	MemFree(void *Addr);

void *	operator new(size_t Size, const char * name = NULL);
void *	operator new[](size_t Size, const char * name = NULL);
void	operator delete(void *Ptr);
void	operator delete[](void *Ptr);


#ifdef __DEBUG_MEM__
	void	dumpDebugMem();
	void	DebugMemFontInit();
	#define MemAlloc( Size, Name )	MemAllocate( (Size), (Name), __FILE__, __LINE__ )
#else
	#define MemAlloc(Size,Name)	MemAllocate( (Size), NULL, NULL, 0 )
	#define	dumpDebugMem	;
	#define	DebugMemFontInit	;

#endif


/*****************************************************************************/
extern sLList		MainRam;		// Ah well!

/*****************************************************************************/

#endif