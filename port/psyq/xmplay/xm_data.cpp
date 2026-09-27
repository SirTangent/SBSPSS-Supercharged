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

#include "system/types.h"
#include "system/lnkopt.h"	/* the arena bound, as api/arena.cpp */
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

}	/* extern "C" */

namespace
{

/*	FT2 structure caps (issue #59).  Every shipped PXM has a 276-byte file
	header, 9-byte pattern headers, 263-byte instrument headers and one
	sample per instrument; the caps are FT2's own limits, loose enough for
	any real module and tight enough that no length from a corrupt file can
	carry the walk off into memory.  instrView reads instrument header bytes
	up to +240, so an instrument with samples needs at least 241.  */
const uint32_t kMaxFileHdr		= 4096;
const uint32_t kMinPatHdr		= 9;		/* through packedSize at +7 */
const uint32_t kMaxPatHdr		= 64;
const uint32_t kMinInstHdr		= 29;		/* through numSamples at +27 */
const uint32_t kMinInstHdrSmp	= 241;
const uint32_t kMaxInstHdr		= 263;
const uint32_t kMaxInstSamples	= 16;
const uint32_t kMaxRows			= 256;

/*	n bytes at offset off lie inside size bytes (no overflow)  */
inline bool fits(size_t off, size_t n, size_t size)
{
	return off <= size && n <= size - off;
}

/*	Bytes from p to the end of the game arena - every CFileIO load is a
	MemAlloc from it - or "unbounded" for a buffer outside it (a unit test's
	malloc), which the caps alone then check.  */
size_t arenaBytesFrom(const void *p)
{
	uintptr_t a = (uintptr_t)p;
	uintptr_t lo = (uintptr_t)OPT_LinkerOpts.FreeMemAddress;
	uintptr_t hi = lo + OPT_LinkerOpts.FreeMemSize;
	if (lo && a >= lo && a < hi)
		return (size_t)(hi - a);
	return (size_t)-1;
}

int refuseModule(XmModule *m, const char *why)
{
	static int logged;
	if (!logged)
	{
		logged = 1;
		fprintf(stderr, "[xm] InitXMData: malformed module (%s) - refused, "
						"the song will be silent\n", why);
	}
	m->inUse = 0;
	return -1;
}

}	/* namespace */

int XmParseModule(const uint8_t *base, size_t size, int xmId, int panType,
				  size_t *parsedBytes)
{
	static int badSlot, badVersion, tooMany;
	if (xmId < 0 || xmId >= g_xmHeaderCount)
	{
		xmLogOnce(&badSlot, "InitXMData: XM_ID has no registered header slot");
		return -1;
	}
	/*	whatever the slot held is gone, whether or not this parse succeeds;
		inUse is set only once the whole walk has fitted  */
	XmModule *m = g_xmHeaderSlot[xmId];
	memset(m, 0, sizeof(*m));
	if (!fits(0, 0x50, size))
		return refuseModule(m, "shorter than an XM header");
	uint16_t version = rd16(base + 0x3A);
	if (version != XM_PXM_VERSION && version != XM_FT2_VERSION)
	{
		xmLogOnce(&badVersion, "InitXMData: not a PXM/XM (bad version word)");
		return -1;
	}

	m->base = base;
	m->panType = panType;

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
		m->numPatterns < 1 || m->songLength < 1 || m->songLength > 256)
	{
		xmLogOnce(&tooMany, "InitXMData: pattern/instrument count out of range");
		return -1;
	}
	/*	the order table (at 0x50) lives inside the header  */
	if (hdrSize > kMaxFileHdr || hdrSize < 0x14u + (uint32_t)m->songLength ||
		!fits(0x3C, hdrSize, size))
		return refuseModule(m, "file header length");

	/* patterns: [u32 hdrLen][u8 packing][u16 rows][u16 packedSize][data] */
	size_t off = 0x3C + hdrSize;
	for (int i = 0; i < m->numPatterns; i++)
	{
		if (!fits(off, kMinPatHdr, size))
			return refuseModule(m, "pattern header past the end");
		uint32_t phLen = rd32(base + off);
		uint16_t rows = rd16(base + off + 5);
		uint16_t packed = rd16(base + off + 7);
		if (phLen < kMinPatHdr || phLen > kMaxPatHdr || rows < 1 || rows > kMaxRows)
			return refuseModule(m, "pattern header fields");
		if (!fits(off, phLen + (size_t)packed, size))
			return refuseModule(m, "pattern data past the end");
		m->pat[i].rows = rows;
		m->pat[i].packedSize = packed;
		m->pat[i].data = base + off + phLen;
		off += phLen + packed;
	}

	/*	instruments: header (numSamples at +27, sample headers appended);
		PXM sample payloads are stripped, so the next instrument follows
		the 40-byte sample headers directly.  VAG indices are positional:
		a running 1-based count of samples across instruments (index 0 is
		the .VH's dummy entry).  */
	int vagIndex = 1;
	for (int i = 0; i < m->numInstruments; i++)
	{
		if (!fits(off, kMinInstHdr, size))
			return refuseModule(m, "instrument header past the end");
		uint32_t ihLen = rd32(base + off);
		uint16_t nSamp = rd16(base + off + 27);
		if (ihLen < kMinInstHdr || ihLen > kMaxInstHdr || nSamp > kMaxInstSamples ||
			(nSamp && ihLen < kMinInstHdrSmp))
			return refuseModule(m, "instrument header fields");
		size_t hdrs = ihLen + (size_t)nSamp * 40;
		if (!fits(off, hdrs, size))
			return refuseModule(m, "sample headers past the end");
		m->ins[i].hdr = base + off;
		m->ins[i].numSamples = nSamp;
		m->ins[i].vagBase = (uint16_t)vagIndex;
		m->ins[i].sampleHdr = nSamp ? base + off + ihLen : 0;
		vagIndex += nSamp;

		size_t sampleBytes = 0;
		const uint8_t *sh = base + off + ihLen;
		for (int s = 0; s < nSamp; s++)
		{
			uint32_t len = rd32(sh + s * 40);	/* 0 in a PXM, real in an XM */
			if (!fits(off + hdrs + sampleBytes, len, size))
				return refuseModule(m, "sample data past the end");
			sampleBytes += len;
		}
		off += hdrs + sampleBytes;
	}

	m->inUse = 1;
	if (parsedBytes)
		*parsedBytes = off;
	return xmId;
}

/*	VH: 32-byte VabHdr + 128 x 16-byte ProgAtr + ps x 16 x 32-byte VagAtr +
	256 x u16 VAG sizes in 8-byte units (entry 0 = dummy).  Every shipped
	bank has ps=1, so VagAtr cannot carry per-instrument envelopes - the
	sequencer programs its own neutral SPU ADSR and does all shaping through
	the XM volume envelopes instead.  The size table decides how much of
	the VB is copied, so it has to lie inside the VH and its total inside
	both the VB and sound RAM: every shipped pair is built matched, the
	table summing exactly to the VB's size.  */
int XmVabInitSized(const uint8_t *vh, size_t vhSize,
				   const uint8_t *vb, size_t vbSize)
{
	static int badMagic, noSlot, noRam, malformed;
	if (!fits(0, 32, vhSize) || memcmp(vh, "pBAV", 4) != 0)
	{
		xmLogOnce(&badMagic, "XM_VABInit: VH lacks the VABp magic");
		return -1;
	}

	uint16_t ps = rd16(vh + 18);
	uint16_t vs = rd16(vh + 22);
	size_t tableOff = 32 + 128 * 16 + (size_t)ps * 16 * 32;
	if (ps > 128 || vs > XM_MAX_VAGS - 1 || !fits(tableOff, XM_MAX_VAGS * 2, vhSize))
	{
		xmLogOnce(&malformed, "XM_VABInit: malformed VH (program/VAG counts, or "
							  "size table past its end) - refused");
		return -1;
	}
	const uint8_t *sizeTable = vh + tableOff;

	uint32_t total = 0;
	for (int i = 0; i < XM_MAX_VAGS; i++)
		total += (uint32_t)rd16(sizeTable + i * 2) << 3;
	if (total > vbSize || total > SPU_RAM_SIZE)
	{
		xmLogOnce(&malformed, "XM_VABInit: VH size table exceeds the VB or sound "
							  "RAM (mismatched VH/VB pair?) - refused");
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

	XmVab &vab = g_xmVab[slot];
	memset(&vab, 0, sizeof(vab));
	vab.numVags = vs;
	for (int i = 0; i < XM_MAX_VAGS; i++)
		vab.vagBytes[i] = (uint32_t)rd16(sizeTable + i * 2) << 3;

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
	SpuWrite((unsigned char *)vb, total);	/* synchronous - caller frees */
	vab.inUse = 1;
	return slot;
}

extern "C" {

/*	The game passes no buffer sizes (source/sound/xmplay.cpp loadModData,
	loadSampleData), so the bound is the end of the arena they came from.  */
int InitXMData(u_char *mpp, int XM_ID, int S3MPan)
{
	const uint8_t *base = (const uint8_t *)mpp;
	return XmParseModule(base, arenaBytesFrom(base), XM_ID, S3MPan);
}

/* ---- VAB (VH/VB) -------------------------------------------------------- */

int XM_VABInit(u_char *VHData, u_char *VBData)
{
	return XmVabInitSized((const uint8_t *)VHData, arenaBytesFrom(VHData),
						  (const uint8_t *)VBData, arenaBytesFrom(VBData));
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
