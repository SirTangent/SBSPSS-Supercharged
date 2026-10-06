/*	Unit tests for the XMPlayer data layer (port/psyq/xmplay/).
	Reads the real shipped assets from data/Music and data/Sfx (run from the
	repo root; absent, it is a skipped test - exit 77, or 1 under
	SBSP_TEST_STRICT, tests/test_skip.h): parses the PXMs and
	cross-checks every pattern slot against the pristine FastTracker .xm
	files that ship beside them (the PXM repack must lose nothing), and
	verifies XM_VABInit uploads the VB byte-exactly at the addresses the
	VH size table dictates, with clean close/re-init allocation accounting.
	The sized entry points (issue #59) must walk every shipped module to
	exactly its file size and refuse truncated or corrupt modules and VABs.
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
#include "test_skip.h"

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

/* ---- synthetic PXM builder (note-delay cases) ---------------------------- */

/*	One pattern cell; fields left 0 are absent from the packed slot.  */
struct Cell
{
	int row, ch, note, instr, vol, eff, param;
};

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/*	A PXM with 2 channels, one pattern of `rows` rows, speed 6 and BPM 150
	(one tick per 60Hz XM_Update) and two instruments of one sample each,
	sample default volume 40 and pan 128, no envelopes - playable against
	the sb-title VAB (VAG 1 and 2).  Returns the byte count.  */
static size_t buildPxm(uint8_t *out, int rows, const Cell *cells, int nCells)
{
	memset(out, 0, 4096);
	put16(out + 0x3A, XM_PXM_VERSION);
	put32(out + 0x3C, 276);					/* header size */
	put16(out + 0x40, 1);					/* song length */
	put16(out + 0x44, 2);					/* channels */
	put16(out + 0x46, 1);					/* patterns */
	put16(out + 0x48, 2);					/* instruments */
	put16(out + 0x4A, 1);					/* linear frequencies */
	put16(out + 0x4C, 6);					/* speed */
	put16(out + 0x4E, 150);					/* BPM: 300 = 5 * 60 per update */
	size_t o = 0x3C + 276;

	uint8_t *ph = out + o;
	put32(ph, 9);
	put16(ph + 5, (unsigned)rows);
	size_t d = o + 9;
	for (int r = 0; r < rows; r++)
	{
		for (int i = 0; i < nCells; i++)
		{
			const Cell &c = cells[i];
			if (c.row != r)
				continue;
			out[d++] = (uint8_t)c.ch;
			uint8_t mask = 0x80;
			if (c.note)  mask |= 0x01;
			if (c.instr) mask |= 0x02;
			if (c.vol)   mask |= 0x04;
			if (c.eff)   mask |= 0x08;
			if (c.param) mask |= 0x10;
			out[d++] = mask;
			if (c.note)  out[d++] = (uint8_t)c.note;
			if (c.instr) out[d++] = (uint8_t)c.instr;
			if (c.vol)   out[d++] = (uint8_t)c.vol;
			if (c.eff)   out[d++] = (uint8_t)c.eff;
			if (c.param) out[d++] = (uint8_t)c.param;
		}
		out[d++] = 0xFF;
	}
	put16(ph + 7, (unsigned)(d - o - 9));
	o = d;

	for (int i = 0; i < 2; i++)
	{
		put32(out + o, 263);
		put16(out + o + 27, 1);				/* one sample; keymap all 0 */
		uint8_t *sh = out + o + 263;
		sh[12] = 40;						/* default volume */
		sh[15] = 128;						/* default pan */
		o += 263 + 40;
	}
	return o;
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
	long pxm1Size, pxmSfxSize;
	uint8_t *pxm1 = loadFile("data/Music/chapter1/chapter1.PXM", &pxm1Size);
	uint8_t *xm1 = loadFile("data/Music/chapter1/CHAPTER1.XM", &xmSize);
	uint8_t *pxmSfx = loadFile("data/Sfx/ingame/ingame.PXM", &pxmSfxSize);
	uint8_t *xmSfx = loadFile("data/Sfx/ingame/ingame.xm", &xmSize);
	long vhSfxSize, vbSfxSize;
	uint8_t *vhSfx = loadFile("data/Sfx/ingame/ingame.VH", &vhSfxSize);
	uint8_t *vbSfx = loadFile("data/Sfx/ingame/ingame.VB", &vbSfxSize);
	if (!pxm || !xm || !vh || !vb || !pxm1 || !xm1 || !pxmSfx || !xmSfx ||
		!vhSfx || !vbSfx)
	{
		std::printf("xm test SKIPPED (data/Music + data/Sfx assets not found"
					" - run from the repo root)\n");
		return testSkipExit("xm_test", 1);	/* tests/test_skip.h: 77, or 1 under SBSP_TEST_STRICT */
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

	/* --- malformed-data bounds (issue #59): the sized entry points ------- */
	{
		/*	every shipped module walks to exactly its file size, so one byte
			less is a truncation the walk must notice  */
		const uint8_t *mods[3] = { pxm, pxm1, pxmSfx };
		const long sizes[3] = { pxmSize, pxm1Size, pxmSfxSize };
		for (int i = 0; i < 3; i++)
		{
			size_t walked = 0;
			check(XmParseModule(mods[i], (size_t)sizes[i], 0, XM_UseXMPanning,
								&walked) == 0 && (long)walked == sizes[i],
				  "sized parse: shipped PXM walks to exactly its file size");
			check(XmParseModule(mods[i], (size_t)sizes[i] - 1, 0,
								XM_UseXMPanning) == -1 &&
				  !g_xmHeaderSlot[0]->inUse,
				  "sized parse: a PXM one byte short is refused");
		}

		/*	corrupt fields on a copy of sb-title, each refused with the true
			size and - where a cap alone must catch it - unbounded too  */
		const size_t unbounded = (size_t)-1;
		check(XmParseModule(pxm, (size_t)pxmSize, 0, XM_UseXMPanning) == 0,
			  "sized parse: sb-title for the offsets below");
		size_t pat0 = 0x3C + rd32(pxm + 0x3C);
		size_t ins0 = (size_t)(g_xmHeaderSlot[0]->ins[0].hdr - pxm);
		uint8_t *bad = (uint8_t *)malloc((size_t)pxmSize);
		struct { size_t at; uint32_t value; int bytes; const char *what; } cases[] = {
			{ pat0,      0x7FFFFFFFu, 4, "pattern header length 0x7FFFFFFF refused" },
			{ ins0,      0x10000u,    4, "instrument header length 0x10000 refused" },
			{ ins0 + 27, 17,          2, "17 samples in an instrument refused" },
		};
		for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
		{
			memcpy(bad, pxm, (size_t)pxmSize);
			for (int k = 0; k < cases[c].bytes; k++)
				bad[cases[c].at + k] = (uint8_t)(cases[c].value >> (8 * k));
			check(XmParseModule(bad, (size_t)pxmSize, 0, XM_UseXMPanning) == -1 &&
				  XmParseModule(bad, unbounded, 0, XM_UseXMPanning) == -1 &&
				  !g_xmHeaderSlot[0]->inUse,
				  cases[c].what);
		}
		free(bad);

		/*	a refused module is silence, not a fault: XM_Init turns it down  */
		SpuInit();
		static char table[SPU_MALLOC_RECSIZ * 201];
		SpuInitMalloc(200, table);
		int vab = XmVabInitSized(vh, (size_t)vhSize, vb, (size_t)vbSize);
		check(vab == 0 && g_xmVab[0].spuBytes == (uint32_t)vbSize,
			  "sized VAB: shipped pair uploads the whole VB");
		check(XmParseModule(pxm, (size_t)pxmSize - 1, 0, XM_UseXMPanning) == -1 &&
			  XM_Init(vab, 0, -1, 0, XM_Loop, -1, XM_Music, 0) == -1,
			  "XM_Init refuses a module whose parse was refused");
		XM_CloseVAB(vab);

		check(XmVabInitSized(vh, (size_t)vhSize, vb, (size_t)vbSize - 1) == -1,
			  "sized VAB: a VB one byte short of the size table is refused");
		check(XmVabInitSized(vh, (size_t)vhSize - 1, vb, (size_t)vbSize) == -1,
			  "sized VAB: a size table running past the VH is refused");
		uint8_t *badVh = (uint8_t *)malloc((size_t)vhSize);
		memcpy(badVh, vh, (size_t)vhSize);
		badVh[18] = 0xFF;						/* ps = 0xFFFF */
		badVh[19] = 0xFF;
		check(XmVabInitSized(badVh, (size_t)vhSize, vb, (size_t)vbSize) == -1 &&
			  XmVabInitSized(badVh, unbounded, vb, unbounded) == -1,
			  "sized VAB: ps = 0xFFFF is refused");
		free(badVh);
		check(!g_xmVab[0].inUse, "refused VABs leave the slot free");
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

	/* --- note delay (EDx), FT2 semantics (issue #59) ---------------------- */
	{
		/*	Synthetic modules at speed 6, one tick per XM_Update once the
			song's tick accumulator is zeroed: update n processes tick
			(n-1)%6 of row (n-1)/6.  The title theme swings speed 3/4, so
			none of this is pinned on real music.  */
		XM_OnceOffInit(XM_NTSC);
		for (int i = 0; i < 24; i++)
			XM_SetSongAddress(songStore[i]);
		XM_SetFileHeaderAddress(hdr0);
		SpuInit();
		static char table[SPU_MALLOC_RECSIZ * 201];
		SpuInitMalloc(200, table);
		SpuSetCommonMasterVolume(0x3FFF, 0x3FFF);
		int vab = XmVabInitSized(vh, (size_t)vhSize, vb, (size_t)vbSize);
		check(vab == 0, "note delay: sb-title VAB resident");

		static uint8_t mod[4096];
		struct Scenario
		{
			const char *name;
			Cell cells[4];
			int nCells;
		};
		/*	A: instrument + set volume 7 + ED2.  B: no instrument, ED2 keeps
			the running volume (row 0 set it to 32).  C: set pan 0x40 + ED2.
			D: ED7 >= speed never fires, even when row 2 (F08 on channel 1,
			channel 0 empty) stretches the row to 8 ticks.
			E-G follow ft2-clone's noteDelay (src/ft2_replayer.c), which
			fires when (speed - song.tick) == x - a count that restarts on
			every pass of an EEx (pattern delay) row.
			E: ED7 with EE1 on channel 1 - the row runs 12 ticks, but the
			delay is counted within each 6-tick pass, so it never fires.
			F: ED2 + instrument + volume-column slide down 2, with EE1 - the
			note fires at tick 2 and again at tick 8 (tick 2 of the second
			pass); each time the slide runs first and the instrument's
			volume reset stands, as the note delay runs after the volume
			column.  G: a key-off row, then ED2 with no note (set volume
			32) - FT2's triggerNote takes note 0 as the channel's last note,
			so 49 plays again; channel 1 has played nothing, so its bare ED2
			(with an instrument, so the delay tick is reached) triggers
			nothing.  H: the last note is the last one TRIGGERED - FT2's
			noteNum, which a tone-portamento target (row 1, 3xx to 61) does
			not change - so row 2's bare ED2 replays 49.  I: an EDx row
			takes its instrument at tick 0 (FT2's getNewNote assigns instrNum
			before its note-delay return), so ED7's never-firing row still
			leaves instrument 2 for row 2's plain note.  J: FT2's triggerNote
			sets noteNum before the instrument lookup, so 65 on instrument 7
			(no such instrument: silence) is still the note a bare ED2 on
			instrument 1 replays.  */
		static const Scenario sc[10] = {
			{ "A", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 61, 2, 0x17, 0x0E, 0xD2 } }, 2 },
			{ "B", { { 0, 0, 49, 1, 0x30, 0,    0    },
					 { 1, 0, 61, 0, 0,    0x0E, 0xD2 } }, 2 },
			{ "C", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 61, 1, 0xC4, 0x0E, 0xD2 } }, 2 },
			{ "D", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 61, 2, 0,    0x0E, 0xD7 },
					 { 2, 1, 0,  0, 0,    0x0F, 0x08 } }, 3 },
			{ "E", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 61, 2, 0,    0x0E, 0xD7 },
					 { 1, 1, 0,  0, 0,    0x0E, 0xE1 } }, 3 },
			{ "F", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 61, 2, 0x62, 0x0E, 0xD2 },
					 { 1, 1, 0,  0, 0,    0x0E, 0xE1 } }, 3 },
			{ "G", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 97, 0, 0,    0,    0    },
					 { 2, 0, 0,  0, 0x30, 0x0E, 0xD2 },
					 { 2, 1, 0,  1, 0,    0x0E, 0xD2 } }, 4 },
			{ "H", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 61, 0, 0,    0x03, 0x10 },
					 { 2, 0, 0,  0, 0,    0x0E, 0xD2 } }, 3 },
			{ "I", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 61, 2, 0,    0x0E, 0xD7 },
					 { 2, 0, 63, 0, 0,    0,    0    } }, 3 },
			{ "J", { { 0, 0, 49, 1, 0,    0,    0    },
					 { 1, 0, 65, 7, 0,    0,    0    },
					 { 2, 0, 0,  1, 0,    0x0E, 0xD2 } }, 3 },
		};
		struct Snap
		{
			int note, instr, volume, pan, row, speed, keyOff, active, active1;
		} snap[21];
		for (int k = 0; k < 10; k++)
		{
			size_t n = buildPxm(mod, 3, sc[k].cells, sc[k].nCells);
			size_t walked = 0;
			check(XmParseModule(mod, n, 0, XM_UseXMPanning, &walked) == 0 &&
				  walked == n, "note delay: synthetic PXM parses");
			int id = XM_Init(vab, 0, -1, 0, XM_Loop, -1, XM_Music, 0);
			check(id >= 0, "note delay: XM_Init");
			if (id < 0)
				continue;
			XmSongState *s = g_xmSongSlot[id];
			s->tickAccumHz = 0;
			for (int u = 1; u <= 20; u++)
			{
				XM_Update();
				const XmChannelState &c = s->ch[0];
				snap[u].note = c.note;
				snap[u].instr = c.instr;
				snap[u].volume = c.volume;
				snap[u].pan = c.pan;
				snap[u].row = s->row;
				snap[u].speed = s->speed;
				snap[u].keyOff = c.keyOff;
				snap[u].active = c.active;
				snap[u].active1 = s->ch[1].active;
			}
			XM_Quit(id);

			int before = g_failures;
			bool ok;
			switch (k)
			{
			case 0:
				ok = snap[6].note == 49 && snap[6].volume == 40 && snap[6].instr == 1;
				for (int u = 7; u <= 8; u++)
					ok = ok && snap[u].note == 49 && snap[u].volume == 40 &&
						 snap[u].instr == 2;
				check(ok, "EDx A: ticks 0-1 take the instrument number but leave "
						  "the old note and its volume alone");
				check(snap[9].note == 61 && snap[9].volume == 7 && snap[9].instr == 2,
					  "EDx A: the delay tick plays the new note at the row's volume");
				break;
			case 1:
				check(snap[8].note == 49 && snap[8].volume == 32 &&
					  snap[9].note == 61 && snap[9].volume == 32,
					  "EDx B: without an instrument the running volume is kept");
				break;
			case 2:
				check(snap[7].pan == 128 && snap[8].pan == 128 && snap[8].note == 49 &&
					  snap[9].note == 61 && snap[9].pan == 0x40 &&
					  snap[9].volume == 40,
					  "EDx C: the set-pan column lands on the delayed note");
				break;
			case 3:
				check(snap[19].row == 2 && snap[19].speed == 8,
					  "EDx D: row 2 runs 8 ticks (the case is live)");
				ok = true;
				for (int u = 7; u <= 20; u++)
					ok = ok && snap[u].note == 49 && snap[u].instr == 2 &&
						 snap[u].volume == 40;
				check(ok, "EDx D: a delay >= speed never fires, not even in a "
						  "later, longer row");
				break;
			case 4:
				check(snap[17].row == 1 && snap[18].row == 2,
					  "EDx E: EE1 stretches row 1 to 12 ticks (the case is live)");
				ok = true;
				for (int u = 7; u <= 18; u++)
					ok = ok && snap[u].note == 49 && snap[u].instr == 2 &&
						 snap[u].volume == 40;
				check(ok, "EDx E: ED7 at speed 6 never fires, even in a row "
						  "EE1 runs for 12 ticks");
				break;
			case 5:
				check(snap[8].note == 49 && snap[8].volume == 38,
					  "EDx F: tick 1 slides the old note");
				check(snap[9].note == 61 && snap[9].instr == 2 &&
					  snap[9].volume == 40,
					  "EDx F: tick 2 slides, then the delayed note resets "
					  "the volume");
				check(snap[14].volume == 30 && snap[17].row == 1,
					  "EDx F: the slide runs on through the second pass");
				check(snap[15].volume == 40,
					  "EDx F: the delay fires again at tick 2 of the second "
					  "pass");
				break;
			case 6:
				check(snap[14].keyOff == 1 && snap[14].active == 0,
					  "EDx G: the key-off row stops the note");
				check(snap[15].note == 49 && snap[15].keyOff == 0 &&
					  snap[15].active == 1 && snap[15].volume == 32,
					  "EDx G: ED2 with no note replays the last note");
				ok = true;
				for (int u = 1; u <= 18; u++)
					ok = ok && snap[u].active1 == 0;
				check(ok, "EDx G: on a channel with no last note it triggers "
						  "nothing");
				break;
			case 7:
				check(snap[14].note == 61,
					  "EDx H: the portamento row made 61 the channel's note");
				check(snap[15].note == 49,
					  "EDx H: ED2 with no note replays the last triggered "
					  "note, not the portamento target");
				break;
			case 8:
				ok = true;
				for (int u = 7; u <= 12; u++)
					ok = ok && snap[u].note == 49 && snap[u].instr == 2 &&
						 snap[u].volume == 40;
				check(ok, "EDx I: the row takes its instrument at tick 0 and "
						  "the old note plays on, as ED7 never fires");
				check(snap[13].note == 63 && snap[13].instr == 2 &&
					  snap[13].active == 1,
					  "EDx I: the next row's plain note plays on that "
					  "instrument");
				break;
			default:
				check(snap[7].active == 0 && snap[7].instr == 7,
					  "EDx J: a note on an instrument with no sample is silent");
				check(snap[15].note == 65 && snap[15].instr == 1 &&
					  snap[15].active == 1,
					  "EDx J: a bare ED2 replays that note, the last one "
					  "triggered");
				break;
			}
			if (g_failures != before)
				for (int u = 6; u <= 18; u++)
					std::printf("  (%s update %d: row %d note %d instr %d vol %d "
								"pan %d keyoff %d active %d)\n",
								sc[k].name, u, snap[u].row, snap[u].note,
								snap[u].instr, snap[u].volume, snap[u].pan,
								snap[u].keyOff, snap[u].active);
		}
		XM_CloseVAB(vab);
	}

	if (g_failures)
	{
		std::printf("xm test FAILED (%d)\n", g_failures);
		return 1;
	}
	std::printf("xm test PASSED\n");
	return 0;
}
