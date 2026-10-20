# Chibi-Robo! PC — native PC port by static recompilation

A native Windows port of *Chibi-Robo!* (Nintendo GameCube, 2005/2006). The game's PowerPC code is statically
recompiled to C, and every Nintendo SDK function that talks to the console hardware is replaced by a native PC
implementation. **Nothing is emulated at the hardware level any more**: no hardware registers, no cycle-driven
devices, no interpreter. The long-term goal is a fully decompiled, readable C++ game, then multiplayer, a remaster
and a modding platform.

**This repository contains no game data.** You must provide a disc image of your own copy of the game. The tools
extract, recompile and build everything locally from that image.

Project status and plans: **[ROADMAP.md](ROADMAP.md)**.

## Current state (October 2026)

- Runs on Windows from boot through the title screen, a new game, the 3D intro and the first party scene, with
  music and sound effects, in real time.
- Native saves, like a PC game: a memory card is always inserted; save files (`.gci`, Dolphin-compatible) live in
  `%USERPROFILE%\Saved Games\Chibi Robo PC\Chibi Robo\`.
- Configurable keyboard and gamepads (`controls.ini`, see below).
- Validated automatically by the project against native reference runs (images, audio, saves), for both the PAL
  and the US versions. The bench references and the regression tests live in a local `tests/` folder that is not
  published (it holds output of the game), so these checks cannot be rerun as is from this repository;
  `tools/ab.py record` records references from your own build.
- Compared with Dolphin only in part: in PAL, the five boot screens up to the title screen and the title music;
  in the US version, only the health and safety warning screen. No audio measurement against Dolphin yet for the
  US version.
- Not yet verified: playing through the whole game (milestone 8 of the roadmap).

## Supported versions

| ID | Region | Revision | SHA-1 of the .iso image |
|---|---|---|---|
| GGTP01 | PAL (Europe) | 0 | `f321aa7fb1ae5101ec60f1bbca8184898142135a` |
| GGTE01 | NTSC-U | 0 | `9052f186411b49845323eaaeb014755bcf96321b` |

Accepted image formats: `.iso` / `.gcm` directly; `.rvz`, `.gcz`, `.wia`, `.ciso` after automatic conversion with
DolphinTool.

## Requirements (Windows)

Install yourself: Git, Python 3.11+ (the `py` launcher), CMake 3.25+, and Visual Studio 2022 with the
"Desktop development with C++" workload (Windows SDK and runtime libraries).

Optional: Dolphin. It is only used to compare the port with the original behavior (`tools/dolphin_ref.py`), and
its DolphinTool only to convert disc images that are not plain `.iso` / `.gcm` files. `tools/dolphin_ref.py` runs
Dolphin on a separate, never-shown Windows desktop, so a reference run never takes the foreground or your keystrokes.

Installed automatically into `external/` by `tools/bootstrap.py`: clang/LLVM, Ninja, Ghidra, a JDK 21, the
GameCube loader for Ghidra, DolRecomp and Aurora.

## Getting started

```
py -3 tools/bootstrap.py      # installs the missing tools into external/
py -3 tools/doctor.py         # checks the environment
```

`tools/doctor.py` fails if a required tool is missing. A missing Dolphin or DolphinTool only gives a warning:
you can build and play without them.

If Dolphin is not on the `PATH`, create `config/local.toml`:

```toml
[paths]
dolphin = 'C:\path\to\Dolphin.exe'
```

Copy your disc image into `orig/`, then extract and build:

```
py -3 tools/extract.py chibi-robo                    # PAL by default
py -3 tools/extract.py chibi-robo --version GGTE01
py -3 tools/build.py chibi-robo                      # -> build/chibi-robo-GGTP01-Release/bin/chibi-robo.exe
py -3 tools/build.py chibi-robo --version GGTE01
```

`tools/gcextract/` is a native, dependency-free version of the extraction (C++, for the player installer, which
cannot rely on Python). It gives the same `disc/` tree, byte for byte, and refuses images that are not listed in
`game.toml`:

```
py -3 tools/gcextract/build.py                       # -> build/gcextract/gcextract.exe
build\gcextract\gcextract.exe <image.iso> <output folder> --game-toml games/chibi-robo/game.toml --u8 files/qp.bin
```

Only uncompressed `.iso` / `.gcm` images are read. Exit codes: 2 usage, 3 read error, 4 not a GameCube image, 5 image
not listed in `game.toml`, 6 inconsistent data or unsafe file name in the image, 7 write error or existing output
(nothing is ever deleted: after a failure, remove the output folder before retrying).

### Installer

`tools/installer/` builds a Windows installer for players (Inno Setup 6, free: `winget install
JRSoftware.InnoSetup`). It contains the game executable of each version, its DLLs, the Visual C++ runtime DLLs,
`gcextract.exe`, `game.toml` and `THIRD_PARTY_LICENSES.txt`, and no game data. The game executable contains this
project's runtime (GPL-3.0), third-party code (listed with its licences in `THIRD_PARTY_LICENSES.txt`) and the
recompiled game code, which is derived from the original game and is not licensed by this project:

```
py -3 tools/gcextract/build.py
py -3 tools/installer/build.py chibi-robo     # -> build/installer/chibi-robo-pc-setup-<commit>.exe
```

By default it takes `build/chibi-robo-<ID>-Release` for every version that is built (`--build ID=DIR` to choose).
One installer holds both versions; the player's image selects which executable is installed. `--no-disc-arg` makes
the shortcuts start the game without `--disc` (executables that find `disc/` next to themselves). Neither
`dxcompiler.dll` nor `dxil.dll` is needed or shipped: Direct3D 12 compiles its shaders with FXC (Aurora patch 0006,
see "Credits and third-party software"). That was checked on the development machine only; a PC where `d3dcompiler_47.dll`
is missing, or without a GPU, has not been tested.
`tools/installer/notices.py` regenerates `THIRD_PARTY_LICENSES.txt` from the real licence files (the build checks
that the committed copy is up to date).

What the player does: run the installer, choose the installation folder (per user, no administrator rights; a new or
empty folder, or an earlier installation of this installer: a folder that holds other files is refused, so that
nothing of the player's is overwritten or later uninstalled), choose their own `.iso` / `.gcm` dump. The image is checked (SHA-1 against `game.toml`); compressed formats (RVZ, WIA,
GCZ, CISO, NKit), modified or unknown dumps are refused with an explanation. The image is copied into the
installation folder as `game.iso`, `sys/` is extracted next to it, and a Start menu shortcut is created
(desktop shortcut optional). Uninstalling removes only the files the installer put there and the game's GPU cache
(a folder that still holds other files is kept): saves and settings stay in
`%APPDATA%\Chibi Robo PC\Chibi Robo`. Running the installer again over an existing installation updates it (the
image already installed is offered and not copied again); if the new image or extraction fails, the previous
installation is kept as it was. Silent install:
`setup.exe /SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=<folder> /IMAGE=<image.iso>` (exit code 0 on success,
1 if the image or the folder is refused, 7 if the image could not be extracted, 9 if its copy could not be checked;
a previous installation is kept in all these cases).

The build recompiles the game with DolRecomp, then compiles it with clang-cl. The first build in each build folder
(`build/<game>-<ID>-<config>`) needs an internet connection: CMake downloads Aurora's dependencies into it (prebuilt
Dawn and SDL3 packages and ten source libraries); later builds in the same folder reuse them.

Run the game by launching the executable directly. `chibi-robo.exe --help` lists the diagnostic options;
`tools/run.py` runs it for automated tests (isolated user folder, audio recorded to a WAV file instead of the
speakers).

### Setup menu

A direct launch opens a small setup menu first: window size, fullscreen, render scale and graphics backend (Enter or "Start game"
starts the game; launches by the project's tools never show it; `GCRT_SETUP_MENU=0` disables it, `=1` forces
it). The choices are
saved in `game_config.ini` in the same folder as `controls.ini` (see below). Tick "Skip this menu next time"
to start the game directly; hold Shift while the game starts to show the menu again, or set `skipMenu=0` in
`game_config.ini`. Values outside the menu's choices fall back to the defaults.

The game no longer opens a console window by itself. "Show console" (checked by default) opens one with the game's
log when the game starts, never while the menu is up; the output written before that moment is shown in it. If the
game was launched from a terminal, it prints in that terminal instead of a new window (Windows does not wait for it,
so the output follows the prompt). The choice is saved as `console=1` or `console=0` in
`game_config.ini`; with "Skip this menu next time", the saved value applies. `--console` and `--no-console` decide
for one launch only: the box starts checked or unchecked and the file is changed only if you toggle the box. When the
output is redirected (to a file or a pipe, as `tools/run.py` and the test benches do), no console is ever opened.

**Graphics backend.** The menu's "Graphics backend" list offers Auto (the default), Direct3D 12 and Vulkan, saved as
`backend=auto|d3d12|vulkan` in `game_config.ini`. The window is already open when the menu shows, so a change applies
the next time the game starts. `--backend d3d12|vulkan|auto` or `GCRT_BACKEND` choose for one launch only (not saved).
Auto uses Direct3D 12, with Vulkan as the alternative. Direct3D 12 compiles its shaders with FXC
(`d3dcompiler_47.dll`, taken from the Windows system folder; present on the development machine, not established on
every Windows), so no extra DLL is needed next to the executable. If the backend asked for cannot start, Aurora goes on
by itself with its own order (Direct3D 12, Vulkan, then its last resorts Direct3D 11 and null), and the game says so on
stderr and in the trace. A PC without a usable GPU (software adapter only) has not been tested. An unknown name falls
back to Auto with a message. On the development machine, Direct3D 12 and Vulkan give the same picture up to tiny
rounding differences: the title-boot captures of both versions are identical, and in some 3D scenes Vulkan differs by
1 or 2 colour levels on at most 0.05 % of the pixels. Direct3D 11 is not offered: Dawn starts it, but its picture
differs on the US version. The trace line `graphics backend: ... in use, requested ...` and crash reports name the
backend actually used.

### Controls

On first launch the game writes a commented `controls.ini` into
`%APPDATA%\Chibi Robo PC\Chibi Robo\`. Each GameCube button can have several keys or gamepad inputs, and
dead zones are adjustable. Keys are named by their physical position (US QWERTY layout), so the defaults work
on AZERTY keyboards too. Default keyboard layout (QWERTY names):

| GameCube | Key | | GameCube | Key |
|---|---|---|---|---|
| Control stick | Arrow keys | | A / B | X / Z |
| C-stick | I / J / K / L | | X / Y / Z | C / S / D |
| D-pad | T / F / G / H | | L / R (digital) | Q / W |
| Start | Enter | | L / R (analog) | A / E |

Gamepads use Aurora's mappings per controller family (Xbox, PlayStation, Switch, GameCube adapters).

### Optional: profile-guided optimization

`py -3 tools/pgo.py chibi-robo train --version GGTP01` builds an instrumented game, replays the training
scenarios and writes a local profile into `build/pgo/`. Later builds use it automatically while it matches the
code (about 20 % faster). Profiles are never committed.

### Crash reports

If the game crashes, it writes a report into the `crashes` folder of the user folder
(`%APPDATA%\Chibi Robo PC\Chibi Robo\crashes\`), or into `%TEMP%\gcrt-crashes` if the crash happens before that
folder is known. Only the 5 most recent reports are kept. A report has three files:

- `crash-<date>-<time>-<pid>.txt`: a text summary (exception, registers, call stacks of every thread, executable,
  processor and its microcode revision, firmware version, graphics adapter and driver, memory use, the last game
  events and log lines, the command line, the `GCRT_*` environment variables, and whether the same crash already
  happened on this computer). It names executables and libraries by their file name only, shortens every absolute
  path it prints to its last part (`<path>\game.iso`; the text between two paths on the same log line may be cut as
  well), and replaces your user-profile folder and user name with `%USERPROFILE%` and `%USERNAME%`. **This is the
  only file you may share**, for example in a public issue, but **read it before you publish it**: a file or folder
  name of your own (disc image, mods, installation folder) can still appear, as can anything you typed in the command
  line or in a `GCRT_*` variable.
- `crash-<date>-<time>-<pid>.dmp` (minidump) and `crash-<date>-<time>-<pid>-guest-ram.bin` (game memory): they
  contain game data and local paths. **Never publish them**; if a developer asks for them, send them privately.

If the game loop stops progressing for 60 seconds (a freeze), a `hang-<date>-<time>-<pid>.txt` report is written in
the same folder, followed by a `.dmp` if the game is still stopped by then. The game is not killed, but it is paused
while the report copies the state of its threads (at most a few seconds) and while the `.dmp` is written.
`GCRT_WATCHDOG=<seconds>` changes the delay (`0` turns off both watchdogs). The start-up (window, setup menu, game
boot up to its first frame) is watched too, with `GCRT_WATCHDOG_STARTUP=<seconds>` (default: `GCRT_WATCHDOG`, at
least 60; `0` turns it off); a start-up report is a `.txt` only, never a `.dmp`.

`GCRT_CRASH_DIR` chooses another folder. For developers, `tools/crash_inspect.py <report.dmp>` decodes the faulting
instruction of a minidump, checks whether it explains the fault and lists the memory regions, the threads and the
last game events stored in the dump (`--help` lists its options, verdicts and exit codes).

### Starting in a chosen stage (testing)

To reproduce a report somewhere in the game, `tools/run.py --stage <name|number>` starts in a chosen stage (both
versions). When the game loads a save, the stage stored in the save is replaced once; the rest of the save
(counters, story progress) is restored as usual, and later stage changes work normally. A save must be loaded,
for example:

```
py -3 tools/run.py chibi-robo --version GGTE01 --stage backyard --user-seed FOLDER_WITH_A_SAVE --scenario FILE.scn
```

`--stage debug` opens the developers' debug scene: walk up to a character and press A for a menu of story points.
Chibi-Robo starts at the stage's own starting point (checked on the backyard and the debug scene). If no save is
restored during the run, `--stage` has no effect and `tools/run.py` prints a warning. Stage names and addresses are
in `games/chibi-robo/game.toml`
(`[stage]`). Without `--stage`, nothing changes.

## Repository layout

- `runtime/core/` — generic GameCube runtime, reusable for other games: native replacements of the SDK
  (OS, DVD, VI, PAD, ARAM, CARD, GX through Aurora, DSP/AI/AX audio), CPU support for the recompiled code,
  platform layer (window, input, audio output, user folders).
- `games/chibi-robo/` — everything specific to Chibi-Robo!: game description (`game.toml`), recompiler
  configuration and hooks, symbol maps, reference scenarios.
- `tools/` — generic scripts: environment check, tool installation, extraction, build, run, A/B comparison,
  Dolphin reference capture, PGO, safe removal of worktrees that contain junctions (`tools/worktree.py`).
- `orig/`, `generated/`, `external/`, `build/` — your disc image, locally generated files, third-party tools and
  build outputs. Never committed.

Code comments sometimes refer to the project's private working notes (`notes/…`, `docs/…`); these are not
published.

## Distribution policy

A ready-to-use installer (see "Installer" above) will be published as a GitHub release once it has passed the
project's review and a test on a clean Windows (decision of the project owner, 2026-10-19; no release exists yet).
It ships the recompiled executable and its libraries, never any game data: players provide their own disc image.
The fully decompiled C++ version (roadmap stage 2, milestone D3) will later replace the recompiled executable in it.
You can always build the game locally from your own copy, as described above.

## License

The repository is licensed under **GPL-3.0** (see `LICENSE`), with the exceptions below. Licenses cover the code
and documents of this repository only, not the game or its data. Code generated by DolRecomp relies on its CPU
runtime (GPL-3.0). A game executable built from this repository also contains the recompiled game code, derived from
the original game: that part is not licensed by this project.

To make reuse easy for other Chibi-Robo! projects (decompilations, research, tools), some parts are offered under
more permissive terms:

| Path | License |
|---|---|
| `games/chibi-robo/symbols/` (symbol maps: names and addresses) | [CC0 1.0](LICENSES/CC0-1.0.txt) (public domain dedication) |
| `runtime/core/hle/sdk_signatures.tsv` (SDK function signatures: names, sizes, CRC-32 of the masked code, and the first masked instruction word as a lookup key) | [CC0 1.0](LICENSES/CC0-1.0.txt) |
| `games/chibi-robo/RESEARCH.md` (what we learned about the game) | [CC0 1.0](LICENSES/CC0-1.0.txt) |
| `tools/`, except `tools/patches/dolrecomp/` and `tools/ghidra/hw_inventory.py` (this includes the Aurora patches in `tools/patches/aurora/`) | GPL-3.0 **or** [MIT](LICENSES/MIT.txt), at your choice |
| `tools/patches/dolrecomp/` (patches to DolRecomp) | GPL-3.0 only |
| `tools/ghidra/hw_inventory.py` (its hardware register names come from Dolphin's headers) | GPL-3.0 only |
| Everything else, including `runtime/` (part of it is derived from Dolphin, GPL-2.0-or-later) | GPL-3.0 |

See also **[RESEARCH.md](games/chibi-robo/RESEARCH.md)** for our findings about the game: executable layout,
REL modules, SDK libraries, identified functions and engine behavior.

*Chibi-Robo!* is a trademark of its respective owners. This project is not affiliated with or endorsed by
Nintendo or skip Ltd. This repository contains no game code or assets, and requires a legally obtained copy of the game.

## Development

Most of the code and documentation is written with AI coding agents (Claude, by Anthropic, through Claude Code),
under the direction of the project owner. Every code change goes through the automated checks described in
[ROADMAP.md](ROADMAP.md) ("How changes are validated"). Reviews: a separate AI review agent before each merge, and
the project owner's validation at the end of each stage. Commits written with AI carry a `Co-Authored-By: Claude`
line.

## Credits and third-party software

- [DolRecomp](https://github.com/ExpansionPak/DolRecomp) (GPL-3.0) — static recompiler from PowerPC to C. A small
  patch is kept in `tools/patches/dolrecomp/`.
- [Aurora](https://github.com/encounter/aurora) (MIT) — GameCube/Wii compatibility layer (GX, input) and its
  dependencies (Dawn, SDL3 and others, under their own licenses). Small patches are kept in
  `tools/patches/aurora/` and applied by `tools/build.py`: the first lets the render target be taller than the
  displayed image (the console's 528-line EFB); the second snaps the viewport, scissor and copy edges to whole
  target pixels, so that a non-integer window scale does not leave a black line at the bottom of the shadow map;
  the third moves the buffer of very large texture uploads (more than 24 MiB in one frame) from the game thread to
  Aurora's render worker, so that creating and unmapping it no longer races with the worker on the Dawn device.
  Patch 0006 turns off Dawn's `use_dxc` toggle: the Direct3D 12 backend compiles its shaders with FXC
  (`d3dcompiler_47.dll`, found in the Windows system folder on the development machine; its presence on every
  Windows, and in Windows Sandbox, is not established) instead of DXC, so `dxcompiler.dll` and `dxil.dll` are neither
  needed nor copied next to the executable.
  Patch 0007 requests Dawn's `ImplicitDeviceSynchronization` feature when the adapter offers it: Dawn then locks the
  device in every API call except command encoding, so the game thread, Aurora's GX FIFO processor and its render
  worker can no longer race inside Dawn (a warning is logged if the adapter does not offer it;
  `AURORA_IMPLICIT_DEVICE_SYNC=0` leaves it off, for comparisons).
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT), through Aurora — the setup menu.
- [Dolphin](https://github.com/dolphin-emu/dolphin) (GPL-2.0-or-later) — reference for the original behavior;
  its AX audio voice processing is ported in `runtime/core/audio/`, with attribution in each file.
- [cbr_decomp](https://github.com/eavpsp/cbr_decomp) — function names in `games/chibi-robo/symbols/` come from its
  `decomp_dtk_main` folder (CC0 1.0). Thanks to its author for the groundwork.
- [unplug](https://github.com/adierking/unplug) (MIT) — the stage numbers in `games/chibi-robo/game.toml`
  (`[stage.names]`), and the reference for the disc and stage formats.
- [Ghidra](https://github.com/NationalSecurityAgency/ghidra) (Apache-2.0) — binary analysis.
- [LLVM/clang](https://github.com/llvm/llvm-project) (Apache-2.0 with LLVM exception),
  [Ninja](https://github.com/ninja-build/ninja) (Apache-2.0).
