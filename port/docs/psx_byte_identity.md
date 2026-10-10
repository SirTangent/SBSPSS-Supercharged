# Keeping the PlayStation build byte-identical

The game source (`source/`, `tools/Data/include/`) is compiled twice: by the
PC port, and by the original 1999 PlayStation toolchain (`ccpsx` / EGCS,
`port/build-psx.sh`).  A change made for the PC must not change one byte of
the PlayStation executable, `Spongey.cpe`.  **Byte-identical means
hash-identical, not just the same size.**

A change that is *meant* to change the PS1 game too says so in its
`conv_pc.md` entry, like #24's `(u8)` casts in `font.cpp`.  Everything else is
held to the hash.

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
   `switch` into a helper function, or a constant into a variable, changes
   the code EGCS generates.
2. **Line numbers, in DEBUG.**  Three macros bake `__LINE__` into the code
   as an immediate:
   - `ASSERT` (`system/dbg.h`)
   - `DBGMSG` / `__DBGMSG` (`system/dbg.h`)
   - `MemAlloc` (`mem/memory.h`, DEBUG only)

   Adding or removing a line *above* one of them in the same file moves its
   number.  M8 found 14 such bytes, all `+3`, after its first two hooks.
   `__LINE__` is the line in the file where the macro is *used*, so new
   lines in a header move only the macros used in that header.
   `mem/memory.h` holds some helpers because `memory.cpp`'s line numbering is
   load-bearing.
3. **The data build.**  `tools/perl/pl/lang.pl` writes the `STR__*` enum in
   `out/<T>/include/trans.h` in Perl's randomised hash order, so every data
   build shuffles the string table.  About 30 objects and 250 KB of the cpe
   then differ for no code reason.  Compare two builds only if they share
   one `out/<T>/include`, and never across a data build.
   `psx_identity.py build` hashes `trans.h` before and after its run and
   voids the comparison if it moved.
4. **Adding or removing a file the PS1 builds.**  A new translation unit
   changes the link, so it cannot be identical.  #43's `pad/padicon.cpp` was
   one, and was accepted as a deliberate change.

## Where the gate comes from

`PSX_MIPS_ASM` is not a compiler flag.  `system/asmport.h` defines it when the
compiler predefines `mips` or `__mips__` (the vintage compiler predefines
`mips`) and `PSX_NO_ASM` is not set.  Every game file reaches `asmport.h`
through `system/global.h`.  A gate placed *before* that include would quietly
read as "PC" on the PlayStation, so check the include order when you gate
somewhere new.

`source/locale/textdbase.cpp` gates on the compiler macros directly
(`#if defined(mips) || defined(__mips__)`), for a `#line` that only the
PlayStation should see in a file whose 32-bit and x64 PC builds use
different arms.

## Patterns

### Same-line edits through a macro

This is the best option when it fits: no line moves, so no `#line` is needed.
M9 did most of the x64 work this way.  For example, `RELOC_PTR(...)` expands
to the original tokens everywhere except x64 (`conv_pc.md` #31).

### PC-only insertion

```cpp
	m_tempBuffer=(unsigned char*)MemAlloc(m_bufferSize,"MEMCARD");     // base line 532
#if !defined(PSX_MIPS_ASM)
	// PC: ... (conv_pc.md #57)
	memset(m_tempBuffer,0,m_bufferSize);
#else
#line 532	// keep the PS1 build's __LINE__ (ASSERTs below) byte-identical
#endif
}                                                                       // base line 533
```

`#line N` gives the *next* line the number N, and here the next line is the
`#endif`.  So **N is the base number of the line just before the
insertion**: the `#endif` takes N, and the line after it gets N+1, which is
its original number.  (`memcard/saveload.cpp`, #57.)

### PC replacement of PS1 code

The PS1 arm keeps the original lines, verbatim, with a `#line` at each end:

```cpp
#if !defined(PSX_MIPS_ASM)
			int	Icon[2];
			promptIcons(Ptr->m_input,Icon);
#else
#line 3440
			int	Button=CPadConfig::getButton((CPadConfig::PAD_CFG)Ptr->m_input);
			...                                      // base lines 3441-3451, the ASSERT on 3450
			}
#line 3452
#endif
			for (int i=0; i<2; i++)                  // base line 3453
```

- **The opening `#line`** is the base number of the first line kept.  Without
  it, the `#if` / PC arm / `#else` lines would push everything in the arm
  down.  It matters whenever a `__LINE__` macro sits in the arm.
- **The closing `#line`** is the base number of the last line kept, because
  the `#endif` takes that number.

Write both even when nothing in the arm uses `__LINE__`.  The next person to
add an `ASSERT` there then doesn't have to think, and `lines` (below)
compares text *and* numbers.  (`player/player.cpp`, `map/map.cpp`, #59.)

### Declarations only the PC uses

Gate them in the header too, as `CPadIcon::getRowPitch` is in
`pad/padicon.h`.  This isn't for the bytes, since an unused inline member
emits nothing.  It's so the PS1 build fails to compile if PS1 code ever
starts calling the PC path.

### Rules of thumb

- The `#line` goes **inside the `#else` arm**, never after the `#endif`.  A
  PC compiler skips that arm, so PC `__LINE__` stays true in assert
  messages, crash reports and PDBs.
- "Base line number" means the number the PS1 compiler assigns in the base
  revision.  In a file that already carries `#line` arms, that is **not** the
  editor's line number.  Run `lines` rather than counting by hand.
- Comments in PS1-visible code: changing a comment on an existing line
  doesn't change the bytes, but adding or removing a comment line moves
  every `__LINE__` below it.  Put new commentary in the PC arm.

## Checking

### `psx_identity.py lines`

Takes seconds.  By default it checks every file under `source/` and
`tools/Data/include/` that the working tree changes against the base:
committed or not, untracked included, and any extension, so the `.mip`
assembly, `utils/gpu.inc` and the upper-case `.H` headers are covered.  A
new file is reported `NEW`, because a new file the PS1 compiles can't be
identical.  You can also name the files.

For each one it preprocesses both revisions the way the PS1 compiler would,
but only as far as gates and `#line` go:
- It evaluates conditions over `PSX_MIPS_ASM`, `mips`, `__mips__`,
  `PSX_NO_ASM` and `SBSP_PC64`, and leaves those directive lines out: what
  they select is what gets compared.
- It keeps every arm of any other condition (including a `?:` expression or
  a `\` continuation), and keeps those `#if`/`#elif`/`#else`/`#endif` lines
  as text.  Flipping such a condition is therefore a visible change.
- An arm after one that already won is dead, as it is for the compiler.
- It honours `#line`, but only in arms the PS1 compiles.

It then requires the two to be the same lines of text on the same numbers:

```
ok    source/map/map.cpp
DIFF  source/player/player.cpp - the PS1 sees different text or line numbers:
        base  3471: Y-=PromptYGap;
        now   3474: Y-=PromptYGap;
```

The check is stricter than the bytes.  It flags comment-only edits in
PS1-visible code.  It also flags a line shift in a file with no `__LINE__`
user below it, which the bytes don't notice.  For example, #67's PC-only
addition to `system/asmport.h` moved the lines after it and the hash stayed
the same.  It is also blind to anything outside the file: data,
and macros defined elsewhere.  Treat a pass as "very probably identical"
and a `DIFF` as "fix it, or know why it is harmless".  `build` is the
arbiter.

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
4. Compares the two `Spongey.cpe` SHA-256 hashes.

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
  into both sides.
- **The same set of files as the base.**  Swapping in base copies can't
  undo an added or removed file, so the tool won't compare that change.
- **A short checkout path** (see the traps below).
- **No other PS1 build running anywhere on the machine.**

Anything that stops it from comparing exits 2 with the reason on stderr:
a build failure, a missing prerequisite, or `trans.h` changing during the
run.  1 always means the two executables really differ.

Copies of each cpe and each build log go to `out/psx_identity/`.  On this
machine a clean build (289 files) takes one to two minutes, so the default
run takes five to ten.

## Build traps

- **Run the PS1 build under MSYS2 bash, not Git Bash** (issue #51).  Git
  Bash's `make` can't parse the POSIX `PATH` the script passes and dies on
  `glecho`.  Run `MSYSTEM=MSYS C:/msys64/usr/bin/bash -lc '... port/build-psx.sh USA DEBUG'`.
- **The territory and version must be upper case** (`USA DEBUG`).  Lower
  case defines `__TERRITORY_usa__` and writes broken objects into the same
  `out/` tree.  A later run then considers those objects up to date.
- **Never run two PS1 builds at once.**  `ccpsx` writes its preprocessed
  intermediate to the fixed path `C:/msys64/tmp/PQ3`, shared by every build
  on the machine.  The loser fails on a random file.  The winner may have
  silently compiled the *other* tree's source.  After a collision, treat
  both trees as dirty.
- **Use a short checkout path.**  `asmpsx` records the source path in a
  fixed-size buffer.  Under a deep path such as `.claude/worktrees/<name>/`
  it overflows, and `slink` rejects `system/except_a.o` with "unknown record
  type".  A sibling worktree such as `../SBSPSS-i50` links fine.
- **"Clean" means deleting `out/<T>/<V>/CD/objs` and `out/<T>/<V>/deps`.**
  Only compare clean builds of every file.  An incremental build can keep
  stale objects.

## Where the `#line` arms are

| file | conv_pc.md | protects |
|---|---|---|
| `source/system/gstate.cpp` | #23 | ASSERT / DBGMSG |
| `source/fma/fma.cpp` | #24 | ASSERTs |
| `source/game/game.cpp` | #28, #56 | ASSERTs, DBGMSGs |
| `source/gfx/animtex.cpp` | #29 | ASSERTs |
| `source/locale/textdbase.cpp` | #32 | ASSERTs (gated on `mips`) |
| `source/memcard/saveload.cpp` | #57 | ASSERTs |
| `source/pad/padicon.h` | #59 | line numbering only (no `__LINE__` user) |
| `source/map/map.cpp` | #59 | MemAlloc |
| `source/player/player.cpp` | #59 | ASSERT |

To list the arms in the tree: `git grep -n "^#line" -- source tools/Data/include`.
