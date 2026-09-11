#
# SBSPSS Win11 port - PC build (M1: scaffold + shim + headless file-I/O proof).
#
# Configure via the presets in port/CMakePresets.json, or directly:
#   cmake -S port -B port/build/debug -G Ninja \
#         --toolchain cmake/mingw32-toolchain.cmake -DSBSP_VARIANT=DEBUG
#
cmake_minimum_required(VERSION 3.28)
project(sbspss LANGUAGES C CXX)

set(SBSP_VARIANT   "DEBUG" CACHE STRING "Game build variant: DEBUG or FINAL")
set(SBSP_TERRITORY "USA"   CACHE STRING "Territory: USA or EUR")
set_property(CACHE SBSP_VARIANT   PROPERTY STRINGS DEBUG FINAL)
set_property(CACHE SBSP_TERRITORY PROPERTY STRINGS USA EUR)

get_filename_component(SBSP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ctest: label `unit` = the shim unit exes + the headless proof, label
# `playthrough` = the M8 harness tiers (tests/run_tier.py).  Every test runs
# from the repo root, where the game's default data path resolves.
enable_testing()
find_package(Python3 COMPONENTS Interpreter)

# The generated headers gate everything (BigLump.h / Sprites.h / trans.h / ...).
if(NOT EXISTS "${SBSP_ROOT}/out/${SBSP_TERRITORY}/include/BigLump.h")
    message(FATAL_ERROR
        "Generated headers missing: ${SBSP_ROOT}/out/${SBSP_TERRITORY}/include/BigLump.h\n"
        "Run port/build-data.cmd (M0 data pipeline) first.")
endif()

include(cmake/game_sources.cmake)

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

set(SBSP_GAME_CXX_FLAGS
    -std=gnu++98
    -fpermissive
    -fno-exceptions
    -fno-rtti
    -fno-strict-aliasing
    -fno-builtin
    -Wall
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
    -g
    -O2
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
set(SBSP_PSYQ_DIRAFTER -idirafter ${SBSP_ROOT}/tools/psyq/include)

function(sbsp_game_flavour target)
    target_compile_definitions(${target} PRIVATE ${SBSP_GAME_DEFINES})
    target_compile_options(${target} PRIVATE ${SBSP_GAME_CXX_FLAGS} ${SBSP_PSYQ_DIRAFTER})
    target_include_directories(${target} PRIVATE ${SBSP_GAME_INCLUDES})
endfunction()

# ---------------------------------------------------------------------------
# Game code (unmodified PS1 sources)
# ---------------------------------------------------------------------------
add_library(sbsp_game STATIC ${SBSP_GAME_SOURCES})
sbsp_game_flavour(sbsp_game)

# system/main.cpp compiles like everything else but is kept out of the lib so
# test executables can supply their own main(); the real game exe (M2+) links
# this object in.
add_library(sbsp_game_main OBJECT ${SBSP_GAME_MAIN_SOURCE})
sbsp_game_flavour(sbsp_game_main)

# ---------------------------------------------------------------------------
# PsyQ-on-PC shim runtime (modern C++)
# ---------------------------------------------------------------------------
# SDL3 (window/input/present host).  The mingw32 package ships a real static
# lib, so the exes stay standalone; SDL3::SDL3-static drags its Win32 deps in
# via the package's CMake config.  Vulkan needs only headers at build time -
# the loader is reached at runtime through SDL_Vulkan_LoadLibrary.
find_package(SDL3 CONFIG REQUIRED)

file(GLOB SBSP_SHIM_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/psyq/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/psyq/*/*.cpp
)

# Argument parsing is kept OUT of the shim archive and linked as objects, for
# the same reason main.cpp is split out of the game lib above: its entire job
# is a priority-101 constructor and it exports only Port_BootLevel, which only
# system/main.cpp calls.  Inside a static archive the linker drops the member
# from every executable that does not reference that one symbol - which is how
# `sbsp_headless --data-dir <path>` came to be silently ignored.  It needs no
# shim headers (stdio/stdlib/string only), so it carries no include set.
list(REMOVE_ITEM SBSP_SHIM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/psyq/host/args.cpp)
add_library(psyq_args OBJECT psyq/host/args.cpp)
target_compile_features(psyq_args PRIVATE cxx_std_17)
target_compile_options(psyq_args PRIVATE -Wall -g -O2)

add_library(psyq_shim STATIC ${SBSP_SHIM_SOURCES})
target_compile_features(psyq_shim PRIVATE cxx_std_17)
target_compile_options(psyq_shim PRIVATE -Wall -g -O2 ${SBSP_PSYQ_DIRAFTER})
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
add_executable(sbsp $<TARGET_OBJECTS:sbsp_game_main> $<TARGET_OBJECTS:psyq_args>)
set_target_properties(sbsp PROPERTIES LINKER_LANGUAGE CXX)
target_link_options(sbsp PRIVATE -static)
target_link_libraries(sbsp PRIVATE
    -Wl,--whole-archive sbsp_game -Wl,--no-whole-archive
    psyq_shim
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# Headless file-I/O proof: whole-archive the game lib so EVERY game TU's
# undefined symbols hit the linker - that is the link-completeness proof.
add_executable(sbsp_headless tests/headless.cpp $<TARGET_OBJECTS:psyq_args>)
sbsp_game_flavour(sbsp_headless)
target_link_options(sbsp_headless PRIVATE -static)
target_link_libraries(sbsp_headless PRIVATE
    -Wl,--whole-archive sbsp_game -Wl,--no-whole-archive
    psyq_shim
)
add_test(NAME sbsp_headless COMMAND sbsp_headless WORKING_DIRECTORY ${SBSP_ROOT})
set_tests_properties(sbsp_headless PROPERTIES LABELS unit)

# Unit tests that exercise the shim directly (no game code): C++17, statically
# linked against psyq_shim, and compiled with the shim's own include
# arrangement - the shadow SDK headers first, the vintage PSY-Q headers only
# via -idirafter - so a test may use the real LIBGPU/LIBGTE prim macros.
function(sbsp_shim_test name)
    add_executable(${name} tests/${name}.cpp)
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_compile_options(${name} PRIVATE ${SBSP_PSYQ_DIRAFTER})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/psyq
        ${SBSP_ROOT}
        ${SBSP_ROOT}/source
        ${SBSP_ROOT}/tools/mod/include
    )
    target_link_options(${name} PRIVATE -static)
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

# M8 playthrough harness (tests/run_tier.py): the exit-code self-test, the
# fast Tier 1 routes and the short Tier 2 sweep, all --uncapped.  The full
# tiers are `port/build-pc.sh soak`.
if(Python3_Interpreter_FOUND)
    set(SBSP_TIER ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tier.py --exe $<TARGET_FILE:sbsp>)
    add_test(NAME exit_codes  COMMAND ${SBSP_TIER} --selftest      WORKING_DIRECTORY ${SBSP_ROOT})
    add_test(NAME tier1_fast  COMMAND ${SBSP_TIER} --tier1 --fast  WORKING_DIRECTORY ${SBSP_ROOT})
    add_test(NAME tier2_short COMMAND ${SBSP_TIER} --tier2 --short WORKING_DIRECTORY ${SBSP_ROOT})
    set_tests_properties(exit_codes tier1_fast tier2_short PROPERTIES LABELS playthrough TIMEOUT 1800)
else()
    message(WARNING "Python3 not found: the playthrough tests are not registered")
endif()
