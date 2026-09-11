/*	The game heap seam.

	On PS1 the linker script computes OPT_LinkerOpts (source/system/lnkopt.mip):
	the free-memory window between the end of the linked image and the top of
	RAM.  MemInit() (source/mem/memory.cpp) takes its entire pool from
	FreeMemAddress/FreeMemSize, and the game's operator new routes there - so
	this one struct is the whole heap.

	The PC shim reserves the arena with VirtualAlloc at a preferred base
	inside the low 16MB: GPU prim tags are 24-bit addresses, and M2's OT
	walker reconstructs them as arenaBase | (tag & 0xFFFFFF), which needs the
	arena inside one 16MB-aligned window.  If the preferred base is taken we
	fall back to any base (harmless for M1's headless file I/O; M2 asserts).

	Also home of the 1KB scratchpad buffer standing in for the PS1's fast RAM
	at 0x1f800000 (see SCRATCH_RAM in source/system/global.h).
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "system/types.h"
#include "system/asmport.h"		/* declares PORT_Scratchpad */
#include "system/lnkopt.h"
#include "host/diag.h"

extern "C" { __attribute__((aligned(16))) unsigned char PORT_Scratchpad[1024+PORT_SCRATCHPAD_GUARD]; }

/*	Guard pattern past the scratchpad's 1KB; host/diag.cpp's Port_MemWatch
	checks it every vblank.  Seeded at priority 101 - before every normal
	static constructor - because cd.cpp's CdBoot already writes the file
	position list into the scratchpad before main() runs.  */
__attribute__((constructor(101)))
static void seedScratchpadGuard(void)
{
	for (int i = 0; i < PORT_SCRATCHPAD_GUARD; i++)
		PORT_Scratchpad[1024 + i] = PORT_SCRATCHPAD_GUARD_BYTE;
}

LNK_OPTS OPT_LinkerOpts;	/* filled before main() by ArenaBoot below */

static const u32	ARENA_SIZE = 8u * 1024u * 1024u;

namespace
{
struct ArenaBoot
{
	ArenaBoot()
	{
		/*	any 16MB-aligned base keeps the arena inside one 24-bit window;
			walk up until one is free  */
		void *base = NULL;
		for (u32 cand = 0x01000000; cand < 0x40000000 && !base; cand += 0x01000000)
			base = VirtualAlloc((void *)(uintptr_t)cand, ARENA_SIZE,
								MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
		if (!base)
		{
			base = VirtualAlloc(NULL, ARENA_SIZE,
								MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			fprintf(stderr, "[shim] arena: no 16MB-aligned base free, got %p "
							"(prim-tag reconstruction unavailable)\n", base);
		}
		if (!base)
		{
			fprintf(stderr, "[shim] arena: VirtualAlloc failed, aborting\n");
			Port_Exit(PORT_EXIT_FAULT);
		}

		OPT_LinkerOpts.RamSize           = 2;			/* matches LNK_RamSize on PS1 */
		OPT_LinkerOpts.StackSize         = 0x3000;
		OPT_LinkerOpts.OrgAddress        = base;
		OPT_LinkerOpts.FreeMemAddress    = base;
		OPT_LinkerOpts.FreeMemSize       = ARENA_SIZE;
		OPT_LinkerOpts.FileSystem        = FS_CD;
		OPT_LinkerOpts.DevKit            = DK_SONY_PCI;
		OPT_LinkerOpts.extraCtorsSize    = 0;
		OPT_LinkerOpts.extraCtorsAddress = NULL;
	}
};
static ArenaBoot g_arenaBoot;
}
