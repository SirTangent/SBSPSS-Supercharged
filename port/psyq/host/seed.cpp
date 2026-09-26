/*	The boot seed (issue #58): one decision, made once, for everyone who
	asks - system/main.cpp's setRndSeed at boot, and --record-pad's
	`# seed` header line (host/input.cpp).

	    --seed / SBSP_SEED  >  the recording's `# seed`  >  the boot tick

	The middle tier is what lets a tester's session replay.  The tester zip
	runs the game without --seed, so the seed used to be the game's own
	VidGetTickCount() and nothing wrote it down; now the shim picks the
	fallback itself (GetTickCount - as arbitrary as the game's), the
	recording carries it, and a replay (--pad-file without --seed) inherits
	it.  An explicit --seed still wins, and says so when it differs from
	the recording's.

	This lives in the shim rather than in args.cpp because input.cpp needs
	it: args.cpp is an OBJECT library linked into the game exes only
	(port/CMakeLists.txt, psyq_args), and a shim archive member that called
	into it would break every shim-only unit exe's link.

	Cached from the first call: CFileIO::Init, loadLanguage and VidInit pump
	before InitSystem reaches setRndSeed, so the recorder can open its file
	- and ask - before the game does.  Whoever asks first fixes the value
	for everyone.  */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

extern "C" int Port_PadFileSeed(long *seed);	/* host/input.cpp */

static long	g_seed;
static int	g_explicit;		/* --seed / SBSP_SEED given (args.cpp) */
static int	g_decided;

/*	args.cpp, from its early constructor - before anyone can ask.  */
extern "C" void Port_SeedExplicit(long seed)
{
	g_seed     = seed;
	g_explicit = 1;
}

/*	Hook read by system/main.cpp's InitSystem (and by the recorder): the
	setRndSeed value.  Always 1 now - the shim owns the fallback too.  */
extern "C" int Port_BootSeed(long *seed)
{
	if (!g_decided)
	{
		g_decided = 1;
		long		rec = 0;
		int			fromRec = Port_PadFileSeed(&rec);
		const char	*how;
		if (g_explicit)
		{
			how = "--seed";
			if (fromRec && rec != g_seed)
				fprintf(stderr, "[input] --seed %ld overrides the recording's # seed %ld\n",
						g_seed, rec);
		}
		else if (fromRec)
		{
			g_seed = rec;
			how    = "recording";
		}
		else
		{
			g_seed = (long)GetTickCount();
			how    = "boot tick";
		}
		fprintf(stderr, "[args] seed %ld (%s)\n", g_seed, how);
	}
	*seed = g_seed;
	return 1;
}
