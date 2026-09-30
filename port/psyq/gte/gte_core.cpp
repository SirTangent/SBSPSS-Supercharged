/*	Software GTE core - see gte_core.h for the contract.

	Every numeric rule here is from the psx-spx "Geometry Transformation
	Engine" chapter; where the hardware is quirky (the RTPS IR3 flag check,
	the MVMVA far-colour bug, the sign-extended cfc2 reads of H) the quirk
	is reproduced, because the game was tuned against it.

	Register storage is the raw word file (s_dr/s_cr); ops unpack typed
	views on demand.  FLAG is rebuilt from zero by every command, exactly
	like hardware.
*/
#include "stub_log.h"
#include "gte_core.h"

static uint32_t s_dr[32];	/* data registers   (mtc2/mfc2)  */
static uint32_t s_cr[32];	/* control registers (ctc2/cfc2) */

/*****************************************************************************/
/*	FLAG (ctrl reg 31).  Rebuilt per command; bit31 = OR of bits 30-23,18-13. */

static uint32_t s_flag;

static void flagB(int bit)
{
	s_flag |= (uint32_t)1 << bit;
}

static void commitFlag(void)
{
	if (s_flag & 0x7F87E000u)
		s_flag |= 0x80000000u;
	s_cr[31] = s_flag;
}

/*****************************************************************************/
/*	Typed views over the register file  */

static int16_t vXofs(int v) { return (int16_t)s_dr[v * 2]; }			/* V0/1/2 */
static int16_t vYofs(int v) { return (int16_t)(s_dr[v * 2] >> 16); }
static int16_t vZofs(int v) { return (int16_t)s_dr[v * 2 + 1]; }

static int16_t irReg(int i) { return (int16_t)s_dr[8 + i]; }			/* IR0-3 */

/*	Matrices pack 9 halfwords into 5 ctrl words: word k holds elements
	2k (low) and 2k+1 (high), row-major.  base: 0=RT, 8=LLM, 16=LCM.  */
static int16_t mtxEl(int base, int row, int col)
{
	int idx = row * 3 + col;
	uint32_t w = s_cr[base + idx / 2];
	return (int16_t)((idx & 1) ? (w >> 16) : w);
}

static int32_t trReg(int i)  { return (int32_t)s_cr[5 + i]; }		/* TRX/Y/Z  */
static int32_t bkReg(int i)  { return (int32_t)s_cr[13 + i]; }		/* RBK/G/B  */
static int32_t fcReg(int i)  { return (int32_t)s_cr[21 + i]; }		/* RFC/G/B  */

/*****************************************************************************/
/*	Saturation / accumulation helpers.  MAC1-3 accumulate in 44 bits with a
	flag check and sign-extension after EVERY addition (psx-spx: intermediate
	overflows are detected even when the final sum is back in range).  */

/*	Left shift of a possibly negative value, done on the unsigned bit
	pattern: a signed left shift of a negative is undefined before C++20
	(the game dialect is gnu++98), and the unsigned form is the exact bits
	every compiler already produces.  */
static int64_t shl64(int64_t v, int n)
{
	return (int64_t)((uint64_t)v << n);
}

static int64_t ext44(int i, int64_t v)			/* i = 1..3 */
{
	if (v >= ((int64_t)1 << 43))
		flagB(31 - i);							/* MAC1/2/3 positive: 30/29/28 */
	if (v < -((int64_t)1 << 43))
		flagB(28 - i);							/* MAC1/2/3 negative: 27/26/25 */
	return shl64(v, 20) >> 20;
}

static int32_t setMAC0(int64_t v)
{
	if (v >= ((int64_t)1 << 31))
		flagB(16);
	if (v < -((int64_t)1 << 31))
		flagB(15);
	s_dr[24] = (uint32_t)(int32_t)v;
	return (int32_t)v;
}

static void setIR(int i, int32_t v, int lm)		/* i = 1..3; sat flag 24/23/22 */
{
	int32_t lo = lm ? 0 : -0x8000;
	if (v < lo)
	{
		v = lo;
		flagB(25 - i);
	}
	else if (v > 0x7FFF)
	{
		v = 0x7FFF;
		flagB(25 - i);
	}
	s_dr[8 + i] = (uint32_t)v;
}

static void setIR0(int32_t v)					/* sat 0..0x1000, flag 12 */
{
	if (v < 0)
	{
		v = 0;
		flagB(12);
	}
	else if (v > 0x1000)
	{
		v = 0x1000;
		flagB(12);
	}
	s_dr[8] = (uint32_t)v;
}

/*	The Z saturation rule: 0..0xFFFF with FLAG bit 18.  Shared by the SZ
	FIFO push and AVSZ3/4's OTZ result so a hardware edge case can only be
	fixed in one place.  */
static uint32_t satZ(int64_t v)
{
	if (v < 0)
	{
		v = 0;
		flagB(18);
	}
	else if (v > 0xFFFF)
	{
		v = 0xFFFF;
		flagB(18);
	}
	return (uint32_t)v;
}

static void pushSZ(int64_t v)
{
	s_dr[16] = s_dr[17];
	s_dr[17] = s_dr[18];
	s_dr[18] = s_dr[19];
	s_dr[19] = satZ(v);
}

static int32_t satSXY(int32_t v, int flagBit)	/* sat -0x400..0x3FF */
{
	if (v < -0x400)
	{
		v = -0x400;
		flagB(flagBit);
	}
	else if (v > 0x3FF)
	{
		v = 0x3FF;
		flagB(flagBit);
	}
	return v;
}

static void pushSXY(int32_t x, int32_t y)
{
	x = satSXY(x, 14);							/* SX2 sat: bit 14 */
	y = satSXY(y, 13);							/* SY2 sat: bit 13 */
	s_dr[12] = s_dr[13];
	s_dr[13] = s_dr[14];
	s_dr[14] = (uint32_t)((x & 0xFFFF) | ((uint32_t)y << 16));
}

static void pushColorFromMAC(void)				/* MAC1-3 >> 4, sat 0..FF */
{
	int32_t c[3];
	for (int i = 0; i < 3; i++)
	{
		int32_t v = (int32_t)s_dr[25 + i] >> 4;
		if (v < 0)
		{
			v = 0;
			flagB(21 - i);						/* R/G/B sat: bits 21/20/19 */
		}
		else if (v > 0xFF)
		{
			v = 0xFF;
			flagB(21 - i);
		}
		c[i] = v;
	}
	uint32_t code = s_dr[6] & 0xFF000000u;		/* RGBC code byte rides along */
	s_dr[20] = s_dr[21];
	s_dr[21] = s_dr[22];
	s_dr[22] = code | (uint32_t)(c[2] << 16) | (uint32_t)(c[1] << 8) | (uint32_t)c[0];
}

/*****************************************************************************/
/*	UNR reciprocal division (RTPS/RTPT's h/sz3).  Table and algorithm are
	psx-spx verbatim; the table is generated from its closed form (entry 0
	must be 0xFF, entry 256 must be 0).  */

static uint8_t	s_unr[257];
static int		s_unrInit;

static void unrInit(void)
{
	if (s_unrInit)
		return;
	for (int i = 0; i <= 256; i++)
	{
		int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
		s_unr[i] = (uint8_t)(v < 0 ? 0 : v);
	}
	s_unrInit = 1;
}

static uint32_t divUNR(uint32_t h, uint32_t sz3)
{
	unrInit();
	if (h < sz3 * 2)
	{
		int z = 0;								/* 16-bit leading zeros of sz3 */
		while (!(sz3 & (0x8000u >> z)))
			z++;
		uint64_t n = (uint64_t)h << z;
		uint32_t d = sz3 << z;
		uint32_t u = s_unr[(d - 0x7FC0) >> 7] + 0x101;
		d = (uint32_t)((0x2000080ull - (uint64_t)d * u) >> 8);
		d = (uint32_t)((0x0000080ull + (uint64_t)d * u) >> 8);
		uint64_t r = ((n * d) + 0x8000) >> 16;
		return (uint32_t)(r > 0x1FFFF ? 0x1FFFF : r);
	}
	flagB(17);									/* divide overflow */
	return 0x1FFFF;
}

/*****************************************************************************/
/*	AVSZ3/AVSZ4 - OTZ = ZSFn * (sum of the newest n entries of the SZ FIFO),
	>>12 and Z-saturated.  One helper: the two ops differ only in the scale
	register and how far back into the FIFO they reach.  */

static void opAVSZ(int zsfReg, int n)
{
	int64_t sum = 0;
	for (int i = 4 - n; i < 4; i++)
		sum += (uint16_t)s_dr[16 + i];			/* SZ0..SZ3 = dr16..dr19 */

	int64_t m = (int64_t)(int16_t)s_cr[zsfReg] * sum;
	setMAC0(m);
	s_dr[7] = satZ(m >> 12);					/* OTZ */
}

/*****************************************************************************/
/*	The RT/TR control words RTPS and RTPT project through.  Unpacked once
	per instruction (RtTr) rather than once per vertex: RTPT is the hottest
	path in the whole core - gte_rtpt_b runs per triangle over all visible
	level geometry (source/level/layertile3d.cpp, source/gfx/actor.cpp) -
	and the packed-halfword decode is pure repeated work there, since only
	GTE_WriteCtrl can change it.  */
struct RtTr
{
	int16_t	m[3][3];
	int32_t	tr[3];
};

static void loadRtTr(RtTr *g)
{
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++)
			g->m[i][j] = mtxEl(0, i, j);
		g->tr[i] = trReg(i);
	}
}

/*	RTPS core, shared with RTPT.  dq: run the depth-cue calc (RTPS always;
	RTPT only on the last vertex).  */

static void rtpsCore(const RtTr *g, int v, int sf, int lm, int dq)
{
	int		shift = sf * 12;
	int16_t	vx = vXofs(v), vy = vYofs(v), vz = vZofs(v);
	int64_t	mac[4], full3 = 0;

	for (int i = 1; i <= 3; i++)
	{
		int64_t m = shl64(g->tr[i - 1], 12);
		m = ext44(i, m + (int64_t)g->m[i - 1][0] * vx);
		m = ext44(i, m + (int64_t)g->m[i - 1][1] * vy);
		m = ext44(i, m + (int64_t)g->m[i - 1][2] * vz);
		if (i == 3)
			full3 = m;
		mac[i] = m >> shift;
		s_dr[24 + i] = (uint32_t)(int32_t)mac[i];
	}
	setIR(1, (int32_t)mac[1], lm);
	setIR(2, (int32_t)mac[2], lm);

	/*	IR3 hardware quirk: the saturation FLAG tests MAC3 >> 12 (the sf=1
		view) regardless of sf, while the stored value clamps the actual
		shifted MAC3 without re-flagging.  */
	{
		int64_t chk = full3 >> 12;
		if (chk < -0x8000 || chk > 0x7FFF)
			flagB(22);
		int32_t ir3 = (int32_t)mac[3];
		int32_t lo  = lm ? 0 : -0x8000;
		if (ir3 < lo)
			ir3 = lo;
		else if (ir3 > 0x7FFF)
			ir3 = 0x7FFF;
		s_dr[11] = (uint32_t)ir3;
	}

	pushSZ(full3 >> 12);						/* SZ3 always the sf=1 view */

	uint32_t div = divUNR((uint16_t)s_cr[26], (uint16_t)s_dr[19]);
	int64_t	 m0;
	m0 = (int64_t)div * irReg(1) + (int32_t)s_cr[24];		/* OFX */
	setMAC0(m0);
	int32_t sx = (int32_t)(m0 >> 16);
	m0 = (int64_t)div * irReg(2) + (int32_t)s_cr[25];		/* OFY */
	setMAC0(m0);
	int32_t sy = (int32_t)(m0 >> 16);
	pushSXY(sx, sy);

	if (dq)
	{
		m0 = (int64_t)div * (int16_t)s_cr[27] + (int32_t)s_cr[28];	/* DQA/DQB */
		setMAC0(m0);
		setIR0((int32_t)(m0 >> 12));
	}
}

/*****************************************************************************/
/*	MVMVA - the general matrix*vector+constant op.  All the rt/rtv/ll/lc
	macro families are presets of this (see the DMPSX table in gteport.cpp).  */

static void opMVMVA(uint32_t inst)
{
	int sf  = (inst >> 19) & 1;
	int mxs = (inst >> 17) & 3;
	int vs  = (inst >> 15) & 3;
	int cvs = (inst >> 13) & 3;
	int lm  = (inst >> 10) & 1;
	int shift = sf * 12;

	int16_t vx, vy, vz;
	if (vs == 3)
	{
		vx = irReg(1);
		vy = irReg(2);
		vz = irReg(3);
	}
	else
	{
		vx = vXofs(vs);
		vy = vYofs(vs);
		vz = vZofs(vs);
	}

	int16_t m[3][3];
	if (mxs == 3)
	{
		/*	The "no matrix" selection is a hardware bug: a garbage matrix
			assembled from RGBC.r and RT elements.  Reproduced verbatim.  */
		int16_t r = (int16_t)((s_dr[6] & 0xFF) << 4);
		m[0][0] = (int16_t)-r;   m[0][1] = r;            m[0][2] = irReg(0);
		m[1][0] = mtxEl(0, 0, 2); m[1][1] = mtxEl(0, 0, 2); m[1][2] = mtxEl(0, 0, 2);
		m[2][0] = mtxEl(0, 1, 1); m[2][1] = mtxEl(0, 1, 1); m[2][2] = mtxEl(0, 1, 1);
	}
	else
	{
		int base = mxs * 8;						/* 0=RT, 8=LLM, 16=LCM */
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				m[i][j] = mtxEl(base, i, j);
	}

	int32_t cv[3] = { 0, 0, 0 };
	if (cvs == 0)
		for (int i = 0; i < 3; i++) cv[i] = trReg(i);
	else if (cvs == 1)
		for (int i = 0; i < 3; i++) cv[i] = bkReg(i);
	else if (cvs == 2)
		for (int i = 0; i < 3; i++) cv[i] = fcReg(i);

	for (int i = 1; i <= 3; i++)
	{
		int64_t mac;
		if (cvs == 2)
		{
			/*	Far-colour bug: the CV+M*vx term is computed only for its
				side effects (MAC overflow flags + an IR saturation CHECK
				with lm=0), then discarded; the result restarts from the
				vy term.  */
			int64_t bug = ext44(i, shl64(cv[i - 1], 12)
								   + (int64_t)m[i - 1][0] * vx);
			int32_t chk = (int32_t)(bug >> shift);
			if (chk < -0x8000 || chk > 0x7FFF)
				flagB(25 - i);
			mac = ext44(i, (int64_t)m[i - 1][1] * vy);
			mac = ext44(i, mac + (int64_t)m[i - 1][2] * vz);
		}
		else
		{
			mac = ext44(i, shl64(cv[i - 1], 12)
						   + (int64_t)m[i - 1][0] * vx);
			mac = ext44(i, mac + (int64_t)m[i - 1][1] * vy);
			mac = ext44(i, mac + (int64_t)m[i - 1][2] * vz);
		}
		mac >>= shift;
		s_dr[24 + i] = (uint32_t)(int32_t)mac;
		setIR(i, (int32_t)mac, lm);
	}
}

/*****************************************************************************/
/*	Depth-cue interpolation core, shared by DPCS/DPCT/INTPL:
	MAC += (FC - MAC) * IR0 with the documented intermediate IR write.  */

static void depthCue(int sf, int lm, int64_t base1, int64_t base2, int64_t base3)
{
	int		shift = sf * 12;
	int64_t base[4] = { 0, base1, base2, base3 };

	for (int i = 1; i <= 3; i++)
	{
		int64_t d = ext44(i, shl64(fcReg(i - 1), 12) - base[i]);
		setIR(i, (int32_t)(d >> shift), 0);		/* intermediate: lm forced 0 */
		int64_t mac = ext44(i, base[i] + (int64_t)irReg(i) * irReg(0)) >> shift;
		s_dr[24 + i] = (uint32_t)(int32_t)mac;
		setIR(i, (int32_t)mac, lm);
	}
	pushColorFromMAC();
}

/*****************************************************************************/
extern "C" void GTE_ExecuteCop2(uint32_t inst)
{
	int sf = (inst >> 19) & 1;
	int lm = (inst >> 10) & 1;
	int shift = sf * 12;

	s_flag = 0;

	switch (inst & 0x3F)
	{
	case 0x01:									/* RTPS */
	{
		RtTr g;
		loadRtTr(&g);
		rtpsCore(&g, 0, sf, lm, 1);
		break;
	}

	case 0x30:									/* RTPT */
	{
		RtTr g;
		loadRtTr(&g);							/* once, not once per vertex */
		rtpsCore(&g, 0, sf, lm, 0);
		rtpsCore(&g, 1, sf, lm, 0);
		rtpsCore(&g, 2, sf, lm, 1);
		break;
	}

	case 0x12:									/* MVMVA */
		opMVMVA(inst);
		break;

	case 0x06:									/* NCLIP */
	{
		int16_t sx0 = (int16_t)s_dr[12], sy0 = (int16_t)(s_dr[12] >> 16);
		int16_t sx1 = (int16_t)s_dr[13], sy1 = (int16_t)(s_dr[13] >> 16);
		int16_t sx2 = (int16_t)s_dr[14], sy2 = (int16_t)(s_dr[14] >> 16);
		setMAC0((int64_t)sx0 * sy1 + (int64_t)sx1 * sy2 + (int64_t)sx2 * sy0
			  - (int64_t)sx0 * sy2 - (int64_t)sx1 * sy0 - (int64_t)sx2 * sy1);
		break;
	}

	case 0x2D:									/* AVSZ3 */
		opAVSZ(29, 3);
		break;

	case 0x2E:									/* AVSZ4 */
		opAVSZ(30, 4);
		break;

	case 0x28:									/* SQR */
		for (int i = 1; i <= 3; i++)
		{
			int32_t mac = (int32_t)(((int64_t)irReg(i) * irReg(i)) >> shift);
			s_dr[24 + i] = (uint32_t)mac;
			setIR(i, mac, lm);
		}
		break;

	case 0x0C:									/* OP - outer product */
	{
		int16_t d1 = mtxEl(0, 0, 0), d2 = mtxEl(0, 1, 1), d3 = mtxEl(0, 2, 2);
		int16_t i1 = irReg(1), i2 = irReg(2), i3 = irReg(3);
		int64_t mac[4];
		mac[1] = ext44(1, (int64_t)i3 * d2 - (int64_t)i2 * d3) >> shift;
		mac[2] = ext44(2, (int64_t)i1 * d3 - (int64_t)i3 * d1) >> shift;
		mac[3] = ext44(3, (int64_t)i2 * d1 - (int64_t)i1 * d2) >> shift;
		for (int i = 1; i <= 3; i++)
		{
			s_dr[24 + i] = (uint32_t)(int32_t)mac[i];
			setIR(i, (int32_t)mac[i], lm);
		}
		break;
	}

	case 0x3D:									/* GPF: MAC = IR * IR0 */
		for (int i = 1; i <= 3; i++)
		{
			int32_t mac = (int32_t)(((int64_t)irReg(i) * irReg(0)) >> shift);
			s_dr[24 + i] = (uint32_t)mac;
			setIR(i, mac, lm);
		}
		pushColorFromMAC();
		break;

	case 0x3E:									/* GPL: MAC = MAC<<sf + IR*IR0 */
		for (int i = 1; i <= 3; i++)
		{
			int64_t mac = ext44(i, shl64((int32_t)s_dr[24 + i], shift)
								   + (int64_t)irReg(i) * irReg(0)) >> shift;
			s_dr[24 + i] = (uint32_t)(int32_t)mac;
			setIR(i, (int32_t)mac, lm);
		}
		pushColorFromMAC();
		break;

	case 0x10:									/* DPCS: from RGBC */
		depthCue(sf, lm,
				 (int64_t)(s_dr[6] & 0xFF) << 16,
				 (int64_t)((s_dr[6] >> 8) & 0xFF) << 16,
				 (int64_t)((s_dr[6] >> 16) & 0xFF) << 16);
		break;

	case 0x2A:									/* DPCT: 3x from RGB0 (fifo) */
		for (int n = 0; n < 3; n++)
			depthCue(sf, lm,
					 (int64_t)(s_dr[20] & 0xFF) << 16,
					 (int64_t)((s_dr[20] >> 8) & 0xFF) << 16,
					 (int64_t)((s_dr[20] >> 16) & 0xFF) << 16);
		break;

	case 0x11:									/* INTPL: from IR1-3 */
		depthCue(sf, lm,
				 shl64(irReg(1), 12),
				 shl64(irReg(2), 12),
				 shl64(irReg(3), 12));
		break;

	default:
		/*	NC lighting family / CC / CDP / DCPL: no game code path reaches
			them (grep census in the M3 plan) - loudly absent, not silent.  */
		PSYQ_LOG_ONCE_KEYED(inst & 0x3F,
			"[gte] unimplemented cop2 op 0x%02X (unreached by game code "
			"- see gte_core.cpp)\n", (unsigned)(inst & 0x3F));
		break;
	}

	commitFlag();
}

/*****************************************************************************/
/*	Register file access - mtc2/mfc2/ctc2/cfc2 conversions  */

static uint32_t lzc(uint32_t v)
{
	/*	LZCR = number of leading bits equal to the sign bit (1..32):
		leading zeros for positive values, leading ones for negative.  */
	uint32_t bit = v & 0x80000000u;
	int n = 0;
	while (n < 32 && (v & 0x80000000u) == bit)
	{
		n++;
		v <<= 1;
	}
	return (uint32_t)n;
}

extern "C" void GTE_WriteData(int reg, uint32_t v)
{
	reg &= 31;
	switch (reg)
	{
	case 15:									/* SXYP: write pushes the fifo */
		s_dr[12] = s_dr[13];
		s_dr[13] = s_dr[14];
		s_dr[14] = v;
		break;
	case 28:									/* IRGB: unpacks into IR1-3 */
		s_dr[28] = v & 0x7FFF;
		s_dr[9]  = (v & 0x1F) * 0x80;
		s_dr[10] = ((v >> 5) & 0x1F) * 0x80;
		s_dr[11] = ((v >> 10) & 0x1F) * 0x80;
		break;
	case 30:									/* LZCS: computes LZCR */
		s_dr[30] = v;
		s_dr[31] = lzc(v);
		break;
	case 29:
	case 31:									/* ORGB / LZCR: read-only */
		break;
	default:
		s_dr[reg] = v;
		break;
	}
}

extern "C" uint32_t GTE_ReadData(int reg)
{
	reg &= 31;
	switch (reg)
	{
	case 1: case 3: case 5:						/* VZ0-2: sign-extended */
	case 8: case 9: case 10: case 11:			/* IR0-3: sign-extended */
		return (uint32_t)(int32_t)(int16_t)s_dr[reg];
	case 7:										/* OTZ */
	case 16: case 17: case 18: case 19:			/* SZ0-3 */
		return s_dr[reg] & 0xFFFF;
	case 15:									/* SXYP reads SXY2 */
		return s_dr[14];
	case 28:
	case 29:									/* IRGB/ORGB read: pack IR1-3 */
	{
		uint32_t c = 0;
		for (int i = 0; i < 3; i++)
		{
			int32_t v = (int32_t)(int16_t)s_dr[9 + i] / 0x80;
			if (v < 0)
				v = 0;
			else if (v > 0x1F)
				v = 0x1F;
			c |= (uint32_t)v << (i * 5);
		}
		return c;
	}
	default:
		return s_dr[reg];
	}
}

extern "C" void GTE_WriteCtrl(int reg, uint32_t v)
{
	reg &= 31;
	if (reg == 31)
	{
		v &= 0x7FFFF000u;
		if (v & 0x7F87E000u)
			v |= 0x80000000u;
	}
	s_cr[reg] = v;
}

extern "C" uint32_t GTE_ReadCtrl(int reg)
{
	reg &= 31;
	switch (reg)
	{
	case 4: case 12: case 20:					/* R33/L33/LC33: sign-extended */
	case 26:									/* H: unsigned, but cfc2 sign-
												   extends it - hardware bug  */
	case 27:									/* DQA */
	case 29: case 30:							/* ZSF3/ZSF4 */
		return (uint32_t)(int32_t)(int16_t)s_cr[reg];
	default:
		return s_cr[reg];
	}
}
