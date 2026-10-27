"""Where the port's build scripts find CMake and Ninja (issue #98).

build_pc.py and data/build_data.py import this.  Nothing here names
C:\\msys64: each tool comes from, in order,
  1. an SBSP_CMAKE / SBSP_NINJA environment variable (the exe's path);
  2. the PATH (a Visual Studio developer prompt puts both there, and so does
     a GitHub runner);
  3. Visual Studio's "C++ CMake tools for Windows" component, newest
     install first, found through vswhere.exe.  It ships cmake.exe, ctest.exe
     and ninja.exe beside the MSVC tools the clang-cl toolchain uses.
A CMake older than MIN_CMAKE is skipped (the presets are schema 8, which
needs 3.28; Visual Studio 2019's bundled CMake is 3.20).
"""
import os
import re
import shutil
import subprocess
import sys

MIN_CMAKE = (3, 28)

VSWHERE = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")
VS_CMAKE_COMPONENT = "Microsoft.VisualStudio.Component.VC.CMake.Project"
VS_CMAKE_DIR = os.path.join("Common7", "IDE", "CommonExtensions", "Microsoft", "CMake")


def vs_installs():
    """Visual Studio installs with the CMake component, newest first."""
    if not os.path.isfile(VSWHERE):
        return []
    r = subprocess.run([VSWHERE, "-all", "-products", "*", "-sort", "-requires", VS_CMAKE_COMPONENT,
                        "-property", "installationPath"], capture_output=True, text=True)
    return [line.strip() for line in r.stdout.splitlines() if line.strip()]


def cmake_version(exe):
    """(major, minor) of a cmake.exe, or None if it doesn't run."""
    try:
        out = subprocess.run([exe, "--version"], capture_output=True, text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    m = re.search(r"cmake version (\d+)\.(\d+)", out)
    return (int(m.group(1)), int(m.group(2))) if m else None


def _candidates(name, env_var, vs_subdir):
    if os.environ.get(env_var):
        yield os.environ[env_var], env_var
    exe = shutil.which(name)
    if exe:
        yield exe, "the PATH"
    for inst in vs_installs():
        yield os.path.join(inst, VS_CMAKE_DIR, vs_subdir, name + ".exe"), inst


def find_cmake():
    """cmake.exe, MIN_CMAKE or newer.  ctest.exe sits beside it."""
    tried = []
    for exe, where in _candidates("cmake", "SBSP_CMAKE", os.path.join("CMake", "bin")):
        if not os.path.isfile(exe):
            continue
        v = cmake_version(exe)
        if v and v >= MIN_CMAKE:
            return exe
        tried.append("%s (%s): %s" % (exe, where, "version %d.%d" % v if v else "doesn't run"))
    sys.exit("no CMake %d.%d or newer.  Install Visual Studio 2022's \"C++ CMake tools for Windows\"\n"
             "component (Visual Studio Installer > Modify > Desktop development with C++), put a CMake on\n"
             "the PATH, or set SBSP_CMAKE to a cmake.exe.%s"
             % (MIN_CMAKE + ("".join("\n  skipped " + t for t in tried),)))


def find_ninja():
    for exe, _ in _candidates("ninja", "SBSP_NINJA", "Ninja"):
        if os.path.isfile(exe):
            return exe
    sys.exit("no Ninja.  Install Visual Studio 2022's \"C++ CMake tools for Windows\" component,\n"
             "put a ninja.exe on the PATH, or set SBSP_NINJA to one.")


def ctest_for(cmake):
    return os.path.join(os.path.dirname(cmake), "ctest.exe")
