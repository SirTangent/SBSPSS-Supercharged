/*	host/window.cpp's pause bookkeeping, on the shim alone (review of #78).

	[summary] paused= is the time game time stood still, for a focus pause
	(M8 shell) or a window move/size loop (issue #63).  The two can overlap:
	a focus-paused window dragged, its focus coming back in the same poll
	that ends the drag.  Each used to book its own length, so the overlap
	was counted twice and paused= could exceed the wall time it covered.

	  1. a drag alone books about its own length
	  2. a drag inside a focus pause, both ending in one poll: the total
	     grows by no more than the wall time from the pause to the poll

	The drag is what Win32's move loop sends from inside SDL_PollEvent:
	SDL_EVENT_WINDOW_EXPOSED with data1 1, which window.cpp's event watch
	takes as the loop starting (SDL calls watches as the event is pushed);
	the next poll ends it.  An interactive run, not a scripted one - only
	that pauses on focus loss - so the pump is capped and nothing here
	sets the harness switches.  Headless: the dummy video driver.
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>

extern "C" void		Host_EnsureVideo(void);		/* host/window.cpp */
extern "C" int		Host_PausePoll(void);
extern "C" double	Host_PausedSeconds(void);
extern "C" int		Port_Paused(void);
extern "C" double	Port_NowSeconds(void);		/* host/pump.cpp */
extern "C" int		VSync(int mode);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static void setEnv(const char *name, const char *value)
{
	char buf[256];
	std::snprintf(buf, sizeof(buf), "%s=%s", name, value);
	_putenv(buf);
}

static void push(SDL_EventType type, int data1)
{
	SDL_Event ev = {};
	ev.type         = type;
	ev.window.data1 = data1;
	check(SDL_PushEvent(&ev), "SDL_PushEvent");
}

int main()
{
	/*	an interactive run: none of the scripted-run switches
		(host/crash.cpp harnessRun), the focus pause left on, no audio
		device  */
	static const char *const clear[] =
	{
		"SBSP_UNCAPPED", "SBSP_PAD_FILE", "SBSP_EXIT_AFTER", "SBSP_PAD_SCRIPT",
		"SBSP_PAUSE_ON_FOCUS_LOSS", "SBSP_DUMP_AUDIO", "SBSP_FRAME_CRC",
		"SBSP_DUMP_FRAMES", "SBSP_WINDOW", "SBSP_SELFTEST",
	};
	for (const char *v : clear)
		setEnv(v, "");
	setEnv("SBSP_NO_AUDIO", "1");
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	Host_EnsureVideo();
	VSync(0);								/* settle: the window's own first events */
	char what[192];

	/*	1. a drag alone  */
	{
		const double before = Host_PausedSeconds();
		push(SDL_EVENT_WINDOW_EXPOSED, 1);	/* the move loop starts */
		Sleep(200);
		VSync(0);							/* this vblank's poll ends it */
		const double got = Host_PausedSeconds() - before;
		std::snprintf(what, sizeof(what), "a 200 ms drag books about 200 ms (%.3f s)", got);
		check(got >= 0.15 && got < 1.0, what);
	}

	/*	2. a drag inside a focus pause, ended in the same poll as the pause  */
	{
		const double before = Host_PausedSeconds();
		const double t0 = Port_NowSeconds();
		push(SDL_EVENT_WINDOW_FOCUS_LOST, 0);
		VSync(0);							/* the poll pauses */
		check(Port_Paused() == 1, "focus lost: paused");
		Sleep(100);
		push(SDL_EVENT_WINDOW_EXPOSED, 1);	/* dragged while paused */
		Sleep(300);
		push(SDL_EVENT_WINDOW_FOCUS_GAINED, 0);
		Host_PausePoll();					/* one poll: focus back, then the loop's end */
		const double wall = Port_NowSeconds() - t0;
		check(Port_Paused() == 0, "focus gained: resumed");
		const double got = Host_PausedSeconds() - before;
		std::snprintf(what, sizeof(what),
					  "pause and drag overlapping are counted once (%.3f s booked in %.3f s of wall time)",
					  got, wall);
		check(got >= 0.35 && got <= wall + 0.001, what);
	}

	SDL_Quit();
	if (g_failures)
	{
		std::printf("window_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("window_test: all passed\n");
	return 0;
}
