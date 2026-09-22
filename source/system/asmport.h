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
int		Port_Language(int deflt);								/* system/main.cpp (args.cpp, M8 EUR) */
int		Port_AutoplayFinish(void);								/* game/game.cpp (autoplay.cpp) */
int		Port_AutoplaySpatulasAll(void);
int		Port_AutoplayLives(void);
int		Port_AutoplayContinues(void);
int		Port_AutoplayDie(int playerIsDead);

/*	Button prompts (issue #43).  Every "press this to do that" line in the
	game draws a pad icon beside it; on PC the PS1 glyph is a lie for a
	player on the keyboard, who has no way to know that [] means A.  The
	prompt layer (pad/padicon.cpp) asks the shim which key is bound to a
	pad button RIGHT NOW and draws that key's cap instead.

	The shim answers with a cap id rather than an SDL scancode so the
	scancode table stays beside the binding table it has to track, and game
	code needs no SDL.  PORT_CAP_NONE means "draw the PS1 glyph": it is
	what a gamepad player gets, what `prompt_icons = pad` pins, and what a
	key rebound to something with no cap art falls back to.
*/
enum
{
	PORT_CAP_NONE=0,
	PORT_CAP_A, PORT_CAP_S, PORT_CAP_X, PORT_CAP_Z,
	PORT_CAP_Q, PORT_CAP_W, PORT_CAP_E, PORT_CAP_R,
	PORT_CAP_UP, PORT_CAP_DOWN, PORT_CAP_LEFT, PORT_CAP_RIGHT,
	PORT_CAP_ENTER, PORT_CAP_RSHIFT,
	PORT_CAP__COUNT
};
int		Port_InputPromptCap(const char *button);	/* pad/padicon.cpp */
int		Port_InputPadActive(void);
int		Port_InputKeyFor(const char *button);
}
#endif

#endif
