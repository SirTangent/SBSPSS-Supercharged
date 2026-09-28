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
    (entry #16).  The retail seed is not a clock: `VidGetTickCount()`
    returns the vblank count the last `VidSwapDraw` stored, and the only two
    swaps before `InitSystem` (in `VidInit`) run before `VidVSyncCallback`
    is installed, so it is 0 on every boot, PlayStation included - an
    unseeded run is reproducible as it stands.  Since issue #58 the hook
    lives in `host/seed.cpp`: `--seed`/`SBSP_SEED`, else a replayed
    recording's `# seed`, else it answers 0 and the retail arm decides -
    once, so the `--record-pad` header (which may ask first, the boot pumps
    before line 210) and the game agree, and a recording made with a seed
    replays without one.  The game arm is unchanged.  The registration
    also hands over `&s_randomSeed` (`utils/utils.cpp`, the RNG state) -
    appended to the existing call's last line, so the PlayStation
    preprocessor's line numbering is untouched - for the `rng=` field of
    every `# epoch` ("Replaying a tester session").

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
      the player's carried count is untouched.  Levels 1-4 only (issue
      #58): a bonus level has no spatula slot, its index
      `(chapter-1)*4+4` is the next chapter's first level - and chapter
      5's runs off the 20-byte array into the kelp-token flags - and the
      `ASSERT(_level<=NUM_LEVELS_WITH_SPATULAS)` is off by one, so
      nothing else would have said.  Retail never gets there: kelp
      tokens go through `collectKelpToken`, not `addSpatula`.
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

30. **`source/system/main.cpp` (`--language` / `SBSP_LANGUAGE`)** -
    `InitSystem()`'s `TranslationDatabase::loadLanguage(ENGLISH)` gains a
    `!PSX_MIPS_ASM` arm that loads `Port_Language(ENGLISH)` instead
    (`port/psyq/host/args.cpp`: a name `english swedish dutch italian
    german` or the `locale/textdbase.h` enum index 0-4; the argument wins
    over the environment; a bad value warns and keeps ENGLISH).  Prototype
    in the PC-only block of `asmport.h`.  No `#line` arm: the only
    `__LINE__` user below it in this file is the `USE_SCREEN_UTILS` ASSERT,
    which the CD build compiles out (entry #15) - and the SHA-256 guard
    agrees.  Three truths worth knowing before reaching for the flag:
    - boot is the only `loadLanguage` the game ever reaches.  The save
      restore (`memcard/saveload.cpp`) re-applies the slot's `m_language`
      only under `if(!TranslationDatabase::isLoaded())`, which is never
      true after boot - so no "re-apply after `DoAutoLoadPC`" exists or is
      needed;
    - the shipped data is English in every slot: `data/translations/`
      `swe/dut/ita/ger.dat` are header-only stubs and the build fills
      every language from `text.dat`'s `eng=` lines, so the five built
      `.dat` files are byte-identical.  The flag proves the load path
      (`[args] language: german (4)`, a clean boot through the GERMAN
      `FileEquate`), not a visible translation;
    - speech has no language dimension: one `TRACK1.IXA` serves both
      territories and `CXAStream::SetLanguage` (`sound/cdxa.h`) has no
      caller - it stays uncalled.
    The `language=` key of the M8 PR-3 `sbsp.ini` must feed
    `SBSP_LANGUAGE` (precedence: argument > environment > ini); nothing
    else changes here.

## Game-source changes (M9)

The x64 PC build (`SBSP_PC64`, clang-cl `x86_64-pc-windows-msvc`).  Same
rule as ever - the PlayStation build stays hash-identical - met here
mostly by *same-line* edits through macros whose non-x64 expansion is the
original token sequence, so no line moves and only one `#line` arm was
needed (#32).  The 32-bit PC builds are unchanged too: their
`[scene]`/`[frame]` streams before and after this batch are identical.

31. **`tools/Data/include/dstructs.h`, new `tools/Data/include/fptr.h`,
    `source/utils/utils.h` (`RELOC_PTR`), `source/level/level.cpp`,
    `source/level/layerback.cpp`, `source/gfx/actor.cpp`** - the pointer
    fields of the file-overlay structs.  MkLevel / MkActor `fwrite`
    `sLevelHdr`, `sLayerShadeHdr`, `sSpriteAnimBank`, `sSpriteAnim` and
    `sSpriteFrameGfx` whole, each pointer member holding a 4-byte file
    offset that the loader relocates in place
    (`X->F=(T*)MakePtr(Base,(int)X->F)`).  The tools are prebuilt 32-bit
    binaries, so the format is fixed: the 14 members are now declared
    `DPTR(T) name;` - `T *` everywhere except under `SBSP_PC64`, where it is
    `FPTR<T>`, a POD holding the 32-bit *address* (not an arena offset:
    every loaded file lives in the arena, which sits below 1GB, so the
    address zero-extends back exactly and 0 stays NULL - `actor.cpp` tests
    `PAKSpr` for a blank frame before relocating it).  One conversion
    operator, to `T*`, carries every read site (`[]`, `+`, `!`, `if()`,
    copies to raw pointers, arguments) through the built-in operators.  The
    14 relocation lines became `RELOC_PTR(X->F,T,Base);`, which expands to
    the original expression off x64 and to `.set(...raw())` on it;
    `actor.cpp`'s `(u32*)Actor->ActorGfx->Palette` became
    `(u32*)(u8*)...` (a C-cast to an unrelated pointer type cannot go
    through the conversion operator; a no-op elsewhere).
32. **`source/locale/textdbase.cpp` (`TransHeader`)** - the one overlay
    struct outside `dstructs.h`: a count and a table of `char *` that
    `relocate()` fixes up with `(u32)ptr+(u32)this`.  `TRANS_PTR` /
    `TRANS_RELOC` are defined in a block above the struct (the originals
    off x64, `FPTR<char>` and `.set((char*)this+raw())` on it) and the two
    lines use them.  The block adds lines above the file's ASSERTs, whose
    `__LINE__` the PlayStation build bakes into Spongey.cpe, so the off-x64
    arm ends in a `#line` that restores the original numbering - gated on
    the MIPS compiler alone (`mips` / `__mips__`, as `vsprintf.h`), since
    the 32-bit PC builds carry no byte-identity contract and want true
    line numbers in their ASSERT messages and PDBs.  The file's
    `ABI_CHECK` on `TransHeader` (`abi_check.h`, shared with
    `port/abi/abi_check.cpp`) sits at the very end for the same reason.
33. **`source/system/vsprintf.h`, `vsprintf.cpp` (varargs)** - the
    hand-rolled `__va_start` / `__va_arg` ("stdarg defs from MSVC", 1999)
    walk the stack from `&v`, the i386 convention; x64 passes the first
    four arguments in registers.  Any non-MIPS compiler now gets
    `<stdarg.h>` behind the same names (gated on the compiler's `mips`
    predefines rather than `PSX_MIPS_ASM`: it is an ABI matter, and
    `PSX_NO_ASM` must not switch it on a PlayStation).  With real `va_arg`
    a `short` must be fetched as the `int` it was promoted to
    (`__va_arg_short` / `_ushort`; the originals on MIPS), and `%p` goes
    through `uintptr_t` (`__va_arg_ptr`), which `number()` carries whole:
    its `num_t` is `long long` under `_WIN64`, where `long` is 32 bits, so
    a 64-bit address prints in full (16 hex digits).  Off `_WIN64` `num_t`
    is the `long` the 1999 code named.  Only `__writeDbgMessage` uses any
    of it.
34. **`source/mem/memory.h`, `memory.cpp` (heap alignment)** -
    `MemAllocate` rounded every block to 4 bytes behind a 4-byte length
    header, so every `new`ed object sat on a 4-byte boundary: wrong for
    8-byte pointers, and clang assumes `operator new` returns 16-aligned
    storage on x64.  `MEM_ALIGN` (4; 16 under `SBSP_PC64`) is now the
    rounding *and* the header size, `MEM_ROUND()` replaces the three
    `(TLen+3)&0xfffffffc`, and the DEBUG guards are `MEM_NUM_GUARDS` ints
    (2; 4 on x64, so header + head guard is still a multiple of 16).  The
    arena base is 16MB-aligned and every length carved from it is a
    multiple of `MEM_ALIGN`, so alignment holds by induction.  Off x64
    every expression folds to the constant it replaced - which is also why
    the 32-bit `RamUsed` numbers (`[summary]`, `# epoch ram=` markers,
    route ceilings) did not move.  On x64 they are larger (DEBUG campaign
    `peak_ram` 1,319,248 against 1,287,020) and still far under the
    ceilings.
35. **`source/gfx/prim.cpp:45`, `source/system/except.cpp` (pointer/int
    casts)** - the only sites the x64 compile flagged
    (`-Wpointer-to-int-cast` / `-Wint-to-pointer-cast` are deliberately
    not on the suppression list): `(int)ptr` became `(int)(size_t)ptr` and
    `(int *)i` became `(int *)(size_t)i`, the `prim.h` spelling.  Identical
    code on a 32-bit target.  `PrimDisplay`'s overflow test still compares
    truncated addresses, which is exact below 2GB; `except.cpp` is the
    PlayStation exception screen (a MIPS register dump) and is never
    entered on PC.  Comment only, same line count: `gfx/prim.h`'s
    portable-branch NOTE now states the real prim-tag constraint (issue
    #14) - 24 bits reach 16MB, so the OT and its prims must share one
    16MB-aligned window, whatever the pointer width.

### EUR build (M8 PR 2)

`port/CMakePresets.json` has `eur-debug` / `eur-final` beside the USA
`debug` / `final` (`SBSP_TERRITORY=EUR` -> `-D__TERRITORY_EUR__`, headers
from `out/EUR/include`, data from `out/EUR/cd` - one data build serves
both variants since PR 3, see "Host shell" below).  Build the data first -
`port/build-data.cmd eur` (the preset words and the two-word `EUR DEBUG`
form still work) -
then `port/build-pc.sh eur` (`test eur`, `soak eur`); `all` / `test` /
`soak` without a territory cover all four trees.  `out/EUR` is regenerated, not
copied: `makefile.gfx` has no territory conditional, but the translation
step emits the `STR__*` enum of `trans.h` (and the string ids inside
BIGLUMP) in a different order on every run, so an exe must always pair with
the include directory it was compiled against - which the presets
guarantee.  (`build-data.sh` also now defaults `COMSPEC`: `MkActor.exe`
packs through `system("lznp ...")`, and an MSYS2 shell started without a
console lacks the variable, failing every actor with a bare "Could not
open temp Pak file Actor.Pak".)

What the territory changes on PC - everything else is the same code:

- **50 Hz.**  `vid.cpp`'s `SetVideoMode(MODE_PAL)` retimes the pump
  (`Port_SetVBlankHz(50)`); the XA/STR sector clocks (exactly 3 sectors
  per vblank), RCnt2 and the audio dump all divide by the live rate.
  `GetVideoMode()` answers from that rate (`Port_VBlankHz`) instead of a
  hardcoded NTSC, `[pace]` prints `hz=` over a 250-vblank window, and
  `getOneSecondInFrames()` is 50.  Scene-relative pad-file offsets count
  frames, so the USA routes run unchanged; `run_tier.py --territory EUR`
  (what the EUR tree's ctest passes) selects the EUR-only routes and the
  `# eur ...` header lines.
- **XM_PAL.**  `sound/xmplay.cpp` initialises the sequencer at 50 ticks/s
  from the territory macro, independently of `SetVideoMode`, so
  `XM_OnceOffInit` cross-checks the two and prints `[xm] WARNING` (a
  forbidden tier tag) on a mismatch; `xm_test` proves both directions.
- **2D tile margin.**  `level/layertile.cpp` has no PC arm, so the EUR
  build takes retail EUR's `SCREEN_TILE_ADJ_H=2` (23 rows, +32 TSPRTs per
  2D layer); the 3D layer's margins are entry #18's PC arm in both
  territories.  (Follow-up, not this PR: the USA window shows the same
  full 256 lines a PAL set did, so `ADJ_H=1` may leave a one-line gap at
  some scroll phases.)
- **PLAY TRAILER.**  The EUR main menu's third item (`frontend/maintitl.cpp`,
  `fmvad.cpp`) plays `DEMO.STR` (`FMV_DEMO`, `FILEPOS_DEMO_STR` - the
  six-entry `FILEPOS_MAX`); the virtual disc and `build-data.sh` already
  carried the file for both territories.  `port/tests/routes/play_trailer.pad`
  (`# territory EUR`, part of `--fast`) drives it and requires
  `# min-frames` distinct display CRCs.
- **Memory-card name.**  Saves are `BESLES-03704*` (`memcard/memcard.h`)
  instead of `BASLUS-01352*`, in the same `card0.mcd`; `memcard.cpp`'s
  card scan matches the first 12 characters, so each territory sees only
  its own saves and both sets coexist in one image.

Documented no-ops on PC: `ScreenYOfs=16` (`vid.cpp`) and the FMV
`TerrOfs` (`fmv/fmv.cpp`) only move `DISPENV.screen.y` - the CRT
placement, which the shim records (`gpu_core.h screenY`) and never uses.
The framebuffer is 512x256 in both territories (`vid.cpp` asserts it) and
the display CRC / BMP dumper read the DISPENV rect only, so a `[frame]`
CRC cannot see the PAL letterbox shift; that is why the trailer route's
oracle is "many distinct frames", not a mid-movie CRC.

### Host shell (M8 PR 3)

No game-source change: everything below is shim-side (`port/psyq/host`,
`port/psyq/vk`, `port/psyq/cd`), so the M8 entry numbering stays at #30
and the PSX build is untouched by construction.

**Data once per territory (issue #35).**  `makefile.gfx` has no `VERSION`
conditional, so a DEBUG and a FINAL data build are byte-identical.
`port/build-data.sh <usa|eur>` still runs the vintage make into a PSX tree
(`out/<T>/<V>/version/CD`, `VERSION` defaulting to DEBUG and only settable
by the two-word form) and then stages the six CD files once into
`out/<T>/cd/`.  `cd.cpp`'s `resolveDataRoot` walks an ordered candidate
list on every directory build - `SBSP_DATA_DIR` verbatim (never fallen
through: `xa_test` points it at a directory that does not exist on
purpose), `data\` beside the exe, `out/<T>/cd`, then the PSX tree for data
built before #35 - and `CdInit` prints `[cd] data: <root>` or lists every
candidate it tried.  `build-pc.sh check_data` and the CMake warning gate
on `out/<T>/cd/BIGLUMP.BIN`.

**sbsp.ini (`host/ini.cpp`).**  Lives beside the exe (`Port_ExeDir`), the
one place a tester looks and the thing that makes an unpacked folder
self-contained; `--ini` / `SBSP_INI` names a different one, and there is
no search path beyond those two - nothing shipped with the file anywhere
else, so there is nothing to be compatible with.
The save directory is a separate question (`Port_SaveDir`,
`host/hostpath.cpp`): `SBSP_SAVE_DIR` verbatim, else `saves\` beside the
exe if that directory exists (the zip layout), else `%APPDATA%\SBSPSS`.
Every key is the ini spelling of an `SBSP_*` variable and loading is
`_putenv` for each key whose variable is unset, so the precedence
**argument > environment > ini** costs the consumers nothing (they all
`getenv` lazily).  `args.cpp` therefore loads the ini *after* its
argument pass and reads `SBSP_BOOT_LEVEL/SEED/LANGUAGE` after that.  The
key set is a whitelist shared with the default writer - the harness
switches (`SBSP_UNCAPPED`, `SBSP_EXIT_AFTER`, `SBSP_PAD_FILE`...) have no
ini spelling, so a stray line can never turn an interactive run into a
scripted one.  A harness run (`Port_HarnessRun`) writes no defaults, and
since issue #58 a run with scripted input (`--pad-file`/`--pad-script`,
which every `run_tier.py` run is) reads only an explicit `--ini` - not
even the file beside the exe - so its key caps, language and prompt icons
cannot depend on the tree it runs from (`run_tier.py` treats an
`[ini] loaded` line as a failure); a live `--uncapped`/`--exit-after` run
still loads it.  Its temp `--save-dir` keeps the card
out of the developer's own (`sbsp_headless`, an interactive run as far as
`args.cpp` can tell, gets a private `SBSP_SAVE_DIR` and `SBSP_INI` for
that).  Argument twins: `--ini`,
`--window`, `--scale`, `--vsync`, `--volume`, `--set key=value`; the
env-only `SBSP_ASSERT_CONTINUE` / `SBSP_MEM_LOG` gained `--assert-continue`
/ `--mem-log`.  Keys: `window` (`WxH` | `fullscreen`), `scale`, `vsync`,
`audio_device`, `audio_buffer_frames`, `volume`, `key_<button>` x14,
`pad_deadzone`, `rumble`, `pause_on_focus_loss`, `language`, `data_dir`,
`save_dir` (no longer circular now that the file is not in the save
directory).
`ini_test` pins the parser, the precedence rule and the defaults
round-trip.

**Presenter.**  `vk/viewport.cpp` (`Port_ViewportRect`, pinned by
`viewport_test`) replaces the inline 4:3 arithmetic: `fit` is the M2
letterbox, `integer` a whole multiple k of the display's line count at
4:3 width (the 256-line frame at k=3 is 768x1024 - horizontally exactly 2
pixels per source pixel; PS1 pixels are not square, so only the vertical
factor is integral) falling back to fit below k=1, `stretch` the whole
window.  `vsync=0` asks for MAILBOX, then IMMEDIATE, else keeps FIFO and
says so (`[host] present mode:`); `vkGetPhysicalDeviceSurfacePresentModesKHR`
joined the loader list.  Emulated time never waited on the display in any
mode (timeout-0 acquire).  `window=` sets the initial size (default
1024x768, three lines per PS1 line) or starts borderless fullscreen;
`Alt+Enter` toggles it at run time (`SDL_SetWindowFullscreen`, no
exclusive mode), and `keyboardMask` returns nothing while Alt is held so
the chord cannot press START.

**Pause on focus loss.**  `SDL_EVENT_WINDOW_FOCUS_LOST/GAINED` set a flag
in `window.cpp` (never for a harness run - its window is nobody's and its
vblank budget is the oracle).  `Port_Pump` asks `Host_PausePoll` after
its re-entrancy guard: while paused it waits for events 100 ms at a time,
repaints at ~10 Hz and returns without a vblank, so no callback, input
frame, memory watch, frame CRC or exit-after fires - game time stops.  On
the resume edge the pump assigns `g_vblankBase = g_vblank` and re-stamps
`g_qpcBase` (not via `wallVblank()`, which is by then far ahead), so the
game continues from the frame it stopped on with no `MAX_PENDING_VBLANKS`
catch-up burst; the audio device is paused across the edge
(`Host_AudioPause`, else the SPU drones on its last voice state), the
watchdog thread resets its stall count while `Port_Paused()`, and
`[summary]` gains `paused=<seconds>`.  The M3 invariants (one vblank per
pump, no nesting, backlog rebased not skipped, `Port_NowSeconds` off the
fixed origin) are untouched.  A paced CD read counts emulated vblanks
(issue #67), so it waits out a pause with the game.

**Input / audio knobs.**  The keyboard map is now one table in
`input.cpp` (`Port_InputBindKeys` from `SBSP_KEY_<BUTTON>` via
`SDL_GetScancodeFromName`, bad names keep the default and say so);
`pad_deadzone` (percent, default 15) centres a stick inside it; `rumble=0`
returns before the motors are ever armed; `audio_device` matches part of
a playback device name (case-insensitive, the list is printed on a miss),
`audio_buffer_frames` is the `SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES` hint
set before the open, `volume` is `SDL_SetAudioStreamGain` on the host
stream (the game's own `SpuSetCommonMasterVolume` keeps driving the SPU
mixer).  `pad_test` covers the dead zone, the name parsing and rumble=0.

**Tester zip.**  `port/package.py [--territory usa|eur] [--build]`
(`port/package.cmd` runs it with MSYS2's python3, sidestepping the
Microsoft Store `python` alias stub some machines have on the PATH)
preflights the two exes and the six data files (LFS-pointer and
2336-multiple guards), stages `sbsp.exe` (FINAL), `sbsp-debug.exe`,
`data\`, an empty `saves\`, `port/package/README.txt` and
`run-test-session.cmd` under `port/build/package/` and zips them.  The
session script snapshots the card, runs `sbsp-debug.exe --record-pad
--assert-continue --mem-log` with stdout and stderr in separate files (the
harness rule), copies `sbsp.ini` into the session folder before the game
starts - it is read once, at boot - or, on a first run that has none yet,
the defaults the game wrote at boot (a replay passes it with `--ini` - see
"Replaying a tester session"), and prints the exit code and `[summary]`.

### CI + clang-cl (M8 PR 4)

No game-source change again: the PS1 sources compile unmodified under
clang-cl, so the M8 entry numbering stays at #30 and the PSX build is
untouched by construction.  What the PR adds is a second toolchain for the
same tree, PDBs for the first one, and CI that runs both.

**`SBSP_CODEVIEW` (MinGW PDBs).**  `-gcodeview` (gcc 15+) puts CodeView
records in the objects and `ld --pdb=` (binutils 2.39+; empty name =
`<exe>.pdb`) collects them, so Visual Studio and WinDbg debug the MinGW
exe with source and locals.  Verified by loading `sbsp.pdb` through
dbghelp (`SymFromName` + `SymGetLineFromAddr64` resolve `main`,
`Raster_Triangle`, `CdInit` to file:line).  Two quirks: llvm-pdbutil
rejects ld's PDB ("DBI file info substream not aligned") although the
Microsoft reader takes it, and ld prints four non-fatal `undefined
reference to __ZZN9CCDFileIO9ReadAsyncER11sASyncQueueE5Error` lines
against `.debug$S` (gcc emits an S_LDATA32 for a function-local static
whose symbol it then never defines).  `SBSP_CODEVIEW=1 port/build-pc.sh
debug` or `-DSBSP_CODEVIEW=ON`; off by default because gdb wants DWARF.

**Portable spellings (`host/compiler.h`).**  Every compiler-specific
construct the shim had lives in one header: `PORT_EARLY_CTOR(fn)` is
`__attribute__((constructor(101)))` on GNU and a function pointer placed
in the `.CRT$XCT` initializer slot under `_MSC_VER` (the CRT walks
`.CRT$XCA..XCZ` in order; compiler-generated static constructors are
`.CRT$XCU`, so XCT runs first - proven at -O2 under clang-cl, where the
pointer is also marked `used` so the optimizer cannot drop an
unreferenced internal global); `PORT_NORETURN` is `__declspec(noreturn)`
/ `__attribute__((noreturn))`.  `PORT_Scratchpad` is `alignas(16)`; the
`PSYQpause()` shadow falls back to `__debugbreak()` where `__builtin_trap`
does not exist.  The two whole-archive links use CMake's
`$<LINK_LIBRARY:WHOLE_ARCHIVE,sbsp_game>` (= `--whole-archive` on GNU ld,
`/WHOLEARCHIVE:` on lld-link).

**clang-cl toolchain (`cmake/clangcl-toolchain.cmake`, presets
`clangcl-debug` / `clangcl-final`).**  `--target=i686-pc-windows-msvc`,
lld-link, llvm-rc; inside a vcvars prompt the INCLUDE/LIB environment is
used, outside one the newest VS 2026/2022 MSVC toolset and Windows SDK are
globbed and passed as `/vctoolsdir` `/winsdkdir` `/winsdkversion` to
clang-cl and as `/libpath:` to lld-link (CMake drives the linker
directly, so the driver cannot derive them).  `cmake/deps_vc.cmake`
fetches the official `SDL3-devel-<ver>-VC.zip` (SDL3.lib + SDL3.dll - no
static lib, so `sbsp_exe_link` copies the DLL beside each exe via
`$<TARGET_RUNTIME_DLLS>`) and Khronos `Vulkan-Headers` at the tag matching
MSYS2's `VK_HEADER_VERSION` (350) into `port/build/deps`, pinned by
SHA-256.  Flag spellings the frontend decides (`SBSP_MSVC`): `/Z7 /O2 /MT
/EHs-c- /GR-` and `/clang:-std=gnu++98 /clang:-fpermissive
/clang:-fno-builtin /clang:-fno-strict-aliasing /clang:-idirafter<dir>`;
`-Wno-<x>` passes through unchanged but `-Wall` must be `/W3` - to
clang-cl a bare `-Wall` *is* `/Wall`, i.e. `-Weverything`, and a
`/clang:-Wall` is appended after every `-Wno-`, undoing them.
`_CRT_SECURE_NO_WARNINGS` silences the CRT's `fopen`/`getenv` advisories
(the `_s` forms are not portable to MinGW).

**The game under clang-cl** (the stretch goal) needed three things, none
in `source/`: `_CRT_NOEXCEPT=throw()` for the game target, because the
UCRT headers spell their exception specifications `noexcept` for every
C++ dialect and gnu++98 has no such keyword (corecrt.h lets the user pick
the spelling first); `StCdIntrFlag` defined with C++ linkage in
`str_stream.cpp`, because fmv.cpp declares it as a plain `extern long` -
one symbol under the Itanium ABI (global variable names are not
mangled), two under MSVC's; and a linker alias
`/alternatename:?SinTable@@3QBFB=?SinTable@@3PAFA`, because mathtab.h
declares `extern const s16 SinTable[1024]` and sincos.cpp defines it
non-const - the same one-symbol/two-symbols story, resolved at link time
rather than with a `!PSX_MIPS_ASM` arm while the MinGW exe is the one
that ships.  Two clang-only warning classes joined the game's suppression
list (`-Wnonportable-include-path`: case-mismatched `#include`s, 1060 of
them; `-Wshift-negative-value`: `(-1)<<n` in pcart/pghost).  With those,
`sbsp.exe`, `sbsp_headless.exe` and all fifteen unit exes build with 0
warnings and the `unit` and `playthrough` ctest labels pass on the
clang-cl tree.  `SBSP_BUILD_GAME=OFF` still drops the game targets for a
shim-only configure.

**CI (`.github/workflows/build.yml`).**  The MinGW job now installs
`mingw-w64-i686-python` and runs `ctest -L unit` then `ctest -L
playthrough` (every route/level's stderr goes to `<build>/tier-logs`,
uploaded on failure); a new `clangcl` job builds the same tree with the
runner's own cmake/ninja/python, its LLVM and Visual Studio (2026 on
today's windows-latest), no MSYS2 at all,
and runs both labels - `continue-on-error` while the toolchain is young.
The MSYS2 SDL3 URL pin stays (the mirror still serves the file); the
durable escape from the shrinking mingw32 index is the clang-cl route.

### x64 build (M9 PR 1)

The shim-side half of the x64 exe; the game-source half is entries #31-#35
above.

**Toolchain.**  `cmake/clangcl-toolchain.cmake` takes `SBSP_ARCH` (`x86`
default, `x64`): it picks `CMAKE_SYSTEM_PROCESSOR`, the target triple
(`i686-` / `x86_64-pc-windows-msvc`) and the leaf of the three lld-link
`/libpath:` directories.  The variable is listed in
`CMAKE_TRY_COMPILE_PLATFORM_VARIABLES` - the toolchain file is re-read
inside every `try_compile` project, which does not otherwise see the cache
and would have probed the compiler as x86.  Inside a vcvars prompt of the
other architecture the configure stops (`VSCMD_ARG_TGT_ARCH`) rather than
link against that prompt's `LIB`.  Presets `clangcl-x64-debug` /
`clangcl-x64-final`; `build-pc.sh clangcl64` (build, `test`, `soak`).
`deps_vc.cmake` needed nothing: the SDL3 VC package carries `lib/x64` and
its config picks by pointer size, and the Vulkan headers have no
architecture.  The `/alternatename` alias for `SinTable` is the same
string on x64: MSVC mangles a global array as a pointer to its element
type, and that outermost pointer carries no `E` (`__ptr64`) qualifier.

**`SBSP_PC64`** is defined for the game target (and `abi/abi_check.cpp`)
when `CMAKE_SIZEOF_VOID_P` is 8, never for the shim, the data tools or the
PlayStation build.

**The shim on x64** built with 0 warnings as it stood - it was written
against `uintptr_t` / `size_t` / `ptrdiff_t` throughout - and hit one link
error: `EnterCriticalSection`.  The PSY-Q function (libapi: disable
interrupts, no arguments) and the Win32 one never met on i686, where
kernel32's is stdcall and decorated `_EnterCriticalSection@4`; x64 has one
calling convention and no decoration, and the static CRT drags kernel32's
import member in, so lld-link saw the symbol twice.  The
`port/include/libapi.h` shadow renames the PSY-Q one to
`psyq_sdk_EnterCriticalSection` on every PC build - declaration and
callers alike, the `rename` / `_get_errno` trick without the `#undef` -
and `api/libapi_stubs.cpp` defines it under that name.  The rename is
unconditional rather than keyed on `_WIN64` or `SBSP_PC64`, so the
declaration and the definition can never disagree on the predicate; on
i686, where the two symbols never collided, it is merely harmless.

**The CD ready callback** was the one behavioural x64 bug, found by the
A/B rather than the compiler: on the `gameover_continue` route the x64
exe never left the Continue screen.  libcd types the callback
`void (*CdlCB)(u_char, u_char *)`, but the game's handler is
`XACDReadyCallback(int Intr, u8 *)` cast to it (`sound/cdxa.cpp`) and
switches on all 32 bits of `Intr`.  MIPS and i686 hand a `u_char` over as
a full zero-extended word; the x64 ABI leaves the upper bits of the
register undefined, so the handler missed `CdlDataReady`, never saw the
ID-352 terminator, and the speech "played" forever.  `cd/xa_stream.cpp`
now calls the handler as `(int, u_char *)`.  `CdlCB` is the only SDK
callback type with a sub-`int` parameter that the shim invokes.

**Arena.**  `api/arena.cpp` keeps its 16MB-aligned probe below 1GB, which
a 64-bit process satisfies as easily as a 32-bit one - so the 24-bit prim
tags (`gfx/prim.h`, `gpu/gp0.cpp`'s `window | addr24`) needed nothing.
What x64 loses is the "any base" fallback: `VirtualAlloc(NULL)` returns a
base above 4GB there, which neither the prim tags nor the `FPTR` fields
(#31) survive, so the x64 build aborts with a message instead.
`host/fptr_fail.cpp` is where an `FPTR::set()` of a pointer above 4GB
lands (`[shim] FPTR: ...`, exit 11).

**`abi/abi_check.cpp`** joins the game library on every PC build:
negative-array-size checks (gnu++98 has no `static_assert`) that the five
pointer-bearing overlay structs and the pointer-free ones around them
still have their on-disc sizes, compiled with the game's own flags.  An
x64 build that lost `SBSP_PC64`, or a new raw pointer member in
`dstructs.h`, fails to compile instead of reading every file at the wrong
offsets.

**CI.**  The `clangcl` job is a two-entry matrix, `clangcl-debug` and
`clangcl-x64-debug` (both advisory): configure, build, `ctest -L unit`,
`ctest -L playthrough`.

### x64 A/B (M9 PR 2)

No game-source change.  The x64 exe is proven against the 32-bit one by
three oracles, `port/build-pc.sh parity64 [final|debug]`:

1. **Streams** - `run_tier.py --compare-frames` (M8 PR 5): every Tier 1
   route and Tier 2 level, `[scene]` + `[frame]` CRC lines identical.
2. **Cross-exe replay** - `--keep-artifacts DIR` makes the first exe keep
   each passing route's `--record-pad` recording; `--replay-from DIR`
   makes the second exe replay those instead of playing the routes.  The
   recording's `# epoch` markers carry the display CRC every 300 vblanks
   and the game checks them itself (`[replay] desync`, exit 13).  They
   also carry `RamUsed`, which is *not* comparable across ABIs (bigger
   objects, 16-byte heap granularity): recordings now start with
   `# abi ptr=<4|8>` (absent = 4), and `host/input.cpp` skips the ram half
   of the check - only that - when the recording's pointer size is not the
   exe's, saying so once (`[input] cross-ABI recording ...`).  Since
   issue #67 `# build debug|final` follows it and does the same across
   build types (RamUsed differs between DEBUG and FINAL too).  Since
   issue #58 the header goes on with `# seed` (only when the run was given
   one) and `# pace`, `# prompt` lines mark the prompt-icon device
   switches, and each epoch also carries
   the game's RNG state (`rng=`, compared across ABIs) - "Replaying a
   tester session", below.
3. **Memory card** - the `card0.mcd` a route leaves is kept beside its
   recording and the cross replay's must be byte-identical (the same-exe
   replay of a plain Tier 1 run now compares cards too).  The save structs
   are all `char` (`memcard/saveload.h`, `game/gameslot.h`), so this holds
   by construction; the check is there for uninitialised bytes.

Result (USA, seed 1, clang-cl): DEBUG x64 = x86 on all 9 routes + 25
levels (163,000 `[frame]` lines), cross replay clean in both directions,
cards identical.  FINAL the same except 39 frames of the campaign route
(6858-6897) where the **32-bit** clang-cl FINAL exe omits one sprite at
the screen edge that x64 FINAL, MinGW FINAL and both DEBUG builds draw -
a latent compiler-dependent read in the game, not an x64 matter, left
for the cross-toolchain work (issue #39) together with the frames on
which MinGW and clang-cl differ whatever the pointer size (campaign
463-512 in both variants; Tier 2 level 24 from frame 306 in DEBUG).

### x64 in the tester zip (M9 PR 3)

No game-source change.  `port/package.py --x64` adds the clang-cl x86_64
exes to the zip *beside* the 32-bit MinGW pair rather than instead of
them: `sbsp64.exe` (from `build/clangcl-x64-final`), `sbsp64-debug.exe`
(`clangcl-x64-debug`) and one `SDL3.dll` - the first non-static file in the
package, because the official SDL VC package has no static library.  The
MinGW exes are static and never look at it, so the x64 DLL can sit in the
same folder; what must never happen is an exe or DLL of the wrong
architecture getting in from a stale tree, so every PE file's machine
word is checked against what its name promises (`pe_machine`).  USA only:
there are no clang-cl EUR presets, and `--territory eur --x64` says so.
Without `--x64` the zip is what it was.

The four exes share `data\`, `sbsp.ini` and `saves\card0.mcd` - everything
is found beside the exe (`cd.cpp`, `hostpath.cpp`) and the save format has
no pointer-size dependence (the A/B above compares cards byte for byte) -
so a tester can move one campaign between them.  `run-test-session.cmd
x64 [final]` records a session on the 64-bit pair; `session.pad`'s
`# abi ptr=8` line says which build made it.  Verified by unpacking the
zip into a scratch directory and booting all four exes from there
headlessly: each finds `data\` beside itself and they agree on the frame
CRC.

### Replaying a tester session (issue #58)

Game source: the one-token bonus-level guard in entry #28, and the
`&s_randomSeed` registration in entry #26 (`main.cpp`, `asmport.h`) -
both inside `!PSX_MIPS_ASM` arms and line-neutral.
A `run-test-session.cmd` session now replays.  `session.pad` carries the
left stick (folded into the mask by the game's own `Pad2Digital` rule,
`host/input.cpp stickFold`, so the grammar is unchanged), a button still
held when a scene opens (pressed again in the new scene's terms, since a
scene open releases every entry on replay), the seed when one was given
(`# seed`, `host/seed.cpp`; a tester's session has none and needs none,
see entry #26), the pacing (`# pace capped|uncapped`) and every
prompt-icon device switch (`# prompt <vblank> pad|keys`, latched once per
input frame so the game, the recorder and a replay all switch on the same
frame); the session folder also gets the tester's `sbsp.ini`.  A scripted
run - `--pad-file` or `--pad-script` - ignores the live keyboard and pad
entirely (mask, sticks and prompt device), reads only an explicit `--ini`, and
`run_tier.py` drops every inherited `SBSP_*` variable, so the harness and
a replay are a function of the exe, the data and the arguments alone.
When the first gamepad goes away the next one still plugged in is adopted
(SDL announces a pad once).  Older exes read the new `#` lines as
comments; this exe reads older recordings unchanged.

    sbsp-debug.exe --pad-file <s>\session.pad --ini <s>\sbsp.ini --save-dir <dir> --uncapped --no-audio --exit-after <n> --frame-crc

with `<dir>` holding `card-before.mcd` renamed `card0.mcd`, and `<n>` the
`vblanks=` of the session's `[summary]` (closing the window is not
recorded, so `--exit-after` stands in for it).  Any exe replays it, DEBUG
or FINAL, 32- or 64-bit: since issue #67 a paced load costs the same
vblanks everywhere, and every vblank a capped session fired outside a wait
is in its recording (`# bare`), where the replay fires it too - so the
uncapped replay lands every frame where the session did (see "Replay
judging" below).  Before #67 a
capped replay's loads took *about* as long as the session's, not exactly -
measured on one machine, record and replay both capped with pacing on, the
level load took 249 vblanks recording and 248 replaying on two levels out
of three, which shifted every later frame by one and failed every epoch;
the third matched exactly, as did all three with `--no-cd-pace`.
`replay_test` records a scripted session with a
virtual pad and requires the replay's own recording to be byte-identical;
`pad_test` covers the stick fold and the second-pad adoption.

**Epochs carry the RNG.**  Every level was recorded without `--seed` and
replayed without one: all 25 x 3600 frames identical, every epoch clean.
The negative control - each recording replayed with its seed + 1 - showed
the epochs were too coarse to be trusted alone: the frames differed on all
25 levels, but on 1-5, 2-3 and 5-5 (111, 601 and 166 differing frames)
every difference fell between two epochs, which matched on RamUsed and the
display CRC, and the replay exited 0.  An epoch is now
`# epoch <vb> ram=<n> crc=<hex> rng=<hex>`, `rng` being the game's
`s_randomSeed` at that vblank: a run that has drawn one different random
number has diverged whatever is on screen, so the wrong-seed replays now
fail on every level.  `long` is 32 bits on both Windows ABIs, so `rng`, like
`crc`, is compared across them (only `ram` is not); an epoch without it -
an older recording - skips it.  `replay_test` replays with the wrong seed
against a constant picture and RamUsed and requires the desync.

### Replay judging (issue #67)

Shim-side: `cd/cd.cpp`, `cd/xa_stream.cpp`, `host/args.cpp`,
`host/input.cpp`, `host/pump.cpp`, the tests and the docs.  Game source:
entries #56 (the pause-menu report) and #57 (the zeroed save buffer), both
inside `!PSX_MIPS_ASM` arms with a `#line` re-sync.

**Loads cost emulated vblanks.**  `CdRead` still copies its sectors at
once; what changed is how long `CdReadSync` then reports "still reading".
It used to be a wall-clock deadline (`Port_NowSeconds() + sectors/150`), so
how many vblanks fired during a capped load was scheduling jitter - the
249 against 248 above - and an uncapped run could not pace at all
(`--uncapped` forced `SBSP_CD_PACE=0`).  Now `Port_CdDataVblank`, ticked
from `Port_CdVblank` beside the XA and STR clocks, counts the drive's
sectors in the XA engine's units: +150 per emulated vblank, one sector per
`hz`, so 2.5 sectors per NTSC vblank and exactly 3 per PAL one.  An idle
drive banks at most one vblank (2 sectors NTSC, 3 PAL), so the loader's
one-sector tail reads finish inside the frame, as on the PlayStation.
After one idle vblank the bank is the same however long the drive sat, so
a read's cost is a function of the vblank and read sequence alone:

| sectors | 1 | 2 | 3 | 4 | 30 | 150 |
|---|---|---|---|---|---|---|
| 60 Hz | 0 | 0 | 1 | 1 | 11 | 59 |
| 50 Hz | 0 | 0 | 0 | 1 | | 49 |

A real double-speed drive takes 60 and 50 vblanks for 150 sectors.
`cdpace_test` pins the table, two reads chained before a wait, a long idle
stretch, and a wall-clock sleep in the middle of a read (the old deadline
would have expired in it).

**Look before waiting.**  `CdReadSync` used to pump first and compare
after.  In a capped run that pump could fire a vblank after the one that
completed the read and before the game saw it complete, and StopLoad's
`while(LoadTime) VSync(0)` then waits for the loading icon to wrap, so a
one-vblank slip there can cost up to 59 more.  A paced `CdReadSync` now
returns at once when nothing is pending, and otherwise takes exactly one
`Port_PumpIdle` step per call: one vblank per call uncapped, one per
wall-clock vblank capped, and either way the read completes on the same
emulated vblank.  `--no-cd-pace` keeps the old single pump.

**`--uncapped` keeps pacing.**  `args.cpp` no longer puts `SBSP_CD_PACE=0`
beside `SBSP_UNCAPPED=1` (`--uncapped` is now a plain switch), and a paced
load lasts as long in both.  `# pace
uncapped` no longer implies instant loads, so a recording also says which
it had (`# loads paced|instant`, after `# pace`); instant against paced
drifts after every load and is reported once.  `run_tier.py` keeps
`--no-cd-pace` in its determinism set, so every `--compare-frames`
baseline and `--replay-from` artifact stays valid; its `--selftest` adds
two paced runs of 1-1 that must agree frame for frame and open the level
later than an instant run does (vblank 244 against 60 on the USA debug
exe).  With `--no-audio` an uncapped run has no wall-clock input left,
paced loads or not.

**Vblanks outside a wait are recorded.**  With loads fixed, a capped
recording still replayed 5 vblanks off uncapped on the machine measured.
A capped run fires a vblank at *any* pump once the wall clock says one is
due, so whenever it has fallen behind - the boot stall, the first present
at about 150 ms, is enough - its vblanks land at bare pumps (`VSync(-1)`,
`DrawSync`, `PadGetState`, the front of a wait) that an uncapped run
passes without one, and StopLoad's wait for the icon to wrap carried the
offset into every later frame.  The PR's first answer fired vblanks only
in wait steps (`Port_PumpIdle`), capped or uncapped; review showed what
that cost live play on a slow host: during any stretch of game code that
does not wait - a level's set-up after its last read - nothing
vblank-driven ran (the music's sequencer tick, `XM_Update`, is a vblank
function; the loading icon; the window's events and presents), and the
next waits then fast-forwarded through the backlog.  So a live capped
run fires due vblanks at bare pumps again, exactly as before #67, and
`--record-pad` writes each one down: `# bare <vblank> <k>`, the vblank
fired at the k-th bare pump since the last wait step
(`Port_VBlankBarePump`).  A scripted run - `--pad-file` or
`--pad-script`: every replay and harness route - fires a vblank at a bare
pump only where its recording says (`Port_ReplayBareVblank`), and any
other uncapped run fires none there.  k counts the game's own calls
(every bare pump is a `VSync`, `DrawSync`, `PadGetState` or unpaced
`CdReadSync` the game made), never the wall clock, so the replay reaches
the same k at the same point, and a recorded vblank the replay reaches any
other way is a desync at exit.  A recording without `# bare` lines - every
uncapped one, and every capped one made before this rule - fires none,
so the harness baselines and the first tester sessions replay as before.
A loop that never waits would spin forever where bare pumps fire nothing -
`VRamViewer` (DEBUG, hold SELECT) spins on `PadGetState`, and pads change
only at a vblank - so after `PORT_SPIN_PUMPS` (10000) bare pumps in a row
every further one is a one-vblank wait, until the game waits again; the
Tier 1 routes and the short Tier 2 levels never pass 200.  `pump_test`
records a live capped run whose bare pumps catch up seven owed vblanks,
replays it capped and uncapped (both replays' own recordings must equal
it), and checks that an uncapped run and a scripted capped one fire
nothing at bare pumps, plus the spin rule.  The `# pace` mismatch warning
is gone: there is nothing left for it to warn about.

**Why epochs stay absolute.**  #67 held a fallback in reserve: anchor each
`# epoch` to the latest scene open (`Scene#n+off`, like the pad entries),
so a replay that opened scenes late would still be judged at the right
frame.  It was not needed.  With loads costed in emulated vblanks and
every vblank fired outside a wait recorded, a replay opens every scene on
the vblank the recording did, capped or uncapped, so an absolute epoch
lands on the same frame; a drift is now itself a divergence, and anchoring would have hidden
it.  The grammar, and every recording made so far, stay as they were.

**Recordings name their build.**  A DEBUG heap block carries guard words
(`mem/memory.h` `MEM_BLOCK_HDR`), so FINAL's RamUsed runs about 3.3 KB
below DEBUG's with identical screens (916,472 against 919,768 at vblank
300 of the first tester session), and a DEBUG recording failed the `ram`
half of every epoch on FINAL.  `--record-pad` now writes
`# build debug|final` as the third line, after `# abi`, and a replay on
the other build type skips `ram` exactly as a replay across ABIs does,
saying so once (`[input] cross-build recording ...`).  A recording
without the line - every one made before #67 - still compares `ram`:
those came from both build types, so there is nothing safe to assume.
An unknown word is refused at boot, like a bad `# abi`.  The desync line
now names what differed, and prints `ram` even when it is not compared:

    [replay] desync at vblank 300 (line 15) on crc: ram 916472 vs 919768 (not compared: cross-build), crc ...

`replay_test` flips `# build` on a copy with a doctored `ram` (replays
clean), doctors `ram` alone (caught) and writes `# build release`
(refused, exit 13).

**The pause menu across builds.**  The first two sessions recorded on a #67
zip - each a new game, 1-1 and a save, one on the 32-bit and one on the
64-bit DEBUG exe, each with a pause of about nine seconds -
replayed frame-identical on both DEBUG exes, but both FINAL exes failed
the `crc` of the two epochs taken under the pause.  DEBUG's pause menu has
one more line, "Invincible SpongeBob" (`game/pause.cpp`, `__VERSION_DEBUG__`),
so the picture differs for exactly as long as the menu is up (539 and 510
frames) while the game is in the same state (`rng` matched).  The game
now reports every frame it renders (`Port_PauseMenuDrawn`, entry #56), and
a replay on the other build type does not compare `crc` while the menu was
in any of the last three frames built: the frame on screen at an epoch was
built up to two frames before the latest.  In both sessions that window
covered every differing frame, with one vblank to spare at each end.  A
scene open forgets the menu, `rng` is still compared, and a replay on the
same build type compares `crc` under the menu as before.  The desync line
says `crc ... (not compared: cross-build pause menu)` when it applies.
What this cannot excuse is a DEBUG-only *action*: turning Invincible
SpongeBob on in that menu, or R2 in a level (`game.cpp`, DEBUG skips to the
next level), changes the game itself, and a FINAL replay rightly reports it.
`replay_test` doctors an epoch's `crc` and reports the menu around it: on
the other build type it replays clean with the menu drawn up to two frames
before the latest, and is caught three frames after, with no menu, after a
scene open, and on the same build type.

**Saves carry no stale heap.**  The same sessions' cards differed between
DEBUG and FINAL, and between the two FINALs, in 7,375 bytes that no one
reads.  `CSaveLoadDatabase::allocateBuffer` rounds the save buffer up to a
whole 8 KB card block but fills only the header, the data and the MD5 at
the end (0x331 bytes of content), so the rest of the block went to the
card as whatever `MemAlloc` handed back: `0x3D` on DEBUG
(`MEM_FILL_PATTERN`, `mem/memory.cpp`), old heap contents on FINAL,
different again on x64 - and the MD5 covers them, so it differed too.
Entry #57 zeroes the buffer on PC: the four exes now write byte-identical
cards, the directory and save data unchanged.  A card saved by an older
build still loads (its MD5 covers the bytes it has), but its tail no
longer matches a replay's, so a session recorded before the change is
judged on the save data rather than on the whole card.

### Host exit path, crash reporting and paths (issue #62)

No game-source change: `host/diag.{h,cpp}`, `host/crash.cpp`,
`host/audio_out.cpp`, `host/wav_writer.{h,cpp}`, `host/pump.cpp`,
`host/input.cpp`, `host/ini.cpp`, `host/hostpath.cpp`, `host/args.cpp`,
`cd/cd.cpp`, the new `host/sbsp.manifest` + `host/sbsp.rc`, `CMakeLists.txt`
and the tests.  The PSX build is untouched by construction.

**Exit hooks.**  `Port_Exit` ends the process with `_exit` (the game's static
destructors were never meant to run), and `_exit` runs no `atexit` handler,
so what used to be registered there never happened.  `Port_OnExit(fn,
faultSafe)` is the replacement: a fixed table of eight, allocation-free,
each slot recording the thread that registered it.  `Port_Exit` now does,
in order: claim the exit (CAS) and store the owner's code and thread;
`fflush(stdout)` unless the code is FAULT or WATCHDOG (unchanged, issue
#58); print `[summary]`; run the hooks newest first, only those registered
on the exiting thread, and on FAULT only the fault-safe ones; `_exit`.  The
contract line goes out before any hook, so a hook that hangs or re-faults
cannot cost a run its summary.  Two hooks exist: the WAV dump's close (not
fault-safe: `fclose` takes the stream lock) and the rumble stop (fault-safe).
A watchdog or audio-thread exit runs neither, by the thread rule.

**The first caller owns the exit.**  A second `Port_Exit` on the owner's own
thread (a re-fault inside a hook or the summary) calls `_exit(ownerCode)` at
once; one from another thread (the watchdog, a fault on SDL's audio thread)
sleeps 5 s, then calls `_exit(ownerCode)`.  The process exit code therefore
always equals the one `[summary]` states - `run_tier --selftest` now checks
that, not only that a summary exists.  On FAULT and WATCHDOG the summary is
formatted on the stack and written with one `WriteFile` to the stderr
handle (`Port_StderrRaw`): the faulting thread (or whichever thread the
watchdog interrupted) may hold the CRT's stderr lock mid-`fprintf`.  The
`[crash]` and `[watchdog]` lines before it go out the same way.  The bytes
are the ones the text-mode stream wrote, CRLF included.  `exit_test` runs
each rule in a child process.

**The WAV dump is valid on every path.**  `Wav_Sync` patches both size
fields to the data so far, seeks back to the end and flushes; the first
seek pushes the PCM out before the header claims it.  `Port_AudioVBlank`
calls it after every vblank's write, so a dump is a WAV of whole vblanks
however the process ends, `TerminateProcess` included, and the exit hook
closes it on the clean and assert paths.  `Port_AudioVBlank` now runs
before `Host_VBlank` in `pumpStep`: `Host_VBlank` can end the process
(window close, `--exit-after`, the self-test) and took the last vblank's
audio with it.  The content is unchanged - the vblank callback (that
vblank's `XM_Update`) and the XA clock have already run - and so is the
first vblank a dump sees, since audio comes up with the video at the first
GPU touch (`ResetGraph`), after `SetVideoMode` has set the rate.
`selftest_wav` checks the clean, assert, fault and watchdog paths.

**Rumble stops at exit.**  Rumble is armed in 100 ms windows only SDL's
event pump expires, so a process that ended inside one (Alt+F4, an assert or
a fault while the vibe pattern ran) left XInput and HIDAPI pads buzzing.  The
first `openGamepad` registers the stop: `SDL_RumbleGamepad(g, 0, 0, 0)`, then
`SDL_CloseGamepad`.  A probe against SDL 3.4.10's virtual driver showed
`SDL_CloseGamepad` delivering a zero rumble of its own while one is armed;
the explicit zero does not rely on that, and whether a HIDAPI pad flushes a
queued rumble before closing needs a physical pad (#66).

**Crash reports.**  `[crash] code=... addr=%p rva=0x%lX link=%p ...`: `link`
is the exe file's preferred ImageBase plus the rva, so it goes straight into
`addr2line` / `llvm-symbolizer` on an ASLR-relocated run - 0x400000-based on
x86, 0x140000000-based on x64, where the old `0x400000 + offset` (truncated
to 32 bits) pointed at nothing.  The base is read from the exe FILE at
`Port_CrashInit`: the loader rewrites the mapped header's ImageBase when it
relocates.  Outside the exe, `rva=- link=-`.  The CRT terminations that raise
no SEH exception now report too, as `[crash] kind=<k>` and exit 11 like a
fault: `abort()` via `signal(SIGABRT)`, `std::terminate` via
`std::set_terminate`, invalid CRT arguments via
`_set_invalid_parameter_handler` (both CRTs), pure virtual calls via
`_set_purecall_handler` (MSVC only).  Both CRTs reset SIGABRT to
`SIG_DFL` before calling the handler, so it re-arms itself first: a second
`abort()` (another thread, or a fault-safe exit hook) is then an ordinary
second `Port_Exit` caller instead of the CRT's own `_exit(3)` under a
`[summary]` of 11.  The UCRT's `abort` raises SIGABRT before it consults
the abort-behavior flags, so `_set_abort_behavior(0, _WRITE_ABORT_MSG |
_CALL_REPORTFAULT)` only matters when no handler is installed: it turns
the `__fastfail` to Windows Error Reporting into `_exit(3)`.  The
`_WRITE_ABORT_MSG` message box exists only in the debug CRT, and these
builds link the release `/MT` one.  The main thread
reserves 64 KB with `SetThreadStackGuarantee`, so a stack overflow is
reported; a fault inside the report itself goes straight to `Port_Exit`.
`SBSP_SELFTEST` gained `abort`, `abort-in-hook` (a fault-safe exit hook
that aborts again: exit 11, one `[summary]`), `terminate`, `invalid-param`
(`_close(-1)`) and `stack-overflow`; the fault case also checks
`link == ImageBase + rva` against the exe's PE header.  MinGW's msvcrt.dll
never calls the invalid-parameter handler (`_close(-1)`, `strcpy_s(NULL)`,
`fclose(NULL)` and `_get_osfhandle(-5)` all just return errors), so there the
self-test logs `[selftest] invalid-param returned` and counts as a SKIP.

What stays uncatchable - no `[crash]`, no `[summary]`, the process just
ends with 0xC0000409 or similar:

- Stack-cookie failures: `/GS` on the clang-cl builds (`__report_gsfailure`
  calls `__fastfail` directly, by design), and on MinGW any code built with
  `-fstack-protector` (the MinGW exe links a `__stack_chk_fail`, which
  terminates without raising anything).
- Any raw `__fastfail` (Control Flow Guard, a corrupted list entry, a CRT
  check that fast-fails without consulting a handler).
- CRT-level failures inside SDL3.dll on the clang-cl builds (an invalid
  parameter, an `abort`): the official VC package's SDL3.dll imports no
  CRT DLL at all - whatever C runtime code it carries is its own, not the
  exe's static UCRT - so the handlers installed here do not cover it.  An
  SEH fault in SDL3.dll is still reported, with `rva=- link=-`.  (The MinGW
  exe links SDL statically, so there is one CRT, msvcrt.dll.)
- A stack overflow on a thread other than main (SDL's audio thread) has no
  guarantee reserved; its report may re-fault and die silently.

**UTF-8 paths.**  Every exe embeds `host/sbsp.manifest` through
`host/sbsp.rc` (`sbsp_exe_link`, so the unit exes too): asInvoker, the
supportedOS list MinGW's `default-manifest.o` carries (Vista through
10), and `<activeCodePage>UTF-8</activeCodePage>`, which makes the process
ANSI code page 65001 on Windows 10 1903 and later.  argv, `getenv`,
`fopen`, `_mkdir` and `GetModuleFileNameA` then carry any path, and none of
the host code had to move to the W APIs.  No `dpiAware` - SDL sets DPI
awareness itself.  The resource is `LANGUAGE 0, 0` at `RT_MANIFEST 1`, the
slot `default-manifest.o` uses, and binutils' resource merge drops the
default when the link supplies its own.  clang-cl links `/MANIFEST:NO`,
or lld-link would add a second one.  `llvm-readobj --coff-resources`
shows exactly one MANIFEST/1 on all three toolchains; the clang-cl exes
gained the supportedOS list (Windows 10 compatibility mode, as MinGW's
always had).  The code page applies to every narrow file operation in the
process, the game's data reads included (all ASCII paths).  The console
keeps its OEM output code page - `SetConsoleOutputCP` is not called - so a
non-ASCII path in a log line shows as mojibake in a console window but is
correct in a redirected log.  Path buffers on the exe-directory route grew
to 1024 bytes (`hostpath.cpp`, `args.cpp`, `cd.cpp`): a `MAX_PATH`-character
path can take three bytes a character.  `ini_test` checks `GetACP()`, a save
directory named in Polish and Japanese, and a copy of itself run from
inside that directory.

**A key the ini names twice takes its later value.**  "Already set" is now
decided once, when the load begins (`preset[]`): a variable the environment
or an argument set is never touched, so argument > environment > ini holds,
while a key the file itself set is the file's to change.  A later line wins
with `[ini] path:N: 'key' repeated - the later value wins`; a later empty
`key=` puts the key back to its built-in default; the count is of distinct
keys, not lines.

## Game-source changes (keyboard prompt icons, issue #43)

**The problem.**  Every "press this to do that" line in the game draws a pad
icon beside it, and all eight icons (`Graphics/UI/+but*.bmp`, 18x11 4bpp) are
PlayStation glyphs.  On PC the player is usually on the keyboard, where the
glyph says nothing: the jellyfishing-net prompt told them to press square,
circle and triangle when the keys are `A`, `X` and `S`.  The glyphs are also
compile-time constants, so they could never be right for a binding chosen at
run time through `sbsp.ini`.

47. **`port/psyq/host/input.cpp`** — `Port_InputPromptCap(button)` answers with
    the key cap to draw for a pad button, and `Port_InputPadActive()` says
    which device the prompts should describe.  The scancode → cap table lives
    beside `g_keys[]`, the only place a binding lives, and the shim answers
    with a cap id rather than an SDL scancode so no game code needs SDL.
    `PORT_CAP_NONE` means "keep the PS1 glyph" and is the answer for a
    gamepad, for `prompt_icons = pad`, and for a key rebound to something the
    art set has no cap for.  The active device is tracked in
    `Port_InputFrame` from whichever of the keyboard/gamepad masks actually
    produced buttons that vblank (a frame where both or neither are pressed
    leaves it alone, so the icons do not flicker), seeded by plug/unplug.

48. **`source/system/asmport.h`** — the `PORT_CAP_*` enum and the three
    declarations, in the existing `#ifndef PSX_MIPS_ASM` shared-contract
    block.  The PS1 build compiles none of it.

49. **`source/pad/padicon.cpp`, `source/pad/padicon.h`** (new TU, added to
    `makefile.gaz` `pad_src` and regenerated into
    `port/cmake/game_sources.cmake`) — `CPadIcon::getFrame(PAD_*)` is the one
    resolver every draw site now calls instead of naming `FRM__BUT*`.  It
    holds the pad-button → PS1-glyph table and the cap → `FRM__KEY*` table;
    on the PS1 toolchain the whole keyboard arm is `#ifndef PSX_MIPS_ASM`-ed
    out and it is a plain lookup.  The cap art has no combined Up+Down
    sprite, so the one prompt slot that wants both (the coral blower's aim
    line) draws the two arrow caps side by side, exactly as it draws the two
    PS1 glyphs.

50. **`Graphics/UI/+key*.bmp` (14 new, Git LFS), `makefile.gfx`** — the key
    caps, cut from the `Keyboard_Thick_v1` sheet.  Same 4bpp/`+`-prefixed
    convention as `+but*.bmp` so `parkgrab` generates `FRM__KEY*` for free.
    The cuts are 14x14 for the letter and arrow caps and 16x14 for
    `ENTR`/`SHFT`, every cap padded to a common 14px height so prompt rows
    line up; entry 55 below says why what the data build actually eats is
    20x14 and 24x14.  All fourteen share one 16-entry palette, so the whole set costs a single CLUT (`PAL__KEYA`);
    `Sprites.Spr` grows 180 bytes, which is why `port/tests/headless.cpp`
    carries a new `EXPECT_SPRITES_SIZE`.

51. **The ten draw sites** — `source/player/player.cpp` (in-game item
    prompts), `source/frontend/options.cpp` (controls readout + footer),
    `source/frontend/start.cpp`, `source/save/save.cpp`, `source/map/map.cpp`,
    `source/shop/shop.cpp`, `source/game/convo.cpp`,
    `source/game/bosstext.cpp`.  Most already measured `fh->W` and re-centred,
    so they needed only the resolver call.  Three did not:
    - `player.cpp` stepped by a hardcoded `PromptXGap=20` (the 18px glyph plus
      2); now `getFrameWidth(icon)+PromptIconGap`, which reproduces the
      original spacing exactly for the glyphs.
    - `convo.h`'s `TEXTBOX_BUTTONS_GAP=20` became `TEXTBOX_BUTTONS_ICON_GAP=2`
      added to the measured up-cap width — measured whether or not the up hint
      is drawn, so the down arrow does not move as the page changes.
    - `shop.cpp` stepped the cross and triangle icons back by `fh2->W`, the
      *right arrow* header left over from the line above.  Invisible while
      every icon was 18px wide; wrong as soon as they are not.  Now `fh1->W`.

52. **`port/psyq/host/ini.cpp`, `port/psyq/host/args.cpp`** — `prompt_icons`
    (`SBSP_PROMPT_ICONS`) `auto|keys|pad`.  `auto` follows the device in use.
    In a scripted run (`--pad-file` / `--pad-script`) it follows the
    recording's `# prompt` marks instead - key caps when there are none -
    so a replay draws what the recording drew and a harness run draws the
    same on every machine, plugged-in pad or not (issue #58; the earlier
    advice to pin `keys` for a recording is obsolete, and pinning it for a
    replay only forces a difference from the recording, which is said once).

53. **`source/gfx/font.cpp`** (github issue #24) — every `fontTab[_char]`
    lookup now indexes through `(u8)`.  `fontTab` is a 256-entry table whose
    upper half is live: `0x91`/`0x92`, the Windows-1252 quotes the dialogue
    text really uses for its apostrophes, map to the `'` glyph, and
    `0xC0`-`0xFF` carry the accented EUR characters.  `_char` is a plain
    `char`, which is signed on x86, so `0x92` read `fontTab[-110]` - from
    *before* the table - and whatever junk sat there was used as a sprite
    frame number.  With a kind link layout that was a wide blank gap where the
    apostrophe should be (#24 as filed).  It is layout-dependent, though: the
    extra sprites of #43 moved the data, the junk became a frame whose header
    was garbage, and `getFrameHeader()` of it drew a screen-high slab sampling
    the framebuffer - the clear colour at first, then a column of grass -
    across every dialogue line containing an apostrophe.  Same latent-bug
    class as #13/#17/#19: harmless-by-luck on PS1, visibly broken on Win32,
    and it would have taken out every accented glyph once the EUR text came up.

54. **`build/mklevel.pl`, `makefile.gfx`** — the generated level rule now
    depends on `$(INC_DIR)/Sprites.h`, which gets a rule of its own (it is only
    a side effect of the `Sprites.Spr` rule).  `MkLevel` bakes the sprite frame
    numbers that header defines into the `.lvl`, but the rule named only the
    `.mex`, so a from-scratch build could run `MkLevel` before the header
    existed and an incremental build never rebuilt a level after the sprite
    bank changed.  Not the cause of #24's symptom, but the same family of
    missing dependency, found while chasing it.

55. **`port/art/keycaps/` (new), `port/tools/stretch_keycaps.py` (new),
    `Graphics/UI/+key*.bmp`** — the caps went in at the size they were cut,
    and a square cut is not a square cap.  The game draws into a 512x256
    frame that is scanned out into a 4:3 box (`vk/viewport.cpp`, a CRT before
    that), so a pixel on screen is 1.5 times taller than it is wide, and
    Climax's art is drawn in that space: `+butC.bmp` is 18x11 because a round
    circle button has to be 18 across to come out round at 18*2/3 = 12 by 11.
    The 14x14 caps came off a sheet drawn for square pixels, so in the window
    they read as narrow upright slabs - 9.3 across by 14 down.  It got through
    review because the screenshots were `--dump-frames` BMPs blown up 2x,
    which is to say at square pixels, where the caps look right and the PS1
    glyphs look stretched.  **Judge sprite art at the window's 4:3 shape, not
    at the dump's** - getting that out of the tooling rather than out of a
    reviewer's memory is github issue #53.  The artist's cuts now live in
    `port/art/keycaps/` and `stretch_keycaps.py` writes the `Graphics/UI`
    copies the data build eats, every column resampled to the largest even
    width no wider than 1.5x the master: 14 -> 20, 16 -> 24.  Even, because
    the resample is mirror-symmetric about the centre line, which is what
    keeps an up arrow pointing straight up; 21 is the exact 1.5x and cannot
    be symmetric with a 14px master, and
    of the two even neighbours 20 doubles six of the fourteen columns rather
    than eight, so the letter strokes stay nearer the master's weight.
    Nearest-neighbour only - nine colours on one shared CLUT leave no room for
    filtering to invent more.  `--check` re-derives and compares instead of
    writing.  The bank did not change size (the packer trims each frame and
    the pages had room), so `EXPECT_SPRITES_SIZE` stands.  Before and after at
    the window's shape: `docs/assets/issues/43-key-caps-aspect.png`.  Every
    draw site already measured `fh->W`, so no game source moved.

Covered by `port/tests/pad_test.cpp` (the cap table, the three modes, a
rebind following its key, and the fall back to the glyph for a key with no
cap art) and by the `playthrough` label on both toolchains.

**Still PS1-worded (deliberately not changed).**  `data/translations/text.dat`
says "Press the **X button** to continue" (the memory-card result screens) and
"PRESS **START**".  One string table serves both builds, so editing it would
make the PS1 build wrong; saying it correctly on each needs device-aware text
(a runtime substitution in the shim, or a second string set), which is a
larger change than an icon swap and is left for its own issue.

## Game-source changes (replay judging, issue #67)

Both inside `#if !defined(PSX_MIPS_ASM)` arms whose `#else` re-syncs the
PS1 build's `__LINE__` with `#line`, so `Spongey.cpe` is unchanged; see
"Replay judging (issue #67)" for the evidence.

56. **`source/game/game.cpp` (`CGameScene::render`), `source/system/asmport.h`** -
    at the end of `render()`, `Port_PauseMenuDrawn(...)` tells the shim, once
    per frame the Game scene renders, whether the pause menu is in it: the
    menu is active and the state is one whose render goes through
    `render_playing`, which draws it (every state but the boss intro and
    the lives screen on the way to it).  DEBUG draws that menu with one
    more line, so a replay on the other build type does not compare an
    epoch's display CRC while the menu is among the last three frames
    built (`host/input.cpp crcSkipped`).  At render rather than in
    `think_playing`: that is the frame the CRC will see, and `think_playing`
    can run three times in one frame (a teleport).  In `render()` rather
    than `render_playing()`: every Game frame reports, so on the lives
    screen or the boss intro the record ages out instead of standing still.

57. **`source/memcard/saveload.cpp` (`CSaveLoadDatabase::allocateBuffer`)** -
    `memset(m_tempBuffer,0,m_bufferSize)` after the `MemAlloc`.  The buffer
    is a whole 8 KB card block of which the save fills 0x331 bytes; the rest
    went to the card, and into its MD5, as stale heap bytes that differ
    between DEBUG, FINAL and x64.  Zeroed, every exe writes the same card.
    The PlayStation build keeps writing its heap (retail behaviour).

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
0 errors** - under MinGW g++ and under clang-cl alike (M8 PR 4; clang's
extra classes are listed there).  Keep it that way: a new warning now means
new code, not inherited noise.
