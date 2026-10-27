"""port/tools/build_pc.py's command line (issue #98), on fixed cases.

build_pc.py replaced port/build-pc.sh and keeps its command line.  The table
below is what build-pc.sh did with each argument list (its build/test/soak/
parity64 steps stubbed out, the issue #98 comparison: 47 lines, all equal),
so a change to the grammar shows up here.  Also: game_sources.cmake must be
what port/cmake/gen_game_sources.py makes from makefile.gaz.

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
    ('clangcl', ('build', ('clangcl-debug', 'clangcl-final'), ())),
    ('clangcl usa', ('build', ('clangcl-debug', 'clangcl-final'), ())),
    ('clangcl eur', ('build', ('clangcl-eur-debug', 'clangcl-eur-final'), ())),
    ('clangcl EUR', ('build', ('clangcl-eur-debug', 'clangcl-eur-final'), ())),
    ('clangcl64', ('build', ('clangcl-x64-debug', 'clangcl-x64-final'), ())),
    ('clangcl64 eur', ('build', ('clangcl-x64-eur-debug', 'clangcl-x64-eur-final'), ())),
    ('clangcl64 eur -j4', ('build', ('clangcl-x64-eur-debug', 'clangcl-x64-eur-final'), ('-j4',))),
    ('clangcl-x64-debug -j2 -v', ('build', ('clangcl-x64-debug',), ('-j2', '-v'))),
    ('clangcl-eur-final', ('build', ('clangcl-eur-final',), ())),
    ('clangcl-x64-eur-debug', ('build', ('clangcl-x64-eur-debug',), ())),
    ('debug -j1', ('build', ('debug',), ('-j1',))),
    ('test', ('test', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('test usa', ('test', ('debug', 'final'), ())),
    ('test eur', ('test', ('eur-debug', 'eur-final'), ())),
    ('test clangcl', ('test', ('clangcl-debug', 'clangcl-final'), ())),
    ('test clangcl eur', ('test', ('clangcl-eur-debug', 'clangcl-eur-final'), ())),
    ('test clangcl64', ('test', ('clangcl-x64-debug', 'clangcl-x64-final'), ())),
    ('test clangcl64 eur', ('test', ('clangcl-x64-eur-debug', 'clangcl-x64-eur-final'), ())),
    ('test clangcl-debug', ('test', ('clangcl-debug',), ())),
    ('soak', ('soak', ('debug', 'final', 'eur-debug', 'eur-final'), ())),
    ('soak eur', ('soak', ('eur-debug', 'eur-final'), ())),
    ('soak clangcl64 eur', ('soak', ('clangcl-x64-eur-debug', 'clangcl-x64-eur-final'), ())),
    ('parity64', ('parity64', ('', 'debug'), ())),
    ('parity64 eur', ('parity64', ('eur-', 'debug'), ())),
    ('parity64 final', ('parity64', ('', 'final'), ())),
    ('parity64 eur final', ('parity64', ('eur-', 'final'), ())),
    ('parity64 FINAL EUR', ('parity64', ('eur-', 'final'), ())),
    ('parity64 debug usa', ('parity64', ('', 'debug'), ())),
    ('bogus', None),
    ('clangcl xyz', ('build', ('clangcl-debug', 'clangcl-final'), ('xyz',))),
    ('test bogus', None),
    ('test clangcl xyz', None),
    ('parity64 eur final x', None),
    ('parity64 nope', None),
    ('Debug', ('build', ('debug',), ())),
    ('CLANGCL64 eur', ('build', ('clangcl-x64-eur-debug', 'clangcl-x64-eur-final'), ())),
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
