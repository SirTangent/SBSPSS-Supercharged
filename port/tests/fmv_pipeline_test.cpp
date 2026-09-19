/*	fmv_pipeline_test (M7): the whole movie pipeline, headless, over the
	real staged data - CdlSeekL/CdRead2/StSetRing/StGetNext (str_stream)
	-> DecDCTvlc3 (vlc3) -> DecDCTin/out (mdec) - for the first 30 frames
	of each movie, exactly as fmv.cpp chains them.

	Each decoded 320x240 frame is converted from the 16px-column
	macroblock stream to raster RGB24 and CRC32'd against the committed
	goldens (port/tests/fmv_crc_<movie>.txt) - the CI-stable form of
	issue #9's "validate frame checksums" clause; the ffmpeg tolerance
	cross-check runs once at golden-generation time via
	check_fmv_ffmpeg.py over the same raw dumps.

	Env knobs (generation, not CI):
	  SBSP_WRITE_GOLDEN=1     rewrite the golden files from this build
	  SBSP_FMV_DUMP_RAW=<dir> also dump <movie>_NNN.rgb raster frames

	Needs the staged movies (the .STR files under out/.../CD, or SBSP_DATA_DIR); skips
	gracefully when they are absent (run port/build-data.cmd first).
*/
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

#include <sys/types.h>
#include <libcd.h>
#include <libpress.h>

#include "cd/xa_stream.h"
#include "cd/str_stream.h"
#include "spu/spu_core.h"

extern "C" int DecDCTvlc3(unsigned long *bs, unsigned long *buf);
extern "C" int DecDCTvlcSize3(int breaksize);
extern "C" void DecDCTvlcBuild3(unsigned short *table);

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

static uint32_t crc32buf(const uint8_t *p, size_t n)
{
	uint32_t crc = 0xFFFFFFFFu;
	while (n--)
	{
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return crc ^ 0xFFFFFFFFu;
}

static u_long		g_ring[32 * 512];
static u_long		g_vlc[19280];			/* 77120B = the game's vlcbuf */
static uint8_t		g_stream[300 * 768];	/* MB-column pixel stream */
static uint8_t		g_raster[320 * 240 * 3];

static const int kFrames = 30;

static int runMovie(const char *diskName, const char *slug)
{
	CdlFILE cf;
	CdlFILE *found = CdSearchFile(&cf, (char *)diskName);
	if (!found || cf.size == 0)
	{
		std::printf("fmv_pipeline_test: %s not staged - SKIPPED "
					"(run port/build-data.cmd)\n", diskName);
		return 0;
	}

	char goldenPath[256];
	snprintf(goldenPath, sizeof(goldenPath), "port/tests/fmv_crc_%s.txt", slug);
	uint32_t golden[kFrames];
	int goldenCount = 0;
	int writeGolden = 0;
	{
		const char *w = getenv("SBSP_WRITE_GOLDEN");
		writeGolden = (w && *w && *w != '0');
	}
	if (!writeGolden)
	{
		FILE *g = fopen(goldenPath, "rb");
		if (!g)
		{
			std::printf("fmv_pipeline_test: %s missing - SKIPPED (generate "
						"with SBSP_WRITE_GOLDEN=1)\n", goldenPath);
			return 0;
		}
		char line[64];
		while (goldenCount < kFrames && fgets(line, sizeof(line), g))
			golden[goldenCount++] = (uint32_t)strtoul(line, NULL, 16);
		fclose(g);
		check(goldenCount == kFrames, "golden file has 30 CRCs");
	}

	const char *rawDir = getenv("SBSP_FMV_DUMP_RAW");

	StrStream_ResetForTest();
	Spu_CdInClear();
	StSetRing(g_ring, 32);
	StSetStream(1, 1, 0xFFFFFFFFu, 0, 0);
	DecDCTvlcBuild3(NULL);
	DecDCTvlcSize3(320 * 240);
	DecDCTReset(0);
	DecDCToutCallback(0);

	CdlLOC loc = cf.pos;
	CdControl(CdlSeekL, (u_char *)&loc, 0);
	CdRead2(0x1C0);						/* CdlModeStream|Speed|RT */

	FILE *goldenOut = NULL;
	if (writeGolden)
	{
		goldenOut = fopen(goldenPath, "wb");
		check(goldenOut != NULL, "golden file writable");
	}

	int bad = 0;
	for (int f = 0; f < kFrames; f++)
	{
		/*	up to 10 sectors per frame; 4 pumped vblanks = 10 credits.
			Nothing renders audio here, so drain the CD ring by hand or
			the movie soundtrack trips the overflow diagnostic.  */
		Spu_CdInClear();
		for (int i = 0; i < 4; i++)
			Port_CdVblank(60);
		u_long *addr = 0, *hdr = 0;
		if (StGetNext(&addr, &hdr) != 0)
		{
			check(false, "frame available");
			break;
		}
		DecDCTvlc3(addr, g_vlc);
		StFreeRing(addr);
		DecDCTin(g_vlc, 3);
		DecDCTout((u_long *)g_stream, (int)sizeof(g_stream) / 4);

		/*	16px-column macroblock stream -> raster  */
		for (int mb = 0; mb < 300; mb++)
		{
			int col = mb / 15, row = mb % 15;
			for (int py = 0; py < 16; py++)
				memcpy(g_raster + ((row * 16 + py) * 320 + col * 16) * 3,
					   g_stream + mb * 768 + py * 48, 48);
		}
		uint32_t crc = crc32buf(g_raster, sizeof(g_raster));
		if (goldenOut)
			fprintf(goldenOut, "%08lX\n", (unsigned long)crc);
		else if (crc != golden[f])
		{
			std::printf("  %s frame %d: crc %08lX want %08lX\n",
						slug, f + 1, (unsigned long)crc,
						(unsigned long)golden[f]);
			bad++;
		}
		if (rawDir)
		{
			char p[512];
			snprintf(p, sizeof(p), "%s/%s_%03d.rgb", rawDir, slug, f + 1);
			FILE *o = fopen(p, "wb");
			if (o)
			{
				fwrite(g_raster, 1, sizeof(g_raster), o);
				fclose(o);
			}
		}
	}
	if (!writeGolden)
		check(bad == 0, slug);
	if (goldenOut)
	{
		fclose(goldenOut);
		std::printf("fmv_pipeline_test: wrote %s\n", goldenPath);
	}
	CdControlB(CdlPause, 0, 0);
	StUnSetRing();
	return 1;
}

int main(void)
{
	int ran = 0;
	ran += runMovie((char *)"\\THQ.STR;1", "thq");
	ran += runMovie((char *)"\\CLIMAX.STR;1", "climax");
	ran += runMovie((char *)"\\INTRO.STR;1", "intro");
	ran += runMovie((char *)"\\DEMO.STR;1", "demo");
	if (ran)
		std::printf("fmv_pipeline_test: %d movie(s) checked\n", ran);

	if (g_failures)
	{
		std::printf("fmv_pipeline_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("fmv_pipeline_test: all passed\n");
	return 0;
}
