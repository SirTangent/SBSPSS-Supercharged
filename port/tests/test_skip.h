/*	A skipped layer is visible, never a pass (issue #61).  A shim test whose
	inputs are absent - the staged movies, a committed fixture, the music
	data - used to print SKIPPED and exit 0, so losing them turned a
	real-data check into a silent pass.  Now it exits 77, the
	SKIP_RETURN_CODE every sbsp_shim_test carries, so ctest reports it as
	skipped rather than passed; and under SBSP_TEST_STRICT=1, which
	build-pc.sh test and CI set because they own the data, it exits 1.
	Failures take precedence: a test calls this only once it has none.  */
#ifndef SBSP_TEST_SKIP_H
#define SBSP_TEST_SKIP_H

#include <stdio.h>
#include <stdlib.h>

static int testSkipExit(const char *test, int skipped)
{
	const char *s = getenv("SBSP_TEST_STRICT");
	int strict = s && *s && *s != '0';
	printf("%s: %d layer(s) SKIPPED - %s\n", test, skipped,
		   strict ? "a FAILURE under SBSP_TEST_STRICT" : "exit 77, which ctest reports as skipped");
	return strict ? 1 : 77;
}

#endif
