/*	Compile-time layout checks for the structs the game overlays on file data.

	The level and actor files are written by prebuilt 32-bit tools
	(MkLevel / MkActor fwrite these structs whole), so their sizes are part of
	the file format.  A build whose pointers are not 4 bytes must see the
	pointer fields as FPTR<T> (SBSP_PC64, tools/Data/include/fptr.h); one that
	does not - the define lost, a new raw pointer member added - would compile
	cleanly and then read every file at the wrong offsets.  This TU is built
	with the game's own flags and include path, so it measures what the game
	measures, and fails the build instead.

	gnu++98: no static_assert - a negative array size is the error.
*/
#include "system\global.h"
#include <dstructs.h>

#define ABI_CHECK(name, cond)	typedef char abi_check_##name[(cond) ? 1 : -1]

ABI_CHECK(sLayerShadeHdr,	sizeof(sLayerShadeHdr)	== 24);
ABI_CHECK(sLevelHdr,		sizeof(sLevelHdr)		== 80);
ABI_CHECK(sSpriteFrameGfx,	sizeof(sSpriteFrameGfx)	== 12);
ABI_CHECK(sSpriteAnim,		sizeof(sSpriteAnim)		== 8);
ABI_CHECK(sSpriteAnimBank,	sizeof(sSpriteAnimBank)	== 20);

/* pointer-free, but every one of them is indexed straight out of a file */
ABI_CHECK(sLayerHdr,		sizeof(sLayerHdr)		== 16);
ABI_CHECK(sLayerRGBHdr,		sizeof(sLayerRGBHdr)	== 8);
ABI_CHECK(sElem2d,			sizeof(sElem2d)			== 8);
ABI_CHECK(sElem3d,			sizeof(sElem3d)			== 16);
ABI_CHECK(sTri,				sizeof(sTri)			== 24);
ABI_CHECK(sQuad,			sizeof(sQuad)			== 28);
ABI_CHECK(sSpriteFrame,		sizeof(sSpriteFrame)	== 4);
ABI_CHECK(sThingActor,		sizeof(sThingActor)		== 14);

#if defined(SBSP_PC64)
ABI_CHECK(FPTR,				sizeof(FPTR<char>)		== 4);
ABI_CHECK(pointer,			sizeof(void *)			== 8);
#else
ABI_CHECK(pointer,			sizeof(void *)			== 4);
#endif
