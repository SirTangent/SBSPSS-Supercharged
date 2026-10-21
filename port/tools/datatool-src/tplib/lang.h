/*	lang.h - transtext's languages (issue #48).

	transtext.exe was built against the Theme Park library's lang.h, which never
	reached this repo; this is its language list as the 1999 exe has it.  Its
	two name tables (file offsets 0x55848 and 0x55868) differ in slot 1 only:
	`fre`, or `ame` under -a.  A name in the input file is matched against the
	table without regard to case (_strcmpi), and the same table names the
	output files.
*/
#ifndef SBSP_TPLIB_LANG_H
#define SBSP_TPLIB_LANG_H

enum LANGUAGE
{
	ENGLISH,
	FRENCH,
	GERMAN,
	ITALIAN,
	SPANISH,
	DUTCH,
	SWEDISH,
	JAPANESE,

	NUM_OF_LANGUAGES,
};

/* the name of language number `lang` (fatal past the last), with slot 1 `ame` if `american` */
char const *	getLanguageName(int lang,bool american);

/* the number of the language called `name`; fatal if there is none */
int				getLanguageNum(char const * name,bool american);

#endif
