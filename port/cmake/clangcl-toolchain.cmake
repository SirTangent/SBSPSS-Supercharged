# clang-cl toolchain for the SBSPSS Win11 port (M8 PR 4): LLVM's MSVC-compatible
# driver, linked by lld-link against the MSVC CRT and Windows SDK.
#
# SBSP_ARCH picks the target: x86 (default, i686-pc-windows-msvc - the
# shipping exe, 32-bit like the MinGW build) or x64 (x86_64-pc-windows-msvc,
# M9).  The on-disc formats embed 4-byte pointers
# (tools/Data/include/dstructs.h); the x64 game build reads them through a
# 4-byte pointer type instead of widening them (SBSP_PC64, conv_pc.md).
#
# Needs: an LLVM install with clang-cl / lld-link / llvm-rc (the official
# LLVM release at C:\Program Files\LLVM, or the "C++ Clang tools for
# Windows" component of Visual Studio 2022/2026) and Visual Studio's MSVC
# x64/x86 build tools + a Windows 10/11 SDK.  No MSYS2 involved except that the
# presets borrow its ninja.exe - pass -DCMAKE_MAKE_PROGRAM=ninja to use one
# on PATH instead.
#
# The MSVC toolset and Windows SDK are always named explicitly to clang-cl
# (/vctoolsdir, /winsdkdir, /winsdkversion) and to lld-link (/libpath), so
# the configured tree builds the same from any shell - a plain PowerShell,
# the MSYS2 shell, or a "Native Tools" (vcvars) prompt of either
# architecture.  Inside such a prompt its toolset and SDK are the ones
# named; outside one, the newest found under the default install roots.
# Override with -DSBSP_VCTOOLSDIR=... / -DSBSP_WINSDKDIR=... /
# -DSBSP_WINSDKVER=... (and -DLLVM_ROOT=... for the compiler).

set(CMAKE_SYSTEM_NAME Windows)

set(SBSP_ARCH "x86" CACHE STRING "Target architecture: x86 (i686) or x64 (x86_64)")
# this file is re-read inside every try_compile project, which otherwise would
# not see the cache and would probe the compiler as x86
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SBSP_ARCH)
if(SBSP_ARCH STREQUAL "x86")
    set(CMAKE_SYSTEM_PROCESSOR x86)
    set(_sbsp_triple i686-pc-windows-msvc)
elseif(SBSP_ARCH STREQUAL "x64")
    set(CMAKE_SYSTEM_PROCESSOR AMD64)
    set(_sbsp_triple x86_64-pc-windows-msvc)
else()
    message(FATAL_ERROR "SBSP_ARCH must be x86 or x64, not '${SBSP_ARCH}'")
endif()

set(LLVM_ROOT "" CACHE PATH "LLVM install with clang-cl.exe/lld-link.exe (default: C:/Program Files/LLVM, else the VS-bundled copy)")

file(GLOB _sbsp_vs_llvm LIST_DIRECTORIES true
    "C:/Program Files/Microsoft Visual Studio/18/*/VC/Tools/Llvm/x64/bin"
    "C:/Program Files/Microsoft Visual Studio/18/*/VC/Tools/Llvm/bin"
    "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/x64/bin"
    "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/bin")
find_program(SBSP_CLANG_CL clang-cl
    HINTS "${LLVM_ROOT}/bin" "C:/Program Files/LLVM/bin" ${_sbsp_vs_llvm})
if(NOT SBSP_CLANG_CL)
    message(FATAL_ERROR "clang-cl.exe not found: install LLVM (https://releases.llvm.org) or set LLVM_ROOT")
endif()
get_filename_component(_sbsp_llvm_bin "${SBSP_CLANG_CL}" DIRECTORY)

set(CMAKE_C_COMPILER   "${SBSP_CLANG_CL}")
set(CMAKE_CXX_COMPILER "${SBSP_CLANG_CL}")
set(CMAKE_C_COMPILER_TARGET   ${_sbsp_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_sbsp_triple})
set(CMAKE_LINKER      "${_sbsp_llvm_bin}/lld-link.exe")
set(CMAKE_RC_COMPILER "${_sbsp_llvm_bin}/llvm-rc.exe")
set(CMAKE_MT          "${_sbsp_llvm_bin}/llvm-mt.exe")

# ---------------------------------------------------------------------------
# MSVC headers/libs, named explicitly on every configure
# ---------------------------------------------------------------------------
# The paths go into the _INIT variables whether or not this is a vcvars
# prompt.  _INIT only seeds the cache on the FIRST configure, so flags that
# depended on the prompt would freeze whichever shell configured first into
# the tree: configure from a Native Tools prompt (INCLUDE/LIB set, no flags
# written), build from a plain PowerShell, and clang-cl cannot find stdio.h
# (issue #41).  With /vctoolsdir given, clang-cl ignores %INCLUDE%, so the
# flags alone decide what is used, in any shell.
#
# Each path: a -D value > the vcvars environment (VCToolsInstallDir,
# WindowsSdkDir, WindowsSDKVersion - which end in a backslash) > the newest
# under the default install roots.
#
# A Native Tools prompt of the other architecture is harmless: its toolset
# and SDK directories are shared by both architectures, and its LIB (the
# other architecture's import libraries) is only searched after the explicit
# /libpath dirs below, which lld-link tries first.
get_property(_sbsp_in_tc GLOBAL PROPERTY IN_TRY_COMPILE)
if(NOT _sbsp_in_tc AND DEFINED ENV{VSCMD_ARG_TGT_ARCH} AND NOT "$ENV{VSCMD_ARG_TGT_ARCH}" STREQUAL "${SBSP_ARCH}")
    message(WARNING "This is a $ENV{VSCMD_ARG_TGT_ARCH} Native Tools prompt but SBSP_ARCH=${SBSP_ARCH}: the prompt's architecture is ignored - its toolset and SDK are used with their ${SBSP_ARCH} libraries, named by /libpath ahead of its LIB")
endif()

set(_sbsp_vc_from "-D")
if(NOT SBSP_VCTOOLSDIR AND DEFINED ENV{VCToolsInstallDir})
    file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" SBSP_VCTOOLSDIR)
    string(REGEX REPLACE "/+$" "" SBSP_VCTOOLSDIR "${SBSP_VCTOOLSDIR}")
    set(_sbsp_vc_from "vcvars")
endif()
if(NOT SBSP_VCTOOLSDIR)
    file(GLOB _sbsp_vc LIST_DIRECTORIES true
        "C:/Program Files/Microsoft Visual Studio/18/*/VC/Tools/MSVC/*"
        "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*"
        "C:/Program Files (x86)/Microsoft Visual Studio/2019/*/VC/Tools/MSVC/*")
    list(LENGTH _sbsp_vc _n)
    if(_n EQUAL 0)
        message(FATAL_ERROR "No MSVC toolset found under Visual Studio 2026 (18)/2022/2019 (VC/Tools/MSVC/<ver>): install the 'MSVC C++ x64/x86 build tools' component or set SBSP_VCTOOLSDIR")
    endif()
    # newest toolset version across every VS install (sorting the full
    # path would rank ".../2022/..." above ".../18/..." = VS 2026)
    set(_sbsp_vc_keyed "")
    foreach(_dir IN LISTS _sbsp_vc)
        get_filename_component(_ver "${_dir}" NAME)
        list(APPEND _sbsp_vc_keyed "${_ver}=${_dir}")
    endforeach()
    list(SORT _sbsp_vc_keyed COMPARE NATURAL)
    list(GET _sbsp_vc_keyed -1 _sbsp_vc_top)
    string(REGEX REPLACE "^[^=]*=" "" SBSP_VCTOOLSDIR "${_sbsp_vc_top}")
    set(_sbsp_vc_from "newest installed")
endif()

set(_sbsp_sdk_from "-D")
if(NOT SBSP_WINSDKDIR AND DEFINED ENV{WindowsSdkDir})
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}" SBSP_WINSDKDIR)
    string(REGEX REPLACE "/+$" "" SBSP_WINSDKDIR "${SBSP_WINSDKDIR}")
    set(_sbsp_sdk_from "vcvars")
endif()
if(NOT SBSP_WINSDKDIR)
    set(SBSP_WINSDKDIR "C:/Program Files (x86)/Windows Kits/10")
    set(_sbsp_sdk_from "default root")
endif()
if(NOT SBSP_WINSDKVER AND DEFINED ENV{WindowsSDKVersion})
    file(TO_CMAKE_PATH "$ENV{WindowsSDKVersion}" SBSP_WINSDKVER)
    string(REGEX REPLACE "/+$" "" SBSP_WINSDKVER "${SBSP_WINSDKVER}")
endif()
if(NOT SBSP_WINSDKVER)
    file(GLOB _sbsp_sdk LIST_DIRECTORIES true "${SBSP_WINSDKDIR}/Lib/10.*")
    list(SORT _sbsp_sdk COMPARE NATURAL)
    list(LENGTH _sbsp_sdk _n)
    if(_n EQUAL 0)
        message(FATAL_ERROR "No Windows 10/11 SDK under ${SBSP_WINSDKDIR}/Lib: install one or set SBSP_WINSDKDIR")
    endif()
    list(GET _sbsp_sdk -1 _sbsp_sdk_newest)
    get_filename_component(SBSP_WINSDKVER "${_sbsp_sdk_newest}" NAME)
endif()
set(_sbsp_sdk_lib "${SBSP_WINSDKDIR}/Lib/${SBSP_WINSDKVER}")
# try_compile projects re-read this file without the cache, so hand them the
# paths resolved here - otherwise a -D pointing at an older x86-capable
# toolset would be re-resolved to the newest install inside every probe
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SBSP_VCTOOLSDIR SBSP_WINSDKDIR SBSP_WINSDKVER)

set(_sbsp_cl_env "/vctoolsdir \"${SBSP_VCTOOLSDIR}\" /winsdkdir \"${SBSP_WINSDKDIR}\" /winsdkversion ${SBSP_WINSDKVER}")
get_filename_component(_sbsp_vc_ver "${SBSP_VCTOOLSDIR}" NAME)
set(_sbsp_resolved "MSVC toolset ${_sbsp_vc_ver} (${_sbsp_vc_from}: ${SBSP_VCTOOLSDIR}), Windows SDK ${SBSP_WINSDKVER} (${_sbsp_sdk_from}: ${SBSP_WINSDKDIR})")

# The _INIT flags below seed the cache on the first configure only, so a
# re-configure that resolves another toolset (a newer VS, a different -D or
# prompt) keeps compiling with the one the tree was created with.  Such a
# tree is stale: say which toolset its cached flags name, rather than
# describing (and checking) paths it does not use.
set(_sbsp_stale OFF)
if(NOT _sbsp_in_tc AND DEFINED CACHE{CMAKE_C_FLAGS})
    set(_sbsp_cached "$CACHE{CMAKE_C_FLAGS}")
    string(FIND "${_sbsp_cached}" "${_sbsp_cl_env}" _sbsp_at)
    if(_sbsp_at EQUAL -1)
        set(_sbsp_stale ON)
        set(_sbsp_cached_tc "no MSVC toolset")
        if(_sbsp_cached MATCHES "/vctoolsdir \"([^\"]*)\"")
            get_filename_component(_sbsp_cached_ver "${CMAKE_MATCH_1}" NAME)
            set(_sbsp_cached_tc "MSVC toolset ${_sbsp_cached_ver} (${CMAKE_MATCH_1})")
        endif()
        if(_sbsp_cached MATCHES "/winsdkversion ([^ ]+)")
            string(APPEND _sbsp_cached_tc ", Windows SDK ${CMAKE_MATCH_1}")
        endif()
    endif()
endif()

# a toolset installed without this architecture's libraries (the x86 ones
# are a separate VS component) otherwise surfaces as unresolved CRT symbols
if(NOT _sbsp_stale)
    foreach(_dir "${SBSP_VCTOOLSDIR}/lib/${SBSP_ARCH}" "${_sbsp_sdk_lib}/ucrt/${SBSP_ARCH}" "${_sbsp_sdk_lib}/um/${SBSP_ARCH}")
        if(NOT IS_DIRECTORY "${_dir}")
            message(FATAL_ERROR "No ${SBSP_ARCH} libraries at ${_dir}: install the MSVC ${SBSP_ARCH} build tools / Windows SDK, or point SBSP_VCTOOLSDIR / SBSP_WINSDKDIR / SBSP_WINSDKVER elsewhere")
        endif()
    endforeach()
endif()

# once per configure (this file is read again by every try_compile)
get_property(_sbsp_told GLOBAL PROPERTY SBSP_TOOLCHAIN_TOLD)
if(NOT _sbsp_told AND NOT _sbsp_in_tc)
    set_property(GLOBAL PROPERTY SBSP_TOOLCHAIN_TOLD ON)
    if(_sbsp_stale)
        message(STATUS "clang-cl ${SBSP_ARCH}: this tree's cached flags use ${_sbsp_cached_tc} (resolved now, unused: ${_sbsp_resolved})")
        message(WARNING "stale tree: flags were cached from an earlier configure and still name ${_sbsp_cached_tc}.  Delete ${CMAKE_BINARY_DIR} and configure again to pick up MSVC toolset ${_sbsp_vc_ver}, Windows SDK ${SBSP_WINSDKVER}.")
    else()
        message(STATUS "clang-cl ${SBSP_ARCH}: ${_sbsp_resolved}")
    endif()
endif()

set(CMAKE_C_FLAGS_INIT   "${_sbsp_cl_env}")
set(CMAKE_CXX_FLAGS_INIT "${_sbsp_cl_env}")
# CMake drives lld-link directly (not through the clang-cl driver, which
# would have derived these itself), so the library dirs go here.
set(_sbsp_libpaths
    "/libpath:\"${SBSP_VCTOOLSDIR}/lib/${SBSP_ARCH}\""
    "/libpath:\"${_sbsp_sdk_lib}/um/${SBSP_ARCH}\""
    "/libpath:\"${_sbsp_sdk_lib}/ucrt/${SBSP_ARCH}\"")
string(JOIN " " _sbsp_libpaths ${_sbsp_libpaths})
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_sbsp_libpaths}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_sbsp_libpaths}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_sbsp_libpaths}")
