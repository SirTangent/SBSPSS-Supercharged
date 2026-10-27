"""port/tools/build_pc.py's command line (issue #98), on fixed cases.

build_pc.py replaced port/build-pc.sh with the same command line: issue #98
held it to what build-pc.sh did with 47 argument lists (its build/test/soak/
parity64 steps stubbed out), all equal.  Issue #39 then renamed the presets
(x64 the plain ones, x86-* the 32-bit ones, the clangcl* words refused), and
the table below is that grammar, so a change to it shows up here.  Also:
game_sources.cmake must be what port/cmake/gen_game_sources.py makes from
makefile.gaz.

    build_scripts_test.py
"""
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import build_pc  # noqa: E402

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

print("%d failed" % failed)
sys.exit(1 if failed else 0)
