/*	SBSP_AUTOPLAY (M8 harness): drive the game to its boundaries without a
	player.  Comma-separated key=value tokens, read once:

	  finish=N        every level ends N vblanks after play starts - the
	                  retail bonus-level timer path in CGameScene::initLevel
	                  is armed for every level (game.cpp, conv_pc.md #28)
	  spatulas=all    on finish, the save slot records every spatula
	                  (bookkeeping only; the player's carried count is untouched)
	  lives=N         starting GameSlot.m_lives (0..127, a signed char) -
	                  written at the FIRST initLevel only: game over ->
	                  continue -> Map -> level runs initLevel again, and
	                  re-writing there would make continues inexhaustible.
	                  Game over comes when m_lives goes negative, so
	                  lives=0,die=1 is the shortest game over.
	  continues=N     starting GameSlot.m_continues (0..127), same one-shot rule
	  die=N           kill the player N times (CPlayer::dieYouPorousFreak),
	                  one per observed death->respawn cycle

	Unknown or malformed tokens warn and are skipped, like args.cpp.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/pump.h"

static int	g_parsed;
static long	g_finish    = -1;
static long	g_lives     = -1;
static long	g_continues = -1;
static long	g_die;
static int	g_spatulasAll;

static void parse(void)
{
	if (g_parsed)
		return;
	g_parsed = 1;

	const char *e = getenv("SBSP_AUTOPLAY");
	if (!e || !*e)
		return;

	char buf[256];
	snprintf(buf, sizeof(buf), "%s", e);
	for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ","))
	{
		char *eq = strchr(tok, '=');
		char *end = NULL;
		long v = eq ? strtol(eq + 1, &end, 10) : 0;
		int  numeric = eq && end != eq + 1 && !*end && v >= 0;

		if (strcmp(tok, "spatulas=all") == 0)
			g_spatulasAll = 1;
		else if (numeric && strncmp(tok, "finish=", 7) == 0)
			g_finish = v;
		else if (numeric && v <= 127 && strncmp(tok, "lives=", 6) == 0)
			g_lives = v;			/* GameSlot.m_lives is a signed char */
		else if (numeric && v <= 127 && strncmp(tok, "continues=", 10) == 0)
			g_continues = v;
		else if (numeric && strncmp(tok, "die=", 4) == 0)
			g_die = v;
		else
			fprintf(stderr, "[args] bad SBSP_AUTOPLAY token '%s' - ignored\n", tok);
	}
	fprintf(stderr, "[args] autoplay: finish=%ld spatulas=%s lives=%ld continues=%ld die=%ld\n",
			g_finish, g_spatulasAll ? "all" : "-", g_lives, g_continues, g_die);
}

extern "C" int Port_AutoplayFinish(void)		{ parse(); return (int)g_finish; }
extern "C" int Port_AutoplaySpatulasAll(void)	{ parse(); return g_spatulasAll; }
/*	one-shot: -1 after the first read that returned a value  */
extern "C" int Port_AutoplayLives(void)
{
	parse();
	int v = (int)g_lives;
	g_lives = -1;
	return v;
}

extern "C" int Port_AutoplayContinues(void)
{
	parse();
	int v = (int)g_continues;
	g_continues = -1;
	return v;
}

/*	Called every playing frame with the player's dead flag; returns 1 on
	the frame a death should be fired.  One death per observed
	death->respawn cycle, so N means exactly N life-losses; the cycle
	survives a game-over -> continue -> initLevel because the count lives
	here, not in the scene.  */
extern "C" int Port_AutoplayDie(int playerIsDead)
{
	static int				state;			/* 0 idle, 1 fired, 2 seen dead */
	static unsigned long	notBefore;

	parse();
	if (g_die <= 0)
		return 0;

	unsigned long now = Port_VBlankCount();
	switch (state)
	{
	case 0:
		if (playerIsDead || now < notBefore)
			return 0;
		if (!notBefore)
		{
			notBefore = now + 60;		/* let the level settle first */
			return 0;
		}
		state = 1;
		g_die--;
		fprintf(stderr, "[autoplay] die: firing (%ld left, vblank %lu)\n", g_die, now);
		return 1;
	case 1:
		if (playerIsDead)
			state = 2;
		return 0;
	default:
		if (!playerIsDead)
		{
			state = 0;
			notBefore = now + 60;
		}
		return 0;
	}
}
