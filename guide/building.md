# Building from source (Windows)

[Back to the README](../README.md)

- [Requirements](#requirements)
- [Getting started](#getting-started)
- [Extract and build](#extract-and-build)
- [Native extraction tool (gcextract)](#native-extraction-tool-gcextract)
- [Building the installer](#building-the-installer)
- [Game icon](#game-icon)
- [Optional: profile-guided optimization](#optional-profile-guided-optimization)
- [Starting in a chosen stage (testing)](#starting-in-a-chosen-stage-testing)
- [Tests and comparison tools](#tests-and-comparison-tools)
- [Checking the documentation links](#checking-the-documentation-links)

## Requirements

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

## Extract and build

Copy your disc image into `orig/`, then extract and build:

```
py -3 tools/extract.py chibi-robo                    # PAL by default
py -3 tools/extract.py chibi-robo --version GGTE01
py -3 tools/build.py chibi-robo                      # -> build/chibi-robo-GGTP01-Release/bin/chibi-robo.exe
py -3 tools/build.py chibi-robo --version GGTE01
```

Accepted image formats: `.iso` / `.gcm` directly; `.rvz`, `.gcz`, `.wia`, `.ciso` after automatic conversion with
DolphinTool. The supported versions and their SHA-1 are listed in
[installation.md](installation.md#supported-versions).

The build recompiles the game with DolRecomp, then compiles it with clang-cl. The first build in each build folder
(`build/<game>-<ID>-<config>`) needs an internet connection: CMake downloads Aurora's dependencies into it (prebuilt
Dawn and SDL3 packages and ten source libraries); later builds in the same folder reuse them. `tools/build.py` also
applies the Aurora patches of `tools/patches/aurora/` ([technical.md](technical.md#aurora-patches)).

The decompiled functions enabled in `games/chibi-robo/decomp/functions.tsv` replace their recompiled versions. The
build first checks each address against the symbol files of `games/chibi-robo/symbols/`, all in Git (maps, gap file
and the transposition report `GGTE01-to-GGTP01.tsv`), so it needs no extra step for the symbols: a missing file stops the build with
the command that restores it. After a change of the reference symbols, `py -3 tools/symbols.py chibi-robo transpose
--to GGTP01` rewrites the report (it needs the cbr_decomp symbols in `external/` and the Ghidra analysis of GGTP01).
With the US version extracted too (`tools/extract.py chibi-robo --version GGTE01`), the PAL build compares the game
globals of `games/chibi-robo/symbols/globals.tsv` with the US main executable; without it, the PAL build still works
and prints a warning: the globals are then checked in the PAL executable only.

Run the game by launching the executable directly ([settings-and-controls.md](settings-and-controls.md)).

## Native extraction tool (gcextract)

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

## Building the installer

`tools/installer/` builds a Windows installer for players (Inno Setup 6, free: `winget install
JRSoftware.InnoSetup`). It contains the game executable of each version, its DLLs, the Visual C++ runtime DLLs,
`gcextract.exe`, `game.toml` and `THIRD_PARTY_LICENSES.txt`, and no game data. The game executable contains this
project's runtime (GPL-3.0), third-party code (listed with its licences in `THIRD_PARTY_LICENSES.txt`) and the
recompiled game code, which is derived from the original game and is not licensed by this project:

```
py -3 tools/gcextract/build.py
py -3 tools/installer/build.py chibi-robo     # -> build/installer/chibi-robo-pc-setup-<version>.exe
```

The installer version (for example `0.1.1`) comes from `[installer] version` in `games/chibi-robo/game.toml`; it is
shown in the Programs list and in the installer's file properties, and is part of its file name.

By default it takes `build/chibi-robo-<ID>-Release` for every version that is built (`--build ID=DIR` to choose).
One installer holds both versions; the player's image selects which executable is installed. `--no-disc-arg` makes
the shortcuts start the game without `--disc` (executables that find `disc/` next to themselves). Neither
`dxcompiler.dll` nor `dxil.dll` is needed or shipped: Direct3D 12 compiles its shaders with FXC (Aurora patch 0006,
see [technical.md](technical.md#aurora-patches)). That was checked on the development machine only; a PC where
`d3dcompiler_47.dll` is missing has not been tested, and a PC without a GPU only through a test switch (see
[technical.md](technical.md#graphics-backends)). `tools/installer/notices.py` regenerates `THIRD_PARTY_LICENSES.txt`
from the real licence files (the build checks that the committed copy is up to date).

What the installer does on the player's machine: [installation.md](installation.md).

## Game icon

The game icon (`games/chibi-robo/chibi-robo.ico`: executable, game window and taskbar, shortcuts, installer and
uninstaller) is an original pictogram drawn by `tools/make_icon.py` from `games/chibi-robo/icon.toml`; nothing in it
comes from the game. `py -3 tools/make_icon.py games/chibi-robo/icon.toml games/chibi-robo/chibi-robo.ico --check`
checks that the committed icon matches its description. The logo of the README (`guide/images/logo.png`) is the
256 x 256 image of the same icon, written by the `--png DIR` option of `tools/make_icon.py`.

## Optional: profile-guided optimization

`py -3 tools/pgo.py chibi-robo train --version GGTP01` builds an instrumented game, replays the training
scenarios and writes a local profile into `build/pgo/`. Later builds use it automatically while it matches the
code (about 20 % faster). Profiles are never committed.

## Starting in a chosen stage (testing)

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
in `games/chibi-robo/game.toml` (`[stage]`). Without `--stage`, nothing changes.

## Tests and comparison tools

The project validates every change automatically against native reference runs (images, audio, saves), for both
the PAL and the US versions (see [ROADMAP.md](../ROADMAP.md#how-changes-are-validated)). The bench references and
the regression tests live in a local `tests/` folder that is not published (it holds output of the game), so these
checks cannot be rerun as is from this repository; `tools/ab.py record` records references from your own build.

- `tools/run.py`: one isolated run (isolated user folder, audio recorded to a WAV file instead of the speakers).
- `tools/ab.py`: A/B bench against native reference runs.
- `tools/dolphin_ref.py`: reference captures from Dolphin, on a hidden desktop.
- `tools/worktree.py`: safe removal of Git worktrees that contain NTFS junctions.
- Progression scenarios: [games/chibi-robo/scenarios/play/README.md](../games/chibi-robo/scenarios/play/README.md).

Each tool documents its options with `--help`.

## Checking the documentation links

`py -3 tools/check_doc_links.py` checks that every relative link and image of the tracked Markdown files points to an
existing file, and that every `#anchor` matches a heading of its target. It exits with code 1 and lists the broken
links if there are any.
