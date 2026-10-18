#!/usr/bin/env python3
"""Check that game-source changes leave the PlayStation build byte-identical.

The port holds the PS1 executable (Spongey.cpe) to byte-identity: a change
made for the PC must not change a single byte the PS1 compiler emits, apart
from the line numbers a DEBUG build bakes in (issue #81).  The rules, the
patterns that meet them and the traps are in port/docs/psx_byte_identity.md;
this is the tooling that goes with it.

  psx_identity.py lines [--base REV] [FILE...]
      Static and fast (seconds).  Preprocesses each file the way the PS1
      compiler would - only as far as the PSX_MIPS_ASM / mips gates go -
      and requires the PS1 to see the tokens it saw in the base revision,
      in the same order.  Line numbers, comments and whitespace don't
      count, and the two same-line patterns of psx_byte_identity.md are
      expanded before comparing: PC_PS1(pc, ps1) as its PS1 argument, and
      a macro the file newly defines (one shared copy), or redefines and
      #undefs, at each use, when all its uses are code it reads.  A
      #line directive the PS1 compiles is reported too: none is needed any
      more.  Default files: every file under source/ and tools/Data/include
      the working tree changes against the base, untracked ones included.

  psx_identity.py build [--base REV] [--territory USA] [--version DEBUG ...] [--strict]
      The proof (one to two minutes a build).  Clean PS1 builds of HEAD and
      of the base, SHA-256 of each Spongey.cpe compared.  The base side
      checks out the base copy of every changed PS1 build input (BUILD_PATHS
      below), not just the game source.  FINAL is compared strictly.  DEBUG
      is compared line-insensitively: both sides are built with
      SBSP_IDENTITY_NOLINE, which compiles every baked-in __LINE__
      (PORT_LINE, source/system/dbg.h) as 0, and their objects are deleted
      afterwards so a later normal DEBUG build cannot reuse them.  Strictly
      instead with --strict, or when either side lacks the plumbing
      (NOLINE_PLUMBING below).  Needs MSYS2 and the territory's data, a
      committed tree, and no other PS1 build running on the machine.

--base defaults to the merge-base of HEAD and SBSP-Win11, so a branch is
compared with where it started, not with whatever has merged since.

Exit status: 0 identical, 1 not identical, 2 could not check (the reason
goes to stderr).
"""

import argparse
import difflib
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import zlib

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

# The line-insensitive DEBUG compare (issue #81) takes three files: the header
# that defines PORT_LINE, the makefile that turns SBSP_IDENTITY_NOLINE into a
# -D, and the wrapper that hands it to make.  A revision has the compare only
# if all three carry the working plumbing - these patterns, matched with the
# file's comments stripped, so a comment naming the define is not enough.
# Against an older base, DEBUG is compared strictly.  The makefile.gaz test
# is the ifeq that compares the variable's origin with "command line", in
# any of make's spellings: ifeq or else ifeq, (a,b) or "a" "b" / 'a' 'b',
# either order, any spacing within the line, $(...) or ${...}.  Only the
# comparison itself counts, so "command line" in a comment after a
# different test doesn't.  A missed spelling would make a line shift a false
# DIFFERENT; a wrong match exits 2 ("did not take") when the build log lacks
# NOLINE_MARKER.
_SP = r"[ \t]"
_ORIGIN = r"\$[({]origin%s+SBSP_IDENTITY_NOLINE%s*[)}]" % (_SP, _SP)
_CMDLINE = r"command%s+line" % _SP
NOLINE_IFEQ = (r"^{sp}*(?:else{sp}+)?ifeq{sp}*(?:"
               r"\({sp}*(?:{o}{sp}*,{sp}*{c}|{c}{sp}*,{sp}*{o}){sp}*\)"
               r"|([\"'])(?:{o}\1{sp}+([\"']){c}\2|{c}\1{sp}+([\"']){o}\3))"
               ).format(sp=_SP, o=_ORIGIN, c=_CMDLINE)
NOLINE_DEFINE = "SBSP_IDENTITY_NOLINE"
NOLINE_PLUMBING = {
    "source/system/dbg.h": [r"#\s*ifdef\s+SBSP_IDENTITY_NOLINE\b",
                            r"#\s*define\s+PORT_LINE\s+0\b"],
    "makefile.gaz": [NOLINE_IFEQ,
                     r"-DSBSP_IDENTITY_NOLINE\b"],
    "port/build-psx.sh": [r"--identity-noline\)",
                          r"SBSP_IDENTITY_NOLINE=1"],
}
NOLINE_FILES = list(NOLINE_PLUMBING)
NOLINE_ARG = "--identity-noline"
# What makefile.gaz prints when it takes the define.  Every line-insensitive
# build's log must hold it, and no strict build's may.
NOLINE_MARKER = "SBSP_IDENTITY_NOLINE: PORT_LINE is 0"


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
    """([(line number the PS1 compiler sees, text)] for every line the PS1
    compiles, [(line in the file, text)] for every #line among them).

    Conditions over the PS1 macros are evaluated and their directive lines
    left out: what they select is in the output, so a change to one shows
    as a change to the lines it keeps.  Any other condition is not
    evaluated: every arm is kept, and so are its #if/#elif/#else/#endif
    lines, so a flipped condition is a changed line too."""
    out = []
    pinned = []
    stack = []          # one frame per open #if: parent/active/taken/known
    active = True
    in_comment = False
    line = 1
    for phys, raw in enumerate(text.split("\n"), 1):
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
                    pinned.append((phys, raw))
                else:
                    line += 1
                continue
        if active:
            out.append((line, raw))
        line += 1
    return out, pinned


# The text the PS1 sees, as tokens: what the compiler reads, so comments,
# whitespace and line breaks drop out.  Two macro patterns are expanded,
# because psx_byte_identity.md recommends them for edits the PS1 must not
# see: PC_PS1 (source/system/asmport.h), whose PS1 expansion is its second
# argument, and a macro the file defines that the base did not (one shared
# copy of code that used to be written out).  A new macro produces no code
# where it is defined, only where it is used, so its #define and #undef go
# and its uses between them are expanded; a use outside them stays the bare
# name, as it does for the compiler.  A macro the file defines differently
# from the base (a parameter added, say) is expanded the same way on both
# sides, each with its own definition, if the file #undefs it in both.
# Either kind is expanded only when every use of it is one this compare
# reads as code: none in another macro's body or an #if, no #include
# between its #define and #undef (an X-macro's uses are in the included
# file), and every #undef unconditional and after the #define.  Otherwise
# its #define line stays in the stream and a changed one is a DIFF.  Every
# other macro is compared as written.

SHARED_MACROS = {"PC_PS1": (["pc", "ps1"], ["ps1"])}
SCOPE = "\0"                            # a new macro's #define or #undef, in the token stream

TOKEN = re.compile(r"""
    [A-Za-z_]\w*                        # identifier or keyword
  | \.?\d(?:[eEpP][+-]|[\w.])*          # number
  | "(?:\\.|[^"\\\n])*"                 # string literal
  | '(?:\\.|[^'\\\n])*'                 # character literal
  | ->|\+\+|--|<<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||::|\.\.\.|\#\#|[-+*/%&|^]=
  | \S
""", re.X)
COMMENT_OR_LITERAL = re.compile(
    r"""//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.S)
DEFINE = re.compile(r"#\s*define\s+([A-Za-z_]\w*)(\(([^)]*)\))?\s*(.*)$", re.S)
UNDEF = re.compile(r"#\s*undef\s+([A-Za-z_]\w*)")
INCLUDE = re.compile(r"#\s*include\b")
COND_OPEN = re.compile(r"#\s*if(n?def)?\b")
COND_CLOSE = re.compile(r"#\s*endif\b")


def parse_view(rows):
    """([(kind, text)] logical lines with comments gone, {name: (params or
    None, body tokens)} for each macro defined once and unconditionally,
    {those whose every use is a code token}, {those also #undef'd})."""
    text = COMMENT_OR_LITERAL.sub(
        lambda m: " " if m.group(0)[0] == "/" else m.group(0), "\n".join(t for _, t in rows))
    text = text.replace("\\\n", "")         # continued lines, as the preprocessor splices them
    items, defines, seen = [], {}, {}
    where = {}                              # name: index of its #define
    undefs = {}                             # name: [(index, depth)] of each #undef
    refs = set()                            # names any other directive mentions
    includes = []                           # index of each #include
    depth = 0                               # open conditions ps1_view could not evaluate
    for line in text.split("\n"):
        s = line.strip()
        if not s:
            continue
        if not s.startswith("#"):
            items.append(("code", s))
            continue
        i = len(items)
        items.append(("dir", s))
        if COND_OPEN.match(s):
            depth += 1
        elif COND_CLOSE.match(s):
            depth = max(depth - 1, 0)
        if INCLUDE.match(s):
            includes.append(i)
        m = DEFINE.match(s)
        u = UNDEF.match(s)
        if m:
            name = m.group(1)
            seen[name] = seen.get(name, 0) + 1
            if depth == 0:
                params = [p.strip() for p in m.group(3).split(",")] if m.group(2) is not None else None
                defines[name] = ([] if params == [""] else params, TOKEN.findall(m.group(4)))
                where[name] = i
            refs.update(t for t in TOKEN.findall(m.group(4)) if t != name)
        elif u:
            undefs.setdefault(u.group(1), []).append((i, depth))
        else:
            refs.update(TOKEN.findall(s[1:]))  # #if, #elif, #include, ...: every name in it
    # defined twice, or only under a condition: not known to apply everywhere
    defines = {n: d for n, d in defines.items() if seen[n] == 1}

    def plain(n):
        """Every use of n is a code token, between its #define and its
        #undef if it has one: what expand_in_scope sees is all there is."""
        if n in refs:
            return False                    # in another macro's body, or an #if
        ends = undefs.get(n, [])
        if any(d or i < where[n] for i, d in ends):
            return False                    # a conditional #undef, or one before the #define
        end = ends[0][0] if ends else len(items)
        return not any(where[n] < i < end for i in includes)   # an X-macro's uses are elsewhere
    clean = {n for n in defines if plain(n)}
    return items, defines, clean, {n for n in clean if n in undefs}


def view_tokens(items, fresh):
    """The token stream; a directive is one token, never expanded.  The
    #define/#undef lines of the macros in fresh become SCOPE markers."""
    out = []
    for kind, s in items:
        if kind == "code":
            out += TOKEN.findall(s)
            continue
        m = DEFINE.match(s) or UNDEF.match(s)
        if m and m.group(1) in fresh:
            out.append(SCOPE + ("define " if DEFINE.match(s) else "undef ") + m.group(1))
        else:
            out.append(" ".join(TOKEN.findall(s)))
    return out


def macro_args(tokens, k):
    """The arguments of a call whose '(' is tokens[k], and the index after
    its ')'.  Only parentheses nest, as in the preprocessor: a comma inside
    braces or brackets still splits."""
    args, cur, depth = [], [], 0
    for i in range(k, len(tokens)):
        t = tokens[i]
        if t == "(":
            depth += 1
            if depth == 1:
                continue
        elif t == ")":
            depth -= 1
            if depth == 0:
                args.append(cur)
                return args, i + 1
        elif t == "," and depth == 1:
            args.append(cur)
            cur = []
            continue
        cur.append(t)
    raise Unknown("unbalanced macro call")


def expand(tokens, macros, disabled=frozenset()):
    out = []
    i = 0
    while i < len(tokens):
        t = tokens[i]
        if t not in macros or t in disabled:
            out.append(t)
            i += 1
            continue
        params, body = macros[t]
        if "#" in body or "##" in body:
            raise Unknown("%s uses # or ##" % t)
        if params is None:
            out += expand(body, macros, disabled | {t})
            i += 1
            continue
        if i + 1 >= len(tokens) or tokens[i + 1] != "(":
            out.append(t)                   # the name without a call: not expanded
            i += 1
            continue
        args, i = macro_args(tokens, i + 1)
        if params == [] and args == [[]]:
            args = []
        if len(args) != len(params):
            raise Unknown("%s called with %d argument(s), takes %d" % (t, len(args), len(params)))
        actual = dict(zip(params, (expand(a, macros, disabled) for a in args)))
        body_out = []
        for b in body:
            body_out += actual.get(b, [b])
        out += expand(body_out, macros, disabled | {t})
    return out


def expand_in_scope(tokens, shared, fresh):
    """expand(), each macro of fresh in force only from its #define to its
    #undef.  A call cut by a marker raises Unknown (unbalanced)."""
    out, live, run = [], dict(shared), []
    for t in tokens + [SCOPE + "end"]:
        if not t.startswith(SCOPE):
            run.append(t)
            continue
        out += expand(run, live)
        run = []
        word, _, name = t[1:].partition(" ")
        if word == "define":
            live[name] = fresh[name]
        elif word == "undef":
            live.pop(name, None)
    return out


def ps1_tokens(old, new):
    """The base's and the working tree's token streams, macros expanded as
    above.  Raises Unknown when a macro cannot be expanded."""
    old_items, old_defs, _, old_local = parse_view(old)
    new_items, new_defs, new_clean, new_local = parse_view(new)
    # Expanded only where every use is a code token this compare reads
    # (parse_view's plain()).  A redefined macro must also be #undef'd on
    # both sides: one that outlives the file reaches code never read here.
    redefined = {n: d for n, d in old_defs.items()
                 if n in old_local and n in new_local and new_defs[n] != d}
    fresh = {n: d for n, d in new_defs.items()
             if n in redefined or (n not in old_defs and n in new_clean)}
    return (expand_in_scope(view_tokens(old_items, redefined), SHARED_MACROS, redefined),
            expand_in_scope(view_tokens(new_items, fresh), SHARED_MACROS, fresh))


def show_token_diff(old_tok, new_tok):
    def span(toks, a, b):
        text = " ".join(toks[max(a - 4, 0):b + 4])
        return text if len(text) <= 110 else text[:107] + "..."
    matcher = difflib.SequenceMatcher(None, old_tok, new_tok, autojunk=False)
    shown = 0
    for op, i1, i2, j1, j2 in matcher.get_opcodes():
        if op == "equal":
            continue
        if shown == 6:
            print("        ...")
            break
        print("        base: %s" % span(old_tok, i1, i2))
        print("        now : %s" % span(new_tok, j1, j2))
        shown += 1


def show_line_diff(old, new):
    old_text, new_text = [t for _, t in old], [t for _, t in new]
    shown = 0
    matcher = difflib.SequenceMatcher(None, old_text, new_text, autojunk=False)
    for op, i1, i2, j1, j2 in matcher.get_opcodes():
        if op == "equal":
            continue
        for side, rows in (("base", old[i1:i2]), ("now ", new[j1:j2])):
            for n, t in rows:
                if shown == 12:
                    break
                print("        %s %5d: %s" % (side, n, t.strip()[:100]))
                shown += 1
        if shown == 12:
            print("        ...")
            break


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
        old, _ = ps1_view(r.stdout.decode("latin-1"))
        path = os.path.join(REPO, f)
        if not os.path.exists(path):
            print("GONE  %s - deleted" % f)
            bad += 1
            continue
        new, pinned = ps1_view(open(path, "rb").read().decode("latin-1"))
        for n, t in pinned:
            # The DEBUG compare ignores line numbers, so nothing needs one,
            # and a pinned number goes stale when code above it changes.
            print("LINE  %s:%d - a #line the PS1 compiles; none is needed since #81: %s"
                  % (f, n, t.strip()[:60]))
            bad += 1
        # The same text is the same tokens.  Checked first, and not only for
        # speed: a file whose PC-only arms alone changed passes here even
        # when ps1_tokens could not expand one of its macros.
        same, note = [t for _, t in old] == [t for _, t in new], ""
        if not same:
            try:
                old_tok, new_tok = ps1_tokens(old, new)
                same = old_tok == new_tok
                note = " - same tokens (comments, layout or the macro patterns differ)"
            except Unknown as e:
                old_tok = new_tok = None
                why = str(e)
        if same:
            if not pinned:
                print("ok    %s%s" % (f, note))
            continue
        bad += 1
        if old_tok is None:
            print("DIFF  %s - the PS1 sees different text (tokens not compared: %s):" % (f, why))
            show_line_diff(old, new)
        else:
            print("DIFF  %s - the PS1 sees different tokens:" % f)
            show_token_diff(old_tok, new_tok)
    if bad:
        print("a DIFF is not proof of a different executable: psx_identity.py build decides")
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


def describe(path):
    """Size, CRC32 and SHA-256, read in one streaming pass: the columns of
    port/docs/psx_build_log.csv."""
    h, crc, size = hashlib.sha256(), 0, 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
            crc = zlib.crc32(chunk, crc)
            size += len(chunk)
    return dict(size=size, crc32="%08X" % (crc & 0xFFFFFFFF), sha256=h.hexdigest())


def strip_file_comments(path, text):
    """text without its comments, by the file's language: C for headers,
    '#' to the end of the line for the makefile and the shell script."""
    if path.endswith((".h", ".cpp", ".H")):
        return COMMENT_OR_LITERAL.sub(
            lambda m: " " if m.group(0)[0] == "/" else m.group(0), text)
    return "\n".join(l for l in text.split("\n") if not l.lstrip().startswith("#"))


def knows_noline(rev):
    """Whether rev's tree has the line-insensitive DEBUG compare end to end:
    every pattern of NOLINE_PLUMBING in code, not only in a comment."""
    for f, patterns in NOLINE_PLUMBING.items():
        r = git("show", "%s:%s" % (rev, f), check=False)
        if r.returncode != 0:
            return False
        text = strip_file_comments(f, r.stdout.decode("latin-1").replace("\r\n", "\n"))
        if not all(re.search(p, text, re.M) for p in patterns):
            return False
    return True


def clean_tree(territory, version):
    """Delete a PS1 tree's objects, dependency files and linked Spongey.cpe,
    so its next build recompiles every TU.  make tracks no -D flags, so this
    also keeps a line-insensitive build's objects out of a normal one."""
    tree = os.path.join(REPO, "out", territory, version)
    for d in (os.path.join(tree, "CD", "objs"), os.path.join(tree, "deps")):
        shutil.rmtree(d, ignore_errors=True)
    cpe = os.path.join(tree, "version", "CD", "Spongey.cpe")
    if os.path.exists(cpe):
        os.remove(cpe)
    return cpe


def psx_build(territory, version, log, noline):
    """A clean PS1 build, with PORT_LINE as 0 if noline.  Returns the path of
    a Spongey.cpe that this build wrote: the old one is deleted first and the
    new one must be newer than the start, so a build that fails without
    saying so cannot hand back the other side's executable (build-psx.sh
    warns its exit status can be swallowed)."""
    cpe = clean_tree(territory, version)
    start = time.time()
    env = dict(os.environ, MSYSTEM="MSYS")
    env.pop(NOLINE_DEFINE, None)        # build-psx.sh ignores it; keep its warning out of the log
    cmd = "cd '%s' && port/build-psx.sh %s %s%s" % (
        posix(REPO), territory, version, " " + NOLINE_ARG if noline else "")
    with open(log, "wb") as f:
        r = subprocess.run([msys_bash(), "-lc", cmd], stdout=f, stderr=subprocess.STDOUT, env=env)
    if r.returncode != 0 or not os.path.exists(cpe) or os.path.getmtime(cpe) < start - 2:
        fail("PS1 build failed (%s %s) - see %s" % (territory, version, log))
    marked = NOLINE_MARKER in open(log, "rb").read().decode(errors="replace")
    if marked != noline:
        fail("the %s %s build %s %s - see %s" % (
            territory, version, "did not take" if noline else "took", NOLINE_DEFINE, log))
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

    # FINAL bakes in no line numbers, so it is always compared strictly.
    # DEBUG is compared with PORT_LINE as 0 on both sides, which needs both
    # trees to have the switch: a base from before it is compared strictly.
    if args.strict:
        debug_noline, why = False, "--strict"
    elif not knows_noline(base):
        debug_noline, why = False, "the base predates the %s plumbing in %s" % (
            NOLINE_ARG, ", ".join(NOLINE_FILES))
    elif not knows_noline("HEAD"):
        debug_noline, why = False, "HEAD lacks the %s plumbing in %s" % (
            NOLINE_ARG, ", ".join(NOLINE_FILES))
    else:
        debug_noline, why = True, "both sides built with %s, PORT_LINE 0" % NOLINE_DEFINE

    def mode(v):
        """(line-insensitive?, the reason printed for it) for one version."""
        if v != "DEBUG":
            return False, "strict - it bakes in no line numbers"
        return debug_noline, ("line-insensitive - " if debug_noline else "strict - ") + why

    outdir = os.path.join(REPO, "out", "psx_identity")
    os.makedirs(outdir, exist_ok=True)
    trans_before = describe(trans)["sha256"]
    print("base %s, %d changed file(s), trans.h %s" % (base[:10], len(files), trans_before[:16]))
    for v in versions:
        print("%s: %s" % (v, mode(v)[1]))
    results = []
    for v in versions:
        noline = mode(v)[0]
        name = v + ("-noline" if noline else "")
        found = {}
        for side in ("head", "base"):
            if side == "base":
                git("checkout", base, "--", *files)
            try:
                cpe = psx_build(territory, v, os.path.join(outdir, "%s-%s.log" % (side, name)), noline)
                keep = os.path.join(outdir, "%s-%s.cpe" % (side, name))
                shutil.copyfile(cpe, keep)
                found[side] = describe(keep)
            finally:
                if side == "base":
                    git("checkout", "HEAD", "--", *files)
                if noline:
                    # Its objects hold PORT_LINE 0, and make would reuse
                    # them in the next normal DEBUG build: leave none.
                    clean_tree(territory, v)
        results.append((v, noline, found))
    if describe(trans)["sha256"] != trans_before:
        fail("trans.h changed during the run (a data build?) - the comparison is void")
    failed = 0
    for v, noline, found in results:
        same = found["head"]["sha256"] == found["base"]["sha256"]
        failed += not same
        print("%s %s %s (%s)" % ("IDENTICAL" if same else "DIFFERENT", territory, v,
                                 "line-insensitive" if noline else "strict"))
        for side in ("head", "base"):
            print("    %s %8d B  crc32 %s  sha256 %s" % (
                side, found[side]["size"], found[side]["crc32"], found[side]["sha256"]))
    print("trans.h sha256 %s" % trans_before)
    if failed:
        print("cpe copies and build logs: %s" % outdir)
    return 1 if failed else 0


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("lines", help="static check: the PS1 sees the same text, in the same order")
    pl.add_argument("--base")
    pl.add_argument("files", nargs="*")
    pb = sub.add_parser("build", help="clean PS1 builds of HEAD and base, Spongey.cpe hashes compared")
    pb.add_argument("--base")
    pb.add_argument("--territory", default="USA")
    pb.add_argument("--version", action="append", help="DEBUG and/or FINAL (default both)")
    pb.add_argument("--strict", action="store_true",
                    help="compare DEBUG with its line numbers too (the normal DEBUG build)")
    args = p.parse_args()
    return cmd_lines(args) if args.cmd == "lines" else cmd_build(args)


if __name__ == "__main__":
    sys.exit(main())
