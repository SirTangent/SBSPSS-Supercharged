"""port/tools/build_pc.py's command line (issue #98), on fixed cases.

build_pc.py replaced port/build-pc.sh with the same command line: issue #98
held it to what build-pc.sh did with 47 argument lists (its build/test/soak/
parity64 steps stubbed out), all equal.  Issue #39 then renamed the presets
(x64 the plain ones, x86-* the 32-bit ones, the clangcl* words refused), and
the table below is that grammar, so a change to it shows up here.  Also:
game_sources.cmake must be what port/cmake/gen_game_sources.py makes from
makefile.gaz, and the PS1 build's object list must resolve.

    build_scripts_test.py
"""
import os
import shlex
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "psx"))
import build_pc  # noqa: E402
import psx_manifest  # noqa: E402

# (command line, (mode, presets, ninja args)) - None is a usage error.
# For parity64 the presets are (EUR infix, variant).
CASES = [
    ('', ('build', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('all', ('build', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('usa', ('build', ('debug', 'final'), ())),
    ('eur', ('build', ('eur-debug', 'eur-final'), ())),
    ('USA', ('build', ('debug', 'final'), ())),
    ('debug', ('build', ('debug',), ())),
    ('final', ('build', ('final',), ())),
    ('usa-debug', ('build', ('debug',), ())),
    ('usa-final', ('build', ('final',), ())),
    ('eur-debug', ('build', ('eur-debug',), ())),
    ('eur-final', ('build', ('eur-final',), ())),
    ('x86', ('build', ('x86-debug', 'x86-final'), ())),
    ('x86 usa', ('build', ('x86-debug', 'x86-final'), ())),
    ('x86 eur', ('build', ('x86-eur-debug', 'x86-eur-final'), ())),
    ('x86 EUR', ('build', ('x86-eur-debug', 'x86-eur-final'), ())),
    ('x86-debug', ('build', ('x86-debug',), ())),
    ('x86-final', ('build', ('x86-final',), ())),
    ('x86-eur-debug', ('build', ('x86-eur-debug',), ())),
    ('x86-eur-final -j4', ('build', ('x86-eur-final',), ('-j4',))),
    ('x86 eur -j4', ('build', ('x86-eur-debug', 'x86-eur-final'), ('-j4',))),
    ('debug -j2 -v', ('build', ('debug',), ('-j2', '-v'))),
    ('Debug', ('build', ('debug',), ())),
    ('test', ('test', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('test usa', ('test', ('debug', 'final'), ())),
    ('test eur', ('test', ('eur-debug', 'eur-final'), ())),
    ('test x86', ('test', ('x86-debug', 'x86-final'), ())),
    ('test x86 eur', ('test', ('x86-eur-debug', 'x86-eur-final'), ())),
    ('test x86-debug', ('test', ('x86-debug',), ())),
    ('test final', ('test', ('final',), ())),
    ('soak', ('soak', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('soak eur', ('soak', ('eur-debug', 'eur-final'), ())),
    ('soak x86 eur', ('soak', ('x86-eur-debug', 'x86-eur-final'), ())),
    ('parity64', ('parity64', ('', 'debug'), ())),
    ('parity64 eur', ('parity64', ('eur-', 'debug'), ())),
    ('parity64 final', ('parity64', ('', 'final'), ())),
    ('parity64 eur final', ('parity64', ('eur-', 'final'), ())),
    ('parity64 FINAL EUR', ('parity64', ('eur-', 'final'), ())),
    ('parity64 debug usa', ('parity64', ('', 'debug'), ())),
    ('bogus', None),
    ('x86 xyz', ('build', ('x86-debug', 'x86-final'), ('xyz',))),
    ('test bogus', None),
    ('test x86 xyz', None),
    ('test usa eur extra', None),
    # a territory word only follows x86; after anything else it would be
    # dropped without a word (PR #100 / #101 reviews)
    ('test usa eur', None),
    ('test debug eur', None),
    ('soak eur usa', None),
    ('soak x y z', None),
    ('test x86 usa eur', None),
    ('parity64 eur final x', None),
    ('parity64 nope', None),
    ('clangcl', None),
    ('clangcl64 eur', None),
    ('clangcl-x64-debug', None),
    ('test clangcl64', None),
]

failed = 0
for line, want in CASES:
    try:
        mode, presets, extra = build_pc.plan(shlex.split(line))
        got = (mode, tuple(presets), tuple(extra))
    except build_pc.Usage:
        got = None
    ok = got == want
    failed += not ok
    print("%s  build-pc %s" % ("pass" if ok else "FAIL", line or "(no arguments)"))
    if not ok:
        print("      got:  %r" % (got,))
        print("      want: %r" % (want,))

r = subprocess.run([sys.executable, os.path.join(HERE, "..", "cmake", "gen_game_sources.py"), "--check"])
failed += r.returncode != 0
print("%s  game_sources.cmake matches makefile.gaz" % ("pass" if r.returncode == 0 else "FAIL"))

# The PS1 build's object list (port/tools/psx/psx_manifest.py): every list
# resolves, every source it names exists, and the generated info.mip entry
# is this configuration's.  The order itself was held to make's (all four
# configurations, link file byte-identical) when issue #98 wrote it.
root = os.path.abspath(os.path.join(HERE, "..", ".."))
for t in ("USA", "EUR"):
    for v in ("DEBUG", "FINAL"):
        try:
            asm, cpp = psx_manifest.manifest(t, v)
            info = "system/%s/%s/CD/info" % (v, t)
            missing = [s for s in asm if s != info and not os.path.isfile(os.path.join(root, "source", s + ".mip"))]
            missing += [s for s in cpp if not os.path.isfile(os.path.join(root, "source", s + ".cpp"))]
            ok = info in asm and not missing and len(set(asm + cpp)) == len(asm + cpp)
            why = "missing %s" % missing[:3] if missing else ""
        except SystemExit as e:
            ok, why, asm, cpp = False, str(e), [], []
        failed += not ok
        print("%s  psx_manifest %s %s: %d asm + %d cpp %s" % ("pass" if ok else "FAIL", t, v, len(asm), len(cpp), why))

# The data build's `datastep.py run <path dir> <work dir> -- <command>`
# (port/tools/data): the 40 MkActor steps run side by side only because
# each one's command starts in a working directory of its own, with nothing
# an earlier run left in it (PR #107 review).  The probe writes down where
# it ran, the first directory on its PATH and whether the old file was
# still there.
DATASTEP = os.path.join(HERE, "..", "tools", "data", "datastep.py")
PROBE = ("import os, sys; open(sys.argv[1], 'w').write('\\n'.join("
         "[os.getcwd(), os.environ['PATH'].split(os.pathsep)[0], str(os.path.exists('Actor.Pak'))]))")


def datastep_run(args):
    return subprocess.run([sys.executable, DATASTEP, "run"] + args,
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode


def same_dir(a, b):
    return os.path.normcase(os.path.realpath(a)) == os.path.normcase(os.path.realpath(b))


with tempfile.TemporaryDirectory() as tmp:
    path_dir = os.path.join(tmp, "path dir")
    os.mkdir(path_dir)
    for label, work, stale in (("a working directory that does not exist yet", os.path.join(tmp, "new", "Gary"), False),
                               ("one with an Actor.Pak left in it", os.path.join(tmp, "old work"), True)):
        if stale:
            os.mkdir(work)
            open(os.path.join(work, "Actor.Pak"), "w").close()
        report = os.path.join(tmp, "report.txt")
        if os.path.exists(report):
            os.remove(report)
        rc = datastep_run([path_dir, work, "--", sys.executable, "-c", PROBE, report])
        got = open(report).read().split("\n") if os.path.exists(report) else []
        ok = rc == 0 and len(got) == 3 and same_dir(got[0], work) and same_dir(got[1], path_dir) and got[2] == "False"
        failed += not ok
        print("%s  datastep run: %s" % ("pass" if ok else "FAIL", label))
        if not ok:
            print("      exit %d, the probe saw: %r" % (rc, got))
    # the form before the working directory was an argument, and a command
    # that fails
    for label, args in (("one directory, not two, is refused", [path_dir, "--", sys.executable, "-c", "pass"]),
                        ("no command is refused", [path_dir, os.path.join(tmp, "w"), "--"]),
                        ("no -- is refused", [path_dir, os.path.join(tmp, "w"), sys.executable]),
                        ("the command's failure is the step's", [path_dir, os.path.join(tmp, "w"), "--",
                                                                 sys.executable, "-c", "import sys; sys.exit(3)"])):
        rc = datastep_run(args)
        ok = rc == 1
        failed += not ok
        print("%s  datastep run: %s%s" % ("pass" if ok else "FAIL", label, "" if ok else " (exit %d)" % rc))

print("%d failed" % failed)
sys.exit(1 if failed else 0)
