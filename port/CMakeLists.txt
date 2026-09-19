#
# SBSPSS Win11 port - PC build (M1: scaffold + shim + headless file-I/O proof).
#
# Configure via the presets in port/CMakePresets.json, or directly:
#   cmake -S port -B port/build/debug -G Ninja \
#         --toolchain cmake/mingw32-toolchain.cmake -DSBSP_VARIANT=DEBUG
#
# Two toolchains (M8 PR 4).  MinGW g++ (cmake/mingw32-toolchain.cmake) is
# the shipping build: static exes, SDL3 from the MSYS2 package.  clang-cl
# (cmake/clangcl-toolchain.cmake, i686-pc-windows-msvc) is the Visual Studio
# route: MSVC CRT, lld-link PDBs, SDL3 from the official VC package as a
# DLL.  The compiler frontend picks the flag spellings below and everything
# else is shared, so a shim change that only builds on one of them shows up
# in CI (the clangcl job) rather than on a tester's machine.
#
cmake_minimum_required(VERSION 3.28)
project(sbspss LANGUAGES C CXX)

set(SBSP_VARIANT   "DEBUG" CACHE STRING "Game build variant: DEBUG or FINAL")
set(SBSP_TERRITORY "USA"   CACHE STRING "Territory: USA or EUR")
set_property(CACHE SBSP_VARIANT   PROPERTY STRINGS DEBUG FINAL)
set_property(CACHE SBSP_TERRITORY PROPERTY STRINGS USA EUR)

# MinGW only (clang-cl always writes a PDB): CodeView debug info in the
# objects (-gcodeview, gcc 15+) and a <exe>.pdb beside every exe (ld --pdb),
# so Visual Studio / WinDbg can debug the MinGW build with source and locals.
option(SBSP_CODEVIEW "MinGW: CodeView debug info + .pdb per exe (Visual Studio / WinDbg)" OFF)

get_filename_component(SBSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(SBSP_MSVC ON)       # clang-cl: MSVC-style command line + CRT
else()
    set(SBSP_MSVC OFF)      # MinGW g++
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
# Compiler-specific spellings.  -Wno-<x> switches take the GNU form on both
# (clang-cl passes -W<name> through) - but a bare -Wall is /Wall to clang-cl,
# which is clang's -Weverything; /W3 is its in-order spelling of -Wall (a
# /clang:-Wall would be appended after every -Wno-, undoing them).  Codegen
# and debug-info switches differ outright.
# ---------------------------------------------------------------------------
if(SBSP_MSVC)
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
    # static CRT (/MT), like MinGW's -static: the exes carry their runtime
    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
    # the MSVC CRT's "fopen/getenv/strncpy are deprecated, use the _s forms"
    # advisories: not warnings about this code, and the _s forms are not
    # portable to MinGW
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
elseif(SBSP_CODEVIEW)
    set(SBSP_DEBUG_FLAGS -gcodeview)
    set(SBSP_OPT_FLAGS   -O2)
else()
    set(SBSP_DEBUG_FLAGS -g)
    set(SBSP_OPT_FLAGS   -O2)
endif()
if(NOT SBSP_MSVC)
    set(SBSP_WARN_ALL -Wall)
    set(SBSP_GAME_DIALECT
        -std=gnu++98
        -fpermissive
        -fno-exceptions
        -fno-rtti
        -fno-strict-aliasing
        -fno-builtin
    )
endif()

# The game exes under clang-cl.  MSVC mangles a global's type into its name
# where the Itanium ABI (MinGW) uses the bare name, so a declaration that
# disagrees with its definition is one symbol under gcc and two under
# clang-cl: mathtab.h declares `extern const s16 SinTable[1024]`, sincos.cpp
# defines it non-const.  The linker is told the const spelling is the other
# one.  (The proper fix is a !PSX_MIPS_ASM arm in the PS1 sources; not
# taken while the MinGW exe is the one that ships.)
if(SBSP_MSVC)
    set(SBSP_GAME_LINK_OPTIONS "/alternatename:?SinTable@@3QBFB=?SinTable@@3PAFA")
endif()

# Every exe.  MinGW: the C/C++ runtimes linked in (SDL3 is a static lib
# there too) so the exes are standalone, plus the PDB when SBSP_CODEVIEW.
# clang-cl: /MT already carries the CRT; /DEBUG makes lld-link write
# <exe>.pdb from the /Z7 objects; SDL3 is a DLL in the official VC package
# (it ships no static lib), so it is copied beside each exe to keep "runs
# from its build dir" true on both toolchains.
function(sbsp_exe_link target)
    if(SBSP_MSVC)
        target_link_options(${target} PRIVATE /DEBUG)
        if(TARGET SDL3::SDL3-shared)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
                COMMAND_EXPAND_LISTS)
        endif()
    else()
        target_link_options(${target} PRIVATE -static)
        if(SBSP_CODEVIEW)
            target_link_options(${target} PRIVATE -Wl,--pdb=)      # empty name = <exe>.pdb
        endif()
    endif()
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
)
if(SBSP_MSVC)
    # the UCRT headers spell their exception specifications `noexcept` for
    # every C++ dialect; gnu++98 has no such keyword, and corecrt.h lets the
    # user pick the spelling first
    list(APPEND SBSP_GAME_DEFINES "_CRT_NOEXCEPT=throw()")
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
# BEHIND the system headers (-idirafter) so MinGW's real stddef/stdlib/stdio
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
if(SBSP_MSVC)
    set(SBSP_PSYQ_DIRAFTER "/clang:-idirafter${SBSP_ROOT}/tools/psyq/include")
else()
    set(SBSP_PSYQ_DIRAFTER -idirafter ${SBSP_ROOT}/tools/psyq/include)
endif()

function(sbsp_game_flavour target)
    target_compile_definitions(${target} PRIVATE ${SBSP_GAME_DEFINES})
    target_compile_options(${target} PRIVATE ${SBSP_GAME_CXX_FLAGS} ${SBSP_PSYQ_DIRAFTER})
    target_include_directories(${target} PRIVATE ${SBSP_GAME_INCLUDES})
endfunction()

# ---------------------------------------------------------------------------
# Game code (unmodified PS1 sources)
# ---------------------------------------------------------------------------
if(SBSP_BUILD_GAME)
    add_library(sbsp_game STATIC ${SBSP_GAME_SOURCES})
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
# SDL3 (window/input/present host).  The mingw32 package ships a real static
# lib, so the exes stay standalone; SDL3::SDL3-static drags its Win32 deps in
# via the package's CMake config.  Vulkan needs only headers at build time -
# the loader is reached at runtime through SDL_Vulkan_LoadLibrary - and MSYS2
# puts them on the system include path.  clang-cl has neither package: the
# official SDL3 VC devel package and the Khronos headers are fetched on
# first configure (cmake/deps_vc.cmake), pinned to MSYS2's versions.
if(SBSP_MSVC)
    include(cmake/deps_vc.cmake)
endif()
find_package(SDL3 CONFIG REQUIRED)

file(GLOB SBSP_SHIM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/psyq/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/psyq/*/*.cpp
)

set(SBSP_SHIM_FLAGS ${SBSP_WARN_ALL} ${SBSP_DEBUG_FLAGS} ${SBSP_OPT_FLAGS})

# Argument parsing is kept OUT of the shim archive and linked as objects, for
# the same reason main.cpp is split out of the game lib above: its entire job
# is an early constructor and it exports only Port_BootLevel, which only
# system/main.cpp calls.  Inside a static archive the linker drops the member
# from every executable that does not reference that one symbol - which is how
# `sbsp_headless --data-dir <path>` came to be silently ignored.  It needs no
# shim headers (stdio/stdlib/string + host/compiler.h beside it), so it
# carries no include set.
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
if(TARGET SDL3::SDL3-static)
    target_link_libraries(psyq_shim PUBLIC SDL3::SDL3-static)
else()
    target_link_libraries(psyq_shim PUBLIC SDL3::SDL3)
endif()
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
    target_link_options(sbsp PRIVATE ${SBSP_GAME_LINK_OPTIONS})
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
    target_link_options(sbsp_headless PRIVATE ${SBSP_GAME_LINK_OPTIONS})
    target_link_libraries(sbsp_headless PRIVATE
        $<LINK_LIBRARY:WHOLE_ARCHIVE,sbsp_game>
        psyq_shim
    )
    add_test(NAME sbsp_headless COMMAND sbsp_headless WORKING_DIRECTORY ${SBSP_ROOT})
    # a private save dir: without one the M8 shell would write its first-run
    # sbsp.ini into the developer's real %APPDATA%\SBSPSS (an interactive
    # run's default location)
    set_tests_properties(sbsp_headless PROPERTIES LABELS unit
        ENVIRONMENT "SBSP_SAVE_DIR=${CMAKE_CURRENT_BINARY_DIR}/headless_save")
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
    set_tests_properties(${name} PROPERTIES LABELS unit)
endfunction()

sbsp_shim_test(gte_trig_test)   # exact fixed-point trig fixtures (M1)
sbsp_shim_test(gte_test)        # software cop2 / libgte fixtures (M3)
sbsp_shim_test(gpu_test)        # software GPU: VRAM ops, OT walker, goldens
sbsp_shim_test(adpcm_test)      # SPU ADPCM decoder golden vectors (M5)
sbsp_shim_test(spu_test)        # SPU voice mixer: gaussian/ADSR/pan (M5)
sbsp_shim_test(xm_test)         # XMPlayer: PXM parse vs .xm, VAB upload (M5)
sbsp_shim_test(pad_test)        # SDL virtual gamepad: mapping + rumble (M6, #21)
sbsp_shim_test(mcrd_test)       # memory-card image FS + libmcrd protocol (M6)
sbsp_shim_test(xa_test)         # XA-ADPCM decoder + stream engine (M6)
sbsp_shim_test(mdec_test)       # MDEC: dequant, IDCT, yuv, DecDCTin/out (M7)
sbsp_shim_test(vlc3_test)       # VLC v2/v3 + an all-frames movie sweep (M7)
sbsp_shim_test(str_test)        # STR streaming engine over a fake disc (M7)
sbsp_shim_test(fmv_pipeline_test) # whole movie pipeline, frame CRCs (M7)
sbsp_shim_test(ini_test)        # sbsp.ini loader/defaults + host paths (M8 shell)
sbsp_shim_test(viewport_test)   # fit/integer/stretch viewport arithmetic (M8 shell)

# M8 playthrough harness (tests/run_tier.py): the exit-code self-test, the
# fast Tier 1 routes and the short Tier 2 sweep, all --uncapped.  The full
# tiers are `port/build-pc.sh soak`.  Every run's log lands in
# <build>/tier-logs (CI uploads that directory when a tier fails).
if(SBSP_BUILD_GAME AND Python3_Interpreter_FOUND)
    set(SBSP_TIER ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tier.py
        --exe $<TARGET_FILE:sbsp> --territory ${SBSP_TERRITORY}
        --logs ${CMAKE_CURRENT_BINARY_DIR}/tier-logs)
    add_test(NAME exit_codes  COMMAND ${SBSP_TIER} --selftest      WORKING_DIRECTORY ${SBSP_ROOT})
    add_test(NAME tier1_fast  COMMAND ${SBSP_TIER} --tier1 --fast  WORKING_DIRECTORY ${SBSP_ROOT})
    add_test(NAME tier2_short COMMAND ${SBSP_TIER} --tier2 --short WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(exit_codes tier1_fast tier2_short PROPERTIES LABELS playthrough TIMEOUT 1800)
elseif(SBSP_BUILD_GAME)
    message(WARNING "Python3 not found: the playthrough tests are not registered")
endif()
