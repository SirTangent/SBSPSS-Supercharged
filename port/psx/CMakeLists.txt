# The original PlayStation executable, Spongey.cpe, built with the 1999
# PSY-Q toolchain in tools/psyq (issue #98).
#
# This replaces makefile.gaz's route (MSYS2's make, sh, sed and Perl) for
# port/build-psx.cmd; makefile.gaz stays, and stays the list of what is
# compiled: port/tools/psx/psx_manifest.py reads its <dir>_src and
# <dir>_src_mip lists at configure time.  The commands are the ones
# makefile.gaz ran, from the repository root with the same relative paths
# (DEBUG bakes __FILE__ in), through port/tools/psx/psxstep.py, so the
# executable is the same, byte for byte (port/docs/psx_byte_identity.md).
# What changes is around them:
# - each compile has its own temporary directory, so Ninja runs them side by
#   side (make shared one a.o and one dep.tmp);
# - the compiler's own dependency output becomes a Ninja depfile, in place
#   of dep.pl and the deps/ tree;
# - the -D flags are part of the command, so switching SBSP_IDENTITY_NOLINE
#   rebuilds every object (make needed a clean tree).
#
# Presets psx-<t>-<v> (port/psx/CMakePresets.json); port/tools/psx/build_psx.py
# configures and builds one.  Outputs go where makefile.gaz put them:
# out/<T>/<V>/CD/objs, out/<T>/<V>/CD/temp/Spongey.ln and
# out/<T>/<V>/version/CD/Spongey.{cpe,sym,map}; info.mip into
# source/system/<V>/<T>/CD (gitignored).  makefile.gaz also copied sn.ini to
# the root; ccpsx reads it through SN_PATH instead.
cmake_minimum_required(VERSION 3.25)
project(sbspss_psx NONE)

set(SBSP_TERRITORY "USA"   CACHE STRING "Territory: USA or EUR")
set(SBSP_VARIANT   "DEBUG" CACHE STRING "Build variant: DEBUG or FINAL")
option(SBSP_IDENTITY_NOLINE "Comparison only: PORT_LINE is 0 (psx_identity.py's line-insensitive DEBUG)" OFF)
set_property(CACHE SBSP_TERRITORY PROPERTY STRINGS USA EUR)
set_property(CACHE SBSP_VARIANT PROPERTY STRINGS DEBUG FINAL)
if(NOT SBSP_TERRITORY MATCHES "^(USA|EUR)$" OR NOT SBSP_VARIANT MATCHES "^(DEBUG|FINAL)$")
    message(FATAL_ERROR "SBSP_TERRITORY must be USA or EUR and SBSP_VARIANT DEBUG or FINAL")
endif()
set(T ${SBSP_TERRITORY})
set(V ${SBSP_VARIANT})

get_filename_component(SBSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PSX_TOOLS ${SBSP_ROOT}/port/tools/psx)
set(STEP ${Python3_EXECUTABLE} ${PSX_TOOLS}/psxstep.py)

if(NOT EXISTS "${SBSP_ROOT}/out/${T}/include/BigLump.h")
    message(FATAL_ERROR "Generated headers missing: out/${T}/include - run port\\build-data.cmd ${T} first.")
endif()

# The objects, from makefile.gaz
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${PSX_TOOLS}/psx_manifest.py --territory ${T} --version ${V}
            --out ${CMAKE_CURRENT_BINARY_DIR}/psx_sources.cmake
    RESULT_VARIABLE _rc)
if(_rc)
    message(FATAL_ERROR "psx_manifest.py failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${SBSP_ROOT}/makefile.gaz ${PSX_TOOLS}/psx_manifest.py)
include(${CMAKE_CURRENT_BINARY_DIR}/psx_sources.cmake)

# Paths as makefile.gaz spelled them, relative to the repository root
set(OBJ_DIR     out/${T}/${V}/CD/objs)
set(TEMP_DIR    out/${T}/${V}/CD/temp)
set(VERSION_DIR out/${T}/${V}/version/CD)
set(INFO_MIP    source/system/${V}/${T}/CD/info.mip)

# build/ccopt.mak's COMMON_CCFLAGS for the CD build, user CDBUILD; parse.mak's
# USER_DEF_OPT, GLIB_OPT and SMALL_OPT are empty there
set(PSX_CCFLAGS
    -Werror
    -D__TERRITORY_${T}__ -D__VERSION_${V}__ -D__USER_CDBUILD__
    -D__VERSION_STR__=${V} -D__TERRITORY_STR__=${T}
    -DPSX -D_LANGUAGE_C_PLUS_PLUS
    -DPC=0 -DCD=1 -D__FILE_SYSTEM__=CD
    -DSONY_ISA=0 -DSONY_PCI=1 -DCLIMAX=2 -DTPWART=3 -D__DEV_KIT__=SONY_PCI
    -Wall -Wmissing-prototypes -Wmissing-declarations -Wno-unused -Wbad-function-cast
    -g -O2 -fno-builtin -fomit-frame-pointer -fno-exceptions -fno-rtti
    -I. -Itools/psyq/include -Itools/climax/libs -Itools/mod/include -Isource
    -Itools/data/include -Idata -Iout/${T}/include)
if(SBSP_IDENTITY_NOLINE)
    # makefile.gaz appended it; psx_identity.py checks the build prints this
    list(APPEND PSX_CCFLAGS -DSBSP_IDENTITY_NOLINE)
    message(STATUS "SBSP_IDENTITY_NOLINE: PORT_LINE is 0 in this build, for comparison only")
endif()
set(PSX_ASM_OPTS /l /zd /z /w /q /o c+ /o w+ /o ws+ /j tools/psyq/include /j source /j ./)

# ccpsx reads tools/psyq/bin/egcs/sn.ini (psxstep.py sets SN_PATH)
set(SN_INI ${SBSP_ROOT}/tools/psyq/bin/egcs/sn.ini)

add_custom_command(OUTPUT ${SBSP_ROOT}/${INFO_MIP}
    COMMAND ${STEP} infomip ${INFO_MIP} ${V} ${T}
    DEPENDS ${PSX_TOOLS}/psxstep.py
    WORKING_DIRECTORY ${SBSP_ROOT}
    COMMENT "info.mip" VERBATIM)

set(PSX_OBJS)
set(PSX_ASM_LINK_NAMES)
foreach(_s IN LISTS PSX_ASM_SOURCES)
    set(_obj ${SBSP_ROOT}/${OBJ_DIR}/${_s}.o)
    add_custom_command(OUTPUT ${_obj}
        COMMAND ${STEP} asm source/${_s}.mip ${OBJ_DIR}/${_s}.o -- ${PSX_ASM_OPTS}
        DEPENDS ${SBSP_ROOT}/source/${_s}.mip ${PSX_TOOLS}/psxstep.py
        WORKING_DIRECTORY ${SBSP_ROOT}
        COMMENT "Assembling source/${_s}.mip" VERBATIM)
    list(APPEND PSX_OBJS ${_obj})
    list(APPEND PSX_ASM_LINK_NAMES ${_s}.o)
endforeach()
foreach(_s IN LISTS PSX_CPP_SOURCES)
    set(_obj ${SBSP_ROOT}/${OBJ_DIR}/${_s}.o)
    set(_dep ${CMAKE_CURRENT_BINARY_DIR}/deps/${_s}.d)
    add_custom_command(OUTPUT ${_obj}
        COMMAND ${STEP} cc source/${_s}.cpp ${OBJ_DIR}/${_s}.o ${_dep} -- ${PSX_CCFLAGS}
        DEPENDS ${SBSP_ROOT}/source/${_s}.cpp ${SN_INI} ${PSX_TOOLS}/psxstep.py
        DEPFILE ${_dep}
        WORKING_DIRECTORY ${SBSP_ROOT}
        COMMENT "Compiling source/${_s}.cpp" VERBATIM)
    list(APPEND PSX_OBJS ${_obj})
endforeach()

# The linker command file and the link (build/lnk.mak)
set(LINK_FILE ${TEMP_DIR}/Spongey.ln)
add_custom_command(OUTPUT ${SBSP_ROOT}/${LINK_FILE}
    COMMAND ${STEP} linkfile ${LINK_FILE} ${OBJ_DIR} ${PSX_ASM_SOURCES} ${PSX_CPP_SOURCES}
    DEPENDS ${PSX_TOOLS}/psxstep.py ${CMAKE_CURRENT_BINARY_DIR}/psx_sources.cmake
    WORKING_DIRECTORY ${SBSP_ROOT}
    COMMENT "Spongey.ln" VERBATIM)
set(PSX_LNK_FLAGS -m -psx -c -v -z -o 0x80010000 -entry __SN_ENTRY_POINT -cpemunge -strip -nsf
    ${PSX_ASM_LINK_NAMES} tools/psyq/lib/libsn.lib -nsf -we -wm -wo
    -nostrip stup0 stup1 stup2 VectorNormalS VectorNormal VectorNormalSS cqsrt -nostrip)
file(GLOB PSX_LIBS ${SBSP_ROOT}/tools/psyq/lib/*.lib ${SBSP_ROOT}/tools/mod/lib/*.lib)
set(CPE ${SBSP_ROOT}/${VERSION_DIR}/Spongey.cpe)
add_custom_command(OUTPUT ${CPE} ${SBSP_ROOT}/${VERSION_DIR}/Spongey.sym ${SBSP_ROOT}/${VERSION_DIR}/Spongey.map
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SBSP_ROOT}/${VERSION_DIR}
    COMMAND ${STEP} link ${LINK_FILE} ${VERSION_DIR}/Spongey.cpe ${VERSION_DIR}/Spongey.sym
            ${VERSION_DIR}/Spongey.map -- ${PSX_LNK_FLAGS}
    DEPENDS ${SBSP_ROOT}/${LINK_FILE} ${PSX_OBJS} ${PSX_LIBS} ${SBSP_ROOT}/tools/vlc/lib/vlc_bit.o
            ${SBSP_ROOT}/tools/psyq/lib/BootObj/snmain.obj
    WORKING_DIRECTORY ${SBSP_ROOT}
    COMMENT "Linking ${VERSION_DIR}/Spongey.cpe" VERBATIM)
add_custom_target(spongey ALL DEPENDS ${CPE})
