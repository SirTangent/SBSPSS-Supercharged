/*	Unit tests for the M6 memory card: the 128KB card-image filesystem
	(port/psyq/mcrd/mcrd_card.cpp) and the libmcrd protocol over it
	(mcrd_shim.cpp).

	Checks the image is DuckStation-shaped (magic, per-frame XOR checksums,
	free directory), the create/write/read/dirent round-trip the game's
	save/load flow performs, multi-block chains, delete/reuse, the
	Unformat -> McErrNotFormat -> Format recovery the in-game format UI
	drives, capacity errors, that every mutation is persisted to the host
	file, and that a relaunch (Card_Open on that existing file) loads it
	back and reads the save.

	Runs against SBSP_SAVE_DIR=./mcrd_test_tmp so a developer's real
	%APPDATA%\SBSPSS card is never touched.
*/
/*	CreateFileA and ACLs, to make a card unreadable.  WIN32_LEAN_AND_MEAN
	keeps winsock's s_addr macro away from the PSY-Q headers below.  */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>				/* a deny-read ACE, see denyRead */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <direct.h>

#include <sys/types.h>
#include <libmcrd.h>

#include "mcrd/mcrd_card.h"

static int g_failures;

static void check(bool ok, const char *what)
{
	if (!ok)
	{
		std::printf("FAIL: %s\n", what);
		g_failures++;
	}
}

/*	XOR of all 128 bytes of a checksummed frame is 0  */
static bool frameOk(const uint8_t *img, int frameNo)
{
	uint8_t x = 0;
	for (int i = 0; i < CARD_FRAME_SIZE; i++)
		x ^= img[frameNo * CARD_FRAME_SIZE + i];
	return x == 0;
}

/*	drain the shim's one-deep completion latch; returns the result  */
static long syncResult(long expectCmd, const char *what)
{
	long cmds = -99, rslt = -99;
	long status = MemCardSync(1, &cmds, &rslt);
	char msg[96];
	std::snprintf(msg, sizeof(msg), "%s: Sync completed", what);
	check(status == 1, msg);
	std::snprintf(msg, sizeof(msg), "%s: Sync cmd tag", what);
	check(cmds == expectCmd, msg);
	std::snprintf(msg, sizeof(msg), "%s: Sync idle after pop", what);
	check(MemCardSync(1, &cmds, &rslt) == -1, msg);
	return rslt;
}

static const char *FNAME = "BASLUS-01352";

/*	The later sections each get a directory of their own:
	SBSP_SAVE_DIR=<dir> and a freshly formatted card opened there.  */
static void freshCard(const char *dir)
{
	static char env[64];
	char path[64];
	_mkdir(dir);
	std::snprintf(path, sizeof(path), "%s\\card0.mcd", dir);
	remove(path);							/* stale run */
	std::snprintf(env, sizeof(env), "SBSP_SAVE_DIR=%s", dir);
	_putenv(env);
	Card_ResetForTest();
	check(Card_Open() == CARD_OK, "fresh card opens");
}

/*	up to CARD_IMAGE_SIZE bytes of a file into buf; -1 if it cannot be
	opened, CARD_IMAGE_SIZE + 1 if it is longer than a card  */
static long readAll(const char *path, uint8_t *buf)
{
	FILE *f = std::fopen(path, "rb");
	if (!f)
		return -1;
	long got = (long)std::fread(buf, 1, CARD_IMAGE_SIZE, f);
	if (got == CARD_IMAGE_SIZE && std::fgetc(f) != EOF)
		got++;
	std::fclose(f);
	return got;
}

static void writeAll(const char *path, const uint8_t *buf, long bytes)
{
	FILE *f = std::fopen(path, "wb");
	check(f != NULL, "test card file written");
	if (f)
	{
		std::fwrite(buf, 1, bytes, f);
		std::fclose(f);
	}
}

/*	What another process holding the card does: open it with no sharing,
	so fopen fails with EACCES (a sharing violation) and MoveFileEx cannot
	replace it.  */
static HANDLE lockNoShare(const char *path)
{
	HANDLE h = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING,
						   FILE_ATTRIBUTE_NORMAL, NULL);
	check(h != INVALID_HANDLE_VALUE, "card locked with no sharing");
	return h;
}

static void unlock(HANDLE h)
{
	if (h != INVALID_HANDLE_VALUE)
		CloseHandle(h);
}

/*	What an ACL restored from another profile does: add a DENY ACE for
	FILE_READ_DATA to Everyone.  fopen fails with EACCES while the
	directory still lets the file be replaced - the case that used to
	format a blank card over it.  Returns the original security descriptor
	for allowRead, or NULL.  */
static PSECURITY_DESCRIPTOR denyRead(const char *path)
{
	PACL oldDacl = NULL, newDacl = NULL;
	PSECURITY_DESCRIPTOR sd = NULL;
	PSID everyone = NULL;
	SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
	DWORD r = GetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
									NULL, NULL, &oldDacl, NULL, &sd);
	if (r == ERROR_SUCCESS &&
		!AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID,
								  0, 0, 0, 0, 0, 0, 0, &everyone))
		r = GetLastError();
	if (r == ERROR_SUCCESS)
	{
		EXPLICIT_ACCESSA ea;
		memset(&ea, 0, sizeof(ea));
		ea.grfAccessPermissions = FILE_READ_DATA;
		ea.grfAccessMode        = DENY_ACCESS;
		ea.grfInheritance       = NO_INHERITANCE;
		ea.Trustee.TrusteeForm  = TRUSTEE_IS_SID;
		ea.Trustee.TrusteeType  = TRUSTEE_IS_WELL_KNOWN_GROUP;
		ea.Trustee.ptstrName    = (LPSTR)everyone;
		r = SetEntriesInAclA(1, &ea, oldDacl, &newDacl);
	}
	if (r == ERROR_SUCCESS)
		r = SetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
								  NULL, NULL, newDacl, NULL);
	if (newDacl)
		LocalFree(newDacl);
	if (everyone)
		FreeSid(everyone);
	check(r == ERROR_SUCCESS, "card made unreadable by a deny-read ACE");
	if (r != ERROR_SUCCESS && sd)
	{
		LocalFree(sd);
		sd = NULL;
	}
	return sd;
}

static void allowRead(const char *path, PSECURITY_DESCRIPTOR sd)
{
	if (!sd)
		return;
	BOOL present = FALSE, defaulted = FALSE;
	PACL dacl = NULL;
	GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
	check(SetNamedSecurityInfoA((LPSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
								NULL, NULL, dacl, NULL) == ERROR_SUCCESS,
		  "card's original ACL restored");
	LocalFree(sd);
}

static bool exists(const char *path)
{
	return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void dropDir(const char *dir)
{
	char path[64];
	std::snprintf(path, sizeof(path), "%s\\card0.mcd", dir);
	remove(path);
	std::snprintf(path, sizeof(path), "%s\\card0.mcd.tmp", dir);
	remove(path);
	_rmdir(dir);
}

int main(void)
{
	_putenv("SBSP_SAVE_DIR=mcrd_test_tmp");
	remove("mcrd_test_tmp\\card0.mcd");		/* stale run */

	/*	-------- protocol: idle sync, exist, accept on a fresh card  */
	check(MemCardSync(1, NULL, NULL) == -1, "Sync idle before any command");

	check(MemCardExist(0) == 1, "Exist registers");
	check(syncResult(McFuncExist, "Exist") == McErrNone, "fresh card exists");

	check(MemCardAccept(0) == 1, "Accept registers");
	check(syncResult(McFuncAccept, "Accept") == McErrNone, "fresh card formatted");

	check(MemCardExist(0x10) == 1, "Exist chan 0x10 registers");
	check(syncResult(McFuncExist, "Exist slot 2") == McErrCardNotExist,
		  "slot 2 reports no card");

	/*	-------- fresh image is DuckStation-shaped  */
	const uint8_t *img = Card_ImageForTest();
	check(img[0] == 'M' && img[1] == 'C', "header magic MC");
	check(frameOk(img, 0), "header frame checksum");
	for (int b = 1; b <= 15; b++)
	{
		check(frameOk(img, b), "directory frame checksum");
		check(img[b * CARD_FRAME_SIZE] == 0xA0, "directory frame free (0xA0)");
	}
	for (int i = 16; i <= 35; i++)
		check(frameOk(img, i), "broken-sector-list frame checksum");
	check(frameOk(img, 63), "write-test frame checksum");
	check(memcmp(img, img + 63 * CARD_FRAME_SIZE, CARD_FRAME_SIZE) == 0,
		  "write-test frame mirrors the header");

	FILE *f = fopen("mcrd_test_tmp\\card0.mcd", "rb");
	check(f != NULL, "card0.mcd created on disk");
	if (f) fclose(f);

	DIRENTRY dir[15];
	long files = -1;
	check(MemCardGetDirentry(0, (char *)"*", dir, &files, 0, 15) == McErrNone,
		  "GetDirentry on empty card");
	check(files == 0, "empty card has no files");

	/*	-------- the game's save flow: create + write + rescan + read  */
	static unsigned char save[8192], back[8192];
	for (int i = 0; i < 8192; i++)
		save[i] = (unsigned char)(i * 7 + 3);
	save[0] = 'S'; save[1] = 'C';

	check(MemCardCreateFile(0, (char *)FNAME, 1) == McErrNone, "CreateFile");
	check(MemCardCreateFile(0, (char *)FNAME, 1) == McErrAlreadyExist,
		  "duplicate CreateFile rejected");

	check(MemCardWriteFile(0, (char *)FNAME, (unsigned long *)save, 0, 8192) == 1,
		  "WriteFile registers");
	check(syncResult(McFuncWriteFile, "WriteFile") == McErrNone, "write ok");

	files = -1;
	check(MemCardGetDirentry(0, (char *)"*", dir, &files, 0, 15) == McErrNone &&
		  files == 1, "one file after save");
	check(strncmp(dir[0].name, FNAME, 12) == 0, "dirent name");
	check(dir[0].size == 8192, "dirent size in bytes");

	/*	the 128-byte file-info read the scan performs, then the full read  */
	memset(back, 0, sizeof(back));
	check(MemCardReadFile(0, dir[0].name, (unsigned long *)back, 0, 128) == 1,
		  "info read registers");
	check(syncResult(McFuncReadFile, "info read") == McErrNone, "info read ok");
	check(back[0] == 'S' && back[1] == 'C', "info read sees the SC magic");

	memset(back, 0, sizeof(back));
	check(MemCardReadFile(0, (char *)FNAME, (unsigned long *)back, 0, 8192) == 1,
		  "full read registers");
	check(syncResult(McFuncReadFile, "full read") == McErrNone, "full read ok");
	check(memcmp(save, back, 8192) == 0, "read data matches written data");

	check(MemCardReadFile(0, (char *)"BASLUS-99999", (unsigned long *)back, 0, 128) == 1,
		  "missing-file read still registers");
	check(syncResult(McFuncReadFile, "missing read") == McErrFileNotExist,
		  "missing file reported via the completion");

	/*	-------- persistence: the host file holds exactly the live image  */
	f = fopen("mcrd_test_tmp\\card0.mcd", "rb");
	check(f != NULL, "card0.mcd readable after save");
	if (f)
	{
		static uint8_t onDisk[CARD_IMAGE_SIZE];
		size_t got = fread(onDisk, 1, CARD_IMAGE_SIZE, f);
		fclose(f);
		check(got == CARD_IMAGE_SIZE, "on-disk image is 128KB");
		check(memcmp(onDisk, img, CARD_IMAGE_SIZE) == 0,
			  "on-disk image matches memory (relaunch loads this)");

		/*	-------- relaunch: drop the open latch and load that file back
			through Card_Open's existing-image branch, then walk the game's
			scan (dirents, then the full read) on the loaded image  */
		Card_ResetForTest();
		check(Card_Open() == CARD_OK, "reopen: existing card image opens");
		check(Card_IsFormatted() != 0, "reopen: loaded image is formatted");
		check(memcmp(onDisk, img, CARD_IMAGE_SIZE) == 0,
			  "reopen: loaded image equals the file on disk");

		files = -1;
		check(MemCardGetDirentry(0, (char *)"*", dir, &files, 0, 15) == McErrNone &&
			  files == 1, "reopen: one file listed");
		check(strcmp(dir[0].name, FNAME) == 0, "reopen: dirent name");
		check(dir[0].size == 8192, "reopen: dirent size");

		memset(back, 0, sizeof(back));
		check(MemCardReadFile(0, dir[0].name, (unsigned long *)back, 0, 8192) == 1,
			  "reopen: full read registers");
		check(syncResult(McFuncReadFile, "reopen read") == McErrNone,
			  "reopen: full read ok");
		check(memcmp(save, back, 8192) == 0, "reopen: the save reads back");
	}

	/*	-------- multi-block chain + delete/reuse  */
	static unsigned char big[3 * 8192];
	for (int i = 0; i < (int)sizeof(big); i++)
		big[i] = (unsigned char)(i ^ (i >> 8));
	check(MemCardCreateFile(0, (char *)"BASLUS-01352TST", 3) == McErrNone,
		  "3-block CreateFile");
	check(MemCardWriteFile(0, (char *)"BASLUS-01352TST",
						   (unsigned long *)big, 0, sizeof(big)) == 1, "3-block write registers");
	check(syncResult(McFuncWriteFile, "3-block write") == McErrNone, "3-block write ok");
	memset(back, 0, sizeof(back));
	check(MemCardReadFile(0, (char *)"BASLUS-01352TST",
						  (unsigned long *)back, 8192, 8192) == 1, "mid-chain read registers");
	check(syncResult(McFuncReadFile, "mid-chain read") == McErrNone, "mid-chain read ok");
	check(memcmp(big + 8192, back, 8192) == 0, "block 2 round-trips through the chain");

	/*	chain states: first 0x51 with size+name, then 0x52, then 0x53  */
	int first = 0;
	for (int b = 1; b <= 15; b++)
		if (img[b * CARD_FRAME_SIZE] == 0x51 &&
			strncmp((const char *)img + b * CARD_FRAME_SIZE + 0x0A,
					"BASLUS-01352TST", 15) == 0)
			first = b;
	check(first != 0, "chain first-link frame found");
	if (first)
	{
		const uint8_t *d = img + first * CARD_FRAME_SIZE;
		unsigned size = d[4] | (d[5] << 8) | (d[6] << 16) | ((unsigned)d[7] << 24);
		check(size == 3 * 8192, "chain size on first link only");
		unsigned next1 = d[8] | (d[9] << 8);
		check(next1 != 0xFFFF, "first link points onward");
		const uint8_t *d2 = img + (next1 + 1) * CARD_FRAME_SIZE;
		check(d2[0] == 0x52, "middle link 0x52");
		unsigned next2 = d2[8] | (d2[9] << 8);
		const uint8_t *d3 = img + (next2 + 1) * CARD_FRAME_SIZE;
		check(d3[0] == 0x53, "end link 0x53");
		check((d3[8] | (d3[9] << 8)) == 0xFFFF, "end link terminates");
	}

	check(MemCardDeleteFile(0, (char *)"BASLUS-01352TST") == McErrNone, "DeleteFile");
	check(MemCardDeleteFile(0, (char *)"BASLUS-01352TST") == McErrFileNotExist,
		  "double delete rejected");
	files = -1;
	MemCardGetDirentry(0, (char *)"*", dir, &files, 0, 15);
	check(files == 1, "back to one file after delete");

	/*	capacity: 14 blocks free -> a 15-block file is refused, 14 fits  */
	check(MemCardCreateFile(0, (char *)"BASLUS-BIG", 15) == McErrBlockFull,
		  "over-capacity create refused");
	check(MemCardCreateFile(0, (char *)"BASLUS-BIG", 14) == McErrNone,
		  "create into freed blocks");
	check(MemCardDeleteFile(0, (char *)"BASLUS-BIG") == McErrNone, "cleanup big file");

	/*	-------- the in-game format UI path: Unformat -> NotFormat -> Format  */
	check(MemCardUnformat(0) == McErrNone, "Unformat");
	MemCardAccept(0);
	check(syncResult(McFuncAccept, "Accept unformatted") == McErrNotFormat,
		  "unformatted card reported");
	check(MemCardFormat(0) == McErrNone, "Format");
	MemCardAccept(0);
	check(syncResult(McFuncAccept, "Accept reformatted") == McErrNone,
		  "reformatted card accepted");
	files = -1;
	MemCardGetDirentry(0, (char *)"*", dir, &files, 0, 15);
	check(files == 0, "format wiped the files");

	/*	-------- hostile card images.

		Card_Open accepts any 128KB file and importing real PS1 saves is a
		documented use, so a dir frame can carry any 16-bit "next block"
		link.  Unvalidated, that indexed the 128KB image with up to
		65535 * 8192 - a wild read on load and a wild WRITE on save/delete -
		and a self-referencing chain never terminated.  Both must now be
		refused rather than followed.  (A crash here fails the test by
		taking the process out, which is the point.)  */
	{
		uint8_t *img = Card_ImageForTest();
		check(MemCardCreateFile(0, (char *)FNAME, 1) == McErrNone,
			  "hostile: file created");

		int first = 0;
		for (int b = 1; b <= 15; b++)
			if (img[b * CARD_FRAME_SIZE] == 0x51)
				first = b;
		check(first != 0, "hostile: found the first link");

		/*	a whole card's worth of destination, so the hostile reads below
			have somewhere legal to land while the chain is walked  */
		static unsigned char wide[16 * 8192];

		/*	link far outside the image  */
		uint8_t *d = img + first * CARD_FRAME_SIZE;
		d[8] = 0xF0; d[9] = 0xFF;			/* next = 0xFFF0, not 0xFFFF */
		check(MemCardReadFile(0, (char *)FNAME, (unsigned long *)wide, 0, sizeof(wide)) == 1,
			  "hostile: out-of-range link read registers");
		check(syncResult(McFuncReadFile, "hostile read") != McErrNone,
			  "out-of-range block link is refused, not followed");

		/*	self-referencing chain: the block links to itself, so the walk
			only ends because of the hop cap  */
		d[8] = (uint8_t)(first - 1); d[9] = 0;
		check(MemCardReadFile(0, (char *)FNAME, (unsigned long *)wide, 0, sizeof(wide)) == 1,
			  "hostile: cyclic link read registers");
		check(syncResult(McFuncReadFile, "cyclic read") != McErrNone,
			  "cyclic block chain terminates instead of spinning");
		check(MemCardDeleteFile(0, (char *)FNAME) == McErrNone,
			  "cyclic chain deletes without running off the image");
	}

	/*	-------- a file that is not a 128KB card image must NOT be
		reformatted out from under the user (a .vmp, a padded dump, or a
		file another process is still writing).  */
	remove("mcrd_test_tmp\\card0.mcd");
	_rmdir("mcrd_test_tmp");
	_mkdir("mcrd_test_tmp2");
	{
		FILE *odd = fopen("mcrd_test_tmp2\\card0.mcd", "wb");
		check(odd != NULL, "odd-sized card written");
		if (odd)
		{
			static unsigned char junk[1024];
			memset(junk, 0xAB, sizeof(junk));
			fwrite(junk, 1, sizeof(junk), odd);
			fclose(odd);
		}
		_putenv("SBSP_SAVE_DIR=mcrd_test_tmp2");
		Card_ResetForTest();
		check(Card_Open() == CARD_IO_ERROR, "odd-sized card file is refused");

		odd = fopen("mcrd_test_tmp2\\card0.mcd", "rb");
		long size = 0;
		if (odd)
		{
			fseek(odd, 0, SEEK_END);
			size = ftell(odd);
			fclose(odd);
		}
		check(size == 1024, "the refused file was left untouched");
		remove("mcrd_test_tmp2\\card0.mcd");
		_rmdir("mcrd_test_tmp2");
	}

	/*	-------- 20-char names: a product code + 8 chars, the longest name
		a card holds (another game's save in an imported image, say).  A
		DIRENTRY carries 19 of them, and the game opens, rewrites and
		deletes every file by the name its DIRENTRY gave it.  */
	{
		static const char *LONG20 = "BASLUS-00000ABCDEFGH";
		static const char *TWIN20 = "BASLUS-00000ABCDEFGX";	/* same 19 */
		static unsigned char data[8192], got[8192];
		for (int i = 0; i < 8192; i++)
			data[i] = (unsigned char)(i * 13 + 5);

		freshCard("mcrd_test_tmp3");
		check(MemCardCreateFile(0, (char *)FNAME, 1) == McErrNone,
			  "20-char: neighbour file created");
		check(MemCardCreateFile(0, (char *)LONG20, 1) == McErrNone,
			  "20-char: file created");
		MemCardWriteFile(0, (char *)LONG20, (unsigned long *)data, 0, 8192);
		check(syncResult(McFuncWriteFile, "20-char write") == McErrNone,
			  "20-char: write by the full name");

		DIRENTRY list[15];
		long n = -1;
		check(MemCardGetDirentry(0, (char *)"*", list, &n, 0, 15) == McErrNone &&
			  n == 2, "20-char: two files listed");
		const DIRENTRY *e = NULL;
		for (long i = 0; i < n; i++)
			if (strncmp(list[i].name, LONG20, 12) == 0 &&
				strcmp(list[i].name, FNAME) != 0)
				e = &list[i];
		check(e != NULL, "20-char: dirent found");
		if (e)
		{
			check(e->name[19] == 0 && strlen(e->name) == 19 &&
				  strncmp(e->name, LONG20, 19) == 0,
				  "20-char: dirent carries the first 19 chars, terminated");

			memset(got, 0, sizeof(got));
			MemCardReadFile(0, (char *)e->name, (unsigned long *)got, 0, 8192);
			check(syncResult(McFuncReadFile, "20-char read") == McErrNone,
				  "20-char: read by the dirent name");
			check(memcmp(got, data, 8192) == 0, "20-char: the right file's data");

			MemCardWriteFile(0, (char *)e->name, (unsigned long *)data, 0, 128);
			check(syncResult(McFuncWriteFile, "20-char rewrite") == McErrNone,
				  "20-char: write by the dirent name");

			check(MemCardCreateFile(0, (char *)e->name, 1) == McErrAlreadyExist,
				  "20-char: a 19-char name that prefixes it cannot be created");
		}
		check(MemCardCreateFile(0, (char *)TWIN20, 1) == McErrAlreadyExist,
			  "20-char: a second 20-char name sharing its 19 cannot be created");

		check(MemCardDeleteFile(0, (char *)LONG20) == McErrNone,
			  "20-char: delete by the full name");
		n = -1;
		MemCardGetDirentry(0, (char *)"*", list, &n, 0, 15);
		check(n == 1 && strcmp(list[0].name, FNAME) == 0,
			  "20-char: the delete took that file and left its neighbour");
		dropDir("mcrd_test_tmp3");
	}

	/*	-------- Card_Open formats only a card that is not there.  One that
		exists but cannot be read (held open with no sharing by another
		process; read denied by its ACL) or is the wrong size (here: empty)
		is refused and left alone; an unformatted 128KB image loads and
		waits for the game's format UI.  */
	{
		static const char *PATH = "mcrd_test_tmp4\\card0.mcd";
		static uint8_t before[CARD_IMAGE_SIZE], after[CARD_IMAGE_SIZE];

		freshCard("mcrd_test_tmp4");
		check(MemCardCreateFile(0, (char *)FNAME, 1) == McErrNone,
			  "open policy: card holds a save");
		check(readAll(PATH, before) == CARD_IMAGE_SIZE, "open policy: card on disk");

		HANDLE h = lockNoShare(PATH);
		Card_ResetForTest();
		check(Card_Open() == CARD_IO_ERROR, "open policy: a locked card is refused");
		MemCardExist(0);
		check(syncResult(McFuncExist, "locked Exist") == McErrCardNotExist,
			  "open policy: the game is told there is no card");
		unlock(h);
		check(Card_Open() == CARD_IO_ERROR,
			  "open policy: the refusal holds for the rest of the session");
		check(readAll(PATH, after) == CARD_IMAGE_SIZE &&
			  memcmp(before, after, CARD_IMAGE_SIZE) == 0,
			  "open policy: the locked card is byte-identical afterwards");
		check(!exists("mcrd_test_tmp4\\card0.mcd.tmp"), "open policy: no temp file left");

		/*	read denied, replace still allowed: the old code formatted a
			blank card and moved it over this one  */
		PSECURITY_DESCRIPTOR sd = denyRead(PATH);
		Card_ResetForTest();
		check(Card_Open() == CARD_IO_ERROR, "open policy: a read-denied card is refused");
		allowRead(PATH, sd);
		check(readAll(PATH, after) == CARD_IMAGE_SIZE &&
			  memcmp(before, after, CARD_IMAGE_SIZE) == 0,
			  "open policy: the read-denied card is byte-identical afterwards");
		check(!exists("mcrd_test_tmp4\\card0.mcd.tmp"), "open policy: no temp file left (ACL)");

		/*	128KB, but no "MC" header: loads, reports unformatted  */
		memset(before, 0xAB, sizeof(before));
		writeAll(PATH, before, CARD_IMAGE_SIZE);
		Card_ResetForTest();
		check(Card_Open() == CARD_OK, "open policy: a corrupt 128KB image loads");
		MemCardAccept(0);
		check(syncResult(McFuncAccept, "corrupt Accept") == McErrNotFormat,
			  "open policy: the corrupt image reports unformatted");
		check(readAll(PATH, after) == CARD_IMAGE_SIZE &&
			  memcmp(before, after, CARD_IMAGE_SIZE) == 0,
			  "open policy: the corrupt image is not rewritten");

		/*	an empty file is a wrong-sized card, not a missing one  */
		writeAll(PATH, before, 0);
		Card_ResetForTest();
		check(Card_Open() == CARD_IO_ERROR, "open policy: a 0-byte card is refused");
		check(exists(PATH) && readAll(PATH, after) == 0,
			  "open policy: the 0-byte card is left as it was");
		dropDir("mcrd_test_tmp4");
	}

	if (g_failures)
	{
		std::printf("mcrd_test: %d failure(s)\n", g_failures);
		return 1;
	}
	std::printf("mcrd_test: all passed\n");
	return 0;
}
