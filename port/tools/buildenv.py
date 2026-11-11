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
data_left_alone, data_tools, build_data), cache_value, and the lock that makes a second build of one
tree wait for the first (hold_lock).
"""
import errno
import msvcrt
import os
import re
import shutil
import stat
import subprocess
import sys
import time

MIN_CMAKE = (3, 28)

PORT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(PORT)
NO_DATA_ARG = "--no-data"
NO_DATA_ENV = "SBSP_NO_DATA"
# what SBSP_NO_DATA may hold and still mean "build the data": the false
# spellings SBSP_PLAYTHROUGH takes (build_pc.CMAKE_FALSE)
ENV_FALSE = ("", "0", "OFF", "NO", "FALSE", "N")
# msvcrt.locking's errors when another process holds the byte
LOCK_HELD = (errno.EACCES, getattr(errno, "EDEADLOCK", errno.EDEADLK))

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
    waiting = False
    while True:
        try:
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
            return f
        except OSError as e:
            # EACCES (EDEADLOCK from some CRTs) is "another process has it".
            # Anything else is a volume that cannot lock, which waiting
            # would never cure (PR #108 review)
            if e.errno not in LOCK_HELD:
                f.close()
                sys.exit("cannot lock %s: %s" % (path, e))
        if not waiting:
            print("Another %s holds %s - waiting for it to finish..." % (what, path), file=sys.stderr, flush=True)
            waiting = True
        time.sleep(1)


def cache_value(tree, key):
    """A CMakeCache.txt entry's value (any type), or None."""
    try:
        with open(os.path.join(tree, "CMakeCache.txt"), errors="replace") as f:
            for line in f:
                if line.startswith(key + ":"):
                    return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return None


def no_data(argv):
    """(argv without --no-data, why the data step is skipped or "").  The PC
    and PS1 builds build their territory's data first; --no-data, or
    SBSP_NO_DATA in the environment, leaves out/<T> as it is.  The variable
    is off when empty or one of ENV_FALSE (0, OFF, NO, FALSE, N in any
    case), on otherwise.  It is for a caller that cannot pass the argument:
    psx_identity.py also drives a base revision's build_psx.py, which may
    not know it."""
    rest = [a for a in argv if a != NO_DATA_ARG]
    if len(rest) != len(argv):
        return rest, NO_DATA_ARG
    value = os.environ.get(NO_DATA_ENV, "")
    if value.strip().upper() not in ENV_FALSE:
        return rest, "%s=%s in the environment" % (NO_DATA_ENV, value)
    return rest, ""


def is_link(path):
    """Whether `path` itself is a junction or a symbolic link."""
    try:
        return bool(os.lstat(path).st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT)
    except OSError:
        return False


def shared_out(repo, territory):
    """The part of <repo>/out/<T> that is a link to somewhere else, or "".
    A worktree whose out\\ is a junction to another checkout's reads that
    checkout's data; a data build here would write this branch's over it,
    under a lock (port/build/data-<t>.lock) the other checkout does not
    take."""
    for rel in ("out", os.path.join("out", territory.upper())):
        if is_link(os.path.join(repo, rel)):
            return rel
    return ""


def data_left_alone(territory, asked=""):
    """Why a PC or PS1 build does not build the territory's data first, or
    "" when it does: it was asked not to (`asked`, from no_data), out/<T>
    is another checkout's (shared_out), or the data tree is configured with
    the test-only final.dat override, which build_data.py clears - the
    build would quietly go back to lang.py's strings (PR #108 review)."""
    if asked:
        return asked
    link = shared_out(REPO, territory)
    if link:
        return "%s is a link to another directory, whose data this would replace" % link
    if cache_value(data_tree(territory), "SBSP_DATA_FINAL_DAT"):
        return "port/build/data-%s is configured with SBSP_DATA_FINAL_DAT" % territory.lower()
    return ""


def data_tree(territory):
    return os.path.join(PORT, "build", "data-" + territory.lower())


def data_tools(territory):
    """The converter set the territory's data tree was last configured with,
    "x64" or "vintage" ("x64" for a tree that does not exist yet)."""
    return cache_value(data_tree(territory), "SBSP_DATA_TOOLS") or "x64"


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
