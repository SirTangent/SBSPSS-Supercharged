#!/usr/bin/env python3
"""Check that game-source changes leave the PlayStation build byte-identical.

The port holds the PS1 executable (Spongey.cpe) to byte-identity: a change
made for the PC must not change a single byte the PS1 compiler emits.  The
rules, the patterns that meet them and the traps are in
port/docs/psx_byte_identity.md; this is the tooling that goes with it.

  psx_identity.py lines [--base REV] [FILE...]
      Static and fast (seconds).  Preprocesses each file the way the PS1
      compiler would - only as far as the PSX_MIPS_ASM / mips gates and
      #line go - and requires the PS1 to see exactly the text it saw in
      the base revision, on exactly the same line numbers.  Default files:
      every file under source/ and tools/Data/include the working tree
      changes against the base, untracked ones included.

  psx_identity.py build [--base REV] [--territory USA] [--version DEBUG ...]
      The proof (about five minutes a build).  Clean PS1 builds of HEAD and
      of the base, SHA-256 of each Spongey.cpe compared.  The base side
      checks out the base copy of every changed PS1 build input (BUILD_PATHS
      below), not just the game source.  Needs MSYS2 and the territory's
      data, a committed tree, and no other PS1 build running on the machine.

--base defaults to the merge-base of HEAD and SBSP-Win11, so a branch is
compared with where it started, not with whatever has merged since.

Exit status: 0 identical, 1 not identical, 2 could not check (the reason
goes to stderr).
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Game source the PS1 build compiles, any extension: .cpp/.h, the .mip
# assembly, utils/gpu.inc, the upper-case .H headers.  Everything under
# port/ is PC-only.
GAME_DIRS = ["source", "tools/Data/include"]

# Everything else that decides what the PS1 build produces: makefile.gaz and
# the build/*.mak it includes (compiler flags, link), the per-user makefile
# build/getuser.mak pulls from users/, the PSY-Q toolchain and tools under
# tools/, and the wrapper that drives it all.
BUILD_PATHS = GAME_DIRS + ["makefile.gaz", "build", "users", "tools", "port/build-psx.sh"]

# What the PS1 compiler (ccpsx / EGCS) has defined, as far as the port's
# gates care.  mips comes from the compiler; PSX_MIPS_ASM from
# system/asmport.h, which every game file reaches through system/global.h.
PS1_MACROS = {
    "PSX_MIPS_ASM": True,
    "mips": True,
    "__mips__": True,
    "PSX_NO_ASM": False,
    "SBSP_PC64": False,
}


def fail(msg):
    """Could not check: exit 2, so nobody reads it as 'not identical'."""
    print("psx_identity: %s" % msg, file=sys.stderr)
    sys.exit(2)


def git(*args, check=True):
    r = subprocess.run(["git", "-C", REPO] + list(args), capture_output=True)
    if check and r.returncode != 0:
        fail("git %s: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r


def git_paths(*args):
    """A git command's NUL-separated path list (-z), so spaces survive."""
    return [p for p in git(*args).stdout.decode().split("\0") if p]


def default_base():
    r = git("merge-base", "HEAD", "SBSP-Win11", check=False)
    if r.returncode != 0:
        fail("no merge-base with SBSP-Win11 - pass --base")
    return r.stdout.decode().strip()


# ---------------------------------------------------------------- lines

class Unknown(Exception):
    """A condition that does not depend only on the PS1 macros above."""


def evaluate(expr):
    expr = re.sub(r"/\*.*?\*/", " ", expr)
    expr = re.sub(r"//.*", "", expr)
    if expr.rstrip().endswith("\\"):
        raise Unknown(expr.strip())     # continued on the next line: not worth parsing

    def defined(m):
        name = m.group(1) or m.group(2)
        if name not in PS1_MACROS:
            raise Unknown(name)
        return " 1 " if PS1_MACROS[name] else " 0 "

    expr = re.sub(r"\bdefined\s*(?:\(\s*(\w+)\s*\)|\s(\w+))", defined, expr)
    if re.search(r"[A-Za-z_]\w*", expr):
        raise Unknown(expr.strip())     # a macro's value, not just whether it is defined
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}))
    except Exception:
        raise Unknown(expr.strip())     # ?:, casts, anything Python reads differently


def ps1_view(text):
    """[(line number the PS1 compiler sees, text)] for every line the PS1
    compiles.

    Conditions over the PS1 macros are evaluated and their directive lines
    left out: what they select is in the output, so a change to one shows
    as a change to the lines it keeps.  Any other condition is not
    evaluated: every arm is kept, and so are its #if/#elif/#else/#endif
    lines, so a flipped condition is a changed line too."""
    out = []
    stack = []          # one frame per open #if: parent/active/taken/known
    active = True
    in_comment = False
    line = 1
    for raw in text.split("\n"):
        raw = raw.rstrip("\r")
        s = raw.strip()
        directive = None
        if not in_comment and s.startswith("#"):
            m = re.match(r"#\s*(\w+)\s*(.*)", s)
            if m:
                directive = (m.group(1), m.group(2))
        # crude block-comment tracking, so a '#' inside /* ... */ is text
        for tok in re.findall(r"/\*|\*/", re.sub(r"//.*", "", raw)):
            in_comment = (tok == "/*")

        if directive:
            name, rest = directive
            if name in ("if", "ifdef", "ifndef"):
                if name == "if":
                    cond = rest
                else:
                    word = re.sub(r"/[/*].*", "", rest).strip()
                    cond = ("" if name == "ifdef" else "!") + "defined(%s)" % word
                try:
                    v = evaluate(cond)
                    stack.append(dict(parent=active, active=active and v, taken=v, known=True))
                except Unknown:
                    stack.append(dict(parent=active, active=active, taken=True, known=False))
                    if active:
                        out.append((line, raw))
                active = stack[-1]["active"]
                line += 1
                continue
            if name in ("elif", "else") and stack:
                f = stack[-1]
                if f["known"]:
                    if f["taken"]:
                        f["active"] = False     # an earlier arm won: this one is dead, unevaluated
                    elif name == "else":
                        f["active"] = f["parent"]
                        f["taken"] = True
                    else:
                        try:
                            v = evaluate(rest)
                            f["active"] = f["parent"] and v
                            f["taken"] = v
                        except Unknown:
                            f["known"] = False  # from here on, every arm is kept
                if not f["known"]:
                    f["active"] = f["parent"]
                    if f["parent"]:
                        out.append((line, raw))
                active = f["active"]
                line += 1
                continue
            if name == "endif" and stack:
                f = stack.pop()
                if not f["known"] and f["parent"]:
                    out.append((line, raw))
                active = f["parent"]
                line += 1
                continue
            if name == "line":
                if active:
                    line = int(re.match(r"\d+", rest).group(0))
                else:
                    line += 1
                continue
        if active:
            out.append((line, raw))
        line += 1
    return out


def cmd_lines(args):
    base = args.base or default_base()
    files = args.files
    if not files:
        files = git_paths("diff", "-z", "--name-only", base, "--", *GAME_DIRS)
        files += git_paths("ls-files", "-z", "--others", "--exclude-standard", "--", *GAME_DIRS)
    if not files:
        print("no game-source changes against %s" % base[:10])
        return 0
    bad = 0
    for f in sorted(set(f.replace("\\", "/") for f in files)):
        r = git("show", "%s:%s" % (base, f), check=False)
        if r.returncode != 0:
            print("NEW   %s - not in the base; a new file the PS1 compiles cannot be identical" % f)
            bad += 1
            continue
        old = ps1_view(r.stdout.decode("latin-1"))
        path = os.path.join(REPO, f)
        if not os.path.exists(path):
            print("GONE  %s - deleted" % f)
            bad += 1
            continue
        new = ps1_view(open(path, "rb").read().decode("latin-1"))
        if old == new:
            print("ok    %s" % f)
            continue
        bad += 1
        print("DIFF  %s - the PS1 sees different text or line numbers:" % f)
        olds, news = set(old), set(new)
        shown = 0
        for n, t in sorted((olds ^ news), key=lambda x: x[0]):
            side = "base" if (n, t) in olds else "now "
            print("        %s %5d: %s" % (side, n, t.strip()[:100]))
            shown += 1
            if shown >= 12:
                print("        ...")
                break
    return 1 if bad else 0


# ---------------------------------------------------------------- build

def msys_bash():
    for p in (r"C:\msys64\usr\bin\bash.exe", "/usr/bin/bash"):
        if os.path.exists(p):
            return p
    fail("MSYS2 bash not found (C:\\msys64) - the PS1 build runs under MSYS2, see psx_byte_identity.md")


def posix(path):
    path = path.replace("\\", "/")
    m = re.match(r"([A-Za-z]):/(.*)", path)
    return "/%s/%s" % (m.group(1).lower(), m.group(2)) if m else path


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def psx_build(territory, version, log):
    """A clean PS1 build.  Returns the path of a Spongey.cpe that this build
    wrote: the old one is deleted first and the new one must be newer than
    the start, so a build that fails without saying so cannot hand back the
    other side's executable (build-psx.sh warns its exit status can be
    swallowed)."""
    tree = os.path.join(REPO, "out", territory, version)
    for d in (os.path.join(tree, "CD", "objs"), os.path.join(tree, "deps")):
        shutil.rmtree(d, ignore_errors=True)        # a clean build: every TU recompiled
    cpe = os.path.join(tree, "version", "CD", "Spongey.cpe")
    if os.path.exists(cpe):
        os.remove(cpe)
    start = time.time()
    env = dict(os.environ, MSYSTEM="MSYS")
    cmd = "cd '%s' && port/build-psx.sh %s %s" % (posix(REPO), territory, version)
    with open(log, "wb") as f:
        r = subprocess.run([msys_bash(), "-lc", cmd], stdout=f, stderr=subprocess.STDOUT, env=env)
    if r.returncode != 0 or not os.path.exists(cpe) or os.path.getmtime(cpe) < start - 2:
        fail("PS1 build failed (%s %s) - see %s" % (territory, version, log))
    return cpe


def cmd_build(args):
    base = args.base or default_base()
    territory = args.territory.upper()
    versions = [v.upper() for v in (args.version or ["DEBUG", "FINAL"])]
    if ".claude/worktrees" in REPO.replace("\\", "/"):
        fail("this checkout's path is too deep for asmpsx (see psx_byte_identity.md) - use a short path")
    if git("diff", "--quiet", "HEAD", "--", *BUILD_PATHS, check=False).returncode != 0:
        fail("commit (or stash) the changes to PS1 build inputs first - the base build swaps files in and out")
    untracked = git_paths("ls-files", "-z", "--others", "--exclude-standard", "--", *BUILD_PATHS)
    if untracked:
        fail("untracked files would be built on both sides - add or remove them first:\n  "
             + "\n  ".join(untracked))
    r = git("diff", "-z", "--no-renames", "--name-status", base, "HEAD", "--", *BUILD_PATHS)
    fields = [p for p in r.stdout.decode().split("\0") if p]
    changes = list(zip(fields[0::2], fields[1::2]))
    odd = [c for c in changes if c[0] != "M"]
    if odd:
        fail("files added or removed against the base - swapping in the base copies cannot rebuild it:\n  "
             + "\n  ".join("%s\t%s" % c for c in odd))
    files = [c[1] for c in changes]
    if not files:
        print("no PS1 build inputs changed against %s - nothing to compare" % base[:10])
        return 0
    trans = os.path.join(REPO, "out", territory, "include", "trans.h")
    if not os.path.exists(trans):
        fail("%s missing - run port/build-data.cmd %s first" % (trans, territory.lower()))

    outdir = os.path.join(REPO, "out", "psx_identity")
    os.makedirs(outdir, exist_ok=True)
    trans_before = sha256(trans)
    print("base %s, %d changed file(s), trans.h %s" % (base[:10], len(files), trans_before[:16]))
    results = []
    for v in versions:
        hashes = {}
        for side in ("head", "base"):
            if side == "base":
                git("checkout", base, "--", *files)
            try:
                cpe = psx_build(territory, v, os.path.join(outdir, "%s-%s.log" % (side, v)))
                keep = os.path.join(outdir, "%s-%s.cpe" % (side, v))
                shutil.copyfile(cpe, keep)
                hashes[side] = sha256(keep)
            finally:
                if side == "base":
                    git("checkout", "HEAD", "--", *files)
        results.append((v, hashes["head"], hashes["base"]))
    if sha256(trans) != trans_before:
        fail("trans.h changed during the run (a data build?) - the comparison is void")
    failed = 0
    for v, head, base_hash in results:
        same = head == base_hash
        failed += not same
        print("%s %s %s: head %s base %s" % ("IDENTICAL" if same else "DIFFERENT", territory, v,
                                              head[:16], base_hash[:16]))
    if failed:
        print("cpe copies and build logs: %s" % outdir)
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("lines", help="static check: the PS1 sees the same text on the same lines")
    pl.add_argument("--base")
    pl.add_argument("files", nargs="*")
    pb = sub.add_parser("build", help="clean PS1 builds of HEAD and base, Spongey.cpe hashes compared")
    pb.add_argument("--base")
    pb.add_argument("--territory", default="USA")
    pb.add_argument("--version", action="append", help="DEBUG and/or FINAL (default both)")
    args = p.parse_args()
    return cmd_lines(args) if args.cmd == "lines" else cmd_build(args)


if __name__ == "__main__":
    sys.exit(main())
