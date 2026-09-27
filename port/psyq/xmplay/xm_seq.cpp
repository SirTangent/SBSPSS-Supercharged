/*	XMPlayer sequencer (M5): XM_Init / XM_Update and the FT2 playback
	engine, written from the FastTracker II semantics for exactly the
	effect set the original XMPLAY.LIB implemented (its symbol map:
	Arpeggio, DoVibrato, DoTremolo, DoToneSlide, DoVolSlide, DoVol, DoPan,
	DoXMPanSlide, DoEEffects, DoS3MRetrig, ProcessEnvelope,
	ProcessPanEnvelope, envelope fadeout, key-off, linear+Amiga periods).
	Anything outside that set logs once and is ignored.

	Timing: XM_Update is called once per emulated vblank (g_xmTickHz).
	The tick rate is BPM*2/5 Hz; an integer accumulator (acc += BPM*2,
	tick while acc >= 5*hz) makes 125 BPM at 60Hz exactly 5 ticks per 6
	vblanks with zero drift.  Tempo therefore derives purely from the
	pump's vblank clock - the SPU renders continuously in between.

	Threading: the sequencer state itself is game-thread-only.  Voice
	programming goes through the public libspu calls, each of which takes
	the SPU lock briefly - register writes interleave with mixer chunks
	exactly the way real hardware register pokes interleaved with SPU
	operation, so no whole-tick lock is needed (and taking one here would
	self-deadlock through the non-recursive SPU mutex).

	SPU envelopes: every shipped .VH has ps=1, so VagAtr carries no usable
	per-instrument ADSR - all volume shaping is done by the XM volume
	envelopes at tick rate.  Voices get one neutral hardware envelope:
	instant attack, pinned sustain, and a short release (shift 5, ~1.5ms)
	whose only job is de-clicking key-offs.
*/
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <sys/types.h>
#include <libspu.h>
#include <XMPLAY.H>

#include "spu/spu_core.h"
#include "xmplay/xm_state.h"

namespace
{

const uint16_t kNeutralAdsr1 = 0x000F;	/* instant attack, sustain level max */
const uint16_t kNeutralAdsr2 = 0x0005;	/* pinned sustain, release shift 5 */

/* FT2 half-sine, 32 steps, amplitude 0..255 */
const uint8_t kSine[32] = {
	0, 24, 49, 74, 97, 120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
	255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97, 74, 49, 24,
};

void seqLogOnce(int *flag, const char *msg)
{
	if (!*flag)
	{
		*flag = 1;
		fprintf(stderr, "[xm] %s\n", msg);
	}
}

int g_warnEffect[36];
int g_warnVolCol;

inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

inline int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- pitch --------------------------------------------------------------- */

/*	linear: period = 7680 - note*64 - finetune/2 (note 0-based incl.
	relative), freq = 8363 * 2^((4608 - period) / 768)  */
int periodForNote(const XmModule *mod, int realNote, int finetune)
{
	if (mod->linearFreq)
		return 7680 - realNote * 64 - finetune / 2;
	/*	Amiga path - unreached by the shipped PXMs (all linear); the pow
		form is accurate to <0.2 cents which is far below ADPCM noise  */
	return (int)(1712.0 * pow(2.0, -(realNote + finetune / 128.0) / 12.0));
}

uint32_t freqForPeriod(const XmModule *mod, int period)
{
	if (period < 1)
		period = 1;
	if (mod->linearFreq)
		return (uint32_t)(8363.0 * pow(2.0, (4608 - period) / 768.0));
	return (uint32_t)(8363.0 * 1712.0 / period);
}

uint16_t spuPitchForFreq(uint32_t freq)
{
	uint32_t pitch = (uint32_t)(((uint64_t)freq << 12) / 44100u);
	return (uint16_t)(pitch > 0x3FFF ? 0x3FFF : pitch);
}

/* ---- sparse pattern walking ---------------------------------------------- */

struct RowSlot
{
	uint8_t note, instr, vol, eff, param;
	uint8_t present;
};

/*	one byte of a slot, or 0 once the pattern's data has run out  */
inline uint8_t slotByte(const uint8_t *&p, const uint8_t *end)
{
	return p < end ? *p++ : 0;
}

/*	decode one packed slot; a slot cut short by the pattern's end reads its
	missing fields as 0 and never looks past end  */
const uint8_t *readSlot(const uint8_t *p, const uint8_t *end, RowSlot *s)
{
	uint8_t b = slotByte(p, end);
	if (b & 0x80)
	{
		if (b & 0x01) s->note = slotByte(p, end);
		if (b & 0x02) s->instr = slotByte(p, end);
		if (b & 0x04) s->vol = slotByte(p, end);
		if (b & 0x08) s->eff = slotByte(p, end);
		if (b & 0x10) s->param = slotByte(p, end);
	}
	else
	{
		s->note = b;
		s->instr = slotByte(p, end);
		s->vol = slotByte(p, end);
		s->eff = slotByte(p, end);
		s->param = slotByte(p, end);
	}
	return p;
}

/*	step over one packed slot without decoding it (row skipping), stopping
	at end  */
const uint8_t *skipSlot(const uint8_t *p, const uint8_t *end)
{
	if (p >= end)
		return end;
	uint8_t b = *p++;
	size_t n = 4;
	if (b & 0x80)
		n = (b & 0x01) + ((b >> 1) & 1) + ((b >> 2) & 1) + ((b >> 3) & 1) +
			((b >> 4) & 1);
	return n <= (size_t)(end - p) ? p + n : end;
}

/*	fetch row `row` of a sparse pattern into slots[chans].  packedSize
	bounds every scan and every slot read: a truncated or mis-parsed PXM
	must stop at the end of the pattern rather than hunt for a 0xFF past
	the module buffer (the parse has already checked that the pattern's
	packedSize bytes lie inside it).  */
void fetchRow(const XmPatternRef *pat, int row, int chans, RowSlot *slots)
{
	memset(slots, 0, sizeof(RowSlot) * (size_t)chans);
	if (!pat->data || pat->packedSize == 0)
		return;
	const uint8_t *p = pat->data;
	const uint8_t *end = pat->data + pat->packedSize;
	for (int r = 0; r < row; r++)
	{
		while (p < end && *p != 0xFF)
		{
			p++;
			p = skipSlot(p, end);
		}
		if (p >= end)
			return;
		p++;
	}
	while (p < end && *p != 0xFF)
	{
		int c = *p++;
		RowSlot tmp;
		memset(&tmp, 0, sizeof(tmp));
		p = readSlot(p, end, &tmp);
		if (c < chans)
		{
			slots[c] = tmp;
			slots[c].present = 1;
		}
	}
}

/* ---- instrument header fields (offsets per the FT2 format) --------------- */

struct InstrView
{
	const uint8_t	*keymap;		/* 96 entries */
	const uint8_t	*volEnv;		/* 12 (x,y) u16 pairs */
	const uint8_t	*panEnv;
	int		numVolPts, numPanPts;
	int		volSus, volLoopStart, volLoopEnd;
	int		panSus, panLoopStart, panLoopEnd;
	int		volType, panType;		/* bit0 on, bit1 sustain, bit2 loop */
	int		vibType, vibSweep, vibDepth, vibRate;
	int		fadeout;
};

int instrView(const XmModule *mod, int instr1, InstrView *iv)
{
	if (instr1 < 1 || instr1 > mod->numInstruments)
		return 0;
	const XmInstrumentRef &ir = mod->ins[instr1 - 1];
	if (!ir.numSamples)
		return 0;
	const uint8_t *h = ir.hdr;
	iv->keymap = h + 33;
	iv->volEnv = h + 129;
	iv->panEnv = h + 177;
	/*	both envelopes are 12 (x,y) u16 pairs; FT2 caps every count and
		point index at that, and a malformed header must not index past
		the 48 bytes that actually exist  */
	iv->numVolPts = clampi(h[225], 0, 12);
	iv->numPanPts = clampi(h[226], 0, 12);
	iv->volSus = clampi(h[227], 0, 11);
	iv->volLoopStart = clampi(h[228], 0, 11);
	iv->volLoopEnd = clampi(h[229], 0, 11);
	iv->panSus = clampi(h[230], 0, 11);
	iv->panLoopStart = clampi(h[231], 0, 11);
	iv->panLoopEnd = clampi(h[232], 0, 11);
	iv->volType = h[233];
	iv->panType = h[234];
	iv->vibType = h[235];
	iv->vibSweep = h[236];
	iv->vibDepth = h[237];
	iv->vibRate = h[238];
	iv->fadeout = rd16(h + 239);
	return 1;
}

/*	sample header (40 bytes) for sample s of instrument instr1  */
const uint8_t *sampleHdr(const XmModule *mod, int instr1, int s)
{
	const XmInstrumentRef &ir = mod->ins[instr1 - 1];
	if (s < 0 || s >= ir.numSamples)
		return 0;
	return ir.sampleHdr + s * 40;
}

/*	evaluate an FT2 envelope (12 points max) at frame pos; returns 0..64  */
int envValue(const uint8_t *env, int numPts, int pos)
{
	if (numPts <= 0)
		return 64;
	int x0 = rd16(env), y0 = rd16(env + 2);
	if (pos <= x0)
		return y0;
	for (int i = 1; i < numPts; i++)
	{
		int x1 = rd16(env + i * 4), y1 = rd16(env + i * 4 + 2);
		if (pos < x1)
		{
			if (x1 == x0)
				return y1;
			return y0 + (y1 - y0) * (pos - x0) / (x1 - x0);
		}
		x0 = x1;
		y0 = y1;
	}
	return rd16(env + (numPts - 1) * 4 + 2);
}

/* ---- the songs ----------------------------------------------------------- */

XmSongState *songById(int id)
{
	if (id < 0 || id >= g_xmSongCount)
		return 0;
	XmSongState *s = g_xmSongSlot[id];
	return (s && s->inUse) ? s : 0;
}

/*	SPU voice for a played module channel: firstCh + (# played below it)  */
int voiceForChannel(const XmSongState *s, int ch)
{
	int n = 0;
	for (int i = 0; i < ch; i++)
		if (s->playMask & (1 << i))
			n++;
	return s->firstCh + n;
}

int waveValue(int wave, int pos)
{
	pos &= 63;
	int v;
	switch (wave & 3)
	{
	default: v = kSine[pos & 31]; break;					/* sine */
	case 1:  v = 255 - ((pos & 31) * 255 / 31); break;		/* ramp down */
	case 2:  v = 255; break;								/* square */
	}
	return (pos >= 32) ? -v : v;
}

void programVoiceStart(XmSongState *s, int ch, uint32_t spuAddr)
{
	SpuVoiceAttr attr;
	memset(&attr, 0, sizeof(attr));
	attr.voice = SPU_VOICECH(voiceForChannel(s, ch));
	attr.mask = SPU_VOICE_WDSA | SPU_VOICE_LSAX |
				SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_ADSR2;
	attr.addr = spuAddr;
	attr.loop_addr = spuAddr;		/* VAG loop flags override during decode */
	attr.adsr1 = kNeutralAdsr1;
	attr.adsr2 = kNeutralAdsr2;
	SpuSetVoiceAttr(&attr);
	SpuSetKey(SPU_ON, attr.voice);
	s->ch[ch].spuKeyed = 1;
}

void silenceChannel(XmSongState *s, int ch, int hardCut)
{
	XmChannelState &c = s->ch[ch];
	int voice = voiceForChannel(s, ch);
	if (c.spuKeyed)
	{
		if (hardCut)
		{
			SpuSetVoiceVolume(voice, 0, 0);
			/*	the write-through cache has to follow the register, or the
				next note that computes this same volume is never written  */
			c.spuVolL = c.spuVolR = 0;
		}
		SpuSetKey(SPU_OFF, SPU_VOICECH(voice));
		c.spuKeyed = 0;
	}
	c.active = 0;
}

void silenceSong(XmSongState *s, int hardCut)
{
	for (int ch = 0; ch < s->numCh; ch++)
		if (s->playMask & (1 << ch))
			silenceChannel(s, ch, hardCut);
}

void endSong(XmSongState *s)
{
	silenceSong(s, 0);
	s->finished = 1;
	s->status = XM_STOPPED;
}

/* ---- note trigger (tick 0) ----------------------------------------------- */

void triggerNote(XmSongState *s, int ch, int note)
{
	XmChannelState &c = s->ch[ch];
	const XmModule *mod = s->mod;
	InstrView iv;
	if (!instrView(mod, c.instr, &iv))
	{
		silenceChannel(s, ch, 1);
		return;
	}
	int sIdx = iv.keymap[clampi(note - 1, 0, 95)];
	const uint8_t *sh = sampleHdr(mod, c.instr, sIdx);
	if (!sh)
	{
		silenceChannel(s, ch, 1);
		return;
	}

	c.note = (uint8_t)note;
	c.finetune = (int8_t)sh[13];
	int realNote = note - 1 + (int8_t)sh[16];
	realNote = clampi(realNote, 0, 118);
	c.basePeriod = periodForNote(mod, realNote, c.finetune);
	c.period = c.basePeriod;
	c.vagIndex = mod->ins[c.instr - 1].vagBase + sIdx;

	/*	the module's positional VAG count can outrun the bank's size table;
		reject the note rather than wrap onto an unrelated sample  */
	const XmVab &vab = g_xmVab[s->vabId];
	if (c.vagIndex < 1 || c.vagIndex >= XM_MAX_VAGS ||
		c.vagIndex > vab.numVags)
	{
		silenceChannel(s, ch, 1);
		return;
	}

	uint32_t addr = vab.vagAddr[c.vagIndex];
	/*	9xx sample offset: units of 256 samples, and only for a note on the
		row that carries the effect.  ADPCM seeks in 28-sample/16-byte
		blocks, so round down to a block and stay inside this VAG.  */
	if (c.effect == 0x09 && c.memSampleOfs)
	{
		uint32_t byteOfs = (((uint32_t)c.memSampleOfs << 8) / 28u) * 16u;
		if (byteOfs < vab.vagBytes[c.vagIndex])
			addr += byteOfs;
	}
	if (!(c.vibWave & 4))
		c.vibPos = 0;
	if (!(c.tremWave & 4))
		c.tremPos = 0;
	/*	the note - not the instrument column - restarts the envelopes,
		fadeout and key-off state (FT2)  */
	c.volEnvPos = 0;
	c.panEnvPos = 0;
	c.autoVibPos = 0;
	c.autoVibSweep = 0;
	c.fadeout = 65536;
	c.keyOff = 0;
	programVoiceStart(s, ch, addr);
	c.active = 1;
}

void resetInstrument(XmSongState *s, int ch)
{
	XmChannelState &c = s->ch[ch];
	InstrView iv;
	if (!instrView(s->mod, c.instr, &iv))
		return;
	/* instrument column resets volume/pan (FT2); envelopes belong to the note */
	int sIdx = iv.keymap[clampi(c.note ? c.note - 1 : 0, 0, 95)];
	const uint8_t *sh = sampleHdr(s->mod, c.instr, sIdx);
	if (sh)
	{
		c.volume = clampi(sh[12], 0, 64);
		c.pan = sh[15];
	}
}

/* ---- row processing (tick 0) --------------------------------------------- */

void doKeyOff(XmSongState *s, int ch)
{
	XmChannelState &c = s->ch[ch];
	c.keyOff = 1;
	InstrView iv;
	if (!instrView(s->mod, c.instr, &iv) || !(iv.volType & 1))
	{
		/* no volume envelope: key-off cuts the note (FT2) */
		c.volume = 0;
		silenceChannel(s, ch, 0);
	}
}

void processSlotTick0(XmSongState *s, int ch, const RowSlot *sl)
{
	XmChannelState &c = s->ch[ch];
	c.effect = sl->present ? sl->eff : 0;
	c.param = sl->present ? sl->param : 0;
	if (!sl->present)
	{
		c.period = c.basePeriod;		/* empty slot: drop any vibrato offset */
		return;
	}

	int note = sl->note;
	int isTonePorta = (sl->eff == 3 || sl->eff == 5 ||
					   (sl->vol >= 0xF0));
	int delayTicks = (sl->eff == 14 && (sl->param >> 4) == 0x0D)
						 ? (sl->param & 0x0F) : 0;

	if (sl->instr)
		c.instr = sl->instr;

	/*	9xx: xx is remembered per channel, but triggerNote applies it only
		while this row's effect IS 0x09 - so it has to land before the
		note below, and a later row without 9xx starts at zero again  */
	if (c.effect == 0x09 && c.param)
		c.memSampleOfs = c.param;

	if (delayTicks)
	{
		c.delayedNote = (uint8_t)note;
		c.delayTick = (uint8_t)delayTicks;
	}
	else if (note == 97)
	{
		doKeyOff(s, ch);
	}
	else if (note >= 1 && note <= 96 && c.instr)
	{
		if (isTonePorta && c.active)
		{
			/* note becomes the portamento target; no retrigger */
			InstrView iv;
			if (instrView(s->mod, c.instr, &iv))
			{
				int sIdx = iv.keymap[clampi(note - 1, 0, 95)];
				const uint8_t *sh = sampleHdr(s->mod, c.instr, sIdx);
				if (sh)
				{
					int realNote = clampi(note - 1 + (int8_t)sh[16], 0, 118);
					c.targetPeriod =
						periodForNote(s->mod, realNote, (int8_t)sh[13]);
					c.note = (uint8_t)note;
				}
			}
		}
		else
		{
			triggerNote(s, ch, note);
		}
	}

	if (sl->instr && note != 97 && !delayTicks)
		resetInstrument(s, ch);

	/* volume column, tick 0 */
	uint8_t v = sl->vol;
	if (v >= 0x10 && v <= 0x50)
		c.volume = (int16_t)clampi(v - 0x10, 0, 64);
	else if ((v & 0xF0) == 0x80)
		c.volume = (int16_t)clampi(c.volume - (v & 0x0F), 0, 64);
	else if ((v & 0xF0) == 0x90)
		c.volume = (int16_t)clampi(c.volume + (v & 0x0F), 0, 64);
	else if ((v & 0xF0) == 0xA0)
		c.vibSpeed = (uint8_t)((v & 0x0F) << 2);
	else if ((v & 0xF0) == 0xC0)
		c.pan = (int16_t)((v & 0x0F) << 4);
	else if ((v & 0xF0) == 0xF0 && (v & 0x0F))
		c.memTonePorta = (uint8_t)((v & 0x0F) << 4);

	/* effect column, tick 0 */
	uint8_t p = c.param;
	switch (c.effect)
	{
	case 0x00:									/* arpeggio: per-tick */
		break;
	case 0x01:
		if (p) c.memPortaUp = p;
		break;
	case 0x02:
		if (p) c.memPortaDown = p;
		break;
	case 0x03:
		if (p) c.memTonePorta = p;
		break;
	case 0x04:
		if (p & 0x0F) c.vibDepth = p & 0x0F;
		if (p >> 4) c.vibSpeed = (uint8_t)((p >> 4) << 2);
		break;
	case 0x05:
	case 0x06:
	case 0x0A:
		if (p) c.memVolSlide = p;
		break;
	case 0x07:
		if (p & 0x0F) c.tremDepth = p & 0x0F;
		if (p >> 4) c.tremSpeed = (uint8_t)((p >> 4) << 2);
		break;
	case 0x08:
		c.pan = p;
		break;
	case 0x09:									/* sample offset: taken above */
		break;
	case 0x0B:
		s->pendingJumpPos = p;
		break;
	case 0x0C:
		c.volume = (int16_t)clampi(p, 0, 64);
		break;
	case 0x0D:
		s->pendingBreakRow = (p >> 4) * 10 + (p & 0x0F);
		break;
	case 0x0E:
		switch (p >> 4)
		{
		case 0x01:
			if (p & 0x0F) c.memFinePortaUp = p & 0x0F;
			c.basePeriod -= c.memFinePortaUp * 4;
			c.period = c.basePeriod;
			break;
		case 0x02:
			if (p & 0x0F) c.memFinePortaDown = p & 0x0F;
			c.basePeriod += c.memFinePortaDown * 4;
			c.period = c.basePeriod;
			break;
		case 0x04:
			c.vibWave = p & 0x0F;
			break;
		case 0x05:
			c.finetune = ((p & 0x0F) - 8) << 4;
			break;
		case 0x06:
			if ((p & 0x0F) == 0)
				c.patLoopRow = (uint8_t)s->row;
			else
			{
				if (c.patLoopCount == 0)
					c.patLoopCount = (uint8_t)(p & 0x0F);
				else
					c.patLoopCount--;
				if (c.patLoopCount)
				{
					s->pendingJumpPos = -2;		/* loop: stay, jump row */
					s->pendingBreakRow = c.patLoopRow;
				}
			}
			break;
		case 0x07:
			c.tremWave = p & 0x0F;
			break;
		case 0x09:
			c.retrigTick = 0;
			break;
		case 0x0A:
			if (p & 0x0F) c.memEFineVolUp = p & 0x0F;
			c.volume = (int16_t)clampi(c.volume + c.memEFineVolUp, 0, 64);
			break;
		case 0x0B:
			if (p & 0x0F) c.memEFineVolDown = p & 0x0F;
			c.volume = (int16_t)clampi(c.volume - c.memEFineVolDown, 0, 64);
			break;
		case 0x0C:		/* note cut: per-tick */
		case 0x0D:		/* note delay: handled above */
			break;
		case 0x0E:
			s->patDelay = p & 0x0F;
			break;
		default:
			seqLogOnce(&g_warnEffect[14], "unimplemented E-effect variant");
			break;
		}
		break;
	case 0x0F:
		if (p == 0)
			break;
		if (p < 0x20)
			s->speed = p;
		else
			s->bpm = p;
		break;
	case 0x14:									/* Kxx key off: K00 is now */
		if (p == 0)
			doKeyOff(s, ch);
		break;
	case 0x19:									/* Pxy pan slide: per-tick */
		if (p) c.memPanSlide = p;
		break;
	case 0x1B:									/* Rxy multi retrig */
		if (p & 0x0F) c.memRetrig = (c.memRetrig & 0xF0) | (p & 0x0F);
		if (p & 0xF0) c.memRetrig = (uint8_t)((c.memRetrig & 0x0F) | (p & 0xF0));
		c.retrigTick = 0;
		break;
	case 0x21:									/* Xxy extra fine porta */
		if ((p >> 4) == 1)
			c.basePeriod -= (p & 0x0F);
		else if ((p >> 4) == 2)
			c.basePeriod += (p & 0x0F);
		c.period = c.basePeriod;
		break;
	default:
		if (c.effect < 36)
			seqLogOnce(&g_warnEffect[c.effect],
					   "unimplemented XM effect (outside XMPLAY.LIB's set)");
		break;
	}

	if (c.effect != 4 && c.effect != 6)
		c.period = c.basePeriod;
}

/* ---- per-tick effects (tick > 0) ------------------------------------------ */

void volSlide(XmChannelState &c, uint8_t p)
{
	if (p >> 4)
		c.volume = (int16_t)clampi(c.volume + (p >> 4), 0, 64);
	else
		c.volume = (int16_t)clampi(c.volume - (p & 0x0F), 0, 64);
}

void tonePorta(XmChannelState &c)
{
	int step = c.memTonePorta * 4;
	if (c.basePeriod < c.targetPeriod)
		c.basePeriod = c.basePeriod + step > c.targetPeriod
						   ? c.targetPeriod : c.basePeriod + step;
	else if (c.basePeriod > c.targetPeriod)
		c.basePeriod = c.basePeriod - step < c.targetPeriod
						   ? c.targetPeriod : c.basePeriod - step;
	c.period = c.basePeriod;
}

void processTickN(XmSongState *s, int ch, int tick)
{
	XmChannelState &c = s->ch[ch];
	uint8_t p = c.param;

	/* delayed note fires on its tick */
	if (c.delayTick && tick == c.delayTick)
	{
		int note = c.delayedNote;
		c.delayTick = 0;
		if (note == 97)
			doKeyOff(s, ch);
		else if (note >= 1 && note <= 96 && c.instr)
		{
			triggerNote(s, ch, note);
			resetInstrument(s, ch);
		}
	}

	switch (c.effect)
	{
	case 0x00:
		if (p)
		{
			int step;
			switch (tick % 3)
			{
			default: step = 0; break;
			case 1: step = p >> 4; break;
			case 2: step = p & 0x0F; break;
			}
			c.period = c.basePeriod - step * 64;
		}
		break;
	case 0x01:
		c.basePeriod -= c.memPortaUp * 4;
		if (c.basePeriod < 64)
			c.basePeriod = 64;
		c.period = c.basePeriod;
		break;
	case 0x02:
		c.basePeriod += c.memPortaDown * 4;
		if (c.basePeriod > 32000)
			c.basePeriod = 32000;
		c.period = c.basePeriod;
		break;
	case 0x03:
		tonePorta(c);
		break;
	case 0x04:
		c.vibPos = (uint8_t)(c.vibPos + c.vibSpeed);
		c.period = c.basePeriod +
				   (waveValue(c.vibWave, c.vibPos >> 2) * c.vibDepth >> 5);
		break;
	case 0x05:
		tonePorta(c);
		volSlide(c, c.memVolSlide);
		break;
	case 0x06:
		c.vibPos = (uint8_t)(c.vibPos + c.vibSpeed);
		c.period = c.basePeriod +
				   (waveValue(c.vibWave, c.vibPos >> 2) * c.vibDepth >> 5);
		volSlide(c, c.memVolSlide);
		break;
	case 0x07:
		c.tremPos = (uint8_t)(c.tremPos + c.tremSpeed);
		break;
	case 0x0A:
		volSlide(c, c.memVolSlide);
		break;
	case 0x0E:
		if ((p >> 4) == 0x09 && (p & 0x0F))
		{
			if (++c.retrigTick >= (p & 0x0F))
			{
				c.retrigTick = 0;
				if (c.note >= 1 && c.note <= 96)
					triggerNote(s, ch, c.note);
			}
		}
		else if ((p >> 4) == 0x0C && tick == (p & 0x0F))
		{
			c.volume = 0;
		}
		break;
	case 0x14:									/* Kxx key off at tick */
		if (tick == p)
			doKeyOff(s, ch);
		break;
	case 0x19:
	{
		uint8_t ps = c.memPanSlide;
		if (ps >> 4)
			c.pan = (int16_t)clampi(c.pan + (ps >> 4), 0, 255);
		else
			c.pan = (int16_t)clampi(c.pan - (ps & 0x0F), 0, 255);
		break;
	}
	case 0x1B:									/* Rxy multi retrig */
	{
		int interval = c.memRetrig & 0x0F;
		if (interval && ++c.retrigTick >= interval)
		{
			c.retrigTick = 0;
			static const int8_t addTab[16] = { 0, -1, -2, -4, -8, -16, 0, 0,
											   0, 1, 2, 4, 8, 16, 0, 0 };
			int sel = c.memRetrig >> 4;
			int vol = c.volume;
			if (sel == 6) vol = vol * 2 / 3;
			else if (sel == 7) vol /= 2;
			else if (sel == 14) vol = vol * 3 / 2;
			else if (sel == 15) vol *= 2;
			else vol += addTab[sel];
			c.volume = (int16_t)clampi(vol, 0, 64);
			if (c.note >= 1 && c.note <= 96)
				triggerNote(s, ch, c.note);
		}
		break;
	}
	default:
		break;
	}
}

/*	volume-column per-tick part needs the row's vol byte; stash it  */
void processVolColTickN(XmSongState *s, int ch, uint8_t v)
{
	XmChannelState &c = s->ch[ch];
	switch (v & 0xF0)
	{
	case 0x60:
		c.volume = (int16_t)clampi(c.volume - (v & 0x0F), 0, 64);
		break;
	case 0x70:
		c.volume = (int16_t)clampi(c.volume + (v & 0x0F), 0, 64);
		break;
	case 0xB0:
		c.vibPos = (uint8_t)(c.vibPos + c.vibSpeed);
		if (v & 0x0F)
			c.vibDepth = v & 0x0F;
		c.period = c.basePeriod +
				   (waveValue(c.vibWave, c.vibPos >> 2) * c.vibDepth >> 5);
		break;
	case 0xD0:
		c.pan = (int16_t)clampi(c.pan - (v & 0x0F), 0, 255);
		break;
	case 0xE0:
		c.pan = (int16_t)clampi(c.pan + (v & 0x0F), 0, 255);
		break;
	case 0xF0:
		tonePorta(c);
		break;
	default:
		if (v >= 1 && v < 0x10)
			seqLogOnce(&g_warnVolCol, "unknown volume-column command");
		break;
	}
}

/* ---- envelopes + SPU programming ------------------------------------------ */

void updateChannelOutput(XmSongState *s, int ch)
{
	XmChannelState &c = s->ch[ch];
	if (!c.active)
		return;
	const XmModule *mod = s->mod;
	int voice = voiceForChannel(s, ch);

	InstrView iv;
	int haveIv = instrView(mod, c.instr, &iv);

	/* volume envelope + fadeout */
	int envVol = 64;
	if (haveIv && (iv.volType & 1))
	{
		envVol = envValue(iv.volEnv, iv.numVolPts, c.volEnvPos);
		int susX = rd16(iv.volEnv + iv.volSus * 4);
		int advance = !( (iv.volType & 2) && !c.keyOff && c.volEnvPos >= susX );
		if (advance)
			c.volEnvPos++;
		if (iv.volType & 4)
		{
			int loopEndX = rd16(iv.volEnv + iv.volLoopEnd * 4);
			if (c.volEnvPos >= loopEndX)
				c.volEnvPos = rd16(iv.volEnv + iv.volLoopStart * 4);
		}
	}
	if (c.keyOff)
	{
		c.fadeout -= (haveIv ? iv.fadeout : 0xFFF) * 2;
		if (c.fadeout <= 0)
		{
			c.fadeout = 0;
			silenceChannel(s, ch, 0);
			return;
		}
	}

	/* pan envelope */
	int pan = c.pan;
	if (haveIv && (iv.panType & 1))
	{
		int envPan = envValue(iv.panEnv, iv.numPanPts, c.panEnvPos);
		int susX = rd16(iv.panEnv + iv.panSus * 4);
		if (!((iv.panType & 2) && !c.keyOff && c.panEnvPos >= susX))
			c.panEnvPos++;
		if (iv.panType & 4)
		{
			int loopEndX = rd16(iv.panEnv + iv.panLoopEnd * 4);
			if (c.panEnvPos >= loopEndX)
				c.panEnvPos = rd16(iv.panEnv + iv.panLoopStart * 4);
		}
		int reach = 128 - (pan >= 128 ? pan - 128 : 128 - pan);
		pan = clampi(pan + (envPan - 32) * reach / 32, 0, 255);
	}

	/*	autovibrato: its own position and sweep counter, ticking whether or
		not the instrument has a volume envelope and whether or not that
		envelope is held at its sustain point  */
	int period = c.period;
	if (haveIv && iv.vibDepth)
	{
		int depth = iv.vibDepth;
		if (iv.vibSweep && c.autoVibSweep < iv.vibSweep)
		{
			depth = depth * c.autoVibSweep / iv.vibSweep;
			c.autoVibSweep++;
		}
		period += (waveValue(iv.vibType, (c.autoVibPos >> 2) & 63) * depth) >> 6;
		c.autoVibPos += iv.vibRate;
	}

	/* tremolo */
	int vol64 = c.volume;
	if (c.effect == 0x07)
		vol64 = clampi(vol64 +
					   (waveValue(c.tremWave, c.tremPos >> 2) * c.tremDepth >> 5),
					   0, 64);

	/* final SPU volume: chanVol * envVol * fadeout * masterVol, unity 0x3FFF */
	int64_t gain = (int64_t)vol64 * envVol;					/* <= 4096 */
	gain = gain * c.fadeout >> 16;							/* <= 4096 */
	gain = gain * clampi(s->masterVol, 0, 128) >> 7;		/* <= 4096 */
	int volBase = (int)((gain * 0x3FFF) >> 12);

	int panShifted = clampi(pan + s->masterPan, 0, 255);
	int volL = volBase * (256 - panShifted) >> 8;
	int volR = volBase * panShifted >> 8;
	if (!g_xmStereo)
		volL = volR = (volL + volR) / 2;

	uint16_t pitch = spuPitchForFreq(freqForPeriod(mod, period));

	if (pitch != c.spuPitch)
	{
		SpuSetVoicePitch(voice, pitch);
		c.spuPitch = pitch;
	}
	if (volL != c.spuVolL || volR != c.spuVolR)
	{
		SpuSetVoiceVolume(voice, (short)volL, (short)volR);
		c.spuVolL = (int16_t)volL;
		c.spuVolR = (int16_t)volR;
	}
}

/* ---- row / position advance ----------------------------------------------- */

int patternIndexForSong(const XmSongState *s)
{
	if (s->songType == XM_SFX)
		return s->sfxPattern;
	return s->mod->orderTable[s->songPos];
}

void advanceRow(XmSongState *s)
{
	const XmModule *mod = s->mod;
	int rows = mod->pat[clampi(patternIndexForSong(s), 0,
							   mod->numPatterns - 1)].rows;

	if (s->pendingJumpPos == -2)
	{
		/* pattern loop: jump within the current pattern */
		s->row = s->pendingBreakRow;
		s->pendingJumpPos = -1;
		s->pendingBreakRow = -1;
		return;
	}

	if (s->pendingJumpPos >= 0 || s->pendingBreakRow >= 0)
	{
		if (s->songType == XM_SFX)
		{
			endSong(s);
			return;
		}
		s->songPos = (s->pendingJumpPos >= 0) ? s->pendingJumpPos
											  : s->songPos + 1;
		s->row = (s->pendingBreakRow >= 0) ? s->pendingBreakRow : 0;
		s->pendingJumpPos = -1;
		s->pendingBreakRow = -1;
	}
	else
	{
		s->row++;
		if (s->row < rows)
			return;
		s->row = 0;
		if (s->songType == XM_SFX)
		{
			endSong(s);
			return;
		}
		s->songPos++;
	}

	if (s->songPos >= mod->songLength)
	{
		if (s->loop == XM_Loop)
			s->songPos = clampi(mod->restartPos, 0, mod->songLength - 1);
		else
		{
			endSong(s);
			return;
		}
	}
	int newRows = mod->pat[clampi(patternIndexForSong(s), 0,
								  mod->numPatterns - 1)].rows;
	if (s->row >= newRows)
		s->row = 0;
}

/* ---- one tick -------------------------------------------------------------- */

void songTick(XmSongState *s)
{
	const XmModule *mod = s->mod;
	static RowSlot slots[XM_MAX_MODULE_CHANNELS];

	if (s->tick == 0)
	{
		int patIdx = clampi(patternIndexForSong(s), 0, mod->numPatterns - 1);
		fetchRow(&mod->pat[patIdx], s->row, s->numCh, slots);
		for (int ch = 0; ch < s->numCh; ch++)
		{
			if (!(s->playMask & (1 << ch)))
				continue;
			s->ch[ch].rowVolCol = slots[ch].present ? slots[ch].vol : 0;
			processSlotTick0(s, ch, &slots[ch]);
		}
	}
	else
	{
		for (int ch = 0; ch < s->numCh; ch++)
		{
			if (!(s->playMask & (1 << ch)))
				continue;
			processTickN(s, ch, s->tick);
			processVolColTickN(s, ch, s->ch[ch].rowVolCol);
		}
	}

	for (int ch = 0; ch < s->numCh; ch++)
		if (s->playMask & (1 << ch))
			updateChannelOutput(s, ch);

	s->tick++;
	if (s->tick >= s->speed * (s->patDelay + 1))
	{
		s->tick = 0;
		s->patDelay = 0;
		advanceRow(s);
	}
}

}	/* namespace */

/* ---- public API ------------------------------------------------------------ */

extern "C" {

int XM_Init(int VabID, int XM_ID, int SongID, int FirstCh,
			int Loop, int PlayMask, int PlayType, int SFXNum)
{
	static int badArgs, noSlot;
	if (VabID < 0 || VabID >= XM_MAX_VABS || !g_xmVab[VabID].inUse ||
		XM_ID < 0 || XM_ID >= g_xmHeaderCount ||
		!g_xmHeaderSlot[XM_ID] || !g_xmHeaderSlot[XM_ID]->inUse ||
		FirstCh < 0 || FirstCh >= SPU_NVOICES)
	{
		seqLogOnce(&badArgs, "XM_Init: bad VabID/XM_ID/FirstCh");
		return -1;
	}

	int id = SongID;
	if (id < 0)
	{
		id = -1;
		for (int i = 0; i < g_xmSongCount; i++)
		{
			if (!g_xmSongSlot[i]->inUse)
			{
				id = i;
				break;
			}
		}
	}
	if (id < 0 || id >= g_xmSongCount)
	{
		seqLogOnce(&noSlot, "XM_Init: no free song slot");
		return -1;
	}

	XmSongState *s = g_xmSongSlot[id];
	memset(s, 0, sizeof(*s));
	const XmModule *mod = g_xmHeaderSlot[XM_ID];
	s->inUse = 1;
	s->vabId = VabID;
	s->xmId = XM_ID;
	s->mod = mod;
	s->firstCh = FirstCh;
	s->numCh = clampi(mod->numChannels, 0, XM_MAX_MODULE_CHANNELS);
	s->loop = Loop;
	s->songType = PlayType;
	s->playMask = (PlayMask == -1) ? (1 << s->numCh) - 1
								   : PlayMask & ((1 << s->numCh) - 1);
	s->status = (PlayMask == 0) ? XM_PAUSED : XM_PLAYING;
	s->speed = mod->defSpeed ? mod->defSpeed : 6;
	s->bpm = mod->defBPM ? mod->defBPM : 125;
	s->masterVol = 128;
	s->masterPan = 0;
	s->pendingJumpPos = -1;
	s->pendingBreakRow = -1;

	if (PlayType == XM_SFX)
		s->sfxPattern = clampi(SFXNum, 0, mod->numPatterns - 1);
	else
		s->songPos = clampi(SFXNum, 0, mod->songLength - 1);

	for (int ch = 0; ch < s->numCh; ch++)
	{
		s->ch[ch].pan = 128;
		s->ch[ch].spuPitch = 0xFFFF;	/* force first write-through */
		s->ch[ch].spuVolL = s->ch[ch].spuVolR = -1;
	}

	/* first XM_Update fires tick 0 immediately */
	s->tickAccumHz = 5 * g_xmTickHz;

	return id;
}

void XM_Update(void)
{
	for (int i = 0; i < g_xmSongCount; i++)
	{
		XmSongState *s = g_xmSongSlot[i];
		if (!s || !s->inUse || s->status != XM_PLAYING || !s->mod)
			continue;
		s->tickAccumHz += s->bpm * 2;
		while (s->tickAccumHz >= 5 * g_xmTickHz)
		{
			s->tickAccumHz -= 5 * g_xmTickHz;
			songTick(s);
			if (s->status != XM_PLAYING)
				break;
		}
	}
}

int XM_GetFeedback(int Song_ID, XM_Feedback *Feedback)
{
	XmSongState *s = songById(Song_ID);
	if (Feedback)
	{
		memset(Feedback, 0, sizeof(*Feedback));
		if (s)
		{
			Feedback->Volume = (u_char)s->masterVol;
			Feedback->Panning = (short)s->masterPan;
			Feedback->Status = (u_char)s->status;
			Feedback->SongPos = (short)s->songPos;
			Feedback->PatternPos = (u_short)s->row;
			Feedback->CurrentPattern = (u_short)patternIndexForSong(s);
			Feedback->SongSpeed = (u_short)s->speed;
			Feedback->SongBPM = (u_short)s->bpm;
			Feedback->SongLength = (u_short)s->mod->songLength;
			Feedback->SongLoop = s->loop;
			Feedback->PlayNext = -1;	/* XM_PlayNext is not implemented */
			int voices = 0;
			for (int ch = 0; ch < s->numCh; ch++)
				if ((s->playMask & (1 << ch)) && s->ch[ch].active)
					voices++;
			Feedback->ActiveVoices = voices;
		}
	}
	if (!s || s->finished)
		return XM_NOT_PROCESSED;	/* nonzero: caller frees the channels */
	return XM_PROCESSING;
}

void XM_PlayStop(int Song_ID)
{
	XmSongState *s = songById(Song_ID);
	if (!s)
		return;
	silenceSong(s, 0);
	s->status = XM_STOPPED;
	s->finished = 1;
}

void XM_Quit(int SongID)
{
	XmSongState *s = songById(SongID);
	if (!s)
		return;
	silenceSong(s, 0);
	s->inUse = 0;
}

void XM_SetMasterVol(int Song_ID, u_char Vol)
{
	XmSongState *s = songById(Song_ID);
	if (s)
		s->masterVol = Vol;
}

void XM_SetMasterPan(int Song_ID, short Pan)
{
	XmSongState *s = songById(Song_ID);
	if (s)
		s->masterPan = Pan;
}

void XM_ClearSFXRange(void)
{
	/*	the game calls this after every SFX init but XM_SetSFXRange is
		commented out at its only site (xmplay.cpp:864) - nothing to clear  */
}

/* ---- deferred dead paths (no caller in the shipped game) ------------------ */

void XM_PlaySample(int addr, int channel, int voll, int volr, int pitch)
{
	static int warned;
	(void)addr; (void)channel; (void)voll; (void)volr; (void)pitch;
	seqLogOnce(&warned, "XM_PlaySample: looping-sample path is deferred "
						"(no SFX table entry has looping=1)");
}

void XM_StopSample(int channel)
{
	(void)channel;
}

}	/* extern "C" */
