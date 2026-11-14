![Supercharged Logo](docs/assets/supercharged-logo.png)

# Spongebob Squarepants: SuperSponge [Supercharged]

The original PS1 game ported over to modern operating systems (Windows 11)

## What the heck is this project?

Back in 2001, SuperSponge was released for the original PlayStation (PS1). It was one of the first video games to be released under the franchise, followed by many more successful titles.

Many years later, the full source code by Climax Development was released on the internet with everything needed to compile the game. The main hurdle was the development environment required, as compilation could only be done on a Windows 98 VM. The other elephant in the room was the target platform. Being solely a PS1 project, the code was written to work with its hardware using the PSYQ SDK. In other words, the codebase would require major re-work with a new abstraction layer to operate on a modern OS (and system architecture)

This project substitutes the PSYQ SDK and its toolchain with a modern one, targeting modern hardware. It's similar to PsyCross, but with some deliberations. The project lives in the `port` directory with the build scripts needed to generate a Windows executable. The game itself behaves and acts like the real thing from the PlayStation. There is no emulation happening in the backend, just  the game running on today's hardware for today's operating system. Hope you enjoy my project!

# How to use?
In line with the overall project purpose, you can build the game executable using Windows 11.

## 1. Prerequisites
| Requirement | Notes |
|---|---|
| Windows 10 or 11, 64-bit | The game is a 64-bit executable. |
| Git with Git LFS | Install [Git for Windows](https://git-scm.com/install/windows), then `git lfs install` once. |
| Python 3.8 or newer | From <https://www.python.org> (or its Python install manager), on the `PATH`. It runs the build scripts, the data build's text steps and the automated playthrough tests. |
| Visual Studio 2022 (Community or Build Tools) | The *Desktop development with C++* workload with its MSVC x64/x86 build tools, a Windows 10/11 SDK and the *C++ CMake tools for Windows* component. The build scripts take CMake and Ninja from it (or from the `PATH`, when both are there). |
| LLVM | [LLVM](https://releases.llvm.org) at `C:\Program Files\LLVM`, or Visual Studio's *C++ Clang tools for Windows* component: the game builds with `clang-cl`. Everything else (SDL3, Vulkan headers) is fetched by CMake. |
| *Optional:* MSYS2, installed at `C:\msys64` | Only for the PlayStation build (`port\build-psx.cmd`, section 4), until issue #98 moves it off MSYS2. Download from <https://www.msys2.org>, then in the **MSYS2 MSYS** shell: `pacman -S --needed make perl`. To install elsewhere, set `MSYS2_WIN` (Windows path). |

Just a note, your GPU must support Vulkan. Most modern systems do.

The `.cmd` scripts run their Python script under `py -3`, or `python` when
there is no launcher. The game's build runs its playthrough tests under the
same Python. To build without those tests, set `SBSP_PLAYTHROUGH=OFF` in the
environment before running the build script (section 4):

```bat
set SBSP_PLAYTHROUGH=OFF
port\build-pc.cmd test usa
```

In PowerShell that is `$env:SBSP_PLAYTHROUGH = "OFF"`. `test` then runs the
unit tests and skips the playthrough tests with a note. Each build directory
remembers the setting until the script runs again with
`SBSP_PLAYTHROUGH=ON`. Configuring by hand instead, the switch is
`cmake --preset debug -DSBSP_PLAYTHROUGH=OFF`, run in `port\` from a
*Developer PowerShell for VS 2022* (which puts CMake and Ninja on the
`PATH`).

## 2. Get the source (Powershell)

**IMPORTANT:** At this point, you can use your default terminal. The correct version of git won't be in MSYS2 shell.

```bat
git clone https://github.com/SirTangent/SBSPSS-Supercharged.git
cd SBSPSS-Supercharged
git checkout SBSP-Win11
git lfs pull
```

The Windows port lives on the `SBSP-Win11` branch until it lands on
`master`, so check it out before pulling the LFS files.

`git lfs pull` is not optional. Without it `Track1.Ixa` is a few-hundred-byte
pointer file and the data build stops with "unmaterialised Git-LFS pointer".

## 3. Build the game data

The game reads one big file, `BIGLUMP.BIN`, plus a set of generated headers
the code build cannot start without. Both come from the original asset
pipeline's converters, rebuilt as 64-bit Windows tools (`port\tools\x64`).
A CMake + Ninja project in `port\data` runs them. From the repository root,
in a normal Command Prompt or PowerShell:

```bat
port\build-data.cmd usa
```

This converts every level, actor, sprite sheet, script and translation, then
stages the speech track and the four FMV movies beside the result. It takes
about a minute the first time; later runs only rebuild what changed. Output:

```
out\USA\cd\BIGLUMP.BIN
out\USA\cd\TRACK1.IXA
out\USA\cd\THQ.STR  CLIMAX.STR  INTRO.STR  DEMO.STR
out\USA\include\BigLump.h  Sprites.h  trans.h  ...
```

The argument is the territory, `usa` (the default) or `eur`. The data is per
territory only: one USA data build serves both the `debug` and `final`
executables. EUR data (`port\build-data.cmd eur`) is only needed for the
`eur-*` builds.

The `.cmd` scripts work from cmd, PowerShell and Git Bash. `build-data.cmd`
and `build-pc.cmd` are Python (`port\tools\data\build_data.py`,
`port\tools\build_pc.py`) and need no MSYS2. `build-psx.cmd` still starts
MSYS2's bash for the 1999 makefiles (issue #98).

## 4. Build the game

```bat
port\build-pc.cmd debug
```

The argument is `debug`, `final`, or `usa` (both). The script configures
with the matching CMake preset and builds with Ninja, using LLVM's `clang-cl`
against the MSVC C runtime and Windows SDK, for 64-bit Windows
(`x86_64-pc-windows-msvc`). A clean build takes a few minutes; the first
configure also downloads SDL3 and the Vulkan headers into
`port\build\deps\`. `eur-debug`, `eur-final` and `eur` are the EUR builds
and need EUR data; `all`, which is also what no argument means, builds all
four and so needs both territories' data. Output:

| Variant | Executable | What it is |
|---|---|---|
| `debug` | `port\build\debug\sbsp.exe` | Asserts on, debug overlays and screen tools available, prim-pool overflow detection. Use this one while developing. |
| `final` | `port\build\final\sbsp.exe` | The shipping configuration: asserts compiled out. Both variants use the same optimisation level (`/O2`). |

Each executable has its `.pdb` and an `SDL3.dll` beside it (the official SDL
VC package has no static library). The same directories also contain
`sbsp_headless.exe` and the unit-test executables. The recommended command
is

```bat
port\build-pc.cmd test usa
```

which builds both USA variants and then runs those executables (ctest label
`unit`) followed by the automated playthrough tiers (label `playthrough`: a
scripted, faster-than-real-time run through every scene and every level) on
each.

If configure fails with "Generated headers missing", section 3 was skipped or
failed.

### Debugging in Visual Studio (optional)

*File > Open > Project/Solution* on `port\build\debug\sbsp.exe` in Visual
Studio (or open it in WinDbg) and you have source, breakpoints and locals
from the `.pdb` beside it.

### 32-bit builds (optional)

The same code also builds as a 32-bit executable (`i686-pc-windows-msvc`):

```bat
port\build-pc.cmd x86-debug
```

`x86` names the debug + final pair, for `test` and `soak` too, and the EUR
trees are `x86-eur-debug` and `x86-eur-final` (`port\build-pc.cmd x86 eur`).
They don't ship; they exist to check the 64-bit build against.
`port\build-pc.cmd parity64 [usa|eur] [debug|final]` plays every route and
level on both and requires the same frames, and the 64-bit exe to replay the
32-bit exe's recordings to the same memory card. The game data is the same
for both: the 4-byte pointer fields in the level and actor files are read
through a 4-byte pointer type (`port/docs/conv_pc.md`, "Game-source changes
(M9)").

The port built with MinGW g++ until issue #39. Those presets used the names
`debug`, `final`, `eur-debug` and `eur-final`, and `build-pc.cmd`
reconfigures such a tree from scratch the first time. The old `clangcl*`
words are refused with their new names.

### Continuous integration

Two GitHub Actions workflows share one set of jobs
(`.github/workflows/build.yml`):

- **PR check** (`pr.yml`) runs on every pull request into `SBSP-Win11`
  that changes more than documentation (`*.md`, `Dox/`, `port/docs/`). It
  builds the USA data, then builds and tests the `debug` and `final` trees,
  and links the PlayStation executable.
- **Build & test (release)** (`build.yml`) runs everything, listed below.
  Nobody's push or pull request starts it: run it before a release from the
  *Actions* tab (*Build & test (release)*, *Run workflow*) or with
  `gh workflow run build.yml --ref SBSP-Win11`. A release run on
  `SBSP-Win11` also leaves the LFS cache that pull requests restore, so
  they need not each fetch the 133 MB `Track1.Ixa`.

The same builds and tests run locally, so check a pull request with them
before opening it:

```bat
port\build-pc.cmd test usa
port\build-pc.cmd test x86
python port\tools\stretch_keycaps.py --check
```

and, if the change touches the game sources, the PlayStation link:
`port\build-psx.cmd`. The PlayStation executable must also stay
byte-identical: `python port\tools\psx_identity.py lines` checks that in
seconds, and `python port\tools\psx_identity.py build` proves it with clean
builds (see `port/docs/psx_byte_identity.md`). A release run builds the USA data, checks the key-cap
bitmaps against their masters, builds and tests the `debug`, `final`,
`x86-debug` and `x86-final` trees, and links the PlayStation executable. It
also builds the EUR data (`data-eur`) and builds and tests the `eur-debug`
and `eur-final` trees on it (`pc-eur`, issue #42); locally that is
`port\build-data.cmd eur` and `port\build-pc.cmd test eur`.

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
buffer / volume, the keyboard bindings, `prompt_icons=auto|keys|pad`
(which icons the button prompts show), gamepad dead zone and rumble,
pause-on-focus-loss, language, and the data and save directories.
Precedence is
command-line argument > `SBSP_*` environment variable > ini. (Only English
text exists in the game data, so `language=` loads the same strings whatever
it says - real localization is issue #37.)

**Tester zip.** `port\package.cmd [--territory usa|eur]` (or `python port\package.py`) bundles the
FINAL and DEBUG executables (`sbsp.exe`, `sbsp-debug.exe`) with their
`SDL3.dll`, the data, a `saves\` folder, a README and `run-test-session.cmd`
(a recorded, logged play session) into
`port\build\sbsp-<territory>-<date>.zip`. `--build` runs
`port\build-data.cmd` and `port\build-pc.cmd` for the territory first.

**Skipping to a level** while testing:

```bat
port\build\debug\sbsp.exe --level 1-1
```

The argument is `chapter-level` (chapters 1 to 5, levels 1 to 5, where level 5 is that chapter's bonus level) or a raw level-table index from 0 to 24. Run with
`--help` for the full list of options, or see the table in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Every
option also has an `SBSP_*` environment-variable form.

**Frame dumps.** `--dump-frames 300,600` writes `sbsp_frame_300.bmp` and
`sbsp_frame_600.bmp` at those vblanks (`--dump-dir` says where). Each is the
framebuffer pixel for pixel, which is what frame comparisons want, but the
PS1 shows its 512x256 frame on a 4:3 screen, so every pixel is 2 wide by 3
tall and the BMP shows the art 1.5 times too wide. Add `--dump-aspect` to
also write `sbsp_frame_<n>_4x3.bmp` at the window's shape (1024x768 for the
game's frame), and **judge sprite art from that file, not from the raw
dump**: the circle button glyph has to look round.

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

The keyboard key-cap icons the PC build shows in its button prompts are by
[Blubrry](https://itsblubrry.itch.io/basic-keyboard-controller-glyphs), used
with permission - see [CREDITS.md](CREDITS.md).

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

Yes, Definately! Right now, the MVP is to get it ported for Windows 11. It builds as a native 64-bit (x86-64) application with clang-cl, and as a 32-bit one for comparison (section 4). Once I finish the Win11 milestone, I can start to work on other ports. Here are some on the to-do list.

* MacOS
* Linux
* Android
* WebAssembly

btw, yes I could have used a cross-platform framework.

### Do you welcome contributors?

Sure, why not. I need to create a `CONTRIBUTING.md`, but feel free to open some PRs. Also, AI generated code and content is allowed, but I will still have to review any changes going in regardless. Please use it responsibly.