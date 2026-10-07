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
      every game-source file the working tree changes against the base.

  psx_identity.py build [--base REV] [--territory USA] [--version DEBUG ...]
      The proof (about five minutes a build).  Clean PS1 rebuilds of HEAD
      and of the base, SHA-256 of each Spongey.cpe compared.  Needs MSYS2
      and the territory's data, a committed tree, and no other PS1 build
      running on the machine.

--base defaults to the merge-base of HEAD and SBSP-Win11, so a branch is
compared with where it started, not with whatever has merged since.

Exit status: 0 identical, 1 not, 2 could not check.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Game source the PS1 build compiles.  Everything under port/ is PC-only.
GAME_DIRS = ["source", "tools/Data/include"]
GAME_EXTS = (".c", ".cpp", ".h", ".hpp", ".inl")

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


def git(*args, check=True):
    r = subprocess.run(["git", "-C", REPO] + list(args), capture_output=True)
    if check and r.returncode != 0:
        sys.exit("git %s: %s" % (" ".join(args), r.stderr.decode(errors="replace").strip()))
    return r


def default_base():
    r = git("merge-base", "HEAD", "SBSP-Win11", check=False)
    if r.returncode != 0:
        sys.exit("no merge-base with SBSP-Win11 - pass --base")
    return r.stdout.decode().strip()


def is_game_file(path):
    return path.endswith(GAME_EXTS) and any(path.startswith(d + "/") for d in GAME_DIRS)


# ---------------------------------------------------------------- lines

class Unknown(Exception):
    """A condition that does not depend only on the PS1 macros above."""


def evaluate(expr):
    expr = re.sub(r"/\*.*?\*/", " ", expr)
    expr = re.sub(r"//.*", "", expr)

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
    return bool(eval(expr, {"__builtins__": {}}))


def ps1_view(text):
    """[(line number the PS1 compiler sees, text)] for every line the PS1
    compiles, the conditional and #line directives themselves left out.

    Conditions that depend on anything but the PS1 macros are not
    evaluated: both arms are kept, which is right for comparing two
    revisions as long as neither touches that block."""
    out = []
    stack = []          # [parent active, this arm active, an arm taken, known]
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
                    stack.append([active, active and v, v, True])
                except Unknown:
                    stack.append([active, active, True, False])
                active = stack[-1][1]
                line += 1
                continue
            if name in ("elif", "else") and stack:
                parent, _, taken, known = stack[-1]
                if not known:
                    stack[-1][1] = parent
                elif name == "else":
                    stack[-1][1] = parent and not taken
                    stack[-1][2] = True
                else:
                    try:
                        v = evaluate(rest)
                    except Unknown:
                        v, stack[-1][3] = True, False
                    stack[-1][1] = parent and not taken and v
                    stack[-1][2] = taken or v
                active = stack[-1][1]
                line += 1
                continue
            if name == "endif" and stack:
                active = stack.pop()[0]
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
        r = git("diff", "--name-only", base, "--", *GAME_DIRS)
        files = [f for f in r.stdout.decode().split() if is_game_file(f)]
    if not files:
        print("no game-source changes against %s" % base[:10])
        return 0
    bad = 0
    for f in files:
        f = f.replace("\\", "/")
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
    sys.exit("MSYS2 bash not found (C:\\msys64) - the PS1 build runs under MSYS2, see psx_byte_identity.md")


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
    tree = os.path.join(REPO, "out", territory, version)
    for d in (os.path.join(tree, "CD", "objs"), os.path.join(tree, "deps")):
        shutil.rmtree(d, ignore_errors=True)        # a clean build: every TU recompiled
    env = dict(os.environ, MSYSTEM="MSYS")
    cmd = "cd '%s' && port/build-psx.sh %s %s" % (posix(REPO), territory, version)
    with open(log, "wb") as f:
        r = subprocess.run([msys_bash(), "-lc", cmd], stdout=f, stderr=subprocess.STDOUT, env=env)
    cpe = os.path.join(tree, "version", "CD", "Spongey.cpe")
    if r.returncode != 0 or not os.path.exists(cpe):
        sys.exit("PS1 build failed (%s %s) - see %s" % (territory, version, log))
    return cpe


def cmd_build(args):
    base = args.base or default_base()
    territory = args.territory.upper()
    versions = [v.upper() for v in (args.version or ["DEBUG", "FINAL"])]
    if ".claude/worktrees" in REPO.replace("\\", "/"):
        sys.exit("this checkout's path is too deep for asmpsx (see psx_byte_identity.md) - use a short path")
    if git("diff", "--quiet", "HEAD", "--", *GAME_DIRS, check=False).returncode != 0:
        sys.exit("commit (or stash) the game-source changes first - the base build swaps files in and out")
    r = git("diff", "--name-status", base, "HEAD", "--", *GAME_DIRS)
    changes = [l.split("\t") for l in r.stdout.decode().splitlines() if l]
    odd = [c for c in changes if c[0] != "M"]
    if odd:
        sys.exit("files added or removed against the base - the exe cannot be identical:\n  "
                 + "\n  ".join("\t".join(c) for c in odd))
    files = [c[1] for c in changes]
    if not files:
        print("no game-source changes against %s - nothing to compare" % base[:10])
        return 0
    trans = os.path.join(REPO, "out", territory, "include", "trans.h")
    if not os.path.exists(trans):
        sys.exit("%s missing - run port/build-data.cmd %s first" % (trans, territory.lower()))

    outdir = os.path.join(REPO, "out", "psx_identity")
    os.makedirs(outdir, exist_ok=True)
    trans_before = sha256(trans)
    print("base %s, %d changed file(s), trans.h %s" % (base[:10], len(files), trans_before[:16]))
    failed = 0
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
        same = hashes["head"] == hashes["base"]
        failed += not same
        print("%s %s %s: head %s base %s" % ("IDENTICAL" if same else "DIFFERENT", territory, v,
                                              hashes["head"][:16], hashes["base"][:16]))
    if sha256(trans) != trans_before:
        sys.exit("trans.h changed during the run (a data build?) - the comparison is void")
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
