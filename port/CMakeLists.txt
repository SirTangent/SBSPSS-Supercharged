#
# SBSPSS Win11 port - PC build (M1: scaffold + shim + headless file-I/O proof).
#
# Configure via the presets in port/CMakePresets.json, or directly:
#   cmake -S port -B port/build/debug -G Ninja \
#         --toolchain cmake/clangcl-toolchain.cmake -DSBSP_ARCH=x64 -DSBSP_VARIANT=DEBUG
#
# One toolchain: clang-cl (cmake/clangcl-toolchain.cmake) against the MSVC
# CRT and Windows SDK, linked by lld-link with a PDB per exe, SDL3 from the
# official VC package as a DLL.  The shipping build is x64
# (x86_64-pc-windows-msvc, the plain debug/final presets), where the game
# target gets SBSP_PC64 - see SBSP_GAME_DEFINES below and
# port/docs/conv_pc.md, "x64 (M9)".  The x86-* presets build the same code
# for i686-pc-windows-msvc, which parity64 holds x64 to.  MinGW g++ built the
# port until issue #39 retired it with MSYS2's 32-bit repository.
#
cmake_minimum_required(VERSION 3.28)
project(sbspss LANGUAGES C CXX)
# every exe links psyq/host/sbsp.rc (the manifest, see sbsp_exe_link); the
# toolchain file names the resource compiler (llvm-rc)
enable_language(RC)

set(SBSP_VARIANT   "DEBUG" CACHE STRING "Game build variant: DEBUG or FINAL")
set(SBSP_TERRITORY "USA"   CACHE STRING "Territory: USA or EUR")
set_property(CACHE SBSP_VARIANT   PROPERTY STRINGS DEBUG FINAL)
set_property(CACHE SBSP_TERRITORY PROPERTY STRINGS USA EUR)

get_filename_component(SBSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR
        "The port builds with clang-cl only (cmake/clangcl-toolchain.cmake, through the presets: "
        "port\\build-pc.cmd debug).  MinGW g++ was retired by issue #39.")
endif()

# OFF = the shim and its unit exes only (no sbsp.exe / sbsp_headless.exe,
# no playthrough tests): a quicker configure for shim-only work, and the
# escape hatch should a compiler update break the 1999 sources on one
# toolchain but not the other.
option(SBSP_BUILD_GAME "Build sbsp.exe + sbsp_headless.exe and register the playthrough tests" ON)

# ctest: label `unit` = the shim unit exes + the headless proof, label
# `playthrough` = the M8 harness tiers (tests/run_tier.py).  Every test runs
# from the repo root, where the game's default data path resolves.
enable_testing()
find_package(Python3 COMPONENTS Interpreter)
# The playthrough tiers are the PR gate, so a missing Python must not quietly
# register none of them (issue #41): skipping them has to be asked for.
option(SBSP_PLAYTHROUGH "Register the playthrough tests (needs Python 3)" ON)
if(SBSP_BUILD_GAME AND SBSP_PLAYTHROUGH AND NOT Python3_Interpreter_FOUND)
    # name the command that reaches configure: build-pc.cmd hands its extra
    # args to the build, so the opt-out is an environment variable there.
    # build_pc.py passes its own Python, so this is a hand configure's error.
    get_filename_component(_sbsp_tree "${CMAKE_BINARY_DIR}" NAME)
    get_filename_component(_sbsp_tree_parent "${CMAKE_BINARY_DIR}" DIRECTORY)
    if(NOT _sbsp_tree_parent STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/build")
        set(_sbsp_tree "<preset>")
    endif()
    message(FATAL_ERROR
        "Python 3 not found, and the playthrough tests (tests/run_tier.py) need it.  "
        "Install Python 3 for Windows (python.org) and put it on PATH, or build without registering them:\n"
        "  set SBSP_PLAYTHROUGH=OFF, then port\\build-pc.cmd ${_sbsp_tree}\n"
        "  (PowerShell: $env:SBSP_PLAYTHROUGH = \"OFF\")\n"
        "or, configuring by hand in port/:\n"
        "  cmake --preset ${_sbsp_tree} -DSBSP_PLAYTHROUGH=OFF")
endif()

# The generated headers gate everything (BigLump.h / Sprites.h / trans.h / ...).
# Both are per territory only (issue #35): out/<T>/include is written by the
# data build, and the CD files the exe opens at run time are staged once
# under out/<T>/cd for DEBUG and FINAL alike (cd.cpp resolveDataRoot, which
# also looks beside the exe and honours SBSP_DATA_DIR) - hence a warning,
# not an error, for the data.
if(NOT EXISTS "${SBSP_ROOT}/out/${SBSP_TERRITORY}/include/BigLump.h")
    message(FATAL_ERROR
        "Generated headers missing: ${SBSP_ROOT}/out/${SBSP_TERRITORY}/include/BigLump.h\n"
        "Run port/build-data.cmd ${SBSP_TERRITORY} (M0 data pipeline) first.")
endif()
if(SBSP_BUILD_GAME AND NOT EXISTS "${SBSP_ROOT}/out/${SBSP_TERRITORY}/cd/BIGLUMP.BIN")
    message(WARNING
        "Game data missing: ${SBSP_ROOT}/out/${SBSP_TERRITORY}/cd/BIGLUMP.BIN\n"
        "The exe will build but not boot - run port/build-data.cmd ${SBSP_TERRITORY} (or set SBSP_DATA_DIR).")
endif()

include(cmake/game_sources.cmake)

# ---------------------------------------------------------------------------
# Compiler switches.  -Wno-<x> switches take the GNU form (clang-cl passes
# -W<name> through) - but a bare -Wall is /Wall to clang-cl, which is
# clang's -Weverything; /W3 is its in-order spelling of -Wall (a
# /clang:-Wall would be appended after every -Wno-, undoing them).
# ---------------------------------------------------------------------------
set(SBSP_WARN_ALL    /W3)
set(SBSP_DEBUG_FLAGS /Z7)               # CodeView in the object; lld-link /DEBUG collects <exe>.pdb
set(SBSP_OPT_FLAGS   /O2)
set(SBSP_GAME_DIALECT
    /clang:-std=gnu++98
    /clang:-fpermissive
    /clang:-fno-strict-aliasing
    /clang:-fno-builtin
    /EHs-c-                             # -fno-exceptions
    /GR-                                # -fno-rtti
)
# static CRT (/MT): the exes carry their runtime
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
# the MSVC CRT's "fopen/getenv/strncpy are deprecated, use the _s forms"
# advisories: not warnings about this code
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# Every exe: /MT already carries the CRT; /DEBUG makes lld-link write
# <exe>.pdb from the /Z7 objects; SDL3 is a DLL in the official VC package
# (it ships no static lib), so it is copied beside the exes to keep "runs
# from its build dir" true.  One target copies it, ahead of every exe: a
# POST_BUILD copy per exe raced when ninja linked two at once, the second
# failing on the DLL the first held open (PR #94's run).
#
# And one application manifest for all of them, psyq/host/sbsp.rc ->
# sbsp.manifest (issue #62): activeCodePage UTF-8, so narrow paths carry any
# character.  lld-link is told to generate none, or the exe would carry two.
set(SBSP_RC ${CMAKE_CURRENT_SOURCE_DIR}/psyq/host/sbsp.rc)
set_source_files_properties(${SBSP_RC} PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/psyq/host     # finds sbsp.manifest
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/psyq/host/sbsp.manifest)
function(sbsp_exe_link target)
    target_sources(${target} PRIVATE ${SBSP_RC})
    target_link_options(${target} PRIVATE /DEBUG /MANIFEST:NO)
    if(NOT TARGET sbsp_runtime_dlls)
        add_custom_target(sbsp_runtime_dlls
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:SDL3::SDL3-shared> ${CMAKE_CURRENT_BINARY_DIR})
    endif()
    add_dependencies(${target} sbsp_runtime_dlls)
endfunction()

# ---------------------------------------------------------------------------
# Compile environment shared by all game code (mirrors build/ccopt.mak +
# build/parse.mak for the CD/CDBUILD configuration; -Werror and the vintage
# warning set are dropped for the modern compiler).
# ---------------------------------------------------------------------------
set(SBSP_GAME_DEFINES
    PSX
    _LANGUAGE_C_PLUS_PLUS
    __TERRITORY_${SBSP_TERRITORY}__
    __VERSION_${SBSP_VARIANT}__
    __USER_CDBUILD__
    PC=0
    CD=1
    __FILE_SYSTEM__=CD
    SONY_ISA=0
    SONY_PCI=1
    CLIMAX=2
    TPWART=3
    __DEV_KIT__=SONY_PCI
    # the UCRT headers spell their exception specifications `noexcept` for
    # every C++ dialect; gnu++98 has no such keyword, and corecrt.h lets the
    # user pick the spelling first
    "_CRT_NOEXCEPT=throw()"
)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    # x64 (M9): the file-overlay structs keep their 4-byte pointer fields
    # (tools/Data/include/fptr.h), the heap aligns to 16 (source/mem/memory.h).
    # Game code only - the data tools and the PlayStation build never see it.
    list(APPEND SBSP_GAME_DEFINES SBSP_PC64)
endif()

set(SBSP_GAME_CXX_FLAGS
    ${SBSP_GAME_DIALECT}
    ${SBSP_WARN_ALL}
    -Wno-unused
    -Wno-write-strings
    # style noise pervasive in the 1999 code - suppressed on the game target
    # only; genuine bug-smell classes (-Wmaybe-uninitialized,
    # -Wreturn-local-addr, -Wdelete-non-virtual-dtor) stay visible
    -Wno-narrowing
    -Wno-overloaded-virtual
    -Wno-non-c-typedef-for-linkage
    -Wno-parentheses
    -Wno-char-subscripts
    -Wno-sign-compare
    -Wno-misleading-indentation
    -Wno-int-in-bool-context
    -Wno-dangling-else
    -Wno-header-guard
    -Wno-nonportable-include-path    # clang: #include "Foo.h" for foo.h (NTFS is case-insensitive)
    -Wno-shift-negative-value        # clang: (-1)<<n in pcart/pghost - gcc keeps quiet in gnu++98
    ${SBSP_DEBUG_FLAGS}
    ${SBSP_OPT_FLAGS}
)

# Include order is load-bearing (mirrors ccopt.mak); port/include shadows the
# few PSY-Q headers that cannot compile on x86, and tools/psyq/include sits
# BEHIND the system headers (-idirafter) so the CRT's real stddef/stdlib/stdio
# win while the SDK's libgpu/libgte/libcd/... layouts stay authoritative.
set(SBSP_GAME_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SBSP_ROOT}
    ${SBSP_ROOT}/tools/mod/include
    ${SBSP_ROOT}/source
    ${SBSP_ROOT}/tools/Data/include
    ${SBSP_ROOT}/data
    ${SBSP_ROOT}/out/${SBSP_TERRITORY}/include
)
set(SBSP_PSYQ_DIRAFTER "/clang:-idirafter${SBSP_ROOT}/tools/psyq/include")

function(sbsp_game_flavour target)
    target_compile_definitions(${target} PRIVATE ${SBSP_GAME_DEFINES})
    target_compile_options(${target} PRIVATE ${SBSP_GAME_CXX_FLAGS} ${SBSP_PSYQ_DIRAFTER})
    target_include_directories(${target} PRIVATE ${SBSP_GAME_INCLUDES})
endfunction()

# ---------------------------------------------------------------------------
# Game code (unmodified PS1 sources)
# ---------------------------------------------------------------------------
if(SBSP_BUILD_GAME)
    # abi/abi_check.cpp: compile-time size checks of the file-overlay structs,
    # seen exactly as the game sees them (hence here, not in the shim glob)
    add_library(sbsp_game STATIC ${SBSP_GAME_SOURCES} abi/abi_check.cpp)
    sbsp_game_flavour(sbsp_game)

    # system/main.cpp compiles like everything else but is kept out of the
    # lib so test executables can supply their own main(); the real game exe
    # (M2+) links this object in.
    add_library(sbsp_game_main OBJECT ${SBSP_GAME_MAIN_SOURCE})
    sbsp_game_flavour(sbsp_game_main)
endif()

# ---------------------------------------------------------------------------
# PsyQ-on-PC shim runtime (modern C++)
# ---------------------------------------------------------------------------
# SDL3 (window/input/present host) and the Vulkan headers - the loader is
# reached at runtime through SDL_Vulkan_LoadLibrary, so Vulkan needs only
# headers at build time.  The official SDL3 VC devel package and the Khronos
# headers are fetched on first configure (cmake/deps_vc.cmake), pinned by
# SHA-256.
include(cmake/deps_vc.cmake)
find_package(SDL3 CONFIG REQUIRED)

file(GLOB SBSP_SHIM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/psyq/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/psyq/*/*.cpp
)

set(SBSP_SHIM_FLAGS ${SBSP_WARN_ALL} ${SBSP_DEBUG_FLAGS} ${SBSP_OPT_FLAGS})

# Argument parsing is kept OUT of the shim archive and linked as objects, for
# the same reason main.cpp is split out of the game lib above: its entire job
# is an early constructor and it exports only Port_BootLevel and Port_Language,
# which only system/main.cpp calls.  Inside a static archive the linker drops
# the member from every executable that does not reference those symbols -
# which is how `sbsp_headless --data-dir <path>` came to be silently ignored.
# It needs no shim headers (stdio/stdlib/string + host/compiler.h beside it),
# so it carries no include set.  The seed is NOT here: host/seed.cpp is a shim
# member because the pad recorder (host/input.cpp) needs it (issue #58).
list(REMOVE_ITEM SBSP_SHIM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/psyq/host/args.cpp)
add_library(psyq_args OBJECT psyq/host/args.cpp)
target_compile_features(psyq_args PRIVATE cxx_std_17)
target_compile_options(psyq_args PRIVATE ${SBSP_SHIM_FLAGS})

add_library(psyq_shim STATIC ${SBSP_SHIM_SOURCES})
target_compile_features(psyq_shim PRIVATE cxx_std_17)
target_compile_options(psyq_shim PRIVATE ${SBSP_SHIM_FLAGS} ${SBSP_PSYQ_DIRAFTER})
target_compile_definitions(psyq_shim PRIVATE
    SBSP_INFO_VERSION="${SBSP_VARIANT}"
    SBSP_INFO_TERRITORY="${SBSP_TERRITORY}"
    SBSP_INFO_FILESYSTEM="CD"
)
target_include_directories(psyq_shim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/psyq
    ${SBSP_ROOT}
    ${SBSP_ROOT}/source
    ${SBSP_ROOT}/tools/mod/include
    ${SBSP_ROOT}/out/${SBSP_TERRITORY}/include
    ${SBSP_VULKAN_INCLUDE}
)
target_link_libraries(psyq_shim PUBLIC SDL3::SDL3)
target_link_libraries(psyq_shim PUBLIC winmm)   # timeBeginPeriod (host/pump.cpp)

# ---------------------------------------------------------------------------
# The game itself (M2+): the real main() from source/system/main.cpp over the
# whole-archive game lib and the shim.  WIN32 subsystem is deliberately NOT
# set - the console carries the shim's stub/diagnostic log.
# ---------------------------------------------------------------------------
if(SBSP_BUILD_GAME)
    add_executable(sbsp $<TARGET_OBJECTS:sbsp_game_main> $<TARGET_OBJECTS:psyq_args>)
    set_target_properties(sbsp PROPERTIES LINKER_LANGUAGE CXX)
    sbsp_exe_link(sbsp)
    target_link_libraries(sbsp PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,sbsp_game>
        psyq_shim
    )
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# Headless file-I/O proof: whole-archive the game lib so EVERY game TU's
# undefined symbols hit the linker - that is the link-completeness proof.
if(SBSP_BUILD_GAME)
    add_executable(sbsp_headless tests/headless.cpp $<TARGET_OBJECTS:psyq_args>)
    sbsp_game_flavour(sbsp_headless)
    sbsp_exe_link(sbsp_headless)
    target_link_libraries(sbsp_headless PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,sbsp_game>
        psyq_shim
    )
    add_test(NAME sbsp_headless COMMAND sbsp_headless WORKING_DIRECTORY ${SBSP_ROOT})
    # A private save dir: without one the M8 shell would put card0.mcd in the
    # developer's real %APPDATA%\SBSPSS (an interactive run's default), and a
    # private ini path: this is an interactive run as far as args.cpp can
    # tell, and without one it would write a first-run sbsp.ini beside the
    # exe in the build tree (an explicit SBSP_INI is never created).
    set_tests_properties(sbsp_headless PROPERTIES LABELS unit
        ENVIRONMENT "SBSP_SAVE_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless_save;SBSP_INI=${CMAKE_CURRENT_BINARY_DIR}/headless_save/sbsp.ini")
endif()

# Unit tests that exercise the shim directly (no game code): C++17, linked
# against psyq_shim, and compiled with the shim's own include arrangement -
# the shadow SDK headers first, the vintage PSY-Q headers only via
# -idirafter - so a test may use the real LIBGPU/LIBGTE prim macros.
function(sbsp_shim_test name)
    add_executable(${name} tests/${name}.cpp)
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_compile_options(${name} PRIVATE ${SBSP_DEBUG_FLAGS} ${SBSP_PSYQ_DIRAFTER})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/psyq
        ${SBSP_ROOT}
        ${SBSP_ROOT}/source
        ${SBSP_ROOT}/tools/mod/include
    )
    sbsp_exe_link(${name})
    target_link_libraries(${name} PRIVATE psyq_shim)
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${SBSP_ROOT})
    # 77: a layer whose inputs are absent (tests/test_skip.h) - ctest then
    # reports the test as skipped, never as passed; build-pc.cmd test and CI
    # set SBSP_TEST_STRICT=1, under which the same case exits 1 (issue #61)
    set_tests_properties(${name} PROPERTIES LABELS unit SKIP_RETURN_CODE 77)
endfunction()

sbsp_shim_test(gte_trig_test)   # exact fixed-point trig fixtures (M1)
sbsp_shim_test(gte_test)        # software cop2 / libgte fixtures (M3)
sbsp_shim_test(gpu_test)        # software GPU: VRAM ops, OT walker, goldens
# raster.cpp vs its frozen pre-optimisation copy, halfword for halfword over
# seeded-random primitives.  The reference is compiled like the shim's own
# raster.cpp so the closing ref-vs-live timing table compares like with like.
sbsp_shim_test(raster_diff_test)
target_sources(raster_diff_test PRIVATE tests/raster_ref.cpp)
set_source_files_properties(tests/raster_ref.cpp PROPERTIES COMPILE_OPTIONS "${SBSP_OPT_FLAGS}")
sbsp_shim_test(adpcm_test)      # SPU ADPCM decoder golden vectors (M5)
sbsp_shim_test(spu_test)        # SPU voice mixer: gaussian/ADSR/pan (M5)
sbsp_shim_test(xm_test)         # XMPlayer: PXM parse vs .xm, VAB upload (M5)
sbsp_shim_test(pad_test)        # SDL virtual gamepad: mapping + rumble (M6, #21)
sbsp_shim_test(replay_test)     # pad recording -> replay round trip: stick, held-across-scene, seed, prompts (#58)
sbsp_shim_test(mcrd_test)       # memory-card image FS + libmcrd protocol (M6)
sbsp_shim_test(xa_test)         # XA-ADPCM decoder + stream engine (M6)
sbsp_shim_test(mdec_test)       # MDEC: dequant, IDCT, yuv, DecDCTin/out (M7)
sbsp_shim_test(vlc3_test)       # VLC v2/v3 + an all-frames movie sweep (M7)
sbsp_shim_test(str_test)        # STR streaming engine over a fake disc (M7)
sbsp_shim_test(cdpace_test)     # CdRead paced in emulated vblanks, not the wall clock (#67)
sbsp_shim_test(pump_test)       # vblanks fire only in waits, capped or uncapped; the spin rule (#67)
sbsp_shim_test(fmv_pipeline_test) # whole movie pipeline, frame CRCs (M7)
sbsp_shim_test(ini_test)        # sbsp.ini loader/defaults + host paths (M8 shell)
sbsp_shim_test(viewport_test)   # fit/integer/stretch viewport arithmetic (M8 shell)
sbsp_shim_test(exit_test)       # Port_Exit owner rule + exit hooks, in child processes (#62)
sbsp_shim_test(window_test)     # paused= counts a focus pause and a window drag once where they overlap (#63)
sbsp_shim_test(framedump_test)  # BMP dumps: pixel-exact, SetDispMask(0) black, write failure = 0 (#27); the 4:3 shape (#53)

# The replacement LZNP encoder (tools/lznp-src) against the game's own decoder
# (source/utils/lznp.cpp): synthetic, structured and fuzzed buffers plus a
# committed fixture must round-trip byte for byte.  No shim - the encoder, the
# decoder and the test alone.  It was a hand-built proof until issue #61.
# A plain tool without SDL, so none of sbsp_exe_link's runtime-DLL copy.
add_executable(lznp_roundtrip tools/lznp-src/roundtrip_test.cpp tools/lznp-src/lznp_encode.cpp
                              ${SBSP_ROOT}/source/utils/lznp.cpp)
target_include_directories(lznp_roundtrip PRIVATE ${SBSP_ROOT}/source/utils)
# the game's lznp.cpp includes "lznp.h" for lznp.H, as the game flavour allows
target_compile_options(lznp_roundtrip PRIVATE ${SBSP_DEBUG_FLAGS} -Wno-nonportable-include-path)
add_test(NAME lznp_roundtrip COMMAND lznp_roundtrip ${SBSP_ROOT}/port/tests/str_frame1.bin
         WORKING_DIRECTORY ${SBSP_ROOT})
set_tests_properties(lznp_roundtrip PROPERTIES LABELS unit)
# The committed port/tools/lznp.exe (LFS; every actor pack goes through it in
# build-data) must be the encoder these sources build: both encode the same
# inputs identically (tests/lznp_exe_check.py).
add_executable(lznp_tool tools/lznp-src/lznp_cli.cpp tools/lznp-src/lznp_encode.cpp)
target_compile_options(lznp_tool PRIVATE ${SBSP_DEBUG_FLAGS})
# The data-pipeline converters rebuilt from Utils/ (issue #48)
include(cmake/datatools.cmake)
if(Python3_Interpreter_FOUND)
    add_test(NAME lznp_exe_matches_source
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lznp_exe_check.py
                     $<TARGET_FILE:lznp_tool> ${SBSP_ROOT}/port/tools/lznp.exe
                     ${SBSP_ROOT}/port/tests/str_frame1.bin
             WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(lznp_exe_matches_source PROPERTIES LABELS unit)
    # ... and so must its clang-cl x64 build in port/tools/x64, which the data
    # build runs by default (issue #48); the one above serves --vintage-tools.
    add_test(NAME lznp_x64_exe_matches_source
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lznp_exe_check.py
                     $<TARGET_FILE:lznp_tool> ${SBSP_ROOT}/port/tools/x64/lznp.exe
                     ${SBSP_ROOT}/port/tests/str_frame1.bin
             WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(lznp_x64_exe_matches_source PROPERTIES LABELS unit)
    # The committed port/tools/x64 converters (LFS, issue #48: build-data runs
    # them in place of the 1999 tools/ exes) must be what the Utils/ sources
    # build.  This tree's copies, built by whichever toolchain, and the
    # committed x64 ones convert the same inputs to the same bytes
    # (tests/datatools_exe_check.py), so a 32/64-bit difference in the output
    # shows up here too.  MkActor, MkLevel, parkgrab and transtext build with
    # clang-cl only (cmake/datatools.cmake).
    set(SBSP_DATATOOL_CHECK_ARGS)
    if(TARGET mkactor_tool)
        list(APPEND SBSP_DATATOOL_CHECK_ARGS
             --mkactor $<TARGET_FILE:mkactor_tool> ${SBSP_ROOT}/port/tools/x64/MkActor.exe
             --mklevel $<TARGET_FILE:mklevel_tool> ${SBSP_ROOT}/port/tools/x64/MkLevel.exe
             --parkgrab $<TARGET_FILE:parkgrab_tool> ${SBSP_ROOT}/port/tools/x64/parkgrab.exe
             --transtext $<TARGET_FILE:transtext_tool> ${SBSP_ROOT}/port/tools/x64/transtext.exe)
    endif()
    add_test(NAME datatools_exe_matches_source
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/datatools_exe_check.py
                     $<TARGET_FILE:mkdata_tool> ${SBSP_ROOT}/port/tools/x64/MkData.exe
                     $<TARGET_FILE:tga2gfx_tool> ${SBSP_ROOT}/port/tools/x64/Tga2Gfx.exe
                     $<TARGET_FILE:scripter_tool> ${SBSP_ROOT}/port/tools/x64/scripter.exe
                     ${SBSP_DATATOOL_CHECK_ARGS}
             WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(datatools_exe_matches_source PROPERTIES LABELS unit)
    # port/tools/psx_identity.py's rules (what `lines` passes as the same PS1
    # tokens, the makefile.gaz pattern `build` keys on) on fixed cases, the
    # PR #91 review's counterexamples included (tests/psx_identity_test.py).
    add_test(NAME psx_identity_rules
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/psx_identity_test.py
             WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(psx_identity_rules PROPERTIES LABELS unit)
    # The data build's text steps, port/tools/data/lang.py and trans_scr.py
    # (the 1999 lang.pl / text_extract.pl rules), on fixed cases (issue #95).
    add_test(NAME data_text_rules
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/lang_test.py
             WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(data_text_rules PROPERTIES LABELS unit)
    # port/tools/build_pc.py's command line, as build-pc.sh's was, and
    # game_sources.cmake in step with makefile.gaz (issue #98).
    add_test(NAME build_scripts_rules
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/build_scripts_test.py
             WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(build_scripts_rules PROPERTIES LABELS unit)
endif()

# M8 playthrough harness (tests/run_tier.py): the exit-code self-test, the
# fast Tier 1 routes and the short Tier 2 sweep, all --uncapped.  The full
# tiers are `port/build-pc.cmd soak`.  Every run's log lands in
# <build>/tier-logs (CI uploads that directory when a tier fails).
if(SBSP_BUILD_GAME AND SBSP_PLAYTHROUGH)
    set(SBSP_TIER ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tier.py
        --exe $<TARGET_FILE:sbsp> --territory ${SBSP_TERRITORY}
        --logs ${CMAKE_CURRENT_BINARY_DIR}/tier-logs)
    add_test(NAME exit_codes  COMMAND ${SBSP_TIER} --selftest      WORKING_DIRECTORY ${SBSP_ROOT})
    add_test(NAME tier1_fast  COMMAND ${SBSP_TIER} --tier1 --fast  WORKING_DIRECTORY ${SBSP_ROOT})
    add_test(NAME tier2_short COMMAND ${SBSP_TIER} --tier2 --short WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(exit_codes tier1_fast tier2_short PROPERTIES LABELS playthrough TIMEOUT 1800)
endif()
