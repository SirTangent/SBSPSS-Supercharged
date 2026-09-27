/*	CdRead pacing in emulated vblanks (issue #67), on the shim alone.

	A paced CdRead copies its data at once, then reports "still reading"
	until a double-speed drive would have delivered it: 150 sectors/s
	against the vblank rate, counted by Port_CdDataVblank once per emulated
	vblank and never by the wall clock.  A load therefore costs the same
	vblanks on a capped run, an uncapped one and a replay, which is what
	lets a capped tester session replay uncapped frame for frame.

	The clock banks at most one vblank while the drive is idle (150 units:
	2 sectors NTSC, 3 PAL), so after a VSync(0) every cost is exact:

	    sectors    1   2   3   4   30   150
	    60 Hz      0   0   1   1   11    59
	    50 Hz      0   0   0   1         49

	Also: two reads issued back to back before any CdReadSync cost what one
	read of their sum does; a long idle stretch banks no more than one
	vblank; sleeping on the wall clock in the middle of a read does not
	shorten it (the old deadline was wall-clock); the bytes are the right
	sectors'.  Uncapped (SBSP_UNCAPPED=1) each Port_PumpIdle in the wait is
	exactly one vblank, so a read's cost is Port_VBlankCount's advance.
	Headless: the dummy video driver.
*/
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <direct.h>

#include <sys/types.h>
#include <libcd.h>

#include "host/pump.h"		/* Port_VBlankCount, Port_SetVBlankHz */
#include "cd/xa_stream.h"	/* Port_CdPaced */

extern "C" void	Port_CdRebuildDirForTest(void);
extern "C" int	VSync(int mode);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static const int		SECTOR  = 2048;
static const int		SECTORS = 400;		/* the synthetic BIGLUMP.BIN: sector s holds byte s & 0xFF */
static unsigned char	g_buf[SECTORS * SECTOR];

static void setloc(int lba)
{
	CdlLOC loc;
	CdIntToPos(lba, &loc);
	CdControlB(CdlSetloc, (u_char *)&loc, 0);
}

static bool holds(const unsigned char *buf, int lba, int n)
{
	for (int s = 0; s < n; s++)
	{
		const unsigned char want = (unsigned char)((lba + s) & 0xFF);
		if (buf[s * SECTOR] != want || buf[s * SECTOR + SECTOR - 1] != want)
			return false;
	}
	return true;
}

/*	vblanks until CdReadSync stops reporting "still reading"  */
static int waitRead(void)
{
	const unsigned long v0 = Port_VBlankCount();
	while (CdReadSync(1, 0) > 0)
		;
	return (int)(Port_VBlankCount() - v0);
}

/*	n sectors from LBA 10 after `idle` vblanks with the drive at rest,
	sleeping `sleepMs` of wall-clock time between CdRead and the wait  */
static void expect(int hz, int n, int want, int idle = 1, DWORD sleepMs = 0)
{
	for (int i = 0; i < idle; i++)
		VSync(0);
	const int lba = 10;
	std::memset(g_buf, 0xEE, (size_t)n * SECTOR);
	setloc(lba);
	CdRead(n, (u_long *)g_buf, CdlModeSpeed);
	char what[160];
	std::snprintf(what, sizeof(what), "%d Hz, %d sector(s): the data is copied at once", hz, n);
	check(holds(g_buf, lba, n), what);
	if (sleepMs)
		Sleep(sleepMs);
	const int got = waitRead();
	std::snprintf(what, sizeof(what), "%d Hz, %d sector(s)%s%s: %d vblank(s), want %d", hz, n,
				  idle > 1 ? " after a long idle" : "", sleepMs ? " with a wall-clock sleep" : "",
				  got, want);
	check(got == want, what);
}

int main(void)
{
	SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
	{
		std::printf("FAIL: SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	/*	before the first pump: Port_Uncapped and Port_CdPaced cache.  Nothing
		ambient may shape the run - an inherited SBSP_EXIT_AFTER would even
		end it.  */
	static const char *const clear[] =
	{
		"SBSP_CD_PACE=", "SBSP_FRAME_CRC=", "SBSP_EXIT_AFTER=", "SBSP_SELFTEST=",
		"SBSP_DUMP_FRAMES=", "SBSP_DUMP_AUDIO=", "SBSP_PACE_LOG=", "SBSP_PAD_FILE=",
		"SBSP_PAD_SCRIPT=", "SBSP_RECORD_PAD=",
	};
	for (const char *c : clear)
		_putenv(c);
	_putenv("SBSP_UNCAPPED=1");
	check(Port_CdPaced() == 1, "SBSP_CD_PACE unset: CdRead is paced");

	_mkdir("cdpace_test_tmp");
	FILE *f = std::fopen("cdpace_test_tmp/BIGLUMP.BIN", "wb");
	check(f != NULL, "synthetic disc created");
	if (!f)
		return 1;
	static unsigned char sec[SECTOR];
	for (int s = 0; s < SECTORS; s++)
	{
		std::memset(sec, s & 0xFF, sizeof(sec));
		std::fwrite(sec, 1, sizeof(sec), f);
	}
	std::fclose(f);
	_putenv("SBSP_DATA_DIR=cdpace_test_tmp");
	Port_CdRebuildDirForTest();

	/*	60 Hz, the rate the game boots at  */
	expect(60, 1, 0);
	expect(60, 2, 0);
	expect(60, 3, 1);
	expect(60, 4, 1);
	expect(60, 30, 11);
	expect(60, 150, 59);
	expect(60, 150, 59, 30);				/* idle for 30 vblanks: still one vblank banked */
	expect(60, 30, 11, 1, 250);			/* the old wall deadline (0.2 s) passes during the sleep */

	/*	two reads back to back, as a file's chunk and its tail are issued:
		the second queues behind the first  */
	{
		VSync(0);
		setloc(0);
		CdRead(60, (u_long *)g_buf, CdlModeSpeed);
		CdRead(90, (u_long *)(g_buf + 60 * SECTOR), CdlModeSpeed);
		check(holds(g_buf, 0, 150), "chained reads: both copied, in order");
		const int got = waitRead();
		char what[96];
		std::snprintf(what, sizeof(what), "60 + 90 sectors back to back: %d vblank(s), want 59", got);
		check(got == 59, what);
	}

	/*	50 Hz (PAL, the EUR build): exactly 3 sectors per vblank  */
	Port_SetVBlankHz(50);
	expect(50, 1, 0);
	expect(50, 2, 0);
	expect(50, 3, 0);
	expect(50, 4, 1);
	expect(50, 150, 49);

	/*	let go of the disc before deleting it  */
	_putenv("SBSP_DATA_DIR=cdpace_test_tmp/gone");
	Port_CdRebuildDirForTest();
	std::remove("cdpace_test_tmp/BIGLUMP.BIN");
	_rmdir("cdpace_test_tmp");
	SDL_Quit();

	if (g_failures)
	{
		std::printf("cdpace_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("cdpace_test: all passed\n");
	return 0;
}
