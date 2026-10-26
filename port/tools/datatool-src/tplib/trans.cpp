/*	trans.cpp - transtext's text databases (issue #48); the rules are in
	trans.h.  The addresses are the 1999 transtext.exe's, where each piece was
	read off; its messages are kept word for word.
*/
#include <string.h>
#include <fstream>

#include <gobject.hpp>
#include <pcre.h>

#include "trans.h"

/* 0x455848 / 0x455868 */
static char const * const	s_names[NUM_OF_LANGUAGES]=
	{"eng","fre","ger","ita","spa","dut","swe","jap"};
static char const * const	s_americanNames[NUM_OF_LANGUAGES]=
	{"eng","ame","ger","ita","spa","dut","swe","jap"};

/* 0x40cd50 */
char const * getLanguageName(int lang,bool american)
{
	if ((unsigned)lang >= NUM_OF_LANGUAGES)
		GObject::Error(ERR_FATAL,"illegal language number");

	return(american ? s_americanNames[lang] : s_names[lang]);
}

/* 0x40cd90 */
int getLanguageNum(char const * name,bool american)
{
	for (int f=0;f<NUM_OF_LANGUAGES;f++)
		{
		if (!_stricmp(name,getLanguageName(f,american)))
			return(f);
		}

	GObject::Error(ERR_FATAL,"can't find language type %s",name);
	return(0);
}

/*----------------------------------------------------------------------
	TextDatabase
  ---------------------------------------------------------------------- */
void TextDatabase::addString(GString const & str)
{
	m_strings.push_back(str);
}

int TextDatabase::getNumOfStrings(void) const
{
	return(int(m_strings.size()));
}

char const * TextDatabase::getText(int stringNum) const
{
	if (stringNum < 0 || stringNum >= getNumOfStrings())
		{
		GObject::Error(ERR_FATAL,"Illegal id");
		return(NULL);
		}

	return(m_strings[stringNum]);
}

static void putU32(std::ofstream & out,unsigned int val)
{
	char	bytes[4]={char(val),char(val>>8),char(val>>16),char(val>>24)};
	out.write(bytes,4);
}

/* 0x40bff0: the count, each string's file offset, then the strings */
void TextDatabase::write(char const * fileName) const
{
	std::ofstream	out(fileName,std::ios::out|std::ios::trunc|std::ios::binary);

	if (!out)
		GObject::Error(ERR_FATAL,"can't open %s",fileName);

	unsigned int	numOfStrings=unsigned(m_strings.size());
	unsigned int	offset=4+4*numOfStrings;

	putU32(out,numOfStrings);
	for (unsigned int f=0;f<numOfStrings;f++)
		{
		putU32(out,offset);
		offset+=m_strings[f].Len()+1;
		}

	for (unsigned int f=0;f<numOfStrings;f++)
		out.write(m_strings[f],m_strings[f].Len()+1);
}

/*----------------------------------------------------------------------
	AllTextDatabases
  ---------------------------------------------------------------------- */
AllTextDatabases::AllTextDatabases(void)
{
	m_american=false;
}

/* 0x40bca0 */
void AllTextDatabases::readTranslationFile(char const * fileName,bool american)
{
	m_american=american;
	open(fileName);

	while (nextLineIsCommand())
		{
		GString		texts[NUM_OF_LANGUAGES];
		GString		id;

		id=peekNextCommand();
		getNextLine();

		while (!nextLineIsCommand() && !eof())
			{
			int		lineNum=getLineCount();		// the count before this line is read
			readLine(getNextLine(),lineNum,texts);
			}

		int		numOfTexts=0;
		for (int f=0;f<NUM_OF_LANGUAGES;f++)
			{
			if (!texts[f].Empty())
				numOfTexts++;
			}

		if (numOfTexts)
			{
			if (texts[ENGLISH].Empty())
				GObject::Error(ERR_FATAL,"String %s has no english text",(char const *)id);

			GString		englishFormat(getFormat(texts[ENGLISH]));

			for (int f=0;f<NUM_OF_LANGUAGES;f++)
				{
				if (!texts[f].Empty() && !(getFormat(texts[f])==englishFormat))
					GObject::Error(ERR_FATAL,"Error with string id %s\nlanguage %s doesn't match with english string",(char const *)id,getLanguageName(f,m_american));
				}

			for (int f=0;f<NUM_OF_LANGUAGES;f++)
				{
				if (texts[f].Empty())
					texts[f]=texts[ENGLISH];
				}

			m_ids.push_back(id);

			for (int f=0;f<NUM_OF_LANGUAGES;f++)
				m_databases[f].addString(texts[f]);
			}
		}

	close();
}

/* 0x40b9b0: one `<language>=<text>` line */
void AllTextDatabases::readLine(char const * line,int lineNum,GString * texts)
{
	static pcre *	pattern;

	if (!pattern)
		{
		char const *	errText;
		int				errOffset;
		pattern=pcre_compile("^(.+)\\s*=\\s*(.+)\\s*",0,&errText,&errOffset,NULL);
		}

	const int	numOfOffsets=20;
	int			offsets[numOfOffsets];

	if (pcre_exec(pattern,NULL,line,int(strlen(line)),0,offsets,numOfOffsets) != 3)
		{
		GObject::Error(ERR_FATAL,"line %d: ill formated line %s",lineNum,line);
		return;
		}

	GString		name(&line[offsets[2]],offsets[3]-offsets[2]);
	int			lang=getLanguageNum(name,m_american);
	GString		text(&line[offsets[4]],offsets[5]-offsets[4]);

	text.Replace("\\n","\n");
	texts[lang]=text;
}

/* 0x40bb90: a string's printf conversions, each from its `%` up to and
   including the next lower-case letter (islower in the "C" locale; the exe
   passes the char sign-extended, and bytes from 0x80 count as not lower);
   fatal if one is still open at the last character.  One character has none. */
static bool isLower(char c)
{
	return(c >= 'a' && c <= 'z');
}

GString AllTextDatabases::getFormat(char const * str)
{
	GString		ret;
	int			len=int(strlen(str));

	if (len > 1)
		{
		for (int f=0;f<len;f++)
			{
			if (str[f]=='%')
				{
				do
					{
					ret.Append(str[f]);
					if (f==len-1 && !isLower(str[f]))
						GObject::Error(ERR_FATAL,"error with format of string %s",str);
					f++;
					}
				while (!isLower(str[f]));

				ret.Append(str[f]);
				}
			}
		}

	return(ret);
}

/* 0x40c380 */
void AllTextDatabases::write(char const * dir)
{
	GString		prefix(dir);
	prefix.Append('\\');

	for (int f=0;f<NUM_OF_LANGUAGES;f++)
		{
		GString		fileName(prefix);
		fileName+=GString(getLanguageName(f,m_american));
		fileName+=GString(".dat");
		m_databases[f].write(fileName);
		}

	TextDatabase	idDatabase;
	TextDatabase &	english=m_databases[ENGLISH];

	for (int f=0;f<int(m_ids.size());f++)
		{
		GString		idText(m_ids[f]);
		idText+=GString(" ");
		idText+=getFormat(english.getText(f));
		idDatabase.addString(idText);
		}

	GString		fileName(prefix);
	fileName+=GString("id.dat");
	idDatabase.write(fileName);
}

/* 0x40c8a0: through a text stream, so CRLF */
void AllTextDatabases::writeHeaderFile(char const * fileName)
{
	std::ofstream	out(fileName,std::ios::out);

	if (!out)
		GObject::Error(ERR_FATAL,"unable to open file %s for writing",fileName);

	out<<"#ifndef __STRING_ENUMS__\n#define __STRING_ENUMS__\nenum\n{\n"<<std::endl;
	for (int f=0;f<int(m_ids.size());f++)
		out<<"\t"<<m_ids[f]<<","<<std::endl;
	out<<"};\n#endif"<<std::endl;
}

TextDatabase * AllTextDatabases::getTextDataBase(int lang)
{
	getLanguageName(lang,false);		// the range check
	return(&m_databases[lang]);
}
