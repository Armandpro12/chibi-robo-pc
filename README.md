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

The build recompiles the game with DolRecomp, then compiles it with clang-cl. The first build in each build folder
(`build/<game>-<ID>-<config>`) needs an internet connection: CMake downloads Aurora's dependencies into it (prebuilt
Dawn and SDL3 packages and ten source libraries); later builds in the same folder reuse them.

Run the game by launching the executable directly. `chibi-robo.exe --help` lists the diagnostic options;
`tools/run.py` runs it for automated tests (isolated user folder, audio recorded to a WAV file instead of the
speakers).

### Setup menu

A direct launch opens a small setup menu first: window size, fullscreen and render scale (Enter or "Start game"
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

If the game stops progressing for 60 seconds (a freeze), a `hang-<date>-<time>-<pid>.txt` report (and `.dmp`) is
written in the same folder while the game keeps running; `GCRT_WATCHDOG=<seconds>` changes the delay (`0` turns it
off).

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
  Dolphin reference capture, PGO.
- `orig/`, `generated/`, `external/`, `build/` — your disc image, locally generated files, third-party tools and
  build outputs. Never committed.

Code comments sometimes refer to the project's private working notes (`notes/…`, `docs/…`); these are not
published.

## Distribution policy

Only the fully decompiled C++ version will ever be published as a ready-to-use installer (roadmap stage 2,
milestone D3). It will ship a precompiled executable that contains no game data, and players will still provide
their own disc image. Until then, the game can only be built locally from your own copy, as described above.

## License

The repository is licensed under **GPL-3.0** (see `LICENSE`), with the exceptions below. Licenses cover the code
and documents of this repository only, not the game or its data. Code generated by DolRecomp relies on its CPU
runtime (GPL-3.0).

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
Nintendo or skip Ltd. It contains no game code or assets, and requires a legally obtained copy of the game.

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
  dependencies (Dawn, SDL3 and others, under their own licenses). Five small patches are kept in
  `tools/patches/aurora/` and applied by `tools/build.py`: the first lets the render target be taller than the
  displayed image (the console's 528-line EFB); the second snaps the viewport, scissor and copy edges to whole
  target pixels, so that a non-integer window scale does not leave a black line at the bottom of the shadow map;
  the third moves the buffer of very large texture uploads (more than 24 MiB in one frame) from the game thread to
  Aurora's render worker, so that creating and unmapping it no longer races with the worker on the Dawn device;
  the fourth sets a busy timeout on the SQLite connections of Aurora's pipeline and Dawn caches, so that two game
  processes sharing one cache folder wait for each other's locks instead of failing with "database is locked";
  the fifth creates Aurora's cached bind groups on the render worker instead of the GX command thread, so that the
  Dawn device (whose D3D12 sampler descriptor cache has no lock) is no longer used by both threads at once.
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
