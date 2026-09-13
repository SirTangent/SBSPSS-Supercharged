/*	Unit tests for the XMPlayer data layer (port/psyq/xmplay/).
	Reads the real shipped assets from data/Music and data/Sfx (run from the
	repo root; skips with a notice if they are absent): parses the PXMs and
	cross-checks every pattern slot against the pristine FastTracker .xm
	files that ship beside them (the PXM repack must lose nothing), and
	verifies XM_VABInit uploads the VB byte-exactly at the addresses the
	VH size table dictates, with clean close/re-init allocation accounting.
*/
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <sys/types.h>
#include <libspu.h>
#include <XMPLAY.H>

#include "spu/spu_core.h"
#include "xmplay/xm_state.h"
#include "host/pump.h"

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static uint8_t *loadFile(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	*size = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = (uint8_t *)malloc((size_t)*size);
	if (fread(buf, 1, (size_t)*size, f) != (size_t)*size)
	{
		fclose(f);
		free(buf);
		return 0;
	}
	fclose(f);
	return buf;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* ---- pattern expansion (both packings share the FT2 slot encoding) ------ */

struct Slot
{
	uint8_t note, instr, vol, eff, param;
};

static const uint8_t *readSlot(const uint8_t *p, Slot *s)
{
	uint8_t b = *p++;
	if (b & 0x80)
	{
		if (b & 0x01) s->note = *p++;
		if (b & 0x02) s->instr = *p++;
		if (b & 0x04) s->vol = *p++;
		if (b & 0x08) s->eff = *p++;
		if (b & 0x10) s->param = *p++;
	}
	else
	{
		s->note = b;
		s->instr = *p++;
		s->vol = *p++;
		s->eff = *p++;
		s->param = *p++;
	}
	return p;
}

/* dense FT2 layout: one slot per channel per row */
static void expandDense(const uint8_t *data, int packedSize, int rows,
						int chans, Slot *out)
{
	memset(out, 0, sizeof(Slot) * rows * chans);
	if (packedSize == 0)
		return;
	const uint8_t *p = data;
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < chans; c++)
			p = readSlot(p, &out[r * chans + c]);
}

/* sparse PXM layout: [channel byte][slot]... 0xFF terminates each row */
static void expandSparse(const uint8_t *data, int packedSize, int rows,
						 int chans, Slot *out)
{
	memset(out, 0, sizeof(Slot) * rows * chans);
	if (packedSize == 0)
		return;
	const uint8_t *p = data;
	for (int r = 0; r < rows; r++)
	{
		while (*p != 0xFF)
		{
			int c = *p++;
			Slot dummy;
			Slot *dst = (c < chans) ? &out[r * chans + c] : &dummy;
			p = readSlot(p, dst);
		}
		p++;
	}
}

/*	cross-check every pattern of a parsed PXM module against the pristine
	FastTracker .xm sitting next to it in data/  */
static void crossCheckPatterns(const XmModule *m, const uint8_t *xm,
							   const char *name)
{
	check(rd16(xm + 0x3A) == XM_FT2_VERSION, "reference .xm has FT2 version");
	check(rd16(xm + 0x44) == m->numChannels, "channel counts agree");
	check(rd16(xm + 0x46) == m->numPatterns, "pattern counts agree");
	check(rd16(xm + 0x40) == m->songLength, "song lengths agree");
	check(memcmp(xm + 0x50, m->orderTable, (size_t)m->songLength) == 0,
		  "pattern order tables agree");

	static Slot a[256 * 32], b[256 * 32];
	const uint8_t *p = xm + 0x3C + rd32(xm + 0x3C);
	int bad = 0;
	for (int i = 0; i < m->numPatterns; i++)
	{
		uint32_t phLen = rd32(p);
		int rows = rd16(p + 5);
		int packed = rd16(p + 7);
		if (rows != m->pat[i].rows)
		{
			std::printf("FAIL: %s pattern %d row count %d vs %d\n",
						name, i, rows, m->pat[i].rows);
			g_failures++;
			return;
		}
		expandDense(p + phLen, packed, rows, m->numChannels, a);
		expandSparse(m->pat[i].data, m->pat[i].packedSize, rows,
					 m->numChannels, b);
		if (memcmp(a, b, sizeof(Slot) * rows * m->numChannels) != 0)
			bad++;
		p += phLen + packed;
	}
	if (bad)
	{
		std::printf("FAIL: %s - %d pattern(s) differ between PXM and .xm\n",
					name, bad);
		g_failures++;
	}
}

int main()
{
	long pxmSize, xmSize, vhSize, vbSize;
	uint8_t *pxm = loadFile("data/Music/sb-title/sb-title.PXM", &pxmSize);
	uint8_t *xm = loadFile("data/Music/sb-title/sb-title.xm", &xmSize);
	uint8_t *vh = loadFile("data/Music/sb-title/sb-title.VH", &vhSize);
	uint8_t *vb = loadFile("data/Music/sb-title/sb-title.VB", &vbSize);
	uint8_t *pxm1 = loadFile("data/Music/chapter1/chapter1.PXM", &pxmSize);
	uint8_t *xm1 = loadFile("data/Music/chapter1/CHAPTER1.XM", &xmSize);
	uint8_t *pxmSfx = loadFile("data/Sfx/ingame/ingame.PXM", &pxmSize);
	uint8_t *xmSfx = loadFile("data/Sfx/ingame/ingame.xm", &xmSize);
	long vhSfxSize, vbSfxSize;
	uint8_t *vhSfx = loadFile("data/Sfx/ingame/ingame.VH", &vhSfxSize);
	uint8_t *vbSfx = loadFile("data/Sfx/ingame/ingame.VB", &vbSfxSize);
	if (!pxm || !xm || !vh || !vb || !pxm1 || !xm1 || !pxmSfx || !xmSfx ||
		!vhSfx || !vbSfx)
	{
		std::printf("xm test SKIPPED (data/Music + data/Sfx assets not found"
					" - run from the repo root)\n");
		return 0;
	}

	/* --- tick clock vs vblank clock (M8 EUR) ------------------------------- */
	/*	The one "[xm] WARNING" line this prints is EXPECTED: unit-test output
		is not tag-scanned (only the playthrough tiers forbid [xm]).  */
	Port_SetVBlankHz(50);
	XM_OnceOffInit(XM_NTSC);
	check(XM_TickClockMismatch() == 1, "NTSC tick clock on a 50Hz pump is flagged");
	XM_OnceOffInit(XM_PAL);
	check(XM_TickClockMismatch() == 0, "PAL tick clock on a 50Hz pump agrees");
	Port_SetVBlankHz(60);

	/* --- slot registries --------------------------------------------------- */
	XM_OnceOffInit(XM_NTSC);
	check(XM_TickClockMismatch() == 0, "NTSC tick clock on a 60Hz pump agrees");
	XM_SetStereo();
	check(XM_GetSongSize() > 0, "XM_GetSongSize is a real byte count");
	check(XM_GetFileHeaderSize() > 0, "XM_GetFileHeaderSize is a real count");
	static uint8_t *songStore[24];
	for (int i = 0; i < 24; i++)
	{
		songStore[i] = (uint8_t *)malloc((size_t)XM_GetSongSize());
		XM_SetSongAddress(songStore[i]);
	}
	uint8_t *hdr0 = (uint8_t *)malloc((size_t)XM_GetFileHeaderSize());
	uint8_t *hdr1 = (uint8_t *)malloc((size_t)XM_GetFileHeaderSize());
	XM_SetFileHeaderAddress(hdr0);
	XM_SetFileHeaderAddress(hdr1);

	/* --- PXM parse: known header facts of the shipped modules ------------- */
	check(InitXMData(pxm, 0, XM_UseXMPanning) == 0, "InitXMData(sb-title)");
	const XmModule *m = g_xmHeaderSlot[0];
	check(m->numChannels == 10, "sb-title: 10 channels");
	check(m->numPatterns == 57, "sb-title: 57 patterns");
	check(m->numInstruments == 32, "sb-title: 32 instruments");
	check(m->songLength == 59, "sb-title: 59-entry order table");
	check(m->defSpeed == 6 && m->defBPM == 125, "sb-title: speed 6, BPM 125");
	check(m->linearFreq == 1, "sb-title: linear frequency table");
	check(m->ins[0].vagBase == 1 && m->ins[1].vagBase == 2,
		  "VAG indices are positional and 1-based");

	crossCheckPatterns(m, xm, "sb-title");

	check(InitXMData(pxm1, 1, XM_UseXMPanning) == 1, "InitXMData(chapter1)");
	const XmModule *m1 = g_xmHeaderSlot[1];
	check(m1->numChannels == 10 && m1->numPatterns == 60 &&
		  m1->numInstruments == 30 && m1->songLength == 70,
		  "chapter1: 10ch / 60 patterns / 30 instruments / length 70");
	crossCheckPatterns(m1, xm1, "chapter1");

	/*	the SFX bank is the stress case: 173 patterns on 2 channels  */
	check(InitXMData(pxmSfx, 1, XM_UseXMPanning) == 1, "InitXMData(ingame)");
	const XmModule *ms = g_xmHeaderSlot[1];
	check(ms->numChannels == 2 && ms->numPatterns == 173,
		  "ingame SFX: 2 channels, 173 patterns");
	crossCheckPatterns(ms, xmSfx, "ingame");

	/* --- VAB upload -------------------------------------------------------- */
	{
		SpuInit();
		static char table[SPU_MALLOC_RECSIZ * 201];
		SpuInitMalloc(200, table);

		int id = XM_VABInit(vh, vb);
		check(id == 0, "XM_VABInit returns the first free slot");
		const XmVab &vab = g_xmVab[0];
		check(vab.numVags == 32, "sb-title VH: 32 VAGs");
		uint32_t total = 0;
		for (int i = 0; i < XM_MAX_VAGS; i++)
			total += vab.vagBytes[i];
		check((long)total == vbSize,
			  "VH size table sums exactly to the VB file size");
		check(vab.vagBytes[0] == 0, "VAG 0 is the dummy entry");
		check(memcmp(&g_spuRam[vab.spuBase], vb, total) == 0,
			  "VB uploaded byte-exactly into SPU RAM");
		check(XM_GetSampleAddress(0, 1) == (int)vab.vagAddr[1],
			  "XM_GetSampleAddress returns the slot's SPU address");
		check(vab.vagAddr[2] == vab.vagAddr[1] + vab.vagBytes[1],
			  "VAG addresses are prefix sums of the size table");

		uint32_t firstBase = vab.spuBase;
		XM_CloseVAB(0);
		int id2 = XM_VABInit(vh, vb);
		check(id2 == 0 && g_xmVab[0].spuBase == firstBase,
			  "close + re-init reuses the same SPU RAM (no leak)");
		XM_CloseVAB(0);

		check(XM_VABInit(pxm, vb) == -1, "non-VAB data is rejected");
	}

	/* --- sequencer end-to-end: the title theme actually plays -------------- */
	{
		XM_OnceOffInit(XM_NTSC);				/* clean re-init */
		XM_SetStereo();
		for (int i = 0; i < 24; i++)
			XM_SetSongAddress(songStore[i]);
		XM_SetFileHeaderAddress(hdr0);
		XM_SetFileHeaderAddress(hdr1);
		check(InitXMData(pxm, 0, XM_UseXMPanning) == 0, "seq: sb-title mod");
		check(InitXMData(pxmSfx, 1, XM_UseXMPanning) == 1, "seq: sfx mod");
		SpuInit();
		static char table[SPU_MALLOC_RECSIZ * 201];
		SpuInitMalloc(200, table);
		SpuSetCommonMasterVolume(0x3FFF, 0x3FFF);
		int vabMusic = XM_VABInit(vh, vb);
		int vabSfx = XM_VABInit(vhSfx, vbSfx);
		check(vabMusic == 0 && vabSfx == 1, "seq: both VABs resident");

		/*	title music: XM_Music, start position 0, all channels from 0
			(exactly playSong()'s call shape)  */
		int id = XM_Init(vabMusic, 0, -1, 0, XM_Loop, -1, XM_Music, 0);
		check(id >= 0 && id <= 23, "XM_Init returns a song id 0..23");
		XM_SetMasterVol(id, 127);

		static int16_t frame[735 * 2];
		XM_Feedback fb;

		/* looping song: feedback stays "processing" forever */
		long long sumL = 0, sumR = 0;
		int peak = 0;
		for (int u = 0; u < 360; u++)			/* six seconds */
		{
			XM_Update();
			Spu_RenderFrames(frame, 735);
			for (int i = 0; i < 735; i++)
			{
				int l = frame[i * 2], r = frame[i * 2 + 1];
				sumL += l < 0 ? -l : l;
				sumR += r < 0 ? -r : r;
				if (l > peak)
					peak = l;
			}
		}
		check(XM_GetFeedback(id, &fb) == XM_PROCESSING,
			  "looping song reports still-playing");
		check(fb.Status == XM_PLAYING && fb.ActiveVoices > 0,
			  "feedback: playing with active voices");
		check(peak > 2000, "title theme renders real signal");
		check(sumL > 0 && sumR > 0, "both stereo sides carry signal");

		/*	row cadence sanity: the title theme swings (alternating F03/F04
			speed commands), so only a bounded average holds here - the
			EXACT tick math is pinned by the constant-tempo one-shot SFX
			check below (finishes within +/-2 vblanks of rows*speed*6/5).  */
		XM_GetFeedback(id, &fb);
		int lastRow = fb.PatternPos, advances = 0, updates = 0;
		while (advances < 20 && updates < 400)
		{
			XM_Update();
			Spu_RenderFrames(frame, 735);
			updates++;
			XM_GetFeedback(id, &fb);
			if (fb.PatternPos != lastRow)
			{
				lastRow = fb.PatternPos;
				advances++;
			}
		}
		check(advances == 20, "row counter advances");
		check(updates >= 20 * 2 && updates <= 20 * 10,
			  "average row duration within a sane tempo band");

		/* stop drains to silence well inside the game's 5-frame window */
		XM_PlayStop(id);
		check(XM_GetFeedback(id, &fb) == XM_NOT_PROCESSED,
			  "stopped song reports finished");
		Spu_RenderFrames(frame, 735);
		Spu_RenderFrames(frame, 735);
		Spu_RenderFrames(frame, 735);
		bool silent = true;
		for (int i = 0; i < 735 * 2; i++)
			silent = silent && frame[i] == 0;
		check(silent, "silent within three vblanks of XM_PlayStop");
		XM_Quit(id);

		/*	one-shot SFX on channel 10, single channel - playSfx()'s shape.
			Feedback must flip to finished exactly when its pattern ends,
			freeing the game's channel bookkeeping.  */
		const XmModule *ms = g_xmHeaderSlot[1];
		int sfxPat = 0;
		int sfxId = XM_Init(vabSfx, 1, -1, 10, XM_NoLoop, 1, XM_SFX, sfxPat);
		check(sfxId >= 0, "SFX XM_Init");
		check(XM_GetFeedback(sfxId, &fb) == XM_PROCESSING,
			  "one-shot SFX starts as still-playing");
		int rows = ms->pat[sfxPat].rows;
		int expect = (rows * 6 * 6 + 4) / 5;	/* rows*6 ticks at 5/6 per vbl */
		int u = 0;
		while (XM_GetFeedback(sfxId, &fb) == XM_PROCESSING && u < expect + 20)
		{
			XM_Update();
			Spu_RenderFrames(frame, 735);
			u++;
		}
		check(u >= expect - 2 && u <= expect + 2,
			  "one-shot SFX finishes exactly at pattern end");
		if (!(u >= expect - 2 && u <= expect + 2))
			std::printf("  (SFX: %d rows, finished after %d updates, expected"
						" ~%d)\n", rows, u, expect);
		XM_Quit(sfxId);
		XM_CloseVAB(vabSfx);
		XM_CloseVAB(vabMusic);
	}

	if (g_failures)
	{
		std::printf("xm test FAILED (%d)\n", g_failures);
		return 1;
	}
	std::printf("xm test PASSED\n");
	return 0;
}
