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

Also here, because the three scripts share them: the data step that
build_pc.py and psx/build_psx.py run ahead of their own build (no_data,
data_tools, build_data), and the lock that makes a second build of one
tree wait for the first (hold_lock).
"""
import msvcrt
import os
import re
import shutil
import subprocess
import sys
import time

MIN_CMAKE = (3, 28)

PORT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NO_DATA_ARG = "--no-data"
NO_DATA_ENV = "SBSP_NO_DATA"

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
        # an explicit choice that isn't there is an error, not a fall-through
        # to whatever the PATH has (PR #100 review)
        if not os.path.isfile(os.environ[env_var]):
            sys.exit("%s=%s: no such file" % (env_var, os.environ[env_var]))
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


def hold_lock(path, what):
    """Take the lock file `path`, waiting while another process holds it.
    Ninja has no lock of its own: two builds of one tree would write the
    same files.  Returns the open file; the lock lasts until it is closed
    or the process exits."""
    f = open(path, "a+")
    try:
        msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
    except OSError:
        print("Another %s holds %s - waiting for it to finish..." % (what, path), file=sys.stderr, flush=True)
        while True:
            try:
                msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                break
            except OSError:
                time.sleep(1)
    return f


def no_data(argv):
    """(argv without --no-data, whether the data step is skipped).  The PC
    and PS1 builds build their territory's data first; --no-data, or
    SBSP_NO_DATA in the environment (anything but empty or 0), leaves
    out/<T> as it is.  The variable is for a caller that cannot pass the
    argument: psx_identity.py also drives a base revision's build_psx.py,
    which may not know it."""
    rest = [a for a in argv if a != NO_DATA_ARG]
    return rest, len(rest) != len(argv) or os.environ.get(NO_DATA_ENV, "") not in ("", "0")


def data_tools(territory):
    """The converter set the territory's data tree was last configured with,
    "x64" or "vintage" ("x64" for a tree that does not exist yet)."""
    try:
        with open(os.path.join(PORT, "build", "data-" + territory.lower(), "CMakeCache.txt"), errors="replace") as f:
            for line in f:
                if line.startswith("SBSP_DATA_TOOLS:"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return "x64"


def build_data(territory):
    """Run the territory's data build (data/build_data.py) under this Python
    and return its exit code.  Up to date, it is a configure and a Ninja
    run that finds nothing to do.  It keeps the converters the tree was
    built with: data built with --vintage-tools is not rebuilt with the x64
    ones because a PC build came after it."""
    cmd = [sys.executable, "-I", os.path.join(PORT, "tools", "data", "build_data.py"), territory.lower()]
    if data_tools(territory) == "vintage":
        cmd.append("--vintage-tools")
    return subprocess.run(cmd).returncode
