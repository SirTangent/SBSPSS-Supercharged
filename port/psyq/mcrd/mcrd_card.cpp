/*	Standard 128KB PS1 memory-card image (see mcrd_card.h).

	Layout (per no$psx and what DuckStation reads):
	  block 0 = system block, 64 x 128-byte frames:
	    frame 0      header: "MC", zero fill, XOR checksum in byte 127
	    frames 1-15  directory frames for data blocks 1-15:
	                   +0x00 u32  state: 0xA0 free, 0x51/52/53 in-use
	                              first/middle/last, 0xA1/A2/A3 freed
	                   +0x04 u32  file size in BYTES (first-link frame only)
	                   +0x08 u16  next block's dir index, 0xFFFF = end
	                   +0x0A      filename, 20 chars + NUL
	                   +0x7F      XOR checksum of bytes 0..126
	    frames 16-35 broken-sector list: sector u32 0xFFFFFFFF = none
	    frames 36-55 broken-sector replacement data, 0xFF fill (no checksum)
	    frames 56-62 unused, 0xFF fill
	    frame 63     write-test frame: a copy of frame 0
	  blocks 1-15 = file data, 8192 bytes each.

	The whole image lives in memory; every mutation rewrites the host file
	via a temp-file + rename so a crash mid-write cannot corrupt the save,
	and is undone in memory if that write fails (txBegin/txCommit).

	Host-file policy (Card_Open, cardFlush): an existing card0.mcd is never
	formatted or replaced behind the user's back.  Only a card that is
	provably missing is created, and the create refuses to land on a file
	that appeared meanwhile.  Anything that exists but cannot be loaded as
	a 128KB image - unreadable (access denied, a sharing lock, a cloud
	placeholder that will not hydrate, a read error) or the wrong size
	(including 0 bytes) - is left alone and the session runs with no card.
	A 128KB image that is not formatted loads as-is; only the game's own
	format UI (Card_Format) writes over it.
*/
/*	WIN32_LEAN_AND_MEAN keeps winsock out: its `#define s_addr S_un.S_addr`
	rewrites the s_addr member of the EXEC struct in the PSY-Q <kernel.h>
	included below, which is a compile error.  */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>			/* MoveFileExA - atomic replace, see cardFlush */
#include <errno.h>
#include <io.h>					/* _commit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <kernel.h>				/* DIRENTRY */

#include "mcrd_card.h"

static uint8_t	g_card[CARD_IMAGE_SIZE];
/*	Paths are narrow in the process code page, UTF-8 once the exe's manifest
	says so (issue #62): up to three bytes a character, so a MAX_PATH-length
	directory needs ~780 bytes.  The file names ride on top of the directory
	with room to spare.  */
static const size_t	SAVE_DIR_MAX = 1024;
static char		g_cardPath[SAVE_DIR_MAX + 32];	/* dir + "\\card0.mcd" */
static int		g_opened;			/* 0 = never, 1 = ok, -1 = host unusable */
static uint8_t	g_undo[CARD_IMAGE_SIZE];	/* g_card before the mutation in flight */

/*****************************************************************************/
/*	little-endian field access inside a frame  */

static void st16(uint8_t *p, unsigned v)	{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void st32(uint8_t *p, unsigned v)	{ st16(p, v); st16(p + 2, v >> 16); }
static unsigned ld16(const uint8_t *p)		{ return p[0] | (p[1] << 8); }
static unsigned ld32(const uint8_t *p)		{ return ld16(p) | (ld16(p + 2) << 16); }

static uint8_t *frame(int n)		{ return &g_card[n * CARD_FRAME_SIZE]; }
static uint8_t *dirFrame(int block)	{ return frame(block); }	/* dir frame n <-> block n */
static uint8_t *blockData(int block){ return &g_card[block * CARD_BLOCK_SIZE]; }

static void frameChecksum(uint8_t *f)
{
	uint8_t x = 0;
	for (int i = 0; i < CARD_FRAME_SIZE - 1; i++)
		x ^= f[i];
	f[CARD_FRAME_SIZE - 1] = x;
}

static int stateInUse(unsigned s)	{ return (s & 0xF0) == 0x50; }
static int stateFirst(unsigned s)	{ return s == 0x51; }

/*****************************************************************************/
/*	host file  */

extern "C" int Port_SaveDir(char *dst, size_t n);	/* host/hostpath.cpp */

/*	SBSP_SAVE_DIR verbatim, else saves\ beside the exe, else
	%APPDATA%\SBSPSS - created if needed.  Resolved on every open: the
	tests re-point the variable between opens.
	Returns 0 if the path does not fit: a truncated one names some other
	file, so it is never opened - the session runs with no card instead.  */
static int resolvePath(void)
{
	char dir[SAVE_DIR_MAX];
	g_cardPath[0] = 0;
	if (!Port_SaveDir(dir, sizeof(dir)))
	{
		fprintf(stderr, "[mcrd] save directory path is longer than %u bytes - "
						"no card this run\n", (unsigned)(sizeof(dir) - 1));
		return 0;
	}
	int len = snprintf(g_cardPath, sizeof(g_cardPath), "%s\\card0.mcd", dir);
	if (len < 0 || (size_t)len >= sizeof(g_cardPath))
	{
		fprintf(stderr, "[mcrd] card path under %s is too long - no card this run\n", dir);
		g_cardPath[0] = 0;
		return 0;
	}
	return 1;
}

/*	Write the image to the host file.  `replace` is 1 for every save (the
	card exists and is being updated) and 0 only when Card_Open creates a
	missing card: then the move will not land on a file that appeared
	since Card_Open looked, so nothing is ever overwritten that was not
	loaded first.

	"flush" itself is a PSY-Q macro (R3000.H) - hence the name  */
static CardResult cardFlush(int replace)
{
	char tmp[sizeof(g_cardPath) + 8];
	int  len = snprintf(tmp, sizeof(tmp), "%s.tmp", g_cardPath);
	if (len < 0 || (size_t)len >= sizeof(tmp))
	{
		fprintf(stderr, "[mcrd] temp path for %s is too long\n", g_cardPath);
		return CARD_IO_ERROR;
	}

	FILE *f = fopen(tmp, "wb");
	if (!f)
	{
		fprintf(stderr, "[mcrd] cannot write %s\n", tmp);
		return CARD_IO_ERROR;
	}
	/*	The data must be on disk before the rename is: MOVEFILE_WRITE_THROUGH
		only makes the move itself durable, so without the commit a power
		cut right after a save can leave a renamed card0.mcd whose contents
		never reached the disk.  */
	size_t wrote = fwrite(g_card, 1, CARD_IMAGE_SIZE, f);
	int    ok    = wrote == CARD_IMAGE_SIZE && fflush(f) == 0 &&
				   _commit(_fileno(f)) == 0;
	if (fclose(f) != 0 || !ok)
	{
		fprintf(stderr, "[mcrd] short write to %s\n", tmp);
		remove(tmp);
		return CARD_IO_ERROR;
	}
	/*	ONE operation, not remove()+rename().  The CRT rename() will not
		overwrite on Windows, and deleting the card first means a failed or
		interrupted rename (the file held open by antivirus, a shell preview
		or a second instance) leaves NO card0.mcd at all - the next launch
		would find nothing, format a blank card and lose every save.  */
	DWORD flags = MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0);
	if (!MoveFileExA(tmp, g_cardPath, flags))
	{
		fprintf(stderr, "[mcrd] cannot move %s into place (error %lu) - %s\n",
				tmp, (unsigned long)GetLastError(),
				replace ? "the previous save is still intact"
						: "not creating the card over whatever is there now");
		remove(tmp);
		return CARD_IO_ERROR;
	}
	return CARD_OK;
}

/*	Every mutation is a transaction: txBegin() before touching g_card,
	txCommit() to persist it.  If the host file cannot be written, or
	the mutation itself fails partway (txAbort), g_card goes back to what
	it was, so the in-memory card always equals card0.mcd.  Without this
	one failed first save left a zero-filled file in memory only: the
	re-scan rejected it (no "SC" magic) and every retry's CreateFile of
	the same name hit CARD_FILE_EXISTS until the game was restarted.  */
static void txBegin(void)	{ memcpy(g_undo, g_card, CARD_IMAGE_SIZE); }
static void txAbort(void)	{ memcpy(g_card, g_undo, CARD_IMAGE_SIZE); }

static CardResult txCommit(void)
{
	CardResult r = cardFlush(1);
	if (r != CARD_OK)
		txAbort();
	return r;
}

/*****************************************************************************/

static void formatImage(void)
{
	memset(g_card, 0, CARD_IMAGE_SIZE);

	uint8_t *hdr = frame(0);
	hdr[0] = 'M';
	hdr[1] = 'C';
	frameChecksum(hdr);

	for (int b = 1; b <= CARD_DATA_BLOCKS; b++)
	{
		uint8_t *d = dirFrame(b);
		st32(d + 0x00, 0xA0);		/* free */
		st32(d + 0x04, 0);
		st16(d + 0x08, 0xFFFF);
		frameChecksum(d);
	}
	for (int i = 16; i <= 35; i++)	/* broken-sector list: none */
	{
		uint8_t *f = frame(i);
		st32(f, 0xFFFFFFFF);
		frameChecksum(f);
	}
	for (int i = 36; i <= 62; i++)	/* replacement data + unused */
		memset(frame(i), 0xFF, CARD_FRAME_SIZE);
	memcpy(frame(63), frame(0), CARD_FRAME_SIZE);	/* write-test frame */
}

int Card_IsFormatted(void)
{
	const uint8_t *hdr = frame(0);
	return hdr[0] == 'M' && hdr[1] == 'C';
}

CardResult Card_Open(void)
{
	if (g_opened)
		return g_opened > 0 ? CARD_OK : CARD_IO_ERROR;

	/*	Every refusal below latches g_opened = -1: the rest of the session
		runs with no card (the game shows its own "no memory card" screens)
		and the file is left for the user to sort out.  No retry - a card
		that came and went mid-session would only confuse the game's scan.  */
	if (!resolvePath())
	{
		g_opened = -1;
		return CARD_IO_ERROR;
	}

	errno = 0;
	FILE *f = fopen(g_cardPath, "rb");
	int   openErrno = errno;
	DWORD openError = GetLastError();
	if (f)
	{
		size_t got   = fread(g_card, 1, CARD_IMAGE_SIZE, f);
		int    extra = !ferror(f) && got == CARD_IMAGE_SIZE && fgetc(f) != EOF;
		int    bad   = ferror(f);
		int    readErrno = errno;
		DWORD  readError = GetLastError();
		fclose(f);
		if (!bad && got == CARD_IMAGE_SIZE && !extra)
		{
			g_opened = 1;
			return CARD_OK;			/* may still be unformatted - that is a
									   state the game handles, not an error */
		}
		if (bad)
			/*	it opened, but the data would not come: a cloud placeholder
				that failed to hydrate, a failing disk  */
			fprintf(stderr, "[mcrd] read error on %s (errno %d, error %lu) - "
							"leaving it alone; no card this run\n",
					g_cardPath, readErrno, (unsigned long)readError);
		else
			/*	Something is there, but it is not a 128KB card image: a .vmp
				(128-byte header + 128KB), a padded dump, a file another
				process is still writing, or an empty file.  Formatting over
				it would destroy whatever it is on the strength of one stderr
				line.  */
			fprintf(stderr, "[mcrd] %s is not a 128KB card image (%u bytes%s) - "
							"refusing to overwrite it; no card this run\n",
					g_cardPath, (unsigned)got, extra ? "+" : "");
		g_opened = -1;
		return CARD_IO_ERROR;
	}

	/*	fopen failed.  Only a card that is not there at all may be created:
		the CRT must say ENOENT and Windows must agree the name does not
		exist.  Anything else - access denied by an ACL restored from
		another profile, a sharing lock, a placeholder the cloud provider
		refuses to open - means a card0.mcd that may hold every save, and
		formatting a blank one over it would lose them.  */
	DWORD attrError = GetFileAttributesA(g_cardPath) == INVALID_FILE_ATTRIBUTES
					? GetLastError() : ERROR_SUCCESS;
	if (openErrno != ENOENT ||
		(attrError != ERROR_FILE_NOT_FOUND && attrError != ERROR_PATH_NOT_FOUND))
	{
		fprintf(stderr, "[mcrd] cannot read %s (errno %d, error %lu) - "
						"leaving it alone; no card this run\n",
				g_cardPath, openErrno, (unsigned long)openError);
		g_opened = -1;
		return CARD_IO_ERROR;
	}

	formatImage();
	if (cardFlush(0) != CARD_OK)
	{
		g_opened = -1;				/* host unusable -> "no card" */
		return CARD_IO_ERROR;
	}
	fprintf(stderr, "[mcrd] created card image %s\n", g_cardPath);
	g_opened = 1;
	return CARD_OK;
}

CardResult Card_Format(void)
{
	txBegin();
	formatImage();
	return txCommit();
}

CardResult Card_Unformat(void)
{
	txBegin();
	memset(frame(0), 0, CARD_FRAME_SIZE);
	return txCommit();
}

/*****************************************************************************/
/*	directory  */

/*	Next block in a file's chain, or 0 at the end.

	The link is DATA read out of the card image, and a card image is not
	necessarily ours: Card_Open accepts any 128KB file, and importing real
	PS1 saves is a documented use.  An unvalidated link indexes blockData()
	(and dirFrame()) with up to 65535 * 8192, far outside the static image -
	a wild read while loading and a wild WRITE while saving or deleting.  So
	anything that does not address a real data block ends the walk; callers
	additionally cap the hop count, because a chain that links back on
	itself is in range yet never terminates.  */
static int nextBlock(int b)
{
	unsigned next = ld16(dirFrame(b) + 0x08);
	if (next >= (unsigned)CARD_DATA_BLOCKS)
		return 0;				/* 0xFFFF end-of-chain, or corrupt */
	return (int)next + 1;
}

/*	The on-card name field holds up to 20 chars + NUL, but a KERNEL.H
	DIRENTRY.name is 20 bytes, so Card_Dirents hands a 20-char name out as
	its first 19 chars - and the game opens every file by exactly the name
	Dirents gave it (memcard.cpp HandleCmd_ReadFileInfo, then strcpy's it
	for the write/delete paths).  A 20-char name is what a real card holds
	for a product code + 8 chars, e.g. another game's save in an imported
	image; if it could not be looked up, the read would fail,
	InvalidateCard would run and the card would never become valid.

	So: an exact match first; failing that, a 19-char name also resolves
	to a 20-char card name that starts with it.  Should an imported card
	hold two 20-char names sharing those 19 chars, the first wins - there
	is no way to tell them apart through a DIRENTRY.  Card_CreateFile
	refuses to make such a pair (nameTaken).  */
static const int CARD_NAME_MAX   = 20;
static const int DIRENT_NAME_MAX = CARD_NAME_MAX - 1;	/* see Card_Dirents */

static const char *cardName(int block)	{ return (const char *)dirFrame(block) + 0x0A; }

static int findFile(const char *name)
{
	for (int b = 1; b <= CARD_DATA_BLOCKS; b++)
		if (stateFirst(ld32(dirFrame(b))) &&
			strncmp(cardName(b), name, CARD_NAME_MAX) == 0)
			return b;

	if (strnlen(name, CARD_NAME_MAX) != (size_t)DIRENT_NAME_MAX)
		return 0;
	for (int b = 1; b <= CARD_DATA_BLOCKS; b++)
		if (stateFirst(ld32(dirFrame(b))) &&
			strnlen(cardName(b), CARD_NAME_MAX) == (size_t)CARD_NAME_MAX &&
			memcmp(cardName(b), name, DIRENT_NAME_MAX) == 0)
			return b;
	return 0;
}

/*	Would a new file called `name` be listed by Card_Dirents under the same
	name as a file already on the card?  Names up to 18 chars compare
	exactly; at 19 or 20 chars only the 19 a DIRENTRY carries count.
	Refusing those keeps every name Dirents hands out pointing at exactly
	one file.  */
static int nameTaken(const char *name)
{
	for (int b = 1; b <= CARD_DATA_BLOCKS; b++)
		if (stateFirst(ld32(dirFrame(b))) &&
			strncmp(cardName(b), name, DIRENT_NAME_MAX) == 0)
			return 1;
	return 0;
}

long Card_Dirents(struct DIRENTRY *out, long maxEntries)
{
	long count = 0;
	for (int b = 1; b <= CARD_DATA_BLOCKS && count < maxEntries; b++)
	{
		const uint8_t *d = dirFrame(b);
		if (!stateFirst(ld32(d)))
			continue;
		struct DIRENTRY *e = &out[count++];
		memset(e, 0, sizeof(*e));
		/*	one short of the field: the game strcpy()s this name around
			(memcard.cpp:1073), so it must be terminated even if a corrupt
			frame fills all 20 bytes.  A legal 20-char name therefore comes
			out as 19; findFile resolves that back to the file.  */
		memcpy(e->name, d + 0x0A, sizeof(e->name) - 1);
		e->size = (long)ld32(d + 0x04);
		e->attr = 0x50;				/* what a real card reports for a save */
		e->head = b - 1;
	}
	return count;
}

CardResult Card_CreateFile(const char *name, long blocks)
{
	if (!Card_IsFormatted())
		return CARD_NOT_FORMATTED;
	if (blocks < 1 || nameTaken(name))
		return blocks < 1 ? CARD_NO_FILE : CARD_FILE_EXISTS;

	/*	collect enough free blocks (0xA0 fresh or 0xA1-A3 freed)  */
	int chain[CARD_DATA_BLOCKS];
	int got = 0;
	for (int b = 1; b <= CARD_DATA_BLOCKS && got < blocks; b++)
		if (!stateInUse(ld32(dirFrame(b))))
			chain[got++] = b;
	if (got < blocks)
		return CARD_FULL;

	txBegin();
	for (int i = 0; i < blocks; i++)
	{
		uint8_t *d = dirFrame(chain[i]);
		memset(d, 0, CARD_FRAME_SIZE);
		st32(d + 0x00, i == 0 ? 0x51 : (i == blocks - 1 ? 0x53 : 0x52));
		st16(d + 0x08, i == blocks - 1 ? 0xFFFF : (unsigned)(chain[i + 1] - 1));
		if (i == 0)
		{
			st32(d + 0x04, (unsigned)(blocks * CARD_BLOCK_SIZE));
			strncpy((char *)d + 0x0A, name, 20);
		}
		frameChecksum(d);
		memset(blockData(chain[i]), 0, CARD_BLOCK_SIZE);
	}
	return txCommit();
}

CardResult Card_DeleteFile(const char *name)
{
	int b = findFile(name);
	if (!b)
		return CARD_NO_FILE;

	txBegin();
	for (int hops = 0; b && hops < CARD_DATA_BLOCKS; hops++)
	{
		uint8_t *d = dirFrame(b);
		unsigned state = ld32(d);
		int next = nextBlock(b);
		st32(d, (state & 0x0F) | 0xA0);		/* 0x51/52/53 -> 0xA1/A2/A3 */
		frameChecksum(d);
		b = next;
	}
	return txCommit();
}

/*****************************************************************************/
/*	file data access, walking the block chain  */

static CardResult fileSpan(const char *name, long ofs, long bytes,
						   uint8_t *dst, const uint8_t *src)
{
	int b = findFile(name);
	if (!b)
		return CARD_NO_FILE;

	long pos = 0;					/* file offset of the current block */
	int hops = 0;
	while (b && bytes > 0)
	{
		if (hops++ >= CARD_DATA_BLOCKS)
			return CARD_NO_FILE;	/* cyclic chain - see nextBlock */
		if (ofs < pos + CARD_BLOCK_SIZE)
		{
			long start = ofs > pos ? ofs - pos : 0;
			long n     = CARD_BLOCK_SIZE - start;
			if (n > bytes)
				n = bytes;
			if (dst)
				memcpy(dst, blockData(b) + start, n);
			else
				memcpy(blockData(b) + start, src, n);
			dst   += dst ? n : 0;
			src   += src ? n : 0;
			ofs   += n;
			bytes -= n;
		}
		pos += CARD_BLOCK_SIZE;
		b = nextBlock(b);
	}
	return bytes > 0 ? CARD_NO_FILE : CARD_OK;	/* ran off the chain */
}

CardResult Card_ReadFile(const char *name, void *dst, long ofs, long bytes)
{
	return fileSpan(name, ofs, bytes, (uint8_t *)dst, NULL);
}

CardResult Card_WriteFile(const char *name, const void *src, long ofs, long bytes)
{
	/*	fileSpan can fail after writing part of the span (it ran off the
		end of the chain, or the chain is cyclic) - undo that part too  */
	txBegin();
	CardResult r = fileSpan(name, ofs, bytes, NULL, (const uint8_t *)src);
	if (r != CARD_OK)
	{
		txAbort();
		return r;
	}
	return txCommit();
}

/*****************************************************************************/

uint8_t *Card_ImageForTest(void)	{ return g_card; }
const char *Card_PathForTest(void)	{ return g_opened ? g_cardPath : NULL; }
void Card_ResetForTest(void)		{ g_opened = 0; memset(g_card, 0, sizeof(g_card)); }
