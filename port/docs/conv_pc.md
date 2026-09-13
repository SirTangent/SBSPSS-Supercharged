# PC (Win32) conversion log — mechanical game-source changes

Companion to `Docs/conv_mips.md` (local notes dir, untracked; it covers the GNU/MIPS inline-asm dual-pathing).
This file logs every change made to game code (`source/`, `tools/`) for the PC
build, per the "mechanical fixes only, logged" policy.  Every entry compiles
identically on the PS1 toolchain — `port/build-psx.cmd` is re-run after each
batch as the regression guard.

The PC build itself lives in `port/` (CMake + MSYS2 MinGW-w64 i686 GCC; see
`port/CMakeLists.txt`).  Shim/runtime code and the shadow SDK headers are
new files under `port/` and are not listed here.

## Include strategy (context for the entries below)

The PC compile keeps Sony's own SDK headers authoritative for type layouts:
`tools/psyq/include` sits at the *end* of the search path (`-idirafter`), so
MinGW's real `stddef.h`/`stdlib.h`/`stdio.h` win, and `port/include` shadows
only the headers that cannot work on x86:

| shadow | reason |
|---|---|
| `inline_c.h` | MIPS `lwc2/swc2/mtc2/cop2` macro bodies → portable `GTEport_*` translation (all 245 macros, register-for-register; `gtemac.h` is pure composition over these and passes through unchanged) |
| `libsn.h` | `pollhost()`/`PSYQpause()` are MIPS `break` instructions → no-op / `__builtin_trap()` |
| `libapi.h` | declares `rename()` and `_get_errno()` at signatures that clash with the MinGW CRT (game calls neither) → renamed out of the way, rest passes through |
| `strings.h`, `memory.h` | vintage headers declare `memcpy`/`strcpy`/`strlen` with empty parameter lists ("to avoid conflicting"), which C++ reads as zero-arg → forward to `<string.h>`; `strings.h` also restores the vintage guarantee that `sys/types.h` precedes the SDK block, and adds `abs(unsigned)` overloads reproducing the vintage `abs(int)` implicit conversion |
| `sys/types.h` | provides the BSD `u_char/u_short/u_int/u_long/ushort` typedefs the SDK headers use, without the vintage `time_t`/`off_t`/`size_t` clashes |

## Game-source changes (M1)

1. **`source/system/asmport.h`, `source/system/gte.h`, `source/utils/cmxmacro.h`,
   `source/level/layertile3d.h`** — portable GTE shim widths `unsigned long`→`u32`,
   magnitude stores through the caller's pointer type, `GTEport_Op` declared
   (issues #12/#13; commit `M1: fix portable GTE shim widths to u32`).

2. **`source/system/global.h`** — `SCRATCH_RAM` gated on `PSX_MIPS_ASM`:
   the PS1 keeps the literal `0x1f800000`; the PC path points at the shim's
   real 1KB buffer (`PORT_Scratchpad`).  Needed because `layertile3d.cpp`
   bakes `(DVECTOR*)SCRATCH_RAM + n` into namespace-scope initialisers, so it
   must stay an address-constant expression.

3. **`source/gfx/animtex.h:18`** — `AddAnimTex(sFrameHdr *Frame,int Frame,...)`
   declared two parameters both named `Frame` (EGCS tolerated it); the second
   is now `FrameNo`, matching the definition in `animtex.cpp:37`.

4. **`source/player/player.h:355`** — `const struct AnimFrameSfx *` named a
   typedef of an anonymous struct after `struct` (ill-formed); dropped the
   `struct` keyword.

5. **`source/player/player.h` (prompt block)** — `sPromptData`/`sPromptTable`
   moved from `private` to `public`: `player.cpp` defines the prompt tables at
   namespace scope (`player.cpp:3281+`), which EGCS let reach private nested
   types.  No layout or behaviour change.

6. **`source/player/player.cpp:1686`** — `getHeightFromGroundNoPlatform`
   repeated the `_maxHeight=32` default argument on the definition (already on
   the declaration, `player.h:250`); removed from the definition.

7. **`source/pickups/pickup.h:81`** — `const struct DVECTOR *` → `const
   DVECTOR *` (same typedef-after-`struct` issue as #4).

8. **`source/game/gamebubs.h:53`** — `static struct BubicleEmitterData` →
   `static BubicleEmitterData` (same).

9. **`tools/vlc/include/VLC_BIT.H:13`** — `void DecDCTvlcBuild3();` given its
   real prototype `(unsigned short *table)`: `fmv.cpp:199` passes the table,
   and in C++ the empty parens mean zero-arg.  Matches the MIPS `VLC_BIT.O`
   ABI (table pointer in `$a0`).

10. **No-op `typedef` keywords dropped** (13 sites): `typedef struct/enum NAME
    {...};` with no declarator name — the keyword was silently ignored by EGCS
    and warned per-including-TU by modern GCC (~2000 warnings).
    `source/sound/xmplay.h:115,123`, `source/sound/sound.h:44,57,62,189,201,207`,
    `source/sound/spu.h:32`, `source/gfx/bubicles.h:47`,
    `source/sound/sound.cpp:54,61,65`.

11. **Default arguments repeated on definitions removed** (4 more sites, same
    class as #6; the declarations keep them): `sound.cpp:638` (`playSfx`),
    `layercollision.cpp:134` (`getHeightFromGroundExcluding`),
    `saveload.cpp:152` (`startSave`), `player.cpp:1732` (`addSpatula`).

12. **`source/system/asmport.h` (post-review)** — the software-GTE contract
    corrected and completed before freezing: `GTEport_Op` documented as
    receiving the DMPSX tag words the vintage `INLINE_C.H` macros embed (not
    the raw 25-bit cop2 immediate the first draft claimed); `GTEport_GetCtrl`
    (cfc2) moved here from the shadow `inline_c.h` so the interface has one
    home; `PORT_Scratchpad` declared here (portable branch only) so game code
    and shim share one declaration instead of three raw externs.

13. **Warning-free pass — two latent bugs fixed** (the PC build is now 0
    warnings in both variants; these are behavioural fixes, not cosmetics):
    - `source/hazard/hrckshrd.cpp:127` and `source/platform/pfishhk.cpp:172`
      — `getThinkBBox()` returned `&objThinkBox`, the address of a stack
      local.  Dangling on PS1 too; it worked only because every caller reads
      through the pointer before the stack is reused.  Now `static`, matching
      the base class (`thing.h:191` returns the member `m_collisionArea`).
    - `source/game/gameslot.h:184` — `getHighestLevelOpen()` wrote its
      out-params only inside `if(isLevelOpen(...))`, so with no level open
      the callers (`start.cpp:302`, `map.h:51`) read uninitialised stack
      ints.  Defaults to chapter 0 / level 0 now.
    - `source/gfx/animtex.cpp:61` — the `default:` (unknown pixel depth) case
      only `ASSERT`ed, and `ASSERT` compiles away in FINAL, leaving
      `PixPerWord` uninitialised as the divisor two lines later.  Sets a safe
      `PixPerWord=1` first.  *(Surfaced only by the FINAL variant's heavier
      inlining — a good argument for building both.)*

14. **Warning-free pass — non-behavioural** (same batch as #13):
    - `source/gui/gui.h:65` — `virtual ~CGUIObject() {}` added: `shutdown()`
      does `delete this` on a polymorphic hierarchy.  No class in the
      hierarchy declares a destructor, so this only fixes dispatch.
    - `source/fileio/fileio.cpp:384,399` — `loadDataBank`/`dumpDataBank`
      index `DataBank[DATABANK_MAX]` where `DATABANK_MAX==0` (a zero-length
      array).  Neither function has any caller; a `if (DATABANK_MAX==0)
      return;` guard makes that explicit and folds the dead indexing away.
    - `source/backend/credits.cpp:85` — `enum {...} CREDIT_CONTROL;`
      accidentally declared a *global variable* of unnamed enum type (used
      nowhere; only the `CC_*` constants are).  Now `enum CREDIT_CONTROL
      {...};`.
    - `source/system/clickcount.cpp:64` — the `OpenEvent` handler cast is
      dual-pathed on `PSX_MIPS_ASM`: EGCS insists on `(long (*)(...))`,
      which modern GCC rejects, and vice versa.

## Game-source changes (M3)

15. **`source/system/main.cpp:89` (USE_SCREEN_UTILS gate)** � the DEBUG
    screen utils (SELECT=VRamViewer, L2+START=SaveScreen) were gated on
    `__FILE_SYSTEM__==PC && !__USER_CDBUILD__`, i.e. compiled out of every
    CD build.  Both outer conditions gained a `|| !defined(PSX_MIPS_ASM)`
    arm so the Win32 port's DEBUG variant keeps them (the shim implements
    the libsn `PC*` file calls SaveScreen needs - `port/psyq/sn/
    pcfile.cpp`).  On the PlayStation build `PSX_MIPS_ASM` is defined, so
    both conditions reduce to the originals - verified by the PSX
    regression build.

## Game-source changes (M4)

16. **`source/system/main.cpp` (boot-scene select)** - the final `#else`
    branch (`setNextScene(&FrontEndScene)`) gained a `!defined(PSX_MIPS_ASM)`
    arm that asks the shim's `Port_BootLevel()` (port/psyq/host/args.cpp,
    `--level` / `SBSP_BOOT_LEVEL`) for a LvlTable index: >=0 sets
    `s_globalLevelSelectThing` and boots straight into `GameScene` for
    testing, -1 keeps the original frontend boot.  Same shape as the
    vintage `__USER_daveo__` dev path a few lines above.  On the
    PlayStation build the arm reduces to the original line - verified by
    the PSX regression build.

17. **`source/platform/platform.cpp` (`CNpcPlatform::setCollisionAngle`)** -
    latent null dereference, crashed on the first C1L1 boot: platforms
    `postInit` during `CLevel::init`, which runs BEFORE `createPlayer()`
    (game.cpp), so `GameScene.getPlayer()` is NULL and
    `player->isOnPlatform()` reads NULL+offset.  On PS1 address 0 is
    readable kernel RAM and the garbage never compares equal to a platform
    pointer, so the bug was invisible; Win32 faults.  Guarded with
    `player&&` - behaviour-identical to what the hardware actually did.
    Same latent-bug class as entry #13.

18. **`source/level/layertile3d.cpp` (tile-window margins)** - user-visible
    on PC: the 3D action layer vanished from the bottom (and, at worst
    scroll phase, ~9px of the right edge) of the view.  The USA margins
    (`SCREEN_TILE_ADJ_D=1`, `_R=3`) are tuned to CRT overscan: the far
    plane (z=+64) projects at 378/442 = 0.855, so the bottom 8-21 lines of
    the 256-line framebuffer were never covered - and never visible on an
    NTSC TV.  A PC window shows the whole framebuffer.  Fix is the one the
    original devs made for PAL's taller visible area (EUR `D=3`, with its
    own comment saying exactly this): a `!defined(PSX_MIPS_ASM)` arm with
    `D=3, R=4`.  Both console territory arms are untouched - verified by
    the PSX regression build.

19. **Blank-frame null dereferences in the render paths** (`gfx/actor.cpp`,
    `player/player.cpp`, `enemy/ndogfish.cpp`, `enemy/ndustdev.cpp`,
    `enemy/nfdutch.cpp`, `enemy/nghost.cpp`, `enemy/nmjfish.cpp`) - the
    largest instance of the entry #13/#17 class, and a genuine crash:
    three levels segfaulted within seconds of the M4 scripted sweep.

    `CActorGfx::Render` returns NULL for a *blank frame* - authored data,
    not an error: `CacheFrame` (actor.cpp:539) does `if
    (!CurrentFrameGfx->PAKSpr) return(0)`.  Callers then write straight
    through the result: `setSemiTrans`/`setShadeTex`/`setRGB0` are stock
    PSY-Q macros that dereference unconditionally, as does
    `CActorGfx::RotateScale`.  On PS1 that wrote a byte at address 0 -
    writable kernel RAM the game never relied on - so the bug was
    invisible for the console's whole life; Win32 faults.

    The crash found was SpongeBob's own anim 14 frame 22 in
    `CPlayer::renderSb` (all four `Render` sites there: the mode addon,
    the jellyfish-in-net addon, the glove addon, and SpongeBob himself).
    Being player code it was reachable in *any* level; C2L4/C3L3/C3L4 just
    happened to play that animation within the 600-vblank sample.

    An audit of every `CActorGfx::Render` call site found 18 more
    unguarded dereferences, 14 of which reach the prim only through
    `RotateScale`.  Those are fixed at the callee: `RotateScale` gets an
    `if (!Ft4) return(0);` alongside its existing no-op early-out (which
    already returns `Ft4` without touching it or the BBox, so callers
    tolerate the passthrough).  The remaining sites - the five enemy
    files above - guard their direct macro writes with `if (SprFrame)`.
    Sites that merely discard the return value were left alone.

20. **`source/gfx/prim.h` (`MAX_PRIMS`)** - the PC build doubles the prim
    pool to 4096 entries (163,840 bytes per buffer); the console arm keeps
    the original 2048 and is verified unchanged by the PSX regression.

    Two reasons.  The PC renders a wider 3D tile window than any console
    territory (entry #18), which raised the per-frame peak; and the budget
    cannot be *proved* by measurement the way a fixed console target's
    could - a played session runs ~1.4x the peak that scripted input
    reaches (measured: C1L1 39,180 and 39,276 bytes played vs 28,516
    scripted), so bounding it honestly would mean playing all 25 levels to
    completion.  A partial human run of the densest level reached 72.7% of
    the ORIGINAL budget without finishing.

    The failure mode is what settles it: `CLayerTile3d::render()` writes
    through a raw `PrimPtr` with no bound check, and `PrimDisplay`'s own
    `ASSERT(!"PRIM OVERFLOW")` is both post-hoc and compiled out of FINAL,
    so an overrun corrupts whatever `MemAlloc` placed after the pool,
    silently, in the shipping variant.  80KB of insurance against an 8MB
    arena is not a trade worth agonising over.  The shim's high-water
    watchdog (port/psyq/gpu/gp0.cpp) remains the detector.

## Game-source changes (M5)

None.  The whole audio milestone - the software SPU (`port/psyq/spu/`),
the XMPlayer reimplementation (`port/psyq/xmplay/`), and the SDL3 output
path (`port/psyq/host/audio_out.cpp`) - lives entirely shim-side behind
the vintage `LIBSPU.H`/`XMPLAY.H` prototypes.  `git diff SBSP-Win11 --
source/ tools/` is empty for the milestone branch, so no PSX regression
build was required.

One correction of record rather than of code: issue #7's exit criterion
mentions a mono/stereo option, but the game has none - `setStereo(true)`
runs once at init (`source/sound/xmplay.cpp:92`) and `XM_SetMono` is
unreachable from any UI.  The shim implements both as real state anyway.

## Game-source changes (M6)

The milestone's machinery is all shim-side - XA speech
(`port/psyq/cd/xa_stream.cpp` + `xa_adpcm.cpp`), the memory card
(`port/psyq/mcrd/`), and rumble (`port/psyq/host/input.cpp`) live behind
the vintage `LIBCD.H`/`LIBMCRD.H`/`LIBPAD.H` prototypes; `sound/cdxa.cpp`,
`memcard/memcard.cpp` and `pad/vibe.cpp` run unmodified.  One deliberate
behavioural divergence was added on user request:

21. **`source/system/main.cpp` (boot-time card autoload)** - a new
    `DoAutoLoadPC()` gated `#if !defined(PSX_MIPS_ASM)` (the entry #16
    pattern: the PlayStation build compiles it away and keeps retail
    behaviour), called from the spot where the original autoload sits
    commented out.  Context: the retail game NEVER autoloads -
    `DoAutoLoad` exists but its call site is commented out upstream
    ("Autoload? Who wants that in this day and age!?"), so after every
    launch the slot-select screen shows EMPTY until a manual Options ->
    Load Game.  On PC that reads as "my save is gone".  The retail
    autoload path would not fix it even if re-enabled: `startAutoload`'s
    completion calls `restoreData(settings-only)` and waits a fixed 2s
    for a physical card.  `DoAutoLoadPC` instead polls the card to
    `CS_ValidCard` (the shim card settles in a handful of frames; a
    120-frame cap covers an unusable save location) and drives the
    ordinary `startLoad(0)` path, whose completion restores settings AND
    game slots after the MD5 check.  A missing/empty/unformatted card
    falls through silently - the in-game screens keep owning every error
    path, and Load Game/save UI are otherwise unchanged.  The card
    location follows the usual resolution (`--save-dir`/`SBSP_SAVE_DIR`,
    else `%APPDATA%\SBSPSS`).  Boot cost: ~10 emulated vblanks with the
    shim card, so pad scripts timed across boot shift by that much.
    PSX regression build re-run clean after the change.

22. **`source/memcard/saveload.cpp` (out-of-date save)** - a save whose
    MD5 verified but whose `m_headerId` did not match `SAVELOAD_HEADERID`
    used to `ASSERT(!"YOUR MEMCARD SAVE IS OUT OF DATE!")` and then call
    `restoreData()` on it regardless: a hard trap in DEBUG, and in FINAL
    the volume / control-style / vibration / game-slot setters all fed
    from a struct the code had just identified as a different layout.
    The load now fails instead (the save/load UI already has a
    load-error path).  Same latent-bug class as entry #13, and load-
    bearing for entry #21: the boot autoload would otherwise walk into
    it on every single launch, before any UI exists to decline.  Found
    by the M6 code-review pass.

## Game-source changes (M8)

The M8 playthrough harness (`port/psyq/host/diag.cpp`, `autoplay.cpp`,
the pad-file player in `input.cpp`, `port/tests/run_tier.py`) is
shim-side; the entries below are its game-side hooks.  Each is a
`#if !defined(PSX_MIPS_ASM)` arm (the entry #16 pattern), so the
PlayStation build is byte-identical.  Prototypes live in the existing
PC-only block of `source/system/asmport.h`.

**Byte-identical means hash-identical here, not size-identical.**  The
DEBUG `ASSERT`/`DBGMSG` macros bake `__LINE__` into the code, so a PC-only
block that adds lines *above* one of them shifts those immediates (M8
found 14 such bytes, all `+3`, after the first two hooks).  Where that
happens the block carries an `#else` / `#line <n>` arm that restores the
original numbering for the PlayStation preprocessor only: `<n>` is the
pristine line number of the block's `#endif` (i.e. one less than the line
after it).  The guard is `port/build-psx.cmd` + a SHA-256 compare of
`Spongey.cpe` against a build of the pristine sources.

23. **`source/system/gstate.cpp` (scene epochs)** - `GameState::think()`
    calls `Port_SceneEvent(getSceneName())` right before a new scene's
    `init()`, *outside* the `__VERSION_DEBUG__` block, so FINAL emits
    `[scene] <name> vblank=<n>` lines too.  The Tier-1 oracle matches a
    run's exact `[scene]` sequence.

24. **`source/fma/fma.cpp` (FMA script identity)** -
    `CFmaScene::getSceneName()` returns `"FMA"` for every script, so
    `CFmaScene::init()` additionally calls `Port_FmaEvent(s_chosenScript)`
    at the point the chosen script is bound, giving a second
    `[scene] FMA:CH3FINISHED`-style line.  `getSceneName()` is unchanged.

25. **`source/system/dbg.cpp` (`DoAssert` -> `Port_Assert`)** - the DEBUG
    `DoAssert` (on-screen dump, then `PSYQpause()` = `__builtin_trap()` via
    the `libsn.h` shadow) now calls `Port_Assert(expr,file,line)` and
    returns: `[assert] <expr> at <file>:<line> (<scene>, vblank <n>)`, then
    exit code 10 - or keep running under `SBSP_ASSERT_CONTINUE=1`.  The
    `ASSERT` macro in `dbg.h` is untouched (still `;` in FINAL).  `dbg.cpp`
    did not include `asmport.h` (directly or via `global.h`), so it gains
    that include - otherwise its `PSX_MIPS_ASM` test would be false on the
    PlayStation build too.

26. **`source/system/main.cpp` (`Port_RegisterGameGlobals`, `--seed`)** -
    `main()` starts by handing the shim the addresses of `MainRam.RamUsed`,
    `MemNodeCount`, `invincibleSponge` and the four prim-pool pointers
    (`Port_RegisterGameGlobals`, `port/psyq/host/diag.cpp`).  The prim-pool
    watch in `port/psyq/gpu/gp0.cpp` reads through that registry instead of
    `__attribute__((weak))` externs, and the `[mem]` watch, `[summary]` and
    `--invincible` use it too; the shim-only unit exes never register, so
    every pointer stays NULL there.  Two new one-line externs
    (`MemNodeCount`, `invincibleSponge`) sit in the existing PC-only block.
    `InitSystem()`'s `setRndSeed(VidGetTickCount())` gains a
    `!PSX_MIPS_ASM` arm that takes `Port_BootSeed()` (`--seed` /
    `SBSP_SEED`) when one was given - the `Port_BootLevel` pattern
    (entry #16).

27. **`source/system/asmport.h` (scratchpad guard bytes)** - the PC-only
    `PORT_Scratchpad` declaration grows by `PORT_SCRATCHPAD_GUARD` (16)
    bytes, matching the definition in `port/psyq/api/arena.cpp`, which
    seeds them with `0xA5`.  `SCRATCH_RAM` users see the same 1KB; the
    shim's `Port_MemWatch` (`host/diag.cpp`, every vblank) reports
    `[mem] LEAK scratchpad overrun` once if a guard byte ever changes.
    There is no scratchpad allocator to measure usage against, so this
    is an overrun canary, not a percentage.  Same watch: `RamUsed`
    high-water (`SBSP_MEM_LOG=1`) and a one-shot `[mem] WARNING` at
    224/256 `MemNodeCount`.  Header-only change inside the block the
    PlayStation build skips.

28. **`source/game/game.cpp` (`SBSP_AUTOPLAY` hooks)** - three arms, each
    with a `#line` re-sync (two `SYSTEM_DBGMSG` sites follow them):
    - `initLevel()`, after the retail bonus-level timer block: `finish=N`
      arms that same `m_levelHasTimer`/`m_timer` path for *every* level,
      so the unmodified countdown in `think_playing()` (beeps, then
      `s_levelFinished`) ends the level N vblanks after play starts - no
      new finish logic.  `lives=N` / `continues=N` write
      `CGameSlotManager::getSlotData()->m_lives/m_continues` directly
      (public `typedef struct`, the idiom `game.cpp:319` already uses) -
      once, at the first `initLevel()`: game over -> continue -> Map ->
      level runs `initLevel()` again, and re-writing there would make
      continues inexhaustible, so the shim's accessor returns -1 after
      its first answer.  (A plain death goes through `respawnLevel()`,
      not `initLevel()`.)
    - the level-finished block, after the hi-spatula-count check:
      `spatulas=all` records every spatula in the save slot
      (`setSpatulaCollectedCount(total,total)`) - slot bookkeeping only,
      the player's carried count is untouched.
    - `think_playing()`, after the timer block: `die=N` calls the public
      `CPlayer::dieYouPorousFreak()` when the shim's
      `Port_AutoplayDie(m_player->isDead())` says so - one death per
      observed death->respawn cycle, so N is exactly N life-losses and
      game-over is reached through the retail `pmdead.cpp` path.  The
      same arm holds the `finish=N` countdown at N while the player is
      dead and clears any finish it fired during the death sequence: a
      level is never recorded as completed by a dead player, and the
      respawned attempt gets the full N again.
    Parser and accessors: `port/psyq/host/autoplay.cpp`; prototypes in
    `asmport.h`.

29. **`source/gfx/animtex.cpp` (`CAnimTex::GetSpeed`/`SetSpeed` on an empty
    list)** - first bug found by the Tier 1 campaign route.  `CLevel::init`
    (`level.cpp:263-267`) calls `CAnimTex::SetSpeed(-4)` when the game
    scene reports chapter 5 level 4 or 5, and it still does during the
    CH5FINISHED FMA (the FMA scene loads level 25 but `GameScene`'s level
    number is the level just finished).  That level has no animated
    textures, so `AnimTexList` is NULL and `ThisTex->Speed=Speed` writes
    to address 0xC.  On the PlayStation that is kernel RAM and the write is
    silently absorbed; on PC it is an access violation
    (`[crash] ... scene=FMA:CH5FINISHED`, deterministic, 64 vblanks after
    the scene opens).  Both accessors now return early on a NULL list in a
    `!PSX_MIPS_ASM` arm; `hcswitch.cpp:116` (`SetSpeed(-GetSpeed())`) is
    the other caller and gets the same protection.  `#line` re-synced
    (ASSERTs follow in the same file).

## Not changed (accepted by `-fpermissive -std=gnu++98`)

- String-literal → `char*` conversions (pervasive; `-Wno-write-strings`).
- Zero-length array `DataBank[DATABANK_MAX==0]` (`fileio.h:68`) — a GNU
  extension modern GCC still accepts in gnu++ mode.
- `operator new(size_t, const char* = NULL)` overload set (`mem/memory.h`) —
  feared ambiguous against the implicit `::operator new`, but GCC 16 resolves
  the game's `new ("name") T` and plain `new T` correctly under gnu++98.
- Backslash `#include` paths and mixed-case generated-header names (NTFS).

## Warning policy (PC game target)

Style classes pervasive in the 1999 code are suppressed on the game target
only (see `SBSP_GAME_CXX_FLAGS` in `port/CMakeLists.txt`): narrowing,
overloaded-virtual, non-c-typedef-for-linkage, parentheses, char-subscripts,
sign-compare, misleading-indentation, int-in-bool-context, dangling-else,
header-guard, unused, write-strings.  The shim keeps plain `-Wall`.

Every warning those flags do *not* suppress has since been fixed at its
source (entries #13/#14 above), so **both variants build with 0 warnings and
0 errors**.  Keep it that way: a new warning now means new code, not
inherited noise.
