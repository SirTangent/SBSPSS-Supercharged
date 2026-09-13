/*	XMPlayer data layer (M5): PXM parsing, VAB upload, the caller-allocated
	slot registries, and the global player switches.

	Contract notes (from the game's call sites, source/sound/xmplay.cpp):
	- XM_SetSongAddress / XM_SetFileHeaderAddress are push-a-slot calls; the
	  game MemAllocs exactly XM_GetSongSize()/XM_GetFileHeaderSize() bytes
	  per slot and never touches the storage again.
	- XM_VABInit must copy the whole VB into SPU RAM before returning - the
	  game frees both the VH and VB buffers on the very next line.
	- InitXMData's PXM buffer stays resident; the module keeps pointers.
*/
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <libspu.h>
#include <XMPLAY.H>

#include "spu/spu_core.h"
#include "xmplay/xm_state.h"
#include "host/pump.h"		/* Port_VBlankHz: the tick-clock cross-check */

XmModule *g_xmHeaderSlot[XM_MAX_HEADER_SLOTS];
int g_xmHeaderCount;
XmSongState *g_xmSongSlot[XM_MAX_SONG_SLOTS];
int g_xmSongCount;
XmVab g_xmVab[XM_MAX_VABS];
int g_xmTickHz = 60;
int g_xmStereo = 1;

namespace
{

void xmLogOnce(int *flag, const char *msg)
{
	if (!*flag)
	{
		*flag = 1;
		fprintf(stderr, "[xm] %s\n", msg);
	}
}

inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

}	/* namespace */

extern "C" {

/* ---- global switches ---------------------------------------------------- */

/*	The sequencer ticks off g_xmTickHz (xm_seq.cpp XM_Update, once per
	emulated vblank) but the game picks XM_PAL/XM_NTSC from its territory
	macro (source/sound/xmplay.cpp), while the pump's rate comes from
	SetVideoMode (vid.cpp) - two independent territory decisions that must
	agree, or every song plays 20% off tempo with nothing else amiss.  The
	game calls XM_OnceOffInit after VidInit, so the pump rate is final here.  */
int XM_TickClockMismatch(void)
{
	return g_xmTickHz != Port_VBlankHz();
}

void XM_OnceOffInit(int PAL)
{
	g_xmTickHz = (PAL == XM_PAL) ? 50 : 60;
	if (XM_TickClockMismatch())
		fprintf(stderr, "[xm] WARNING: tick clock %dHz (XM_OnceOffInit %s) but the vblank clock is %dHz (SetVideoMode) - territory/video-mode mismatch\n",
				g_xmTickHz, PAL == XM_PAL ? "XM_PAL" : "XM_NTSC", Port_VBlankHz());
	/*	give the SPU RAM back before dropping the slots - clearing inUse
		alone would orphan every allocation for the life of the process  */
	for (int i = 0; i < XM_MAX_VABS; i++)
		if (g_xmVab[i].inUse)
			XM_CloseVAB(i);
	memset(g_xmVab, 0, sizeof(g_xmVab));
	g_xmHeaderCount = 0;
	g_xmSongCount = 0;
}

void XM_SetStereo(void)
{
	g_xmStereo = 1;
}

void XM_SetMono(void)
{
	/*	unreachable from the shipped game (no mono/stereo option exists);
		recorded so a future host-side option can use it  */
	g_xmStereo = 0;
}

/* ---- caller-allocated slot registries ----------------------------------- */

int XM_GetSongSize(void)
{
	return (int)sizeof(XmSongState);
}

void XM_SetSongAddress(u_char *Address)
{
	static int overflow;
	if (g_xmSongCount >= XM_MAX_SONG_SLOTS)
	{
		xmLogOnce(&overflow, "XM_SetSongAddress: more than 24 song slots");
		return;
	}
	memset(Address, 0, sizeof(XmSongState));
	g_xmSongSlot[g_xmSongCount++] = (XmSongState *)Address;
}

int XM_GetFileHeaderSize(void)
{
	return (int)sizeof(XmModule);
}

void XM_SetFileHeaderAddress(u_char *Address)
{
	static int overflow;
	if (g_xmHeaderCount >= XM_MAX_HEADER_SLOTS)
	{
		xmLogOnce(&overflow, "XM_SetFileHeaderAddress: more than 8 slots");
		return;
	}
	memset(Address, 0, sizeof(XmModule));
	g_xmHeaderSlot[g_xmHeaderCount++] = (XmModule *)Address;
}

/* ---- module (PXM) parsing ----------------------------------------------- */

int InitXMData(u_char *mpp, int XM_ID, int S3MPan)
{
	static int badSlot, badVersion, tooMany;
	if (XM_ID < 0 || XM_ID >= g_xmHeaderCount)
	{
		xmLogOnce(&badSlot, "InitXMData: XM_ID has no registered header slot");
		return -1;
	}
	const uint8_t *base = (const uint8_t *)mpp;
	uint16_t version = rd16(base + 0x3A);
	if (version != XM_PXM_VERSION && version != XM_FT2_VERSION)
	{
		xmLogOnce(&badVersion, "InitXMData: not a PXM/XM (bad version word)");
		return -1;
	}

	XmModule *m = g_xmHeaderSlot[XM_ID];
	memset(m, 0, sizeof(*m));
	m->inUse = 1;
	m->base = base;
	m->panType = S3MPan;

	uint32_t hdrSize = rd32(base + 0x3C);
	m->songLength = rd16(base + 0x40);
	m->restartPos = rd16(base + 0x42);
	m->numChannels = rd16(base + 0x44);
	m->numPatterns = rd16(base + 0x46);
	m->numInstruments = rd16(base + 0x48);
	m->linearFreq = rd16(base + 0x4A) & 1;
	m->defSpeed = rd16(base + 0x4C);
	m->defBPM = rd16(base + 0x4E);
	m->orderTable = base + 0x50;

	/*	a module with no patterns or an empty order table has nothing to
		play, and every "last valid index" downstream would be -1  */
	if (m->numPatterns > XM_MAX_PATTERNS || m->numInstruments > XM_MAX_INSTRUMENTS ||
		m->numPatterns < 1 || m->songLength < 1)
	{
		xmLogOnce(&tooMany, "InitXMData: pattern/instrument count out of range");
		m->inUse = 0;
		return -1;
	}

	/* patterns: [u32 hdrLen][u8 packing][u16 rows][u16 packedSize][data] */
	const uint8_t *p = base + 0x3C + hdrSize;
	for (int i = 0; i < m->numPatterns; i++)
	{
		uint32_t phLen = rd32(p);
		m->pat[i].rows = rd16(p + 5);
		m->pat[i].packedSize = rd16(p + 7);
		m->pat[i].data = p + phLen;
		p += phLen + m->pat[i].packedSize;
	}

	/*	instruments: header (numSamples at +27, sample headers appended);
		PXM sample payloads are stripped, so the next instrument follows
		the 40-byte sample headers directly.  VAG indices are positional:
		a running 1-based count of samples across instruments (index 0 is
		the .VH's dummy entry).  */
	int vagIndex = 1;
	for (int i = 0; i < m->numInstruments; i++)
	{
		uint32_t ihLen = rd32(p);
		uint16_t nSamp = rd16(p + 27);
		m->ins[i].hdr = p;
		m->ins[i].numSamples = nSamp;
		m->ins[i].vagBase = (uint16_t)vagIndex;
		m->ins[i].sampleHdr = nSamp ? p + ihLen : 0;
		vagIndex += nSamp;

		uint32_t sampleBytes = 0;
		const uint8_t *sh = p + ihLen;
		for (int s = 0; s < nSamp; s++)
			sampleBytes += rd32(sh + s * 40);	/* 0 in a PXM, real in an XM */
		p += ihLen + (uint32_t)nSamp * 40 + sampleBytes;
	}

	return XM_ID;
}

/* ---- VAB (VH/VB) -------------------------------------------------------- */

int XM_VABInit(u_char *VHData, u_char *VBData)
{
	static int badMagic, noSlot, noRam;
	const uint8_t *vh = (const uint8_t *)VHData;
	if (memcmp(vh, "pBAV", 4) != 0)
	{
		xmLogOnce(&badMagic, "XM_VABInit: VH lacks the VABp magic");
		return -1;
	}

	int slot = -1;
	for (int i = 0; i < XM_MAX_VABS; i++)
	{
		if (!g_xmVab[i].inUse)
		{
			slot = i;
			break;
		}
	}
	if (slot < 0)
	{
		xmLogOnce(&noSlot, "XM_VABInit: all 8 VAB slots in use");
		return -1;
	}

	/*	VH: 32-byte VabHdr + 128 x 16-byte ProgAtr + ps x 16 x 32-byte
		VagAtr + 256 x u16 VAG sizes in 8-byte units (entry 0 = dummy).
		Every shipped bank has ps=1, so VagAtr cannot carry per-instrument
		envelopes - the sequencer programs its own neutral SPU ADSR and
		does all shaping through the XM volume envelopes instead.  */
	uint16_t ps = rd16(vh + 18);
	uint16_t vs = rd16(vh + 22);
	const uint8_t *sizeTable = vh + 32 + 128 * 16 + (uint32_t)ps * 16 * 32;

	XmVab &vab = g_xmVab[slot];
	memset(&vab, 0, sizeof(vab));
	vab.numVags = vs;

	uint32_t total = 0;
	for (int i = 0; i < XM_MAX_VAGS; i++)
	{
		vab.vagBytes[i] = (uint32_t)rd16(sizeTable + i * 2) << 3;
		total += vab.vagBytes[i];
	}

	long base = SpuMalloc((long)total);
	if (base < 0)
	{
		xmLogOnce(&noRam, "XM_VABInit: out of SPU RAM");
		return -1;
	}
	vab.spuBase = (uint32_t)base;
	vab.spuBytes = total;

	uint32_t off = 0;
	for (int i = 0; i < XM_MAX_VAGS; i++)
	{
		vab.vagAddr[i] = vab.spuBase + off;
		off += vab.vagBytes[i];
	}

	SpuSetTransferStartAddr(vab.spuBase);
	SpuWrite((unsigned char *)VBData, total);	/* synchronous - caller frees */
	vab.inUse = 1;
	return slot;
}

void XM_CloseVAB(int VabID)
{
	static int badId;
	if (VabID < 0 || VabID >= XM_MAX_VABS || !g_xmVab[VabID].inUse)
	{
		xmLogOnce(&badId, "XM_CloseVAB: id not open");
		return;
	}
	SpuFree(g_xmVab[VabID].spuBase);
	g_xmVab[VabID].inUse = 0;
}

int XM_GetSampleAddress(int vabid, int samplenum)
{
	static int badArgs;
	if (vabid < 0 || vabid >= XM_MAX_VABS || !g_xmVab[vabid].inUse ||
		samplenum < 0 || samplenum >= XM_MAX_VAGS)
	{
		xmLogOnce(&badArgs, "XM_GetSampleAddress: bad vab/sample");
		return -1;
	}
	return (int)g_xmVab[vabid].vagAddr[samplenum];
}

}	/* extern "C" */
