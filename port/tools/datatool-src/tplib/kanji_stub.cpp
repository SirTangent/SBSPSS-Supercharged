/*	kanji_stub.cpp - transtext's Japanese output, unavailable (issue #48).

	Utils/transtext/kanjiclass.cpp draws the Japanese font from
	fontdata/got*.h (Gothic 11/13/15-dot kanji bitmaps), which never reached
	this repo, so the rebuilt transtext has no kanji or font output.  The
	data build never asks for it (no -e/-d/-j/-p/-s in makefile.gfx), and
	the game has no Japanese text path.  Each entry main.cpp reaches stops
	with this message.
*/
#include <gobject.hpp>

#include "kanjiclass.h"

static void noKanji(void)
{
	GObject::Error(ERR_FATAL,"kanji and font output need Utils/transtext/fontdata, which is not in this repo (issue #48)");
}

KanjiClass::KanjiClass(void)
{
	noKanji();
}

void KanjiClass::addStr(u8 const *)
{
	noKanji();
}

int KanjiClass::howManyKanjiChars(void)
{
	noKanji();
	return(0);
}

void KanjiClass::saveKanjiLbms(char const *,KanjiFontSize)
{
	noKanji();
}

void KanjiClass::saveKanjiLbmNames(char const *,char const *)
{
	noKanji();
}

void KanjiClass::saveKanjiTable(char const *)
{
	noKanji();
}

void KanjiClass::writeEnglishFontLbms(char const *,KanjiFontSize,char const *)
{
	noKanji();
}
