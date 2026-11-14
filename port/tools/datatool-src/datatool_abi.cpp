/*	datatool_abi.cpp - linked into the rebuilt MkActor and MkLevel (issue #48).

	Those tools fwrite the file-overlay structs of tools/Data/include/dstructs.h
	whole, and MkLevel walks MapEdit's .MEX export by sizeof() of its structs
	(Utils/MapEdit/ExportHdr.h).  Their sizes are the file formats, as the
	1999 32-bit tools laid them out, so a 64-bit build has to measure the same:
	the pointer fields are FPTR<T> there (dstructs.h, SBSP_PC64), and
	sExpTile's unused RGB pointer is a 4-byte integer.  The sizes are the ones
	port/abi/abi_check.cpp holds the game to, plus the export structs; any
	that drift stop the build here instead of writing every file at the
	wrong offsets.

	Also the range check FPTR<T>::set reports to: the tools store file
	offsets in those fields, never addresses, so one that needs more than 32
	bits is a corrupt offset.
*/
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <DaveLib.h>
#include "../../../Utils/MapEdit/ExportHdr.h"

static_assert(sizeof(void *) == 4 || sizeof(sLevelHdr) == 80,
              "x64 tool build without the 4-byte DPTR fields (dstructs.h, SBSP_PC64)");

/* dstructs.h - the sizes port/abi/abi_check.cpp checks for the game */
static_assert(sizeof(sLayerShadeHdr)			== 24, "sLayerShadeHdr");
static_assert(sizeof(sLevelHdr)					== 80, "sLevelHdr");
static_assert(sizeof(sSpriteFrameGfx)			== 12, "sSpriteFrameGfx");
static_assert(sizeof(sSpriteAnim)				== 8,  "sSpriteAnim");
static_assert(sizeof(sSpriteAnimBank)			== 20, "sSpriteAnimBank");
static_assert(sizeof(sLayerHdr)					== 16, "sLayerHdr");
static_assert(sizeof(sLayerRGBHdr)				== 8,  "sLayerRGBHdr");
static_assert(sizeof(sElem2d)					== 8,  "sElem2d");
static_assert(sizeof(sElem3d)					== 16, "sElem3d");
static_assert(sizeof(sTri)						== 24, "sTri");
static_assert(sizeof(sQuad)						== 28, "sQuad");
static_assert(sizeof(sSpriteFrame)				== 4,  "sSpriteFrame");
static_assert(sizeof(sThingActor)				== 14, "sThingActor");
static_assert(sizeof(sVtx)						== 8,  "sVtx");
static_assert(sizeof(sBBox)						== 8,  "sBBox");
static_assert(sizeof(sModel)					== 12, "sModel");
static_assert(sizeof(sLayerShadeBackGfxType)	== 8,  "sLayerShadeBackGfxType");
static_assert(sizeof(sLayerShadeBackGfx)		== 28, "sLayerShadeBackGfx");

/* MapEdit's export (.MEX), read by MkLevel */
static_assert(sizeof(sExpFileHdr)				== 108, "sExpFileHdr");
static_assert(sizeof(sExpTile)					== 24, "sExpTile");
static_assert(sizeof(sExpTri)					== 68, "sExpTri");
static_assert(sizeof(sExpLayerHdr)				== 16, "sExpLayerHdr");
static_assert(sizeof(sExpLayerTile)				== 4,  "sExpLayerTile");
static_assert(sizeof(sLayerShadeGfx)			== 64, "sLayerShadeGfx");

extern "C" void Port_FptrRangeFail(const void *p)
{
	fprintf(stderr, "FATAL: file offset %p does not fit the 4-byte pointer field\n", p);
	exit(123);
}
