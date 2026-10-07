/*
	roundtrip_test.cpp — proof that lznp_encode.cpp produces streams the GAME's
	own decoder (source/utils/lznp.cpp) reconstructs byte-for-byte.

	Build + run from the repo root:
		g++ -O2 -I source/utils port/tools/lznp-src/roundtrip_test.cpp \
		    port/tools/lznp-src/lznp_encode.cpp source/utils/lznp.cpp -o roundtrip
		./roundtrip [files...]
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lznp_encode.h"

int LZNP_Decode(unsigned char *in, unsigned char *out);	/* the game's decoder */

static int testOne(const unsigned char *data, int n, const char *name)
{
	unsigned char *packed = (unsigned char *)malloc(n + n / 2 + 4096);
	unsigned char *out = (unsigned char *)malloc(n + 4096);
	int packedSize = LZNP_Encode(packed, data, n);
	int outSize = LZNP_Decode(packed, out);
	int ok = (outSize == n) && (memcmp(out, data, n) == 0);
	printf("%-12s in=%7d packed=%7d out=%7d  %s\n", name, n, packedSize, outSize,
		ok ? "OK" : "MISMATCH");
	free(packed);
	free(out);
	return ok;
}

int main(int argc, char *argv[])
{
	int pass = 1;

	static unsigned char buf[200000];
	memset(buf, 0, sizeof(buf));
	pass &= testOne(buf, 65536, "zeros");
	for (int i = 0; i < 100000; i++) buf[i] = (unsigned char)(i % 7 + 'a');
	pass &= testOne(buf, 100000, "repeats");
	unsigned s = 12345;
	for (int i = 0; i < 100000; i++) { s = s * 1103515245u + 12345u; buf[i] = (unsigned char)(s >> 16); }
	pass &= testOne(buf, 100000, "noise");
	/* mixed: runs + noise + copies at varied distances (exercises pair + superlen) */
	for (int i = 0; i < 150000; i++)
	{
		if ((i / 700) % 3 == 0)      buf[i] = (unsigned char)(i / 700);
		else if ((i / 700) % 3 == 1) { s = s * 69069u + 1u; buf[i] = (unsigned char)(s >> 24); }
		else                          buf[i] = buf[i - 137];
	}
	pass &= testOne(buf, 150000, "mixed");
	/* fuzz: many small random buffers with random run structure */
	for (int t = 0; t < 200; t++)
	{
		int n = 1 + (int)(s % 3000); s = s * 1103515245u + 12345u;
		for (int i = 0; i < n; i++)
		{
			s = s * 1103515245u + 12345u;
			buf[i] = ((s >> 20) & 3) ? (i > 0 ? buf[i - 1] : 0) : (unsigned char)(s >> 16);
		}
		if (!testOne(buf, n, "fuzz")) { pass = 0; break; }
	}

	/*	A file named here is an input the caller expects to be tested (ctest
		passes the committed port/tests/str_frame1.bin): one that cannot be
		read fails the run rather than being skipped, which read as a pass
		with the fixture gone (review of #78).  */
	for (int a = 1; a < argc; a++)
	{
		FILE *f = fopen(argv[a], "rb");
		if (!f) { printf("FAIL: cannot open %s\n", argv[a]); pass = 0; continue; }
		fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
		if (n > 4000000) n = 4000000;
		unsigned char *d = (unsigned char *)malloc(n > 0 ? n : 1);
		if (n <= 0 || (long)fread(d, 1, n, f) != n)
		{
			printf("FAIL: cannot read %s\n", argv[a]);
			pass = 0;
			fclose(f);
			free(d);
			continue;
		}
		fclose(f);
		pass &= testOne(d, (int)n, argv[a] + (strlen(argv[a]) > 30 ? strlen(argv[a]) - 30 : 0));
		free(d);
	}

	printf(pass ? "ALL ROUND-TRIPS PASS\n" : "FAILURES PRESENT\n");
	return pass ? 0 : 1;
}
