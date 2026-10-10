/*	SDL3 host: window, event pump, and the M2 verification tooling.

	The game owns main() (source/system/main.cpp), so SDL's entry-point
	machinery is bypassed: SDL_MAIN_HANDLED here, SDL_SetMainReady() before
	SDL_Init, and video comes up lazily at the game's first ResetGraph()
	(from VidInit) - the headless/trig test exes link this TU but never
	create a window.

	Tooling (all optional, env-driven; they work even if Vulkan fails,
	because they read emulated VRAM directly):
	  SBSP_DUMP_FRAMES=n[,n...]  write the displayed VRAM region as BMP at
	                             those vblank numbers (sbsp_frame_<n>.bmp;
	                             black while SetDispMask(0) is in force, as
	                             on screen - host/framedump.cpp)
	  SBSP_DUMP_DIR=<dir>        where to write them (default .)
	  SBSP_EXIT_AFTER=<n>        clean exit(0) at vblank n (the game's
	                             MainLoop has no exit path of its own)
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>	/* SDL_SetMainReady; harmless with SDL_MAIN_HANDLED */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpu/gpu_core.h"
#include "host/diag.h"
#include "host/pump.h"

bool VkPresent_Init(SDL_Window *window);	/* port/psyq/vk/vk_present.cpp */
void VkPresent_Frame(void);

extern "C" void Port_InputHandleEvent(const void *ev);	/* host/input.cpp */
extern "C" void Port_InputFrame(unsigned long vblank);
extern "C" void Host_AudioPause(int on);				/* host/audio_out.cpp */
extern "C" int  Port_HarnessRun(void);					/* host/crash.cpp */

static SDL_Window	*g_window;
static int			g_videoUp;
static int			g_vkUp;

/* dump/exit config, parsed once */
static int			g_toolingParsed;
static unsigned long g_exitAfter;		/* 0 = off */
static unsigned long g_dumpAt[16];
static int			g_dumpCount;
static const char	*g_dumpDir = ".";
static int			g_frameCrc;			/* SBSP_FRAME_CRC=1: [frame] line per vblank */

/*	Pause on focus loss (M8 shell).  SBSP_PAUSE_ON_FOCUS_LOSS (sbsp.ini
	`pause_on_focus_loss`, default on) - and never for a harness run, whose
	window nobody is looking at and whose vblank budget is the oracle.  */
static int			g_pauseOnFocusLoss;
static volatile int	g_paused;			/* read unlocked by the watchdog thread */
static double		g_pauseStart;
static double		g_pausedSeconds;

/*	A title-bar drag, a border resize or the Alt+Space system menu runs
	Win32's modal loop inside SDL_PollEvent (crash.cpp notes it too): the
	pump delivers no vblank for as long as the user holds it, XM_Update and
	the XA feed stop, and the audio thread drones on the frozen voice state
	- the very thing the focus-loss pause prevents - while speech drains
	and cuts out.  SDL sends SDL_EVENT_WINDOW_EXPOSED with data1 == 1 from
	inside that loop (its live-resize timer), reachable only through an
	event watch: the watch pauses the audio and keeps the picture up, and
	the first poll after the loop resumes the audio, books the time as
	paused and has the pump rebase its clock like a focus pause, so the
	vblanks the loop ate are not burst afterwards (issue #63).  */
static int			g_modal;			/* inside a move/size/menu loop */
static double		g_modalStart;
static int			g_modalRebase;		/* one "paused" Host_PausePoll answer: rebase the clock */

/*	[summary] paused= is the time game time stood still for either reason,
	a focus pause or a move/size loop, counted once where the two overlap
	(a focus-paused window dragged, the focus coming back in the same poll
	that ends the drag): an interval opens when the first of them starts
	and is booked when neither holds any more.  Each still logs its own
	length.  */
static int			g_holding;
static double		g_holdStart;

static void holdUpdate(void)
{
	const int hold = g_paused || g_modal;
	if (hold && !g_holding)
		g_holdStart = Port_NowSeconds();
	else if (!hold && g_holding)
		g_pausedSeconds += Port_NowSeconds() - g_holdStart;
	g_holding = hold;
}

static bool SDLCALL liveResizeWatch(void *userdata, SDL_Event *ev)
{
	(void)userdata;
	if (ev->type != SDL_EVENT_WINDOW_EXPOSED || ev->window.data1 != 1)
		return true;
	double now = Port_NowSeconds();
	if (!g_modal)
	{
		g_modal = 1;
		g_modalStart = now;
		holdUpdate();
		if (!g_paused)
			Host_AudioPause(1);
		fprintf(stderr, "[host] paused (window move/size)\n");
	}
	if (g_vkUp)
	{
		static double lastPresent = -1.0;
		if (now - lastPresent >= 0.1)
		{
			lastPresent = now;
			VkPresent_Frame();		/* the window keeps its picture while held */
		}
	}
	return true;
}

/*	after a poll: a move/size loop that ran inside it has ended  */
static void modalEnd(void)
{
	if (!g_modal)
		return;
	g_modal = 0;
	double d = Port_NowSeconds() - g_modalStart;
	holdUpdate();
	if (!g_paused)
		Host_AudioPause(0);
	/*	The rebase answer costs the pump step it lands on: that step returns
		before its bare-pump replay check (pump.cpp), so a scripted run - a
		replay a person may well be watching and dragging - would miss a
		recorded `# bare` vblank due there and fail on a window move.  A
		scripted run fires its vblanks where the recording says, not by the
		wall clock, so it needs no rebase; it is left out exactly as from the
		focus-loss pause (Port_HarnessRun).  */
	if (!Port_HarnessRun())
		g_modalRebase = 1;
	fprintf(stderr, "[host] resumed after %.1fs (window move/size)\n", d);
}

static void parseTooling(void)
{
	if (g_toolingParsed)
		return;
	g_toolingParsed = 1;

	const char *p = getenv("SBSP_PAUSE_ON_FOCUS_LOSS");
	g_pauseOnFocusLoss = !(p && *p == '0') && !Port_HarnessRun();

	const char *e = getenv("SBSP_EXIT_AFTER");
	if (e)
		g_exitAfter = strtoul(e, NULL, 10);

	const char *dir = getenv("SBSP_DUMP_DIR");
	if (dir && *dir)
		g_dumpDir = _strdup(dir);

	const char *fc = getenv("SBSP_FRAME_CRC");
	g_frameCrc = fc && *fc && *fc != '0';

	const char *d = getenv("SBSP_DUMP_FRAMES");
	while (d && *d && g_dumpCount < 16)
	{
		g_dumpAt[g_dumpCount++] = strtoul(d, (char **)&d, 10);
		while (*d == ',' || *d == ' ')
			d++;
	}
}

/*****************************************************************************/
/*	host/framedump.cpp: the BMP writer (mask-aware, raw or 4:3).  */
extern "C" int Host_WriteDisplayBMP(const char *path, int aspect);

static void dumpDisplayBMP(unsigned long vblank)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/sbsp_frame_%lu.bmp", g_dumpDir, vblank);
	Host_WriteDisplayBMP(path, 0);
}

/*****************************************************************************/
extern "C" void Host_EnsureVideo(void)
{
	if (g_videoUp)
		return;
	g_videoUp = 1;
	parseTooling();

	SDL_SetMainReady();

	/*	audio joins the lazy bring-up here, ahead of the video guards below:
		it needs no window and no video subsystem, and --dump-audio has to
		work on a host with no display at all.  (Not from SpuInit: the unit
		tests drive SpuInit directly and must never open a device.)  */
	extern void Host_EnsureAudio(void);
	Host_EnsureAudio();

	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD))
	{
		fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
		return;
	}

	/*	SBSP_WINDOW (sbsp.ini `window`, --window; M8 shell): "WxH" or
		"fullscreen" (borderless, the desktop mode).  1024x768 shows the
		4:3 picture at exactly 3 lines per PS1 line.  */
	int winW = 1024, winH = 768;
	SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
	const char *win = getenv("SBSP_WINDOW");
	if (win && *win)
	{
		int w = 0, h = 0;
		char tail = 0;
		if (_stricmp(win, "fullscreen") == 0)
			flags |= SDL_WINDOW_FULLSCREEN;
		else if (sscanf(win, "%dx%d%c", &w, &h, &tail) == 2 && w >= 64 && h >= 64 && w <= 16384 && h <= 16384)
		{
			winW = w;
			winH = h;
		}
		else
			fprintf(stderr, "[host] bad window '%s' - want WxH or fullscreen - using %dx%d\n", win, winW, winH);
	}
	g_window = SDL_CreateWindow("SpongeBob SquarePants: SuperSponge",
								winW, winH, flags);
	int vulkanWindow = g_window != NULL;
	if (!g_window)
	{
		/*	SDL_WINDOW_VULKAN loads the Vulkan loader as part of creating the
			window, so on a machine without one (a VM, a basic display
			adapter) there was no window at all: no focus, no close button,
			the music playing until the console was killed.  The black
			window this file promises needs a window - make one without the
			flag (issue #63).  */
		fprintf(stderr, "[host] SDL_CreateWindow with Vulkan failed: %s - retrying without\n",
				SDL_GetError());
		g_window = SDL_CreateWindow("SpongeBob SquarePants: SuperSponge",
									winW, winH, flags & ~(SDL_WindowFlags)SDL_WINDOW_VULKAN);
		if (!g_window)
		{
			fprintf(stderr, "[host] SDL_CreateWindow failed: %s\n", SDL_GetError());
			return;
		}
	}
	SDL_AddEventWatch(liveResizeWatch, NULL);

	g_vkUp = vulkanWindow ? VkPresent_Init(g_window) : 0;
	if (!g_vkUp)
		fprintf(stderr, "[host] Vulkan presenter unavailable - window will stay "
						"black (frame dumps still work)\n");
}

/*****************************************************************************/
/*	Pause / fullscreen (M8 shell)  */

static void setPaused(int on)
{
	if (on == g_paused)
		return;
	g_paused = on;
	holdUpdate();
	if (on)
	{
		g_pauseStart = Port_NowSeconds();
		Host_AudioPause(1);
		fprintf(stderr, "[host] paused (focus lost)\n");
	}
	else
	{
		double d = Port_NowSeconds() - g_pauseStart;
		Host_AudioPause(0);
		fprintf(stderr, "[host] resumed after %.1fs\n", d);
	}
}

extern "C" int Port_Paused(void)
{
	return g_paused;
}

extern "C" double Host_PausedSeconds(void)
{
	return g_pausedSeconds;
}

static void toggleFullscreen(void)
{
	bool fs = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
	if (!SDL_SetWindowFullscreen(g_window, !fs))	/* borderless: no exclusive mode is ever set */
	{
		fprintf(stderr, "[host] fullscreen toggle failed: %s\n", SDL_GetError());
		return;
	}
	fprintf(stderr, "[host] fullscreen %s\n", fs ? "off" : "on");
}

/*	One handler for both event loops (the per-vblank poll and the paused
	wait), so quit / hotplug / the shortcuts cannot drift apart.  */
static void handleHostEvent(const SDL_Event *ev)
{
	switch (ev->type)
	{
	case SDL_EVENT_QUIT:
		fprintf(stderr, "[host] window closed - exiting\n");
		/*	a replay closed early is judged on the epochs it reached  */
		Port_Exit(Port_InputAtExit(0) ? PORT_EXIT_ORACLE : PORT_EXIT_CLEAN);
	case SDL_EVENT_GAMEPAD_ADDED:
	case SDL_EVENT_GAMEPAD_REMOVED:
		Port_InputHandleEvent(ev);
		break;
	case SDL_EVENT_KEY_DOWN:
		if (ev->key.key == SDLK_RETURN && (ev->key.mod & SDL_KMOD_ALT) && !ev->key.repeat)
			toggleFullscreen();
		break;
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		if (g_pauseOnFocusLoss)
			setPaused(1);
		break;
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		setPaused(0);
		break;
	default:
		break;
	}
}

/*	pump.cpp, at the top of every pump step (pumpStep, wait or bare, not
	nested): while paused, wait for events
	(100ms at a time, so the CPU is idle) and keep the window painted; do
	NOTHING that is keyed by the vblank number - no input frame, no memory
	watch, no dump, no frame CRC, no exit-after - those belong to a vblank
	and none is passing.  */
extern "C" int Host_PausePoll(void)
{
	/*	the pump step after a move/size loop: answer "paused" once, with no
		vblank, so the pump's resume edge rebases the wall clock onto the
		counter (pump.cpp) instead of bursting the vblanks the loop ate  */
	if (g_modalRebase)
	{
		g_modalRebase = 0;
		return 1;
	}
	if (!g_paused)
		return 0;
	SDL_Event ev;
	if (SDL_WaitEventTimeout(&ev, 100))
	{
		handleHostEvent(&ev);
		while (SDL_PollEvent(&ev))
			handleHostEvent(&ev);
		modalEnd();
	}
	if (g_paused && g_vkUp)
	{
		static double lastPresent = -1.0;
		double now = Port_NowSeconds();
		if (now - lastPresent >= 0.1)
		{
			lastPresent = now;
			VkPresent_Frame();
		}
	}
	return g_paused;
}

/*****************************************************************************/
extern "C" void Host_VBlank(unsigned long vblankNo)
{
	static int inHere;
	if (inHere)
		return;		/* the game's vblank callback can pump recursively */
	inHere = 1;

	parseTooling();

	if (g_videoUp && g_window)
	{
		SDL_Event ev;
		while (SDL_PollEvent(&ev))
			handleHostEvent(&ev);
		modalEnd();		/* a move/size loop that ran inside the poll has ended */
	}

	/*	Outside the video gate on purpose.  SBSP_PAD_SCRIPT needs no SDL at
		all, and the degraded no-window mode this file promises (frame dumps
		+ SBSP_EXIT_AFTER on a display-less machine or CI) is exactly where
		scripted input is the ONLY way to navigate - gating it here left
		such runs dumping the title screen forever, without even the
		"[input] SBSP_PAD_SCRIPT: N entries" line to say why.  The keyboard
		and gamepad readers already handle their absence.  */
	Port_InputFrame(vblankNo);	/* rebuild the PS1 pad packet */
	Port_MemWatch();

	for (int i = 0; i < g_dumpCount; i++)
	{
		if (g_dumpAt[i] == vblankNo)
			dumpDisplayBMP(vblankNo);
	}

	if (g_frameCrc)
	{
		int masked;
		uint32_t crc = GPU_DisplayCRC32(&masked);
		fprintf(stderr, "[frame] %lu crc=%08X%s\n", vblankNo, crc, masked ? " masked" : "");
	}

	/*	The game's stdout is block-buffered when redirected to a file or a
		pipe and Port_Exit cannot safely flush it on a fault (diag.cpp); one
		flush per vblank - a lock and a test when nothing is pending - keeps
		a crash from swallowing more than the current frame's lines.  */
	fflush(stdout);

	/*	Uncapped runs would otherwise be re-capped by the presenter's vsync
		wait: present at most once per vblank period of wall time (60/s NTSC,
		50/s PAL) and let the emulated vblanks run ahead.  */
	if (g_vkUp)
	{
		static double lastPresent = -1.0;
		double now = Port_NowSeconds();
		if (!Port_Uncapped() || now - lastPresent >= 1.0 / Port_VBlankHz())
		{
			lastPresent = now;
			VkPresent_Frame();
		}
	}

	if (g_exitAfter && vblankNo >= g_exitAfter)
	{
		fprintf(stderr, "[host] SBSP_EXIT_AFTER=%lu reached - exiting\n", g_exitAfter);
		Port_Exit(Port_InputAtExit(1) ? PORT_EXIT_ORACLE : PORT_EXIT_CLEAN);
	}

	inHere = 0;
}
