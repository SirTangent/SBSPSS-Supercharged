![Supercharged Logo](docs/assets/supercharged-logo.png)

# Spongebob Squarepants: SuperSponge [Supercharged]

The original PS1 game ported over to modern operating systems (Windows 11)

## What the heck is this project?

Back in 2001, SuperSponge was released for the original PlayStation (PS1). It was one of the first video games to be released under the franchise, followed by many more successful titles.

Many years later, the full source code by Climax Development was released on the internet with everything needed to compile the game. The main hurdle was the development environment required, as compilation could only be done on a Windows 98 VM. The other elephant in the room was the target platform. Being solely a PS1 project, the code was written to work with its hardware using the PSYQ SDK. In other words, the codebase would require major re-work with a new abstraction layer to operate on a modern OS (and system architecture)

This project substitutes the PSYQ SDK and its toolchain with a modern one, targeting modern hardware. It's similar to PsyCross, but with some deliberations. The project lives in the `port` directory with the build scripts needed to generate a Windows executable. The game itself behaves and acts like the real thing from the PlayStation. There is no emulation happening in the backend, just  the game running on today's hardware for today's operating system. Hope you enjoy my project!

# How to use?
In line with the overall project purpose, you can build the game executable using Windows 11.

## Prerequisites
| Requirement | Notes |
|---|---|
| Windows 10 or 11, 64-bit | The shipping game is a 32-bit executable; it runs fine on 64-bit Windows using WoW64 translation. (A 64-bit build exists too, section 4.) |
| Git with Git LFS | Install [Git for Windows](https://git-scm.com/install/windows), then `git lfs install` once. |
| MSYS2, installed at `C:\msys64` | Needed to substitute parts of the 1999 cygwin toolchain. Download from <https://www.msys2.org>. The default install path matters as `port/CMakePresets.json` hard-codes `C:/msys64/mingw32/bin/ninja.exe`. If you must install elsewhere, edit that line and set `MSYS2_WIN` (Windows path) before running the `.cmd` scripts. |
| *Optional:* LLVM + Visual Studio 2022/2026 C++ x64/x86 build tools | Only for the `clangcl-*` presets (section 4): [LLVM](https://releases.llvm.org) at `C:\Program Files\LLVM` (or the "C++ Clang tools for Windows" VS component) plus the MSVC x64/x86 build tools and a Windows 10/11 SDK. Everything else (SDL3, Vulkan headers) is fetched by CMake. |

Just a note, your GPU must support Vulkan. Most modern systems do.

### MSYS2 packages (MSYS2 Shell)

Open the **MSYS2 MSYS** shell and install the data-build tools:

```bash
pacman -S --needed make perl
```

Then install the 32-bit compiler toolchain (same shell is fine, these are
just packages):

```bash
pacman -S --needed mingw-w64-i686-gcc mingw-w64-i686-cmake mingw-w64-i686-ninja mingw-w64-i686-vulkan-headers
```

SDL3 needs special handling. MSYS2 is retiring its 32-bit environment and has
removed `mingw-w64-i686-sdl3` from the package index, but the package file is
still on the mirror. Install it by URL:

```bash
pacman -U https://repo.msys2.org/mingw/mingw32/mingw-w64-i686-sdl3-3.4.10-1-any.pkg.tar.zst
```

## 2. Get the source (Powershell)

**IMPORTANT:** At this point, you can use your default terminal. The correct version of git won't be in MSYS2 shell.

```bat
git clone https://github.com/SirTangent/SBSPSS-Supercharged.git
cd SBSPSS-Modernized
git lfs pull
```

`git lfs pull` is not optional. Without it `Track1.Ixa` is a few-hundred-byte
pointer file and the data build stops with "unmaterialised Git-LFS pointer".

The Windows port lives on the `SBSP-Win11` branch until it lands on `master`:

```bat
git checkout SBSP-Win11
```

## 3. Build the game data

The game reads one big file, `BIGLUMP.BIN`, plus a set of generated headers
the code build cannot start without. Both come from the original 1999 asset
pipeline, run under MSYS2. From the repository root, in a normal Command
Prompt or PowerShell:

```bat
port\build-data.cmd
```

This runs `makefile.gfx` through the vintage converter executables for every
level, actor, sprite sheet, script and translation, then stages the speech
track and the four FMV movies beside the result. Expect it to take a few
minutes the first time; later runs only rebuild what changed. Output:

```
out\USA\DEBUG\version\CD\BIGLUMP.BIN
out\USA\DEBUG\version\CD\TRACK1.IXA
out\USA\DEBUG\version\CD\THQ.STR  CLIMAX.STR  INTRO.STR  DEMO.STR
out\USA\include\BigLump.h  Sprites.h  trans.h  ...
```

The script takes optional `TERRITORY VERSION` arguments (defaults `USA
DEBUG`). Only USA data is needed for the PC build. If you intend to run the
`final` variant of the game, see the note in section 5.

## 4. Build the game

```bat
port\build-pc.cmd debug
```

The argument is `debug`, `final`, or `all` (both). The script puts the MinGW
toolchain on the path, configures with the matching CMake preset and builds
with Ninja. A clean build takes a few minutes. Output:

| Variant | Executable | What it is |
|---|---|---|
| `debug` | `port\build\debug\sbsp.exe` | Asserts on, debug overlays and screen tools available, prim-pool overflow detection. Use this one while developing. |
| `final` | `port\build\final\sbsp.exe` | The shipping configuration, heavier optimisation, asserts compiled out. |

The same directories also contain `sbsp_headless.exe` and the fifteen
`*_test.exe` unit-test executables. `port\build-pc.cmd test` builds and then
runs them all (ctest label `unit`) followed by the automated playthrough
tiers (label `playthrough`: a scripted, faster-than-real-time run through
every scene and every level); CI does the same on each pull request.

If configure fails with "Generated headers missing", section 3 was skipped or
failed.

### Debugging in Visual Studio (optional)

The MinGW executables carry DWARF debug info, which gdb reads and Visual
Studio does not. Set `SBSP_CODEVIEW=1` before building to get CodeView
instead, with a `.pdb` beside every executable:

```bat
set SBSP_CODEVIEW=1
port\build-pc.cmd debug
```

Then *File > Open > Project/Solution* on `port\build\debug\sbsp.exe` in
Visual Studio (or open it in WinDbg) and you have source, breakpoints and
locals. The same flag is a CMake option, `-DSBSP_CODEVIEW=ON`. Building
with it prints a few harmless `undefined reference ... (.debug$S)` lines at
link time (a GCC CodeView quirk with one function-local static); the PDB is
fine.

### Building with clang-cl (optional)

The port also builds with LLVM's `clang-cl` against the MSVC C runtime and
Windows SDK (32-bit, `i686-pc-windows-msvc`), the toolchain Visual Studio
debugs natively and the one that does not depend on MSYS2's shrinking
32-bit package repository:

```bat
port\build-pc.cmd clangcl-debug
```

Output is `port\build\clangcl-debug\` with the same executables, each
with its `.pdb` and an `SDL3.dll` beside it (the official SDL VC package
has no static library). The first configure downloads SDL3 and the Vulkan
headers into `port\build\deps\`. `port\build-pc.cmd test clangcl` runs
the same tests. The MinGW executables remain the ones that ship; CI builds
the clang-cl variant as an advisory job.

The same toolchain also builds a true 64-bit executable
(`x86_64-pc-windows-msvc`; it needs the MSVC x64 build tools):

```bat
port\build-pc.cmd clangcl-x64-debug
```

`clangcl64` names the debug + final pair, for `test` and `soak` too. The
game data is unchanged - the 4-byte pointer fields in the level and actor
files are read through a 4-byte pointer type (`port/docs/conv_pc.md`,
"Game-source changes (M9)"), and the 64-bit exe renders the same frames as
the 32-bit one.

## 5. Run

Run from the **repository root**, because the game resolves its data
directory relative to the working directory:

```bat
port\build\debug\sbsp.exe
```

A console window opens alongside the game window; it carries the shim's log
(stub warnings, boot-level messages, pacing diagnostics). The game window is
resizable and letterboxes the PS1 output to 4:3. Close the window to quit;
there is no way to exit from the game's UI because the PlayStation game never had one.

**Data location.** The game looks for `BIGLUMP.BIN` in, in order: the
directory named by `--data-dir` / `SBSP_DATA_DIR` (taken as is), `data\`
beside the exe, `out\<territory>\cd\` (what `port\build-data.cmd usa` or
`eur` stages - one data build serves the `debug` and `final` executables
alike, since the two are byte-identical), and finally the PSX build tree
`out\<territory>\<version>\version\CD\`. The console's `[cd] data:` line
says which one it took.

**Saves and settings.** The memory card is a real 128 KB PS1 card image,
`card0.mcd`, kept in `%APPDATA%\SBSPSS\` - or in a `saves\` folder beside
the exe if one exists (the tester zip's portable layout), or wherever
`--save-dir` points. Unlike the retail game, the port loads it at boot so
your slots are populated without visiting Options. Settings live in
`sbsp.ini` **beside the executable**, written with commented defaults on
the first run: window size
or `fullscreen`, `scale=fit|integer|stretch`, `vsync`, audio device /
buffer / volume, the keyboard bindings, gamepad dead zone and rumble,
pause-on-focus-loss, language, and the data and save directories.
Precedence is
command-line argument > `SBSP_*` environment variable > ini. (Only English
text exists in the game data, so `language=` loads the same strings whatever
it says - real localization is issue #37.)

**Tester zip.** `port\package.cmd [--territory usa|eur]` (or `python port\package.py`) bundles the
FINAL and DEBUG executables, the data, a `saves\` folder, a README and
`run-test-session.cmd` (a recorded, logged play session) into
`port\build\sbsp-<territory>-<date>.zip`. Add `--x64` (USA, after
`port\build-pc.cmd clangcl64`) to include the 64-bit executables as
`sbsp64.exe` / `sbsp64-debug.exe` with their `SDL3.dll`; they share the data,
settings and memory card with the 32-bit ones, and
`run-test-session.cmd x64` records a session on them.

**Skipping to a level** while testing:

```bat
port\build\debug\sbsp.exe --level 1-1
```

The argument is `chapter-level` (chapters 1 to 5, levels 1 to 5, where level 5 is that chapter's bonus level) or a raw level-table index from 0 to 24. Run with
`--help` for the full list of options, or see the table in
[ARCHITECTURE.md](ARCHITECTURE.md#55-command-line-and-environment). Every
option also has an `SBSP_*` environment-variable form.

## 6. Controls

Player one is a DualShock as far as the game is concerned. The first gamepad
SDL recognises is used if present, with rumble; the keyboard always works.

| PS1 button | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrow keys | D-pad (analog sticks are also reported) |
| Cross | `Z` | South (A on Xbox layout) |
| Circle | `X` | East (B) |
| Square | `A` | West (X) |
| Triangle | `S` | North (Y) |
| Start | `Enter` | Start |
| Select | `Right Shift` | Back / View |
| L1 / R1 | `Q` / `W` | Left / right shoulder |
| L2 / R2 | `E` / `R` | Left / right trigger |

`Alt+Enter` toggles borderless fullscreen, and the game pauses (audio
included) while another window has the focus. The keyboard bindings are
the `key_<button>=` lines of `sbsp.ini` (SDL key names such as `Space`,
`Right Shift`, `Keypad 0`); `Alt` is reserved for host shortcuts and never
reaches the game.

What each button does in the game is configurable from the in-game Options
menu, exactly as on the console.

**Debug build only:** two development tools from the original code are kept
alive on PC. Select (`Right Shift`) opens the VRAM viewer, and L2 + Start
(`E` + `Enter`) writes a screenshot. If a menu seems to have vanished into a
grid of textures, you pressed Select; press it again.

# Frequently Asked Questions (Probably...)

### Can't you just emulate the game anyway?
Yes, there is nothing really stopping you from compiling for the PS1 and emulating it. However, I believe there are benefits from stripping away the translation layer and applying optimizations that come from modern platforms. It also extends the game to run on less powerful (not that a PS1 game is power hungry) hardware.

### Why not use PsyCross?
I guess there isn't a reason not to, but personally, I wanted to see what it took to port a game over from scratch with minimal dependencies. I'm aware of its capabilities, and its success with porting over titles such as Driver 2. My long-term goal is to re-design some of the mechanisms that will diverge from the translation layer completely.

### How dare you use AI for coding slop?
If it's not obvious, the project used generative AI and agentic coding for a large chunk of the work. I do acknowlege that similar projects want to avoid AI for good reasons, which I can respect.

Realisticaly, these projects have large scopes as a result of how coupled the game logic is with PSYQ. From what I read, it took almost two years to port Driver 2 using Psy-Cross, which I understood had to be reverse engineered. From the get-go, porting a game can require an extensive overhaul of the codebase and therefore a lot of man hours.

Given this was a solo project and that I have a full-time software engineering job (yes, we use AI there too), using it responsibly really cut down on the amount of time needed to get to an MVP. I still had input on some of the high-level design decisions and try to ensure the software has adequate testing. I do acknowlege there are inherit risks with quality and technical-debt build-up. At the end of the day, you still need to posses some understanding of the technical details and steer these models correctly.

> "If you can’t beat them, join them”

### Plan to support other platforms?

Yes, Definately! Right now, the MVP is to get it ported for Windows 11. In addition, it only compiles as a 32-bit application (Using WoW64188) and have it in-scope to refactor it to target x86-64. Once I finish the Win11 milestone, I can start to work on other ports. Here are some on the to-do list.

* MacOS
* Linux
* Android
* WebAssembly

btw, yes I could have used a cross-platform framework.

### Do you welcome contributors?

Sure, why not. I need to create a `CONTRIBUTING.md`, but feel free to open some PRs. Also, AI generated code and content is allowed, but I will still have to review any changes going in regardless. Please use it responsibly.