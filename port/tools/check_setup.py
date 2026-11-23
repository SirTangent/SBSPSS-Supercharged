#!/usr/bin/env python3
"""Check this machine has what the build needs; fix what is safe to fix.
port\\setup.cmd runs this.

    setup             check each prerequisite: ok, or what to do
    setup --fix       also run `git lfs pull`, and add Visual Studio's missing
                      components from the repository's .vsconfig to the
                      newest install (the Installer asks for admin rights)

Each check asks what the build itself asks, the same way:

- Python 3.8+ (this interpreter, which port\\findpy.cmd picked);
- Git, Git LFS, and every LFS file pulled rather than a pointer;
- CMake MIN_CMAKE+ and Ninja, through buildenv.find_cmake / find_ninja;
- clang-cl, in cmake/clangcl-toolchain.cmake's order (the LLVM release,
  then every Visual Studio install vswhere reports), and no older than that
  file's SBSP_CLANG_MIN;
- an MSVC toolset (VC/Tools/MSVC/<ver>) and a Windows 10/11 SDK;
- the Vulkan loader, which a GPU driver installs.

A missing Visual Studio is not installed from here (several GB): the winget
command is printed instead.  Exit 0 when nothing required is missing, 1
otherwise.
"""
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
VSCONFIG = os.path.join(ROOT, ".vsconfig")
TOOLCHAIN = os.path.join(ROOT, "port", "cmake", "clangcl-toolchain.cmake")
MIN_PYTHON = (3, 8)
LFS_POINTER = b"version https://git-lfs.github.com/spec"

PF = os.environ.get("ProgramFiles", r"C:\Program Files")
PF86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
VS_INSTALLER = os.path.join(PF86, "Microsoft Visual Studio", "Installer")
LLVM_RELEASE = os.path.join(PF, "LLVM", "bin")
# the toolchain's fallback roots, for a machine without vswhere
VS_FIXED_ROOTS = [os.path.join(PF, "Microsoft Visual Studio", "18", "*"),
                  os.path.join(PF, "Microsoft Visual Studio", "2022", "*"),
                  os.path.join(PF86, "Microsoft Visual Studio", "2019", "*")]

OK, MISSING, WARN = "ok", "MISSING", "warn"


class Result:
    """One check.  `fixer` names what --fix runs for it: "lfs", "vs" (the
    Visual Studio Installer with the .vsconfig) or "" (nothing: the fix
    text says what to do by hand)."""
    def __init__(self, name, status, detail, fix="", fixer=""):
        self.name, self.status, self.detail, self.fix, self.fixer = name, status, detail, fix, fixer


VS_FIX = "import the repository's .vsconfig into Visual Studio (setup --fix does it)"


def run(cmd, **kw):
    """(exit code, stdout) of a command; (None, "") when it cannot start."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=120, **kw)
    except (OSError, subprocess.SubprocessError):
        return None, ""
    return r.returncode, r.stdout


def clang_min():
    """SBSP_CLANG_MIN from the toolchain file: one number for both."""
    try:
        with open(TOOLCHAIN) as f:
            m = re.search(r"set\(SBSP_CLANG_MIN (\d+)\)", f.read())
    except OSError:
        m = None
    return int(m.group(1)) if m else None


def clang_version(version_text):
    """(major, "x.y.z") from `clang-cl --version` output, or None."""
    m = re.search(r"clang version (\d+)\.(\d+)\.(\d+)", version_text)
    return (int(m.group(1)), m.group(0).split()[-1]) if m else None


def is_lfs_pointer(path):
    """Whether a file is a Git LFS pointer rather than its content."""
    try:
        with open(path, "rb") as f:
            return f.read(len(LFS_POINTER)) == LFS_POINTER
    except OSError:
        return True             # a file that is not there is not pulled either


def vs_roots():
    """Every Visual Studio install, newest first (vswhere)."""
    vswhere = os.path.join(VS_INSTALLER, "vswhere.exe")
    if not os.path.isfile(vswhere):
        return []
    _, out = run([vswhere, "-all", "-products", "*", "-sort", "-property", "installationPath"])
    return [line.strip() for line in out.splitlines() if line.strip()]


# --- the checks ---------------------------------------------------------------

def check_python():
    v = sys.version_info
    ok = v[:2] >= MIN_PYTHON
    return Result("Python", OK if ok else MISSING, "%d.%d.%d (%s)" % (v[0], v[1], v[2], sys.executable),
                  "" if ok else "install Python %d.%d or newer: winget install --id Python.Python.3.15" % MIN_PYTHON)


def check_git():
    rc, out = run(["git", "--version"])
    if rc != 0:
        return Result("Git", MISSING, "git is not on the PATH",
                      "install Git for Windows: winget install --id Git.Git (then open a new terminal)")
    git = out.strip().replace("git version ", "")
    rc, out = run(["git", "lfs", "version"])
    if rc != 0:
        return Result("Git", MISSING, "Git %s, but no Git LFS" % git,
                      "Git for Windows includes Git LFS: reinstall it, then run git lfs install")
    lfs = re.search(r"git-lfs/(\S+)", out)
    return Result("Git", OK, "Git %s, Git LFS %s" % (git, lfs.group(1) if lfs else "?"))


def check_lfs_files():
    rc, out = run(["git", "lfs", "ls-files", "-n"], cwd=ROOT)
    if rc != 0:
        return Result("Git LFS files", MISSING, "could not list them (is this a clone of the repository?)",
                      "clone the repository, then run git lfs pull in it")
    files = [line.strip() for line in out.splitlines() if line.strip()]
    missing = [f for f in files if is_lfs_pointer(os.path.join(ROOT, f))]
    if missing:
        shown = ", ".join(missing[:3]) + (" and %d more" % (len(missing) - 3) if len(missing) > 3 else "")
        return Result("Git LFS files", MISSING, "%d of %d not pulled: %s" % (len(missing), len(files), shown),
                      "git lfs pull (setup --fix runs it)", "lfs")
    return Result("Git LFS files", OK, "all %d pulled" % len(files))


def _buildenv():
    sys.path.insert(0, HERE)
    import buildenv             # noqa: E402 - Windows-only (msvcrt), imported once the platform is known
    return buildenv


def check_found(name, finder, env_var):
    """CMake or Ninja, found exactly as the build scripts find them."""
    try:
        exe = finder()
    except SystemExit as e:     # buildenv's finders exit with what to do
        detail = str(e).splitlines()[0]
        if os.environ.get(env_var):     # the variable names a file that is not there: Visual Studio won't help
            return Result(name, MISSING, detail, "correct %s, or unset it to use Visual Studio's" % env_var)
        return Result(name, MISSING, detail, VS_FIX, "vs")
    return Result(name, OK, exe)


def check_cmake(buildenv):
    r = check_found("CMake", buildenv.find_cmake, "SBSP_CMAKE")
    if r.status == OK:
        v = buildenv.cmake_version(r.detail)
        r.detail = "%d.%d (%s)" % (v[0], v[1], r.detail) if v else r.detail
    return r


def clang_candidates(roots):
    yield LLVM_RELEASE
    for root in roots:
        yield os.path.join(root, "VC", "Tools", "Llvm", "x64", "bin")
        yield os.path.join(root, "VC", "Tools", "Llvm", "bin")
    for pattern in VS_FIXED_ROOTS[:2]:
        for root in glob.glob(pattern):
            yield os.path.join(root, "VC", "Tools", "Llvm", "x64", "bin")


def check_clang(roots):
    need = clang_min()
    for d in clang_candidates(roots):
        exe = os.path.join(d, "clang-cl.exe")
        if os.path.isfile(exe):
            _, out = run([exe, "--version"])
            got = clang_version(out)
            if got is None:
                return Result("clang-cl", WARN, "%s (version not recognised)" % exe)
            major, full = got
            if need and major < need:
                return Result("clang-cl", MISSING, "%s is Clang %s; the build needs %d or newer" % (exe, full, need),
                              "update Visual Studio (its C++ Clang tools) or LLVM (winget upgrade --id LLVM.LLVM)")
            return Result("clang-cl", OK, "%s (%s)" % (full, d))
    return Result("clang-cl", MISSING, "not found in the LLVM release or any Visual Studio install",
                  VS_FIX + ", or winget install --id LLVM.LLVM", "vs")


def check_msvc(roots):
    dirs = []
    for root in roots + [r for p in VS_FIXED_ROOTS for r in glob.glob(p)]:
        dirs += [d for d in glob.glob(os.path.join(root, "VC", "Tools", "MSVC", "*")) if os.path.isdir(d)]
    if not dirs:
        return Result("MSVC toolset", MISSING, "no VC/Tools/MSVC/<version> in any Visual Studio install", VS_FIX, "vs")
    newest = max(dirs, key=lambda d: [int(x) for x in re.findall(r"\d+", os.path.basename(d))])
    return Result("MSVC toolset", OK, "%s (%s)" % (os.path.basename(newest), os.path.dirname(newest)))


def check_sdk():
    libs = glob.glob(os.path.join(PF86, "Windows Kits", "10", "Lib", "10.*", "um", "x64", "kernel32.lib"))
    if not libs:
        return Result("Windows SDK", MISSING, "no Windows 10/11 SDK under %s" % os.path.join(PF86, "Windows Kits", "10"),
                      VS_FIX, "vs")
    versions = sorted((os.path.basename(os.path.dirname(os.path.dirname(os.path.dirname(p)))) for p in libs),
                      key=lambda v: [int(x) for x in v.split(".")])
    return Result("Windows SDK", OK, versions[-1])


def check_vulkan():
    loader = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "vulkan-1.dll")
    if os.path.isfile(loader):
        return Result("Vulkan", OK, "loader present (from the GPU driver)")
    return Result("Vulkan", WARN, "no vulkan-1.dll: the game builds, but cannot open its window",
                  "install or update the GPU driver")


# --- fixes --------------------------------------------------------------------

def fix_lfs():
    print("\n> git lfs pull", flush=True)
    return subprocess.run(["git", "lfs", "pull"], cwd=ROOT).returncode == 0


def fix_visual_studio(roots):
    if not roots:
        print("\nNo Visual Studio is installed. From the repository root, in PowerShell:\n"
              "  winget install --id Microsoft.VisualStudio.Community "
              "--override \"--config `\"$PWD\\.vsconfig`\" --passive --wait\"\n"
              "(Microsoft.VisualStudio.BuildTools for Build Tools), then run setup again.")
        return False
    setup = os.path.join(VS_INSTALLER, "setup.exe")
    cmd = [setup, "modify", "--installPath", roots[0], "--config", VSCONFIG, "--passive", "--norestart"]
    print("\n> Visual Studio Installer: adding the .vsconfig's missing components to %s\n"
          "  (close Visual Studio first; approve the admin prompt; this can take a while)" % roots[0], flush=True)
    return subprocess.run(cmd).returncode == 0


def checks():
    results = [check_python(), check_git(), check_lfs_files()]
    if sys.platform != "win32":
        results.append(Result("Platform", MISSING, "%s: the build runs on Windows only, for now" % sys.platform))
        return results, []
    buildenv = _buildenv()
    roots = vs_roots()
    results += [check_cmake(buildenv), check_found("Ninja", buildenv.find_ninja, "SBSP_NINJA"), check_clang(roots),
                check_msvc(roots), check_sdk(), check_vulkan()]
    return results, roots



def report(results):
    print("SBSPSS setup check: %s" % ROOT)
    width = max(len(r.name) for r in results)
    for r in results:
        print("  [%-7s] %-*s  %s" % (r.status, width, r.name, r.detail))
        if r.fix and r.status != OK:
            print("  %s  %*s  -> %s" % (" " * 9, width, "", r.fix))
    return [r for r in results if r.status == MISSING]


def main(argv):
    fix = "--fix" in argv
    if [a for a in argv if a != "--fix"]:
        print(__doc__.split("\n\n")[1], file=sys.stderr)
        return 1
    results, roots = checks()
    missing = report(results)
    fixers = {r.fixer for r in missing}
    if fix and fixers - {""}:
        if "lfs" in fixers:
            fix_lfs()
        if "vs" in fixers:
            fix_visual_studio(roots)
        print()
        results, roots = checks()
        missing = report(results)
    if missing:
        print("\n%d missing.%s" % (len(missing), "" if fix else " port\\setup.cmd --fix fixes what it can."))
        return 1
    print("\nAll set: port\\build-pc.cmd debug")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
