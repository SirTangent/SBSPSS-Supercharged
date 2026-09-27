/*	libcd over the host filesystem: the M1 synchronous subset.

	Model: a virtual disc directory of the files filetab.cpp knows about
	(BIGLUMP.BIN, TRACK1.IXA, *.STR), each with a fabricated LBA range;
	BIGLUMP.BIN sits at LBA 0 so that with FileStart==0 BigLump sector N maps
	to byte N*2048 of the host file - exactly what CCDFileIO expects.

	This shim also plays PsxBoot's role: on the retail (__USER_CDBUILD__)
	build, filetab.cpp is compiled out and the game reads its file-position
	table from the scratchpad (CFileIO::GetAllFilePos), which the boot
	program pre-filled on real hardware.  Here a static initialiser fills
	PORT_Scratchpad with the virtual LBAs before main() runs.

	Reads copy their data at once; the emulated drive then takes its time.
	CdReadSync reports "still reading" until a double-speed drive (150
	sectors/s) would have delivered the sectors, counted in EMULATED vblanks
	by Port_CdDataVblank and never by the wall clock, so a load costs the
	same vblanks on every run, capped or uncapped, and the vblank-time work
	(loading icon, XM_Update) runs "during" it exactly as often (issue #67).
*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <libcd.h>

#include "stub_log.h"
#include "host/pump.h"
#include "host/diag.h"
#include "cd/xa_stream.h"
#include "cd/str_stream.h"
#include "spu/spu_core.h"

#include "system/types.h"
#include "system/asmport.h"		/* PORT_Scratchpad */
#include "system/info.h"		/* INF_Version/Territory/FileSystem */

/*	Virtual disc directory - names and order must match FilenameList in
	source/fileio/filetab.cpp (FILEPOS_* enum order).  DEMO.STR is EUR-only
	but harmlessly present here.
*/
struct VirtFile
{
	const char	*name;
	long		startLBA;
	long		sizeBytes;		/* 0 if the host file is absent */
	FILE		*fp;
	int			bytesPerSector;	/* 2048 data; 2336 for the raw-XA TRACK1.IXA
								   (8-byte subheader + 2328 data per sector,
								   no sync/header - see xa_stream.cpp) */
};

static VirtFile g_files[] =
{
	{ "BIGLUMP.BIN", 0, 0, NULL, 2048 },
	{ "TRACK1.IXA",  0, 0, NULL, 2336 },
	{ "THQ.STR",     0, 0, NULL, 2336 },	/* .STRs are raw-XA sectors too */
	{ "CLIMAX.STR",  0, 0, NULL, 2336 },	/* (measured: every one an exact */
	{ "INTRO.STR",   0, 0, NULL, 2336 },	/*  multiple of 2336, subheaders */
	{ "DEMO.STR",    0, 0, NULL, 2336 },	/*  at each boundary - M7) */
};
static const int	g_fileCount = sizeof(g_files) / sizeof(g_files[0]);
static const int	SECTOR = 2048;

static int		g_inited;
static long		g_curLBA;
static int		g_pace = -1;	/* -1 unparsed; SBSP_CD_PACE=0 disables */
/*	CD pacing (issue #67): the emulated drive's sector clock, in the units
	xa_stream.cpp uses - +150 per vblank, one sector per `hz` - so 2.5
	sectors per NTSC vblank and exactly 3 per PAL one.  g_pending is what the
	drive still owes the reads issued so far, g_acc the fractional clock.  */
static long		g_pending;
static long		g_acc;

static int paceOn(void)
{
	if (g_pace < 0)
	{
		const char *e = getenv("SBSP_CD_PACE");
		g_pace = !(e && *e == '0');
	}
	return g_pace;
}

/*	for --record-pad's `# loads` header line (host/input.cpp)  */
extern "C" int Port_CdPaced(void)
{
	return paceOn();
}

/*	Once per emulated vblank, from Port_CdVblank (xa_stream.cpp): inside
	Port_Pump's single-fire block, after the game's own vblank work.  An idle
	drive banks at most one vblank of clock (150 units: 2 sectors NTSC, 3
	PAL), so a small read issued after an idle vblank completes inside the
	frame, as it would on the PlayStation, while a big one waits.  After one
	idle vblank the bank is exactly 150 however long the drive sat idle, so
	the cost of every read is a function of the vblank and read sequence
	alone - the same on a capped run, an uncapped one and a replay.  */
extern "C" void Port_CdDataVblank(int vblankHz)
{
	g_acc += 150;
	if (!g_pending)
	{
		if (g_acc > 150)
			g_acc = 150;
		return;
	}
	while (g_acc >= vblankHz && g_pending)
	{
		g_acc -= vblankHz;
		g_pending--;
	}
}

extern "C" int Port_ExeDir(char *dst, size_t n);		/* host/hostpath.cpp */
extern "C" int Port_FileExists(const char *path);

/*	Where the disc files are (M8 shell, issue #35).  The candidates, in
	order - the first directory holding BIGLUMP.BIN wins:
	  1. SBSP_DATA_DIR / --data-dir, taken VERBATIM and never fallen
	     through: xa_test points it at a directory that does not exist
	     precisely to release the real TRACK1.IXA handle;
	  2. data\ beside the exe (the tester-zip layout);
	  3. out/<T>/cd - what port/build-data.cmd stages, once per territory
	     (the DEBUG and FINAL data are byte-identical, so the variant is
	     not part of the path);
	  4. out/<T>/<V>/version/CD - the PSX build tree, kept as a fallback for
	     data built before #35.
	Resolved on every cdBuildDir() call (Port_CdRebuildDirForTest re-scans
	after a _putenv), so nothing is cached.  `slot` walks the candidates
	one at a time for the CdInit failure report.  */
static int dataCandidate(int slot, char *dst, size_t n)
{
	char exe[512];
	switch (slot)
	{
	case 0:
	{
		const char *root = getenv("SBSP_DATA_DIR");
		if (!root)
			return 0;
		snprintf(dst, n, "%s", root);
		return 1;
	}
	case 1:
		if (!Port_ExeDir(exe, sizeof(exe)))
			return 0;
		snprintf(dst, n, "%s\\data", exe);
		return 1;
	case 2:
		snprintf(dst, n, "out/%s/cd", INF_Territory);
		return 1;
	case 3:
		snprintf(dst, n, "out/%s/%s/version/%s",
				 INF_Territory, INF_Version, INF_FileSystem);
		return 1;
	}
	return -1;		/* no more candidates */
}

static char g_dataRoot[512];

static void resolveDataRoot(void)
{
	char probe[600];
	for (int slot = 0; ; slot++)
	{
		int r = dataCandidate(slot, g_dataRoot, sizeof(g_dataRoot));
		if (r < 0)
			break;
		if (!r)
			continue;
		if (slot == 0)
			return;						/* explicit: verbatim, no fallback */
		snprintf(probe, sizeof(probe), "%s/%s", g_dataRoot, g_files[0].name);
		if (Port_FileExists(probe))
			return;
	}
	/*	nothing found: g_dataRoot holds the last candidate, and CdInit
		reports the whole list  */
}

static void dataPath(char *dst, size_t dstSize, const char *name)
{
	snprintf(dst, dstSize, "%s/%s", g_dataRoot, name);
}

static void cdBuildDir(void)
{
	long lba = 0;
	resolveDataRoot();
	for (int i = 0; i < g_fileCount; i++)
	{
		char path[512];
		dataPath(path, sizeof(path), g_files[i].name);
		FILE *f = fopen(path, "rb");
		long size = 0;
		if (f)
		{
			fseek(f, 0, SEEK_END);
			size = ftell(f);
		}
		g_files[i].fp        = f;
		g_files[i].sizeBytes = size;
		g_files[i].startLBA  = lba;
		long bps     = g_files[i].bytesPerSector;
		if (f && bps == 2336 && size % bps)
			fprintf(stderr, "[shim] %s: %ld bytes is not a whole number of "
					"2336-byte sectors - stale or wrong file?\n",
					g_files[i].name, size);
		long sectors = (size + bps - 1) / bps;
		if (sectors < 16) sectors = 16;			/* keep ranges distinct */
		lba += (sectors + 15) & ~15;			/* 16-sector aligned */
	}

	/*	PsxBoot protocol: int FilePosList[FILEPOS_MAX] at the scratchpad,
		each entry CdPosToInt() of the file's start position (== start LBA).
		FILEPOS_MAX is 5 on USA, 6 on EUR; writing all 6 is harmless.  */
	int *pos = (int *)PORT_Scratchpad;
	for (int i = 0; i < g_fileCount; i++)
		pos[i] = (int)g_files[i].startLBA;
}

static VirtFile *fileForLBA(long lba)
{
	for (int i = 0; i < g_fileCount; i++)
	{
		long bps     = g_files[i].bytesPerSector;
		long sectors = (g_files[i].sizeBytes + bps - 1) / bps;
		if (lba >= g_files[i].startLBA && lba < g_files[i].startLBA + sectors)
			return &g_files[i];
	}
	return NULL;
}

namespace { struct CdBoot { CdBoot() { cdBuildDir(); g_inited = 1; } }; static CdBoot g_cdBoot; }

/*	Test support (xa_test): the CdBoot constructor resolves SBSP_DATA_DIR
	before main() runs, so a test that stages its own synthetic disc needs a
	way to re-scan after _putenv.  */
extern "C" void Port_CdRebuildDirForTest(void)
{
	for (int i = 0; i < g_fileCount; i++)
	{
		if (g_files[i].fp)
			fclose(g_files[i].fp);
		g_files[i].fp = NULL;
	}
	cdBuildDir();
	g_inited = 1;
}

/*****************************************************************************/
/*	BCD conversion: use the SDK's own itob/btoi macros from LIBCD.H  */

extern "C" int CdInit(void)
{
	if (!g_inited)
	{
		cdBuildDir();
		g_inited = 1;
	}

	/*	The game cannot run without its data lump; a wrong path must fail
		HERE, loudly, not as zero-filled reads parsed far from the cause.
		(The static CdBoot stays soft so data-less tools like gte_trig_test
		can still link the shim.)  */
	if (!g_files[0].fp)
	{
		char path[512];
		fprintf(stderr, "[shim] CdInit: no %s - looked in:\n", g_files[0].name);
		for (int slot = 0; ; slot++)
		{
			int r = dataCandidate(slot, path, sizeof(path));
			if (r < 0)
				break;
			if (r)
				fprintf(stderr, "       %s%s\n", path,
						slot == 0 ? "   (SBSP_DATA_DIR: taken as is)" : "");
			else if (slot == 0)
				fprintf(stderr, "       (SBSP_DATA_DIR / --data-dir not set)\n");
		}
		fprintf(stderr, "       run port/build-data.cmd %s, or point --data-dir at the directory holding it\n",
				INF_Territory);
		Port_Exit(PORT_EXIT_FAULT);
	}
	fprintf(stderr, "[cd] data: %s\n", g_dataRoot);
	return 1;
}

extern "C" CdlLOC *CdIntToPos(int i, CdlLOC *p)
{
	i += 150;								/* 2-second lead-in */
	p->sector = (u_char)itob(i % 75);
	i /= 75;
	p->second = (u_char)itob(i % 60);
	p->minute = (u_char)itob(i / 60);
	p->track  = 0;
	return p;
}

extern "C" int CdPosToInt(CdlLOC *p)
{
	return ((btoi(p->minute) * 60 + btoi(p->second)) * 75 + btoi(p->sector)) - 150;
}

extern "C" CdlFILE *CdSearchFile(CdlFILE *fp, char *name)
{
	/*	filetab.cpp searches "\NAME;1" - strip the path and version chars  */
	const char *base = name;
	while (*base == '\\' || *base == '/')
		base++;

	char clean[32];
	size_t n = 0;
	while (base[n] && base[n] != ';' && n < sizeof(clean) - 1)
	{
		clean[n] = base[n];
		n++;
	}
	clean[n] = 0;

	for (int i = 0; i < g_fileCount; i++)
	{
		if (_stricmp(clean, g_files[i].name) == 0)
		{
			CdIntToPos((int)g_files[i].startLBA, &fp->pos);
			fp->size = (u_long)g_files[i].sizeBytes;
			strncpy(fp->name, g_files[i].name, sizeof(fp->name) - 1);
			fp->name[sizeof(fp->name) - 1] = 0;
			return fp;
		}
	}
	fprintf(stderr, "[shim] CdSearchFile: unknown file '%s'\n", clean);
	return NULL;
}

extern "C" int CdControl(u_char com, u_char *param, u_char *result)
{
	return CdControlB(com, param, result);
}

extern "C" int CdControlB(u_char com, u_char *param, u_char *result)
{
	(void)result;
	switch (com)
	{
	case CdlSetloc:
		g_curLBA = CdPosToInt((CdlLOC *)param);
		return 1;
	case CdlSeekL:
		/*	fmv.cpp strKickCD: seek, then CdRead2 streams from here  */
		if (param)
		{
			g_curLBA = CdPosToInt((CdlLOC *)param);
			StrStream_Seek(g_curLBA);
		}
		return 1;
	case CdlSetmode:
		XaStream_SetMode(param ? param[0] : 0);
		return 1;
	case CdlPause:
		StrStream_Stop();		/* movie stream freezes... */
		XaStream_Pause();		/* ...and the XA stream stops + ring flush */
		return 1;
	case CdlNop:
	case CdlMute:
	case CdlDemute:
		return 1;
	case CdlSetfilter:
		if (param)				/* CdlFILTER: file, chan */
			XaStream_SetFilter(param[0], param[1]);
		return 1;
	case CdlReadS:
		/*	XA streaming read (M6): the engine walks TRACK1.IXA from this
			position at 150 sectors/s of emulated time - see xa_stream.cpp  */
		XaStream_ReadS((const CdlLOC *)param);
		return 1;
	default:
		PSYQ_STUB_ONCE();
		return 1;
	}
}

extern "C" int CdControlF(u_char com, u_char *param)
{
	return CdControlB(com, param, 0);
}

extern "C" int CdRead(int sectors, u_long *buf, int mode)
{
	(void)mode;
	unsigned char *dst = (unsigned char *)buf;
	int paced = 0;

	while (sectors > 0)
	{
		VirtFile *vf = fileForLBA(g_curLBA);
		if (vf && vf->bytesPerSector != SECTOR)
		{
			/*	CdRead is the 2048-byte data path; landing in the raw-XA
				range means a broken position, not a recoverable read  */
			fprintf(stderr, "[shim] CdRead: LBA %ld is inside %s (raw XA, "
							"%d-byte sectors) - data reads cannot go there\n",
					g_curLBA, vf->name, vf->bytesPerSector);
			Port_Exit(PORT_EXIT_FAULT);
		}
		if (!vf || !vf->fp)
		{
			/*	Success + zeros here would defeat the callers' entire error
				path (cdfile.cpp retries while(!Error) forever on 0, and
				parses whatever we hand it on 1) - a data-configuration
				problem is unrecoverable, so stop at the cause.  */
			fprintf(stderr, "[shim] CdRead: LBA %ld maps to no host file "
							"(missing data file or wrong SBSP_DATA_DIR)\n", g_curLBA);
			Port_Exit(PORT_EXIT_FAULT);
		}

		long offset = (g_curLBA - vf->startLBA) * SECTOR;
		fseek(vf->fp, offset, SEEK_SET);
		size_t got = fread(dst, 1, SECTOR, vf->fp);
		if (got < (size_t)SECTOR)
			memset(dst + got, 0, SECTOR - got);	/* zero-fill the EOF tail sector */

		dst += SECTOR;
		g_curLBA++;
		sectors--;
		paced++;
	}

	/*	CD pacing: the data is already in the buffer, but CdReadSync reports
		"still reading" until the emulated drive has delivered it (see
		Port_CdDataVblank).  This is what gives the loading icon its window -
		with instant reads, zero vblanks elapse between StartLoad and
		StopLoad and the game itself skips the icon.  Whatever the clock has
		banked is spent first.  SBSP_CD_PACE=0 (--no-cd-pace) turns pacing off
		for instant loads.  */
	if (paceOn())
	{
		const long hz = Port_VBlankHz();
		while (paced && g_acc >= hz)
		{
			g_acc -= hz;
			paced--;
		}
		g_pending += paced;
	}
	return 1;
}

extern "C" int CdReadSync(int mode, u_char *result)
{
	(void)result;
	if (!paceOn())
	{
		Port_Pump();		/* PS1 interrupt-time work happens during reads */
		return 0;
	}

	/*	Paced: look before waiting.  Pumping first, as this did while the
		deadline was wall-clock, could fire a vblank after the one that
		completed the read and before the game saw it complete; StopLoad's
		`while(LoadTime) VSync(0)` then waits for the icon to wrap, so a one-
		vblank slip could cost up to 59 more (issue #67).  Every vblank a paced
		read waits through comes from Port_PumpIdle below - one per call
		uncapped, one per wall-clock vblank capped - and either way the read
		completes on the same emulated vblank.  */
	if (mode == 0)
	{	/* blocking wait - PumpIdle, not Pump: a bare spin burns a whole core
		   for the duration of every load */
		while (g_pending)
			Port_PumpIdle();
		return 0;
	}
	if (!g_pending)
		return 0;
	/*	the live caller (cdfile.cpp:45) is `while (CdReadSync(1,0) > 0);` - a
		bare spin.  One wait step per call; its vblank may finish the read.  */
	Port_PumpIdle();
	return g_pending ? 1 : 0;
}

extern "C" int CdSync(int mode, u_char *result)
{
	(void)mode;
	(void)result;
	return CdlComplete;
}

extern "C" int CdGetSector(void *madr, int size)
{
	/*	Serves the sector the XA engine staged for the current CdlDataReady
		callback (Size1 layout: 4-byte header + subheader + user data), or
		zeros outside one - FMV's CdGetSector use is M7.  */
	XaStream_Serve((uint32_t *)madr, size);
	return 1;
}

extern "C" int CdMix(CdlATV *vol)
{
	if (vol)
		Spu_SetCdAtv(vol->val0, vol->val1, vol->val2, vol->val3);
	return 1;
}

extern "C" int CdSetDebug(int level)
{
	(void)level;
	return 0;
}

/*	The ready callback is fired by the XA engine's per-vblank sector clock
	(xa_stream.cpp Port_CdVblank) for every delivered data sector.  CFmvScene
	clears it at both ends of a movie (source/fmv/fmv.cpp:186,267); the
	engine holds the stream in place while it is NULL.  CdReadCallback stays
	registration-only (M7).  */
static CdlCB g_readCallback;	/* registration-only; a real CdlCB (psxboot.cpp) */
PortCdCB g_cdReadyCallback;		/* read by xa_stream.cpp */

/*	xa_stream.cpp binds the stream file lazily through this: TRACK1.IXA's
	host file and virtual-disc geometry (g_files[1]).  */
int Port_CdXaTrackInfo(FILE **fp, long *startLBA, long *sectors)
{
	VirtFile *vf = &g_files[1];
	if (!vf->fp)
		return 0;
	*fp       = vf->fp;
	*startLBA = vf->startLBA;
	*sectors  = vf->sizeBytes / vf->bytesPerSector;
	return 1;
}

/*	str_stream.cpp binds movie files by position through this (M7).  */
int Port_CdFileForLBA(long lba, FILE **fp, long *startLBA, long *sectors,
					  int *bytesPerSector, const char **name)
{
	VirtFile *vf = fileForLBA(lba);
	if (!vf || !vf->fp)
		return 0;
	*fp             = vf->fp;
	*startLBA       = vf->startLBA;
	*sectors        = vf->sizeBytes / vf->bytesPerSector;
	*bytesPerSector = vf->bytesPerSector;
	*name           = vf->name;
	return 1;
}

/*	fmv.cpp strKickCD: `while(CdRead2(CdlModeStream|CdlModeSpeed|CdlModeRT)
	== 0)` with no pump - must start streaming (str_stream.cpp) and report
	nonzero immediately.  */
extern "C" int CdRead2(long mode)
{
	return StrStream_Start(mode);
}

/*	CdReadyCallback is the seam that converts between libcd's CdlCB and the
	handler's real (int, u_char *) signature - the one place the cast
	belongs, so no call site can get it wrong on x64 (see PortCdCB in
	xa_stream.h).  CdReadCallback's only registrant, PsxBoot/psxboot.cpp's
	cdread_callback(u_char, u_char *), is a genuine CdlCB, so that one is
	stored as libcd types it and would be fired that way.  */
extern "C" CdlCB CdReadCallback(CdlCB func)
{
	CdlCB old = g_readCallback;
	g_readCallback = func;
	PSYQ_STUB_ONCE();	/* registered but not yet fired (M7) */
	return old;
}

extern "C" CdlCB CdReadyCallback(CdlCB func)
{
	CdlCB old = (CdlCB)g_cdReadyCallback;
	g_cdReadyCallback = (PortCdCB)func;
	return old;
}
