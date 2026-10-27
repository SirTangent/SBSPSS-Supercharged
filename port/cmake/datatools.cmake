# Data-pipeline converters rebuilt from Utils/ (issue #48), included from
# port/CMakeLists.txt.  Every preset builds them;
# the clang-cl x64 copies are committed to port/tools/x64/, beside
# lznp_tool's (as lznp.exe), and build/globals.mak puts that directory first
# on the data build's PATH.  The tests that hold the committed exes to these
# sources (datatools_exe_matches_source, lznp_x64_exe_matches_source) are
# registered in port/CMakeLists.txt with the other unit tests.
#
# 1999 sources, built as they are - the old CRT names (strupr), char* string
# literals, MkData's `if(*StrPtr=':')` (intended: it skips the colon) - plus
# the raw 0xA3 byte in MkData's illegal-character list, which must stay one
# byte.

set(SBSP_DATATOOL_OPTS ${SBSP_DEBUG_FLAGS} ${SBSP_OPT_FLAGS}
    -Wno-parentheses -Wno-writable-strings -Wno-invalid-source-encoding
    -Wno-nonportable-include-path)
set(SBSP_UTILS ${SBSP_ROOT}/Utils)

# sbsp_datatool(<target>...): the shared defines and warnings
function(sbsp_datatool)
    foreach(_t IN LISTS ARGN)
        target_compile_definitions(${_t} PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
        target_compile_options(${_t} PRIVATE ${SBSP_DATATOOL_OPTS})
    endforeach()
endfunction()

# ---------------------------------------------------------------------------
# MkData (BigLump.Bin + BigLump.h) and Tga2Gfx (the .gfx screens)
# ---------------------------------------------------------------------------
add_executable(mkdata_tool ${SBSP_UTILS}/MkData/MkData.cpp)
add_executable(tga2gfx_tool ${SBSP_UTILS}/Tga2Gfx/Tga2Gfx.cpp)
set_target_properties(mkdata_tool PROPERTIES OUTPUT_NAME MkData)
set_target_properties(tga2gfx_tool PROPERTIES OUTPUT_NAME Tga2Gfx)
sbsp_datatool(mkdata_tool tga2gfx_tool)

# ---------------------------------------------------------------------------
# scripter compiles the game's .scr scripts; its lexer.cpp/parser.cpp are the
# committed ALex/AYACC output of lexer.l/parser.y, over Parser Generator's
# C++ runtime (the yyc*.cpp half of its SOURCE, in place of the prebuilt
# VC6 yl.lib).  myparser.cpp also prints to yyparseerr, a global of the C
# half (yyspvars.c): built as C++, because `= stderr` is no constant
# initializer for C under the UCRT, and the reference is a C++ one.
# ---------------------------------------------------------------------------
set(SBSP_PG_DIR "${SBSP_UTILS}/Parser Generator")
file(GLOB SBSP_PG_YL_SOURCES "${SBSP_PG_DIR}/SOURCE/yyc*.cpp")
set_source_files_properties("${SBSP_PG_DIR}/SOURCE/yyspvars.c" PROPERTIES LANGUAGE CXX)
add_library(pg_yl STATIC ${SBSP_PG_YL_SOURCES} "${SBSP_PG_DIR}/SOURCE/yyspvars.c")
target_include_directories(pg_yl PUBLIC "${SBSP_PG_DIR}/INCLUDE")
set(SBSP_SCRIPTER_SOURCES codegen function lexer main mylexer myparser parser pfile prepro var)
list(TRANSFORM SBSP_SCRIPTER_SOURCES PREPEND ${SBSP_UTILS}/Scripter/)
list(TRANSFORM SBSP_SCRIPTER_SOURCES APPEND .cpp)
add_executable(scripter_tool ${SBSP_SCRIPTER_SOURCES})
set_target_properties(scripter_tool PROPERTIES OUTPUT_NAME scripter)
target_link_libraries(scripter_tool PRIVATE pg_yl)
# Parser Generator's headers open a #pragma pack in one header and close it
# in another
target_compile_options(pg_yl PRIVATE -Wno-pragma-pack)
target_compile_options(scripter_tool PRIVATE -Wno-pragma-pack)
sbsp_datatool(pg_yl scripter_tool)

# ---------------------------------------------------------------------------
# MkActor (.SBK sprite banks) and MkLevel (.Lvl levels), over the shared
# libraries in Utils/Libs, built as their VC6 projects list them (GLib's
# ..\..\Template\Template.cpp never existed in this repo, and nothing uses
# it).  Both write the file-overlay structs of tools/Data/include/dstructs.h
# whole; on x64 their pointer fields are FPTR<T>, 4 bytes as in the files
# (SBSP_PC64, as for the x64 game; dstructs.h).
# ---------------------------------------------------------------------------
set(SBSP_LIBS ${SBSP_UTILS}/Libs)
# compat/ holds <iostream.h> and <fstream.h>, gone from every modern library
set(SBSP_DATATOOL_LIB_INCLUDES
    ${SBSP_LIBS}/GLib ${SBSP_LIBS}/GinLib ${SBSP_LIBS}/DaveLib ${SBSP_LIBS}/Maths ${SBSP_LIBS}/TexGrab
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/datatool-src/compat)

set(_glib GLib/Gal.c GLib/Gmain.c GLib/Gtimer.c GLib/Gutils.c GLib/Ll.c GLib/Objs.c
    GLib/Tasker.c GLib/Tick.c GLib/Dpanim.cpp GLib/Frame.cpp GLib/Ganim.cpp GLib/Gdebug.c
    GLib/Gfname.cpp GLib/Gobject.cpp GLib/Gstring.cpp GLib/GSYS.C GLib/GTIMSYS.C GLib/IFF.CPP
    GLib/Ilbm.cpp GLib/MISC.CPP GLib/Niff.cpp GLib/Pal.cpp GLib/REGEX.C GLib/tquant.cpp
    GLib/chartables.c GLib/maketables.c GLib/pcre.c GLib/study.c)
set(_ginlib GinLib/animheader.cpp GinLib/ginio.cpp GinLib/gintex.cpp GinLib/mapread.cpp
    GinLib/repread.cpp GinLib/script.cpp)
set(_davelib DaveLib/DaveLib.cpp DaveLib/facestore.cpp DaveLib/list2d.cpp)
set(_maths Maths/axisangle.cpp Maths/Decompose.cpp Maths/matrix4x4.cpp
    Maths/MgcAppr3DLineFit.cpp Maths/MgcEigen.cpp Maths/quat.cpp)
set(_texgrab TexGrab/AllFiles.cpp TexGrab/bmploader.cpp TexGrab/cross.cpp TexGrab/grect.cpp
    TexGrab/pak.cpp TexGrab/sprset.cpp TexGrab/TexGrab.cpp TexGrab/tpage.cpp TexGrab/VImage.cpp)
# CMake takes an upper-case .C for C++; these three are C
set_source_files_properties(${SBSP_LIBS}/GLib/GSYS.C ${SBSP_LIBS}/GLib/GTIMSYS.C
    ${SBSP_LIBS}/GLib/REGEX.C PROPERTIES LANGUAGE C)
# GLib's own allocator, object lists, tasks and GNU regex keep pointers in
# 32-bit integers.  Nothing in MkActor or MkLevel reaches them (only other
# GLib C modules call GAL_ / OBJ_ / TASK_, and nothing calls the regex), so
# they build as they are, without the warnings.
set_source_files_properties(${SBSP_LIBS}/GLib/Gal.c ${SBSP_LIBS}/GLib/Objs.c
    ${SBSP_LIBS}/GLib/Tasker.c ${SBSP_LIBS}/GLib/REGEX.C PROPERTIES COMPILE_OPTIONS
    "-Wno-void-pointer-to-int-cast;-Wno-int-to-void-pointer-cast;-Wno-pointer-to-int-cast;-Wno-int-to-pointer-cast;-Wno-non-literal-null-conversion")
# What clang-cl still says about these sources is 1999 style, none of it a
# 64-bit problem (each pointer that went through a 32-bit integer is fixed at
# its source): K&R prototypes and `register` in GLib's C; `delete` of a
# new[]'d array of plain data; switches without every enum value;
# int-to-pointer casts, every one a file offset stored into a 4-byte DPTR
# field (dstructs.h); and long / size_t printed with %i, which a Win64
# vararg slot passes intact.
set(SBSP_DATATOOL_QUIET -Wno-deprecated-non-prototype -Wno-deprecated-register
    -Wno-mismatched-new-delete -Wno-switch -Wno-microsoft-extra-qualification
    -Wno-int-to-pointer-cast -Wno-format -Wno-unused-value -Wno-main-return-type -Wno-comment)
foreach(_lib glib ginlib davelib maths texgrab)
    list(TRANSFORM _${_lib} PREPEND ${SBSP_LIBS}/)
    add_library(dt_${_lib} STATIC ${_${_lib}})
    target_include_directories(dt_${_lib} PUBLIC ${SBSP_DATATOOL_LIB_INCLUDES})
    target_compile_definitions(dt_${_lib} PUBLIC SBSP_DATATOOL WIN32 _MBCS
        # the 4-byte FPTR<T> view of dstructs.h's pointer fields, as the x64 game's
        $<$<EQUAL:${CMAKE_SIZEOF_VOID_P},8>:SBSP_PC64>)
    target_compile_options(dt_${_lib} PUBLIC ${SBSP_DATATOOL_QUIET})
    sbsp_datatool(dt_${_lib})
endforeach()

# the struct-size checks and FPTR's range report, and a crash report
# (port/tools/datatool-src)
set(SBSP_DATATOOL_ABI ${CMAKE_CURRENT_SOURCE_DIR}/tools/datatool-src/datatool_abi.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/datatool-src/datatool_crash.cpp)
add_executable(mkactor_tool ${SBSP_UTILS}/MkActor/MkActor.cpp ${SBSP_DATATOOL_ABI})
set_target_properties(mkactor_tool PROPERTIES OUTPUT_NAME MkActor)
target_link_libraries(mkactor_tool PRIVATE dt_glib dt_ginlib dt_texgrab dt_maths dt_davelib)

set(_mklevel Main.cpp MkLevel.cpp pak.cpp
    Layers/MkLevelLayer3d.cpp Layers/MkLevelLayerActor.cpp Layers/MkLevelLayerCollision.cpp
    Layers/MkLevelLayerFX.cpp Layers/MkLevelLayerHazard.cpp Layers/MkLevelLayerItem.cpp
    Layers/MkLevelLayerPlatform.cpp Layers/MkLevelLayerRGB.cpp Layers/MkLevelLayerShade.cpp
    Layers/MkLevelLayerThing.cpp Layers/MkLevelLayerTile.cpp Layers/MkLevelLayerTrigger.cpp)
list(TRANSFORM _mklevel PREPEND ${SBSP_UTILS}/MkLevel/)
add_executable(mklevel_tool ${_mklevel} ${SBSP_DATATOOL_ABI})
set_target_properties(mklevel_tool PROPERTIES OUTPUT_NAME MkLevel)
target_link_libraries(mklevel_tool PRIVATE dt_glib dt_ginlib dt_texgrab dt_maths dt_davelib)
sbsp_datatool(mkactor_tool mklevel_tool)

# ---------------------------------------------------------------------------
# parkgrab (the sprite banks: Sprites.Spr/.h, shop, party).  Its own sprset,
# tpage, VImage... are the forerunners of TexGrab's, with the same class
# names, so it links GLib and GinLib (its .dsp's glibdev win32lib and
# ginutils tplib; GinLib's gintex brings Maths) and not dt_texgrab.
# ---------------------------------------------------------------------------
set(_parkgrab bmploader cross grect main pak sprset tpage VImage)
list(TRANSFORM _parkgrab PREPEND ${SBSP_UTILS}/parkgrab/)
list(TRANSFORM _parkgrab APPEND .cpp)
add_executable(parkgrab_tool ${_parkgrab} ${CMAKE_CURRENT_SOURCE_DIR}/tools/datatool-src/datatool_crash.cpp)
set_target_properties(parkgrab_tool PROPERTIES OUTPUT_NAME parkgrab)
target_link_libraries(parkgrab_tool PRIVATE dt_glib dt_ginlib dt_maths)
sbsp_datatool(parkgrab_tool)

# ---------------------------------------------------------------------------
# transtext (the text databases <language>.dat + id.dat, and trans.h).  Its
# main.cpp and ScriptFile survive in Utils/transtext; the Theme Park library
# classes they drive (trans.h, lang.h) did not, and are rewritten from the
# 1999 exe in tools/datatool-src/tplib, with a stub for the Japanese output,
# whose font data is missing too.  Utils/transtext and tplib come before the
# library include directories, so <script.h> and <trans.h> are these and
# not GinLib's; GinLib itself is not linked (its ScriptFile is a later copy).
# ---------------------------------------------------------------------------
set(SBSP_TPLIB ${CMAKE_CURRENT_SOURCE_DIR}/tools/datatool-src/tplib)
add_executable(transtext_tool ${SBSP_UTILS}/transtext/main.cpp ${SBSP_UTILS}/transtext/script.cpp
    ${SBSP_TPLIB}/trans.cpp ${SBSP_TPLIB}/kanji_stub.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/datatool-src/datatool_crash.cpp)
set_target_properties(transtext_tool PROPERTIES OUTPUT_NAME transtext)
target_include_directories(transtext_tool BEFORE PRIVATE ${SBSP_UTILS}/transtext ${SBSP_TPLIB})
target_link_libraries(transtext_tool PRIVATE dt_glib)
sbsp_datatool(transtext_tool)
