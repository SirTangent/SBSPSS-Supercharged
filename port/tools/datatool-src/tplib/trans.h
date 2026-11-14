/*	trans.h - transtext's text databases (issue #48).

	transtext's main.cpp survives in Utils/transtext, but the Theme Park
	library classes it drives (AllTextDatabases, TextDatabase) never reached
	this repo.  This is a rewrite from the 1999 transtext.exe, its
	disassembly and its output, holding the interface main.cpp calls.

	The input is lang.pl's final.dat, read through Utils/transtext's
	ScriptFile (CRLF lines; each cut at the first `;` and its last character,
	the CR, dropped; blank lines skipped):

		[STR__ID]
		eng=English text
		ger=German text
		...

	- While the next line is a [command]: it names a string, and the lines up
	  to the next command are `<language>=<text>`, split by the PCRE
	  `^(.+)\s*=\s*(.+)\s*` (so the language name runs to the last `=`, and
	  the text loses its leading white space unless it is all white space).
	  A line that does not match is fatal; the same language twice keeps the
	  last.  `\n` in the text becomes a newline; every other byte is kept.
	- A string with no lines is dropped.  One with no English is fatal.
	  Every language's printf conversions (getFormat) must match the
	  English, and a language left out takes the English text.
	- Lines before the first command end the file, as do LF-only lines
	  (their last character is not a CR).  A repeated ID is a second string.

	write() writes `<dir>\<language>.dat` for all eight languages, then
	`<dir>\id.dat`, whose string i is `<ID> ` plus the English string's
	conversions.  Each is the game's TransHeader (source/locale/textdbase.cpp):
	a u32 count, a u32 file offset for each string, then the strings with
	their NULs, back to back.  writeHeaderFile() writes the STR__ enum, CRLF,
	in input order.
*/
#ifndef SBSP_TPLIB_TRANS_H
#define SBSP_TPLIB_TRANS_H

#include <vector>

#include <gstring.hpp>
#include <script.h>

#include "lang.h"

/* one language's strings, in input order */
class TextDatabase
{
public:
	void			addString(GString const & str);
	int				getNumOfStrings(void) const;
	char const *	getText(int stringNum) const;		// fatal "Illegal id" if out of range
	void			write(char const * fileName) const;

private:
	std::vector<GString>	m_strings;
};

class AllTextDatabases : protected ScriptFile
{
public:
	AllTextDatabases(void);

	void			readTranslationFile(char const * fileName,bool american=false);
	void			write(char const * dir);
	void			writeHeaderFile(char const * fileName);
	TextDatabase *	getTextDataBase(int lang);

private:
	bool					m_american;
	std::vector<GString>	m_ids;
	TextDatabase			m_databases[NUM_OF_LANGUAGES];

	void			readLine(char const * line,int lineNum,GString * texts);
	static GString	getFormat(char const * str);
};

#endif
