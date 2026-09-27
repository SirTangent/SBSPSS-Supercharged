/*	The boot seed (issue #58): one decision, made once, for everyone who
	asks - system/main.cpp's setRndSeed at boot, and --record-pad's
	`# seed` header line (host/input.cpp).

	    --seed / SBSP_SEED  >  the recording's `# seed`  >  the game's own

	The game's own is InitSystem's VidGetTickCount(), and at that point it
	is always 0 - on the PlayStation too.  It returns the vblank count the
	last VidSwapDraw stored, and the only two swaps before InitSystem (in
	VidInit) run before VidVSyncCallback is installed, so nothing has
	counted yet.  An unseeded boot is therefore already reproducible, and
	this hook answers 0 ("no seed given") then, so main.cpp's retail arm
	still decides it.  (An earlier cut of #58 fell back to GetTickCount()
	here, which made every unseeded run random and a recording made before
	it unreplayable.)

	The middle tier lets a recording made with --seed replay without one:
	--record-pad writes `# seed` only when a seed was given, and a replay
	that has no --seed adopts it.  An explicit --seed still wins, and says
	so when it differs from the recording's.

	This lives in the shim rather than in args.cpp because input.cpp needs
	it: args.cpp is an OBJECT library linked into the game exes only
	(port/CMakeLists.txt, psyq_args), and a shim archive member that called
	into it would break every shim-only unit exe's link.

	Cached from the first call: CFileIO::Init, loadLanguage and VidInit pump
	before InitSystem reaches setRndSeed, so the recorder can open its file
	- and ask - before the game does.  Whoever asks first fixes the answer
	for everyone.  */
#include <stdio.h>

extern "C" int Port_PadFileSeed(long *seed);	/* host/input.cpp */

static long	g_seed;
static int	g_explicit;		/* --seed / SBSP_SEED given (args.cpp) */
static int	g_decided;
static int	g_have;			/* 1: g_seed is the answer; 0: the game's own */

/*	args.cpp, from its early constructor - before anyone can ask.  */
extern "C" void Port_SeedExplicit(long seed)
{
	g_seed     = seed;
	g_explicit = 1;
}

/*	Hook read by system/main.cpp's InitSystem (and by the recorder): 1 and
	the setRndSeed value when --seed or a replayed recording gave one, else
	0 and the game keeps setRndSeed(VidGetTickCount()).  */
extern "C" int Port_BootSeed(long *seed)
{
	if (!g_decided)
	{
		g_decided = 1;
		long	rec = 0;
		int		fromRec = Port_PadFileSeed(&rec);
		if (g_explicit)
		{
			g_have = 1;
			if (fromRec && rec != g_seed)
				fprintf(stderr, "[input] --seed %ld overrides the recording's # seed %ld\n",
						g_seed, rec);
			fprintf(stderr, "[args] seed %ld (--seed)\n", g_seed);
		}
		else if (fromRec)
		{
			g_have = 1;
			g_seed = rec;
			fprintf(stderr, "[args] seed %ld (recording)\n", g_seed);
		}
	}
	if (g_have)
		*seed = g_seed;
	return g_have;
}
