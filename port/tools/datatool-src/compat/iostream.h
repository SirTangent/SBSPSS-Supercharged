/*	<iostream.h> for the 1999 data-tool sources (issue #48): VC6's
	pre-standard iostreams lived in the global namespace, which is all the
	sources rely on.  On the include path of the data-tool targets only.  */
#ifndef SBSP_COMPAT_IOSTREAM_H
#define SBSP_COMPAT_IOSTREAM_H
#include <iostream>
using namespace std;
#endif
