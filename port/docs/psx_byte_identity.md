# Keeping the PlayStation build byte-identical

The game source (`source/`, `tools/Data/include/`) is compiled twice: by the
PC port, and by the original 1999 PlayStation toolchain (`ccpsx` / EGCS,
`port/build-psx.sh`).  A change made for the PC must not change the
PlayStation executable, `Spongey.cpe`:
- **FINAL** must stay hash-identical.  **Byte-identical means
  hash-identical, not just the same size.**
- **DEBUG** must stay hash-identical apart from the line numbers it bakes
  in.  Since issue #81 the check builds DEBUG with every such number as 0
  (the line-insensitive compare, below), so moving lines is not a PS1
  change.

A change that is *meant* to change the PS1 game too says so in its
`conv_pc.md` entry, like #24's `(u8)` casts in `font.cpp`, and adds a row to
"Deliberate PS1 changes" below.  Everything else is held to the hash.

CI's `psx-build` job only proves that the PS1 build still compiles and links.
The hash is checked locally:

```
python port/tools/psx_identity.py lines     # static, seconds
python port/tools/psx_identity.py build     # clean PS1 builds of HEAD and base, hashes compared
```

Both compare against the merge-base of `HEAD` and `SBSP-Win11` unless given
`--base REV`.  Both exit 0 when identical, 1 when not, 2 when they could not
check.

## What changes the executable

1. **Any code the PS1 compiles.**  A token changed outside a gate changes
   the bytes, even a refactor that "does the same thing".  Turning an inline
   `switch` into a helper function, or a constant into a variable, can
   change the code EGCS generates.  The build compare is the arbiter.
2. **Line numbers, in DEBUG only.**  Three macros bake the line where they
   are used into the code as an immediate, all through `PORT_LINE`
   (`system/dbg.h`):
   - `ASSERT` (`system/dbg.h`)
   - `DBGMSG` / `__DBGMSG` (`system/dbg.h`)
   - `MemAlloc` (`mem/memory.h`), only with `__DEBUG_MEM__`, which only
     user `paul`'s build defines.  The `CDBUILD` build the port uses never
     does.

   Adding or removing a line *above* one of them in the same file moves its
   number, so a normal DEBUG build changes.  The line-insensitive compare
   doesn't see it, and FINAL compiles all three without a line number.
   `__LINE__` is the line in the file where the macro is *used*, so new
   lines in a header move only the macros used in that header.
3. **The data build.**  transtext numbers the `STR__*` enum of
   `out/<T>/include/trans.h` in its input's order. When the order changes,
   about 30 objects and 250 KB of the cpe differ for no code reason.
   - **Before #95**, the 1999 `lang.pl` wrote that input, `final.dat`, in
     Perl's randomised hash order, so every unseeded data build shuffled the
     string table.
   - **Since #95**, `port/tools/data/lang.py` writes the strings in the order
     their ids first appear in `data/translations/text.dat`. `trans.h` is the
     same on every build and in both territories, and changes only with the
     text.

   `psx_identity.py build` still hashes `trans.h` before and after its run
   and voids the comparison if it moved. Rows of the build log compare only
   when their `trans_h_sha256` matches.
4. **Adding or removing a file the PS1 builds.**  A new translation unit
   changes the link, so it cannot be identical.  #43's `pad/padicon.cpp` was
   one, and was accepted as a deliberate change.

## The two compare modes

`psx_identity.py build` compares each version its own way, and says which
it used:

- **FINAL: strict.**  The two `Spongey.cpe` files must hash the same.
  FINAL bakes in no line numbers, so nothing is left out.
- **DEBUG: line-insensitive.**  Both sides are built with
  `SBSP_IDENTITY_NOLINE`, which makes `PORT_LINE` 0 instead of `__LINE__`.
  Every code change still shows; a line shift doesn't.  The tool passes
  `--identity-noline` to `port/build-psx.sh`, which puts
  `SBSP_IDENTITY_NOLINE=1` on the `make` command line.  It is an argument,
  not an environment variable: `build-psx.sh` ignores (and warns about) one
  in the environment, and `makefile.gaz` takes the variable from the
  command line only.  `makefile.gaz` then adds `-DSBSP_IDENTITY_NOLINE` and
  prints `SBSP_IDENTITY_NOLINE: PORT_LINE is 0 ...`.  The tool requires that
  line in the log of every line-insensitive build, and its absence from
  every other build, so a side that silently ignored the define can't pass.
  The result is a different, slightly smaller executable than the normal
  DEBUG build (220 bytes, USA), made for comparing and nothing else.  make
  does not track `-D` flags, so the tool deletes the objects of every
  line-insensitive build when it is done: the next normal DEBUG build then
  recompiles everything instead of linking in objects with line 0.
  `build-psx.sh` guards a hand run too: it writes the mode to
  `out/<T>/<V>/identity-mode` (no file means normal) and starts the tree
  clean when a build's mode differs from the last one.
- **DEBUG: strict**, line numbers included, in two cases:
  - **Either side lacks the plumbing.**  The tool reads both trees, with
    comments stripped: `system/dbg.h` must define `PORT_LINE` as 0 under
    `#ifdef SBSP_IDENTITY_NOLINE`, `makefile.gaz` must have the command-line
    `ifeq` and the `-D`, and `port/build-psx.sh` must take
    `--identity-noline` (`NOLINE_PLUMBING` in the tool).  A branch started
    before #81 that moves lines therefore sees DEBUG differ.  Rebase it
    rather than add `#line` arms.
  - **`--strict`**, for the hash of the normal DEBUG build.

No shipped or normal build sets `SBSP_IDENTITY_NOLINE`: not CI, not
`build-psx.sh` without `--identity-noline`, and never the PC builds,
where `PORT_LINE` is always `__LINE__`.

## Deliberate PS1 changes

The PS1 build has not been identical to `master`'s since entry 13.  What is
held is each change against its base: `psx_identity.py build` compares a
branch with the commit it started from, never with `master` (issue #56).
These `conv_pc.md` entries change the PS1 game on purpose, and together they
are the whole difference from `master`:

| entry | files | what the PS1 build gets |
|---|---|---|
| 13 | `hazard/hrckshrd.cpp`, `platform/pfishhk.cpp`, `game/gameslot.h`, `gfx/animtex.cpp` | three latent bugs fixed: a stack bbox made `static`, out-params defaulted, a safe `PixPerWord` |
| 14 | `gui/gui.h`, `fileio/fileio.cpp`, `backend/credits.cpp`, `system/clickcount.cpp` | a virtual destructor, a guard on dead code, an enum that also declared a global, and a dual-pathed cast whose extra lines move DEBUG `__LINE__`s |
| 17 | `platform/platform.cpp` | a null-player guard |
| 19 | `gfx/actor.cpp` and six render sites | blank-frame null guards |
| 22 | `memcard/saveload.cpp` | an out-of-date save fails the load instead of being restored |
| 49 | `pad/padicon.cpp` (new), `pad/padicon.h`, `makefile.gaz` | the prompt-icon resolver, a plain glyph lookup on the PS1 |
| 50 | `Graphics/UI/+key*.bmp`, `makefile.gfx` | 14 key caps in `Sprites.Spr`, which renumbers every `FRM__` after them and the `.lvl` data that stores those numbers |
| 51 | the ten prompt draw sites | resolver calls and measured icon widths |
| 53 | `gfx/font.cpp` | `(u8)` font-table indexes (issue #24) |
| 61 | `shop/shop.cpp` | the cross and triangle glyphs back on `master`'s step (issue #56) |
| 65 | the nine files that carried `#line` arms | DEBUG only: its line immediates move, once, to the files' true line numbers (issue #81).  FINAL and the line-insensitive DEBUG compare are unchanged |
| #95 | `port/tools/data/lang.py` (in place of `lang.pl`), through `trans.h` | the `STR__*` ids in `data/translations/text.dat`'s order, so every use of one is renumbered, once.  The old order was Perl's hash order, which no build reproduced reliably, `master`'s included |

A change that alters the PS1 build on purpose adds its row here.  It also
appends its before and after `Spongey.cpe` hashes to
`port/docs/psx_build_log.csv`, and gives them in its pull request.  The log
takes one row per side for each version `psx_identity.py build` compared,
USA: FINAL, and DEBUG as the tool compared it.  That is `DEBUG-NOLINE` (the
line-insensitive build) since #81, or `DEBUG` where it fell back to strict.
Each row has:
- `baseline` for the base commit, `change` for the branch;
- the size, the CRC32 (zlib), the SHA-256, as the tool prints them;
- the SHA-256 of the `trans.h` that build used.

A normal DEBUG build's hash moves with every line shift, so it isn't
logged after #81's own pair of rows.  `historical` rows are older, partial
records.  A hash is only comparable with one from the same data build
(`trans.h`, above), so compare rows only when their `trans_h_sha256`
matches.

## Where the gate comes from

`PSX_MIPS_ASM` is not a compiler flag.  `system/asmport.h` defines it when the
compiler predefines `mips` or `__mips__` (the vintage compiler predefines
`mips`) and `PSX_NO_ASM` is not set.  Every game file reaches `asmport.h`
through `system/global.h`.  A gate placed *before* that include would quietly
read as "PC" on the PlayStation, so check the include order when you gate
somewhere new.

`source/system/vsprintf.h` gates on the compiler macros directly
(`#if !defined(mips) && !defined(__mips__)`): its varargs are an ABI matter,
and `PSX_NO_ASM` must not switch them on a PlayStation (`conv_pc.md` #33).

## Patterns

In order of preference.  None of them needs a `#line`.

### Same-line edits through a macro

The best option when it fits: one line, no `#if` arm, and the PS1 compiler
gets exactly the old tokens.  M9 did most of the x64 work this way.  For
example, `RELOC_PTR(...)` expands to the original tokens everywhere except
x64 (`conv_pc.md` #31).

For a value that should differ on PC, use `PC_PS1(pc, ps1)` from
`system/asmport.h`: `MAP_INSTRUCTIONS_KEYCAP_Y_OFFSET=PC_PS1(2,3),` gives the
PC 2 and hands the PS1 compiler the original `3` (`conv_pc.md` #60).  It
expands unparenthesised, so use it as a whole initializer or operand.  It
also takes a PC-only expression, which the PS1 compiler never sees:

```cpp
	y+=PC_PS1(CPadIcon::getRowPitch(sb,PAD_CROSS,MAP_INSTRUCTIONS_Y_SPACE_BETWEEN_LINES),MAP_INSTRUCTIONS_Y_SPACE_BETWEEN_LINES);
```

(`map/map.cpp`, #81.  `getRowPitch` is declared for the PC only.)

### PC-only logic in PC-only code, called from one line

Put the logic where the PS1 compiler never looks: the shim under `port/`
(`Port_SceneEvent`, the `Port_Autoplay*` accessors), or a function in a
PC-only block.  The game file gets the call, gated:

```cpp
	m_tempBuffer=(unsigned char*)MemAlloc(m_bufferSize,"MEMCARD");
#if !defined(PSX_MIPS_ASM)
	// PC: ... (conv_pc.md #57)
	memset(m_tempBuffer,0,m_bufferSize);
#endif
}
```

The lines after the arm move.  That is fine: FINAL has no line numbers,
and the DEBUG compare ignores them.  (`memcard/saveload.cpp`, #57.)

### PC replacement of PS1 code: keep one copy

Don't keep a frozen copy of the PS1 code in an `#else` arm beside the PC
version: a fix to one copy won't reach the other.  In order:
1. **Share it**, if the shared form compiles to the same PS1 code.  The
   FINAL hash and the line-insensitive DEBUG compare are the proof.  #50's
   button-to-icon switch in `player/player.cpp` lived twice, as the PC's
   `promptIcons()` and an inline PS1 copy.  It is one macro now,
   `PROMPT_ICONS`, which expands to the original statements, so the PS1
   build compiles the drawing pass as before.  A `static inline` function
   was tried first and failed: EGCS kept the array's address in a register
   and stored through it, and FINAL came out 4 bytes smaller (#81).
2. **The difference on one line**, through `PC_PS1` (above).
   `player.cpp`'s row step is `Y-=PC_PS1(RowPitch,PromptYGap);`.
3. **The PC side in a PC-only function**, and a gated call (above).

### Declarations only the PC uses

Gate them in the header too, as `CPadIcon::getRowPitch` is in
`pad/padicon.h`.  This isn't for the bytes, since an unused inline member
emits nothing.  It's so the PS1 build fails to compile if PS1 code ever
starts calling the PC path.

### `#line`: the last resort

Before #81, every PC-only block that added lines above a `__LINE__` user
carried an `#else` arm ending in `#line <n>`, which made the PS1 compiler
number the rest of the file as an older revision had.  #81 retired all 20
of them, in 9 files.  The compare no longer needs them, and each one pinned
a number that went quietly wrong once PS1-visible code above it changed:
every `ASSERT` and `DBGMSG` below it then reported the wrong line.

The tree has none, and `psx_identity.py lines` reports any `#line` the PS1
compiles.  The one case left is a branch whose base predates `PORT_LINE`,
which gets the strict DEBUG compare; rebase it instead.  If an arm is ever
unavoidable, put the `#line` inside the `#else` arm, never after the
`#endif`, so PC `__LINE__` stays true in assert messages, crash reports and
PDBs, and list it here with its reason.

To check: `git grep -n "^#line" -- source tools/Data/include`.

### Comments

Changing a comment in PS1-visible code doesn't change the bytes.  Adding or
removing comment lines moves the DEBUG line numbers below them, which the
compare ignores.  `lines` passes a comment edit too, as "same tokens": it
compares what the compiler reads, and comments aren't part of that.

## Checking

### `psx_identity.py lines`

Takes seconds, needs no data and checks uncommitted work.  By default it
checks every file under `source/` and `tools/Data/include/` that the
working tree changes against the base: committed or not, untracked
included, and any extension, so the `.mip` assembly, `utils/gpu.inc` and
the upper-case `.H` headers are covered.  A new file is reported `NEW`,
because a new file the PS1 compiles can't be identical.  You can also name
the files.

For each one it preprocesses both revisions the way the PS1 compiler would,
but only as far as the gates go:
- It evaluates conditions over `PSX_MIPS_ASM`, `mips`, `__mips__`,
  `PSX_NO_ASM` and `SBSP_PC64`, and leaves those directive lines out: what
  they select is what gets compared.
- It keeps every arm of any other condition (including a `?:` expression or
  a `\` continuation), and keeps those `#if`/`#elif`/`#else`/`#endif` lines
  as text.  Flipping such a condition is therefore a visible change.
- An arm after one that already won is dead, as it is for the compiler.

It then requires the two to be the same tokens in the same order: what the
compiler reads, so line numbers (as in the DEBUG compare), comments,
whitespace and line breaks don't count, and a gated PC-only block passes.
Before comparing it expands the two same-line patterns above, the way the
PS1 preprocessor would:
- **`PC_PS1(pc, ps1)`** becomes its `ps1` argument (`system/asmport.h`).
- **A macro the file defines and the base did not** (one shared copy of
  code that used to be written out) is expanded at each use between its
  `#define` and its `#undef`, and those two lines drop out: a new macro
  makes code only where it is used.  A use outside them stays the bare name,
  as it does for the compiler.  It must be defined once, outside any
  condition the tool can't evaluate, and use neither `#` nor `##`;
  otherwise the file falls back to the line compare.
- **A macro the file defines differently from the base**, with the same
  conditions, is expanded the same way on both sides, each with its own
  definition, but only if the file `#undef`s it in both revisions.  #89
  gave `PROMPT_ICONS` a `Button` parameter: every use expands to the same
  statements, so `lines` passes it.

Either kind is expanded only when every use of it is code `lines` reads:
- no other `#define` uses it in its body, and no `#if`, `#elif` or
  `#include` line names it;
- no `#include` sits between its `#define` and its `#undef` (an X-macro's
  uses are in the included file);
- every `#undef` of it is outside any condition the tool can't evaluate,
  and comes after the `#define`.

Otherwise the `#define` line stays in the comparison as written, so a
changed macro is a `DIFF`.  A redefined macro that the file doesn't
`#undef` reaches files this check doesn't read, so it isn't expanded
either.  `port/tests/psx_identity_test.py` (ctest `psx_identity_rules`,
label `unit`) runs these rules on fixed cases, the PR #91 review's
counterexamples among them.

Every other macro is compared as written, not expanded.  It also reports,
as `LINE`, every `#line` directive the PS1 compiles in those files.  On
#81's own branch, where `dbg.h` and `memory.h` change `__LINE__` to
`PORT_LINE` on purpose:

```
ok    source/game/game.cpp
ok    source/map/map.cpp - same tokens (comments, layout or the macro patterns differ)
DIFF  source/mem/memory.h - the PS1 sees different tokens:
        base: DebugMemFontInit ( ) ; # define MemAlloc ( Size , Name ) MemAllocate ( ( Size ) , ( Name ) , __FILE__ , __L...
        now : DebugMemFontInit ( ) ; # define MemAlloc ( Size , Name ) MemAllocate ( ( Size ) , ( Name ) , __FILE__ , POR...
ok    source/player/player.cpp - same tokens (comments, layout or the macro patterns differ)
a DIFF is not proof of a different executable: psx_identity.py build decides
```

`map.cpp` passes through `PC_PS1`, and `player.cpp` through its new
`PROMPT_ICONS` macro, expanded back into the statements the PS1 compiled
before.  A directive is shown as one token, so `memory.h`'s difference is
at the end of that `#define`.

Before #81 it compared the line numbers too, which is how it caught a
stale `#line`.  With the arms gone and the DEBUG compare ignoring line
numbers, that half had nothing left to check.  What stays is the part that
still pays: a check in seconds, before `build`'s four clean builds, that a
PC change left the PS1 tokens alone, and the `LINE` report that keeps new
arms out.

The token check is still stricter than the bytes: a deliberate change that
compiles the same (redundant parentheses, a renamed local) is a `DIFF`.  It
is blind to anything outside the file: data, and macros defined elsewhere.
Treat a pass as "very probably identical" and a `DIFF` as "fix it, or know
why it is harmless".  `build` is the arbiter.

### `psx_identity.py build`

The proof.  For each of `--version DEBUG` and `--version FINAL` (both by
default) it:
1. Does a clean PS1 build of `HEAD`.
2. Checks out the base revision's copy of every changed PS1 build input and
   does a clean build of that. The inputs are the game source,
   `makefile.gaz`, the `build/*.mak` it includes (compiler flags, link), the
   per-user makefile under `users/`, the toolchain under `tools/`, and
   `port/build-psx.sh`.  So a change to compiler flags is compared too.
3. Puts `HEAD`'s copies back with `git checkout HEAD`.
4. Compares the two `Spongey.cpe` SHA-256 hashes, in the mode it printed at
   the start (above).

```
base 0785f0afe, 11 changed file(s), trans.h ca662f971dc60bd0
DEBUG: line-insensitive - both sides built with SBSP_IDENTITY_NOLINE, PORT_LINE 0
FINAL: strict - it bakes in no line numbers
IDENTICAL USA DEBUG (line-insensitive)
    head   752527 B  crc32 1084A321  sha256 e7d4d5f3e9e539f1...
    base   752527 B  crc32 1084A321  sha256 e7d4d5f3e9e539f1...
IDENTICAL USA FINAL (strict)
    head   665207 B  crc32 367F237E  sha256 d408200c15066e94...
    base   665207 B  crc32 367F237E  sha256 d408200c15066e94...
trans.h sha256 ca662f971dc60bd0eedf16ebc85277920be2961cc6d9428ce385e8fab9c5db63
```

(#81 against its own first commit; the tool prints the whole SHA-256.)

The size, CRC32 and SHA-256 lines are the columns of `psx_build_log.csv`.

Before each build it deletes the old `Spongey.cpe`, and it accepts only one
written by that build.  A build that fails without saying so (a login shell
can swallow make's exit status) therefore stops the run. It can't hand back
the other side's executable.

It needs:
- **MSYS2** at `C:\msys64`.  The tool runs `port/build-psx.sh` under MSYS2
  bash itself.
- **The territory's data:** `port/build-data.cmd usa` (or `eur` with
  `--territory EUR`).
- **The changes committed, and no untracked files among the build inputs.**
  Step 2 overwrites the working copies, and an untracked file would be built
  into both sides.  Don't edit build inputs while it runs either: the `HEAD`
  side compiles the working tree.
- **The same set of files as the base.**  Swapping in base copies can't
  undo an added or removed file, so the tool won't compare that change.
- **A short checkout path** (see the traps below).
- **No other PS1 build running anywhere on the machine.**

Anything that stops it from comparing exits 2 with the reason on stderr:
a build failure, a missing prerequisite, a build that did or didn't take
`SBSP_IDENTITY_NOLINE` against the mode, or `trans.h` changing during the
run.  1 always means the two executables really differ.

Copies of each cpe and each build log go to `out/psx_identity/`, named by
side and version (`head-DEBUG-noline.cpe`, `base-FINAL.cpe`).  On this
machine a clean build (289 files) takes one to two minutes, so the default
run, four builds, takes four to eight.

## Build traps

- **Run the PS1 build under MSYS2 bash, not Git Bash** (issue #51).  Git
  Bash's `make` is a native Windows make that can't parse the POSIX `PATH`
  and dies on `glecho`.  `build-psx.sh` now refuses to start outside MSYS2's
  bash and says what to run: `port\build-psx.cmd` (also from Git Bash), or
  `MSYSTEM=MSYS C:/msys64/usr/bin/bash -lc '... port/build-psx.sh USA DEBUG'`.
- **The territory and version must be upper case** (`USA DEBUG`).  Lower
  case defines `__TERRITORY_usa__` and writes broken objects into the same
  `out/` tree.  A later run then considers those objects up to date.
  `build-psx.sh` upper-cases both and accepts only `USA`/`EUR` and
  `DEBUG`/`FINAL`; a bare `make -f makefile.gaz` does neither.
- **Never run two PS1 builds at once.**  `ccpsx` writes its preprocessed
  intermediate to the fixed path `C:/msys64/tmp/PQ3`, shared by every build
  on the machine.  The loser fails on a random file.  The winner may have
  silently compiled the *other* tree's source.  After a collision, treat
  both trees as dirty.  `build-psx.sh` holds `flock /tmp/sbsp-psx.lock`
  from its argument checks to its exit, so a second build through the
  script says it is waiting and waits for the first; a bare make, or a
  build-psx.sh from before #51, takes no lock.
- **Use a short checkout path.**  `asmpsx` records the source path in a
  fixed-size buffer.  Under a deep path such as `.claude/worktrees/<name>/`
  it overflows, and `slink` rejects `system/except_a.o` with "unknown record
  type".  A sibling worktree such as `../SBSPSS-i50` links fine.
- **"Clean" means deleting `out/<T>/<V>/CD/objs` and `out/<T>/<V>/deps`.**
  Only compare clean builds of every file.  An incremental build can keep
  stale objects.
