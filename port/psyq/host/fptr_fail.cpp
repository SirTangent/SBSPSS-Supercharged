/*	Port_FptrRangeFail: the x64 game stored a pointer above 4GB into a 4-byte
	file-overlay pointer field (tools/Data/include/fptr.h).

	Every such field is relocated to an address inside the file it was loaded
	with, and every loaded file lives in the arena, which api/arena.cpp
	reserves below 1GB - so this is unreachable unless a file came from
	somewhere else (a static, the CRT heap).  Truncating would be a wild
	pointer much later; stop here, where the culprit is on the stack.
*/
#include <stdio.h>

#include "host/diag.h"

extern "C" void Port_FptrRangeFail(const void *p)
{
	fprintf(stderr, "[shim] FPTR: %p does not fit a 4-byte pointer field "
					"(loaded file outside the arena?)\n", p);
	Port_Exit(PORT_EXIT_FAULT);
}
