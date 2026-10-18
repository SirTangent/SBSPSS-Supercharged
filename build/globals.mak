#----------------------------------------------------------------------------
#   globals.mak
#
#	Global defintions for all makefiles
#	Directory definitions and user preference inclusion
#
#	Tabbed for 5 9 (Please do not change)
#
#	Created:	19th April 1997 by G R Liddon @ Fareham
#
#	Copyright (C) 1997 G R Liddon
#----------------------------------------------------------------------------

ifndef __GLOBALS_MAK__
__GLOBALS_MAK__ := 1

#----------------------------------------------------------------------------
#	Useful Macros
#	-------------

NULL		:=
SPACE		:=	$(NULL) $(NULL)
TAB			:=	$(NULL)	$(NULL)
COMMA		:=	,
LBRACKET	:=	(
RBRACKET	:=	)
DOLLAR		:=	$$
SQUOTE		:=	'
DQUOTE		:=	"

define	NEWLINE


endef


#----------------------------------------------------------------------------
#	Directory Definitions
#	---------------------


#	Miscellaneous Dirs
#	------------------
TOOL_DIR :=				tools
GRAF_DIR :=				graphics
GAME_DATA_DIR :=		data
USERS_DIR :=			users
SOURCE_DIR :=			source
SOUND :=				sounds

#	Psyq Utilities and libraries
#	----------------------------
PSYQ_DIR :=				$(TOOL_DIR)/psyq
PSYQ_BIN_DIR :=			$(PSYQ_DIR)/bin
PSYQ_LIB_DIR :=			$(PSYQ_DIR)/lib
PSYQ_INC_DIR :=			$(PSYQ_DIR)/include

#	DDX Dodgy test kit
#	------------------
DDX_DIR :=				$(TOOL_DIR)/climax
DDX_BIN_DIR :=			$(DDX_DIR)/exes
DDX_INC_DIR :=			$(DDX_DIR)/libs
DDX_LIB_DIR :=			$(DDX_DIR)/libs

#	MOD - mod playing lib
#	---------------------
MOD_DIR :=				$(TOOL_DIR)/mod
MOD_INC_DIR :=			$(MOD_DIR)/include
MOD_LIB_DIR :=			$(MOD_DIR)/lib

#	VLC - FMV decoder
#	---------------------
VLC_DIR :=				$(TOOL_DIR)/vlc
VLC_INC_DIR :=			$(VLC_DIR)/include
VLC_LIB_DIR :=			$(VLC_DIR)/lib

#	Data tools and includes
#	---------------------
DATA_TOOL_DIR :=		$(TOOL_DIR)/data
DATA_BIN_DIR :=			$(DATA_TOOL_DIR)/bin
DATA_INC_DIR :=			$(DATA_TOOL_DIR)/include

MKLEVEL :=				$(DATA_BIN_DIR)/MkLevel
MKACTOR :=				$(DATA_BIN_DIR)/MkActor

# Cygwin
# ------
CYG_DIR :=				$(TOOL_DIR)/cygwin
CYG_BIN_DIR :=			$(CYG_DIR)

# Perl
# ----
PERL_DIR :=				$(TOOL_DIR)/perl
PERL_BIN_DIR :=			$(PERL_DIR)/bin
PERL_SCRIPT_DIR :=		$(PERL_DIR)/pl

# Cygwin tools
# ------------
ECHO :=					$(CYG_BIN_DIR)/echo
RMDIR :=				$(CYG_BIN_DIR)/rmdir
MKDIR :=				$(CYG_BIN_DIR)/mkdir
REDIR :=				glecho
TOUCH :=				touch
RM :=					rm
CP :=					cp
SED :=					sed
LS :=					$(CYG_BIN_DIR)/ls
MV :=					$(CYG_BIN_DIR)/mv
DATE :=					$(CYG_BIN_DIR)/date
SED :=					$(CYG_BIN_DIR)/sed
ATTRIB :=				$(CYG_BIN_DIR)/chmod +w

LOG_NAME :=				uname
PARKGRAB :=				parkgrab
BANKER :=				banker
BIGLUMP :=				bl2
TGA2GFX :=				Tga2Gfx

RAR :=					rar
BUILDCD :=				buildcd
STRIPISO :=				stripiso

# PSX and misc tools
# ------------------
LNK :=					slink
PL :=					perl
CC :=					ccpsx
ASM :=					asmpsx
CPE2EXE :=				cpe2exe
CPE2BIN :=				cpe2bin
LZNP :=					lznp -Q
SIZEAPP :=				sizeapp
MAKEDATA :=				MkData

#----------------------------------------------------------------------------
#	Vars that need exporting
#	------------------------

#	Path
#	----
export PATH =		$(TOOL_DIR);$(CYG_BIN_DIR);$(PSYQ_BIN_DIR);$(PERL_BIN_DIR)
export Path =		$(TOOL_DIR);$(CYG_BIN_DIR);$(PSYQ_BIN_DIR);$(PERL_BIN_DIR)

#	MSYS2's make
#	------------
#	The tool pins and the path above are the 1999 route: tools/make.exe
#	(GNU make 3.79, a native Win32 build; MakeUSA.bat and friends) with the
#	cygwin b20 utilities in tools/cygwin, which no longer start on Windows
#	10/11.  MSYS2's make (4.x, built for cygwin, so MAKE_HOST is *-cygwin,
#	or *-msys from MSYS2 releases before the triplet changed; 3.79 has no
#	MAKE_HOST) gets MSYS2's own utilities and a POSIX PATH:
#	port/tools first, so its lznp.exe shadows the 16-bit tools/lznp.exe,
#	then every tool directory the data and PS1 builds use.  PATH only, no
#	Path: MSYS2 folds Windows' Path into PATH already.  This is the one
#	copy - port/build-data.sh, port/build-psx.sh and a bare make from an
#	MSYS2 shell all get it (issue #16).  Any other make stops here: a native
#	Windows make 4.x (Git Bash's, say) can run neither set of tools, and a
#	make older than 4.0 that isn't 3.79 (GnuWin32's 3.81) has no MAKE_HOST
#	either but would get the 1999 tools that no longer start.
#	LOG_NAME is the one tool whose name means something else in MSYS2:
#	getuser.mak runs it for the login name when no USER_NAME is given, and
#	/usr/bin's uname, first on PATH, prints the system name (MSYS_NT-...).
#	So the name comes from the environment (issue #90): USERNAME, which
#	Windows sets but a shell started without the Windows environment can
#	lack, then USER, which MSYS2's login shell sets.  Only with neither
#	does the shell run tools/uname.exe (GetUserName) by path; the $$( is
#	the shell's command substitution, kept as text by the := here.
ifneq ($(filter %-cygwin %-msys,$(MAKE_HOST)),)
LOG_NAME :=				echo $(or $(USERNAME),$(USER),$$("$(CURDIR)/$(TOOL_DIR)/uname"))
ECHO :=					echo
RMDIR :=				rmdir
MKDIR :=				mkdir
LS :=					ls
MV :=					mv
DATE :=					date
SED :=					sed
ATTRIB :=				chmod +w
unexport Path
export PATH :=			/usr/bin:$(CURDIR)/port/tools:$(CURDIR)/$(TOOL_DIR):$(CURDIR)/$(DATA_BIN_DIR):$(CURDIR)/$(PSYQ_BIN_DIR):$(CURDIR)/$(PSYQ_BIN_DIR)/egcs
else
ifneq ($(MAKE_HOST),)
$(error GNU make $(MAKE_VERSION) for $(MAKE_HOST) cannot run this build - use MSYS2's make: port\build-data.cmd / port\build-psx.cmd, or /usr/bin/make from an MSYS2 shell)
endif
ifeq ($(filter 3.79%,$(MAKE_VERSION)),)
$(error GNU make $(MAKE_VERSION) is neither MSYS2's make nor tools/make.exe 3.79 - use MSYS2's make: port\build-data.cmd / port\build-psx.cmd, or /usr/bin/make from an MSYS2 shell)
endif
endif

#	PsyQ Vars overidden for those who've installed PSYQ on their machine
#	--------------------------------------------------------------------
export	COMPILER_PATH :=
export  PSYQ_PATH :=
export	ASSEMBLER_PATH :=
export  LIBRARY_PATH :=
export  C_PLUS_INCLUDE_PATH :=
export  C_INCLUDE_PATH :=

#	These vars are also used by GCC
#	-------------------------------
export  GCC_EXEC_PREFIX :=
export  COMPILER_PATH :=
export  LIBRARY_PATH  :=
export  C_INCLUDE_PATH  :=
export  CPLUS_INCLUDE_PATH  :=
export	OBJC_INCLUDE_PATH  :=

#----------------------------------------------------------------------------
endif		# __GLOBALS_MAK__
#----------------------------------------------------------------------------
# end

