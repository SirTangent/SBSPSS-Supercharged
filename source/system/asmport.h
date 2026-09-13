/*********************************/
/*** Inline Assembly Selection ***/
/*********************************/

#ifndef __SYSTEM_ASMPORT_H__
#define __SYSTEM_ASMPORT_H__

/*	The original PlayStation build (ccpsx / EGCS targeting MIPS) compiles the
	hand-written GNU inline assembly exactly as it always did.  Any other
	compiler gets portable C++ equivalents instead.

	The vintage compiler predefines 'mips' (verified against cpppsx -dM);
	modern MIPS compilers define '__mips__'.  Define PSX_NO_ASM to force the
	portable paths onto a MIPS compiler (to test them against the originals).
*/

#if (defined(mips) || defined(__mips__)) && !defined(PSX_NO_ASM)
#define	PSX_MIPS_ASM	1
#endif

#ifndef	PSX_MIPS_ASM
#include "system/types.h"
/*	Software-GTE register interface.  The portable equivalents of the
	coprocessor-2 macros funnel through these calls, which a PC port must
	implement in its software GTE.  Register numbers are the raw cop2
	register numbers from the original asm (mfc2/mtc2 = data, cfc2/ctc2 =
	control).

	GTEport_Op receives the DMPSX TAG WORD exactly as embedded in the
	vintage INLINE_C.H macros (e.g. 0x0000007f for rtps, 0x000000bf for
	rtpt) - NOT the raw 25-bit cop2 instruction immediate.  gte_mvmva ORs
	its sf/mx/v/cv/lm fields into the tag at <<25/<<23/<<21/<<19/<<18, so
	the software GTE must decode that packing, not the hardware encoding.
*/
extern "C"
{
u32		GTEport_GetData(int reg);			/* mfc2 */
void	GTEport_SetData(int reg,u32 v);		/* mtc2 */
u32		GTEport_GetCtrl(int reg);			/* cfc2 */
void	GTEport_SetCtrl(int reg,u32 v);		/* ctc2 */
void	GTEport_Op(u32 op);					/* cop2 command (DMPSX tag word) */

/*	The PS1 scratchpad (1KB of fast RAM at 0x1f800000) becomes a real
	buffer on PC, supplied by the PsyQ shim; SCRATCH_RAM in system/global.h
	points at it.  Declared here so game code and shim share one contract.
	The guard bytes past the 1KB are never handed out: the shim seeds them
	and reports "[mem] LEAK" if a scratchpad user ever writes past 1024.
*/
#define PORT_SCRATCHPAD_GUARD		16
#define PORT_SCRATCHPAD_GUARD_BYTE	0xA5
extern unsigned char	PORT_Scratchpad[1024+PORT_SCRATCHPAD_GUARD];

/*	M8 harness hooks (port/psyq/host/diag.cpp).  Game code calls them from
	`#if !defined(PSX_MIPS_ASM)` arms only - see port/docs/conv_pc.md (M8).
*/
void	Port_SceneEvent(const char *sceneName);					/* system/gstate.cpp */
void	Port_FmaEvent(int fmaScript);							/* fma/fma.cpp */
void	Port_Assert(const char *expr,const char *file,int line);	/* system/dbg.cpp */
void	Port_RegisterGameGlobals(u32 *ramUsed,int *memNodeCount,int *invincibleSponge,
								 unsigned char **currPrim,unsigned char **endPrim,
								 unsigned char **primListStart,unsigned char **primListEnd);	/* system/main.cpp */
int		Port_BootSeed(long *seed);								/* system/main.cpp (args.cpp) */
int		Port_AutoplayFinish(void);								/* game/game.cpp (autoplay.cpp) */
int		Port_AutoplaySpatulasAll(void);
int		Port_AutoplayLives(void);
int		Port_AutoplayContinues(void);
int		Port_AutoplayDie(int playerIsDead);
}
#endif

#endif
