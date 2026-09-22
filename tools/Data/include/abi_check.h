/*	ABI_CHECK - a compile-time size check for a struct the game overlays on
	file data: the dstructs.h set (port/abi/abi_check.cpp) and TransHeader
	(source/locale/textdbase.cpp).  gnu++98 has no static_assert; a negative
	array size is the error.  Lives beside dstructs.h and fptr.h so the
	PlayStation build (DATA_INC_DIR) and the PC builds (SBSP_GAME_INCLUDES)
	reach it by the same bare include.  */
#ifndef __ABI_CHECK_H__
#define __ABI_CHECK_H__

#define ABI_CHECK(name, cond)	typedef char abi_check_##name[(cond) ? 1 : -1]

#endif	/* __ABI_CHECK_H__ */
