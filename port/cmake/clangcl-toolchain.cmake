# clang-cl toolchain for the SBSPSS Win11 port (M8 PR 4): LLVM's MSVC-compatible
# driver targeting i686-pc-windows-msvc, linked by lld-link against the MSVC
# CRT and Windows SDK.  32-bit for the same reason as the MinGW build (the
# on-disc formats embed 4-byte pointers, tools/Data/include/dstructs.h).
#
# Needs: an LLVM install with clang-cl / lld-link / llvm-rc (the official
# LLVM release at C:\Program Files\LLVM, or the "C++ Clang tools for
# Windows" component of Visual Studio 2022/2026) and Visual Studio's MSVC
# x86 build tools + a Windows 10/11 SDK.  No MSYS2 involved except that the
# presets borrow its ninja.exe - pass -DCMAKE_MAKE_PROGRAM=ninja to use one
# on PATH instead.
#
# Inside a "x86 Native Tools" (vcvars) prompt the INCLUDE/LIB environment is
# used as-is.  Outside one, the newest MSVC toolset and Windows SDK found
# under the default install roots are handed to clang-cl (/vctoolsdir,
# /winsdkdir) and to lld-link (/libpath), so a plain PowerShell or the
# MSYS2 shell works too.  Override with -DSBSP_VCTOOLSDIR=... /
# -DSBSP_WINSDKDIR=... (and -DLLVM_ROOT=... for the compiler).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

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
set(CMAKE_C_COMPILER_TARGET   i686-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET i686-pc-windows-msvc)
set(CMAKE_LINKER      "${_sbsp_llvm_bin}/lld-link.exe")
set(CMAKE_RC_COMPILER "${_sbsp_llvm_bin}/llvm-rc.exe")
set(CMAKE_MT          "${_sbsp_llvm_bin}/llvm-mt.exe")

# ---------------------------------------------------------------------------
# MSVC headers/libs when not inside a vcvars prompt
# ---------------------------------------------------------------------------
if(NOT DEFINED ENV{VCToolsInstallDir})
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
    endif()
    if(NOT SBSP_WINSDKDIR)
        set(SBSP_WINSDKDIR "C:/Program Files (x86)/Windows Kits/10")
    endif()
    file(GLOB _sbsp_sdk LIST_DIRECTORIES true "${SBSP_WINSDKDIR}/Lib/10.*")
    list(SORT _sbsp_sdk COMPARE NATURAL)
    list(LENGTH _sbsp_sdk _n)
    if(_n EQUAL 0)
        message(FATAL_ERROR "No Windows 10/11 SDK under ${SBSP_WINSDKDIR}/Lib: install one or set SBSP_WINSDKDIR")
    endif()
    list(GET _sbsp_sdk -1 _sbsp_sdk_lib)
    get_filename_component(SBSP_WINSDKVER "${_sbsp_sdk_lib}" NAME)

    set(_sbsp_cl_env "/vctoolsdir \"${SBSP_VCTOOLSDIR}\" /winsdkdir \"${SBSP_WINSDKDIR}\" /winsdkversion ${SBSP_WINSDKVER}")
    set(CMAKE_C_FLAGS_INIT   "${_sbsp_cl_env}")
    set(CMAKE_CXX_FLAGS_INIT "${_sbsp_cl_env}")
    # CMake drives lld-link directly (not through the clang-cl driver, which
    # would have derived these itself), so the x86 library dirs go here.
    set(_sbsp_libpaths
        "/libpath:\"${SBSP_VCTOOLSDIR}/lib/x86\""
        "/libpath:\"${_sbsp_sdk_lib}/um/x86\""
        "/libpath:\"${_sbsp_sdk_lib}/ucrt/x86\"")
    string(JOIN " " _sbsp_libpaths ${_sbsp_libpaths})
    set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_sbsp_libpaths}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_sbsp_libpaths}")
    set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_sbsp_libpaths}")
endif()
