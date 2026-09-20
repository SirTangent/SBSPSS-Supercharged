/*	M1 headless file-I/O proof.

	Drives the game's own file plane - CFileIO over the libcd shim over
	BIGLUMP.BIN - with zero rendering.  Mirrors the pre-GPU slice of the real
	boot (main.cpp): GetAllFilePos -> MemInit -> CFileIO::Init, then loads
	real assets by FileEquate and checks sizes against the FAT.

	Exit code 0 == the M1 exit criterion holds.  Compiled with the game
	flavour (gnu++98, game includes/defines) since it consumes game headers.
*/
#include <stdio.h>

#include "system\global.h"
#include "fileio\fileio.h"

/*	The known-good FAT values verified in M0 (FAT parser + LZNP round-trip):
	entry 0 SYSTEM_CACHE at 2048, 3920 bytes; entry 1 SPRITES_SPRITES_SPR at
	4096.

	The sprite size tracks the contents of Sprites.Spr, so it moves whenever
	a bitmap is added to the in-game banks in makefile.gfx.  It was 134604
	before the 14 keyboard key caps of github issue #43 (+168, one 12-byte
	frame header each).  */
static const s32 EXPECT_CACHE_POS   = 2048;
static const s32 EXPECT_CACHE_SIZE  = 3920;
static const s32 EXPECT_SPRITES_POS = 4096;
static const s32 EXPECT_SPRITES_SIZE= 134772;

static int	s_failures;

static void check(int ok, char *what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		s_failures++;
	}
}

static u32 crc32buf(const u8 *p, s32 len)
{
	u32 crc = 0xffffffff;
	for (s32 i = 0; i < len; i++)
	{
		crc ^= p[i];
		for (int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
	}
	return crc ^ 0xffffffff;
}

static void loadAndReport(FileEquate fe, char *name)
{
	s32	size = CFileIO::getFileSize(fe);
	u8	*data = CFileIO::loadFile(fe, "headless");

	check(size > 0, "file has a size in the FAT");
	check(data != NULL, "loadFile returned a buffer");
	if (data)
	{
		printf("loaded %-32s size=%8ld sector=%6ld crc32=%08lx\n",
			   name, (long)size, (long)CFileIO::getFileSector(fe),
			   (unsigned long)crc32buf(data, size));
		MemFree(data);
	}
}

int main()
{
	printf("SBSPSS headless file-I/O proof (%s/%s/%s)\n",
		   INF_Territory, INF_Version, INF_FileSystem);

	/* pre-GPU boot slice, same order as system/main.cpp */
	CFileIO::GetAllFilePos();
	MemInit();
	CFileIO::Init();

	/* dump the FAT: every entry, position and size */
	printf("\nFAT: %d entries\n", (int)FileEquate_MAX);
	for (int i = 0; i < (int)FileEquate_MAX; i++)
	{
		FileEquate fe = (FileEquate)i;
		printf("  [%3d] pos=%9ld size=%9ld\n",
			   i, (long)CFileIO::getFileOffset(fe), (long)CFileIO::getFileSize(fe));
	}

	/* spot-check against the M0-verified values */
	check(CFileIO::getFileOffset(SYSTEM_CACHE) == EXPECT_CACHE_POS,   "SYSTEM_CACHE FAT position");
	check(CFileIO::getFileSize(SYSTEM_CACHE)   == EXPECT_CACHE_SIZE,  "SYSTEM_CACHE FAT size");
	check(CFileIO::getFileOffset(SPRITES_SPRITES_SPR) == EXPECT_SPRITES_POS, "SPRITES_SPRITES_SPR FAT position");
	check(CFileIO::getFileSize(SPRITES_SPRITES_SPR)   == EXPECT_SPRITES_SIZE,"SPRITES_SPRITES_SPR FAT size");

	printf("\n");
	loadAndReport(SPRITES_SPRITES_SPR,          "SPRITES_SPRITES_SPR");
	loadAndReport(LEVELS_CHAPTER01_LEVEL01_LVL, "LEVELS_CHAPTER01_LEVEL01_LVL");
	loadAndReport(SYSTEM_CACHE,                 "SYSTEM_CACHE");

	if (s_failures)
	{
		printf("\nheadless proof FAILED (%d checks)\n", s_failures);
		return 1;
	}
	printf("\nheadless proof PASSED\n");
	return 0;
}
