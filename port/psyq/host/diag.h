/*	M8 harness diagnostics (host/diag.cpp): process exit with a [summary]
	line, [scene] epochs, [assert] routing.  Every line is stderr, one per
	event, tagged so port/tests/run_tier.py can grep it.
*/
#ifndef PORT_DIAG_H
#define PORT_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	PORT_EXIT_CLEAN    = 0,
	PORT_EXIT_ASSERT   = 10,
	PORT_EXIT_FAULT    = 11,
	PORT_EXIT_WATCHDOG = 12,
	PORT_EXIT_ORACLE   = 13,	/* replay/oracle mismatch */
};

/*	host/input.cpp: reports pad-file entries the run never satisfied and
	replay desyncs; nonzero = the run failed its own script.  Only the
	scripted exit (SBSP_EXIT_AFTER) consults it - a user closing the window
	mid-route is not a failure.  */
int		Port_InputAtExit(void);

/*	The one way out of the process: prints [summary] then _exit(code).
	_exit, not exit: the game never shuts down on PS1, so its static
	destructors were never designed to run (one traps).  */
void	Port_Exit(int code) __attribute__((noreturn));

/*	Game-side hooks; the game sees these through source/system/asmport.h.  */
void	Port_SceneEvent(const char *sceneName);
void	Port_FmaEvent(int fmaScript);
void	Port_Assert(const char *expr, const char *file, int line);

/*	Game globals the shim's watches read through.  Registered once from
	system/main.cpp; every pointer stays NULL in the shim-only unit exes,
	which is how the watches know there is no game code linked.  */
struct PortGameGlobals
{
	unsigned long	*ramUsed;			/* MainRam.RamUsed   (mem/memory.h)   */
	int				*memNodeCount;		/* MemNodeCount      (mem/memory.cpp) */
	int				*invincibleSponge;	/* player/player.cpp                  */
	unsigned char	**currPrim;			/* gfx/prim.cpp                       */
	unsigned char	**endPrim;
	unsigned char	**primListStart;
	unsigned char	**primListEnd;
};
void	Port_RegisterGameGlobals(unsigned long *ramUsed, int *memNodeCount,
								 int *invincibleSponge,
								 unsigned char **currPrim, unsigned char **endPrim,
								 unsigned char **primListStart, unsigned char **primListEnd);
const struct PortGameGlobals *Port_GameGlobals(void);

/*	Once per vblank (Host_VBlank): RamUsed high-water (SBSP_MEM_LOG=1),
	MemNodeCount vs its 256 cap, scratchpad guard bytes ([mem] LEAK), and
	the SBSP_SELFTEST=assert|fault|hang@<vblank> exit-path self-test.  */
void	Port_MemWatch(void);

/*	host/crash.cpp - armed by Port_RegisterGameGlobals  */
void	Port_CrashInit(void);
void	Port_WatchdogStart(void);

const char		*Port_CurrentScene(void);
int				Port_SceneOpenCount(const char *sceneName);
/*	vblank of the nth (1-based) open of a scene; 0 = not opened that often  */
int				Port_SceneOpenVblank(const char *sceneName, int nth, unsigned long *vblank);
/*	vblank of the most recent scene open of any kind (0 before the first)  */
unsigned long	Port_LastSceneOpenVblank(void);

#ifdef __cplusplus
}
#endif

#endif
