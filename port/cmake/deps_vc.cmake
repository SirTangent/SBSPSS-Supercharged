# Prebuilt dependencies for the clang-cl build (M8 PR 4), fetched once into
# port/build/deps (gitignored) with pinned versions and hashes.  The MinGW
# build takes both from MSYS2 packages; clang-cl has no package manager, so:
#
#   SDL3           the official "VC" development package from the SDL release
#                  (SDL3.lib + SDL3.dll - it ships no static lib, which is why
#                  the clang-cl exes carry the DLL beside them, sbsp_exe_link).
#                  Handed to find_package(SDL3) through SDL3_DIR.
#   Vulkan-Headers the Khronos header repo at the tag matching MSYS2's
#                  vulkan-headers (VK_HEADER_VERSION 350) - build-time only,
#                  vk_present.cpp is VK_NO_PROTOTYPES over SDL's loader.
#                  Exported as SBSP_VULKAN_INCLUDE.
#
# To substitute either, pass -DSDL3_DIR=<dir with SDL3Config.cmake> /
# -DSBSP_VULKAN_INCLUDE=<dir containing vulkan/vulkan.h>; a value already set
# skips that fetch.

set(SBSP_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/deps")

# sbsp_fetch_dep(<url> <sha256> <marker file relative to SBSP_DEPS_DIR>):
# download + unpack unless the marker already exists.
function(sbsp_fetch_dep url sha256 marker)
    if(EXISTS "${SBSP_DEPS_DIR}/${marker}")
        return()
    endif()
    get_filename_component(_zip "${url}" NAME)
    set(_zip "${SBSP_DEPS_DIR}/${_zip}")
    message(STATUS "Fetching ${url}")
    file(DOWNLOAD "${url}" "${_zip}" EXPECTED_HASH SHA256=${sha256} STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _st 1 _msg)
        message(FATAL_ERROR "download failed: ${_msg}\n"
                            "Fetch ${url} by hand into ${SBSP_DEPS_DIR} and unpack it there.")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_zip}" DESTINATION "${SBSP_DEPS_DIR}")
    if(NOT EXISTS "${SBSP_DEPS_DIR}/${marker}")
        message(FATAL_ERROR "${_zip} unpacked to an unexpected layout: no ${marker}")
    endif()
endfunction()

if(NOT DEFINED SDL3_DIR AND NOT DEFINED ENV{SDL3_DIR})
    set(_ver 3.4.10)
    sbsp_fetch_dep(
        "https://github.com/libsdl-org/SDL/releases/download/release-${_ver}/SDL3-devel-${_ver}-VC.zip"
        e2b336b10b037934af98308027410732ef7b22f2c6697d58092aa1c209fae7d7
        "SDL3-${_ver}/cmake/SDL3Config.cmake")
    set(SDL3_DIR "${SBSP_DEPS_DIR}/SDL3-${_ver}/cmake" CACHE PATH "SDL3 CMake package dir (clang-cl: the official VC devel package)")
endif()

if(NOT SBSP_VULKAN_INCLUDE)
    set(_tag vulkan-sdk-1.4.350.0)
    sbsp_fetch_dep(
        "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${_tag}.zip"
        43441595055ff0f740bd9d417013c1f5b2f5f1f7b0e5ca1d49e3deeb80777ada
        "Vulkan-Headers-${_tag}/include/vulkan/vulkan.h")
    set(SBSP_VULKAN_INCLUDE "${SBSP_DEPS_DIR}/Vulkan-Headers-${_tag}/include" CACHE PATH "Directory containing vulkan/vulkan.h (clang-cl build)")
endif()
