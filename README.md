<p align="center">
  <img src="guide/images/logo.png" width="128" height="128" alt="Project icon: a simple robot pictogram">
</p>

<h1 align="center">Chibi-Robo! PC</h1>

<p align="center">
  <b>A native Windows port of <i>Chibi-Robo!</i> (GameCube), made by static recompilation, with nothing emulated at the hardware level.</b><br>
  <a href="https://github.com/Armandpro12/chibi-robo-pc/releases/latest">Download</a> ·
  <a href="guide/installation.md">Install guide</a> ·
  <a href="ROADMAP.md">Roadmap</a> ·
  <a href="https://github.com/Armandpro12/chibi-robo-pc/issues">Issues</a>
</p>

## Table of contents

- [What it is](#what-it-is)
- [Current state](#current-state)
- [Download and install](#download-and-install)
- [Requirements](#requirements)
- [Help test it](#help-test-it)
- [Known problems](#known-problems)
- [Guide](#guide) (installation, settings and controls, crash reports, building, technical notes)
- [License and credits](#license-and-credits)

## What it is

- The game's PowerPC code is recompiled to C, and every Nintendo SDK function that talks to the console hardware is
  replaced by native PC code. **Nothing is emulated at the hardware level**: no hardware registers, no cycle-driven
  devices, no interpreter.
- Both the **US** (GGTE01) and **PAL** (GGTP01) versions are supported.
- **You provide your own disc image.** No game data (disc image, textures, sounds, levels) is in this repository or
  in the installer.
- Next steps: a fully decompiled C++ game (started: a first dozen functions are rewritten and checked against the
  recompiled ones, in the source but not yet in release 0.1.1), then multiplayer, a technical remaster (unlimited frame rate, widescreen), mods, a PS Vita port and a remaster of models and textures ([ROADMAP.md](ROADMAP.md)).

## Current state

> **Early beta (0.1.x).** It runs, and it has been compared automatically with Dolphin on the key scenes, but
> nobody has played the whole game on it yet. That is where you can help: see [Help test it](#help-test-it).

- ✅ Boots and runs in real time with music and sound effects, on both versions: title screen, new game, the prologue
  up to and including the birthday party, the first save, and leaving the Chibi-House.
- ✅ Scripted game sessions without a crash: 50 game minutes from a late save (US), 30 minutes from a new-game save started in the backyard (PAL).
- ✅ Native saves like a PC game (`.gci` files, Dolphin-compatible) and configurable keyboard and gamepads.
- ✅ A Windows installer: pick your disc image, per-user install without administrator rights, uninstall keeps your saves unless you tick a box to delete them.
- ✅ Images and sound compared with Dolphin on the key scenes; the known differences are listed in the ROADMAP.
- ⬜ Not verified yet: playing through the whole game, most video cutscenes, a clean Windows, Windows 10, other
  graphics hardware, a PC without a GPU (only simulated), and listening and playing by a human.

More detail: [guide/technical.md](guide/technical.md#current-state-in-detail-october-2026) and [ROADMAP.md](ROADMAP.md).

## Download and install

1. Download `chibi-robo-pc-setup-<version>.exe` (about 30 MB) from the
   **[latest release](https://github.com/Armandpro12/chibi-robo-pc/releases/latest)**.
2. Check its SHA-256 against `SHA256SUMS.txt` on the same page (`certutil -hashfile <file> SHA256`).
3. Run it and choose **your own disc image** and a new or empty folder. The image is checked, then copied into that
   folder (about 1.5 GB in total).
4. Start the game from the Start menu. A small setup menu opens first (window size, fullscreen, render scale, graphics
   backend).

Details, updating and uninstalling: [guide/installation.md](guide/installation.md).

## Requirements

- 64-bit Windows (developed and tested on Windows 11).
- A graphics driver with Direct3D 12 or Vulkan.
- Your own disc image of the game, US or PAL, as an uncompressed `.iso` or `.gcm` file (convert other formats first,
  for example with Dolphin).
- About 1.6 GB of free disk space.

## Help test it

You do not need to be a developer. The most useful reports, in this order:

1. **Play, and tell us where it differs from the original** (or from Dolphin): wrong or missing graphics, sound that
   cuts or loops, a cutscene that does not play, a quest that gets stuck, a save that does not load. Say which
   version (US or PAL), where in the game, and what you expected.
2. **Try it on your machine.** Does it install and start on Windows 10, on a clean Windows, with AMD or Intel
   graphics, on a laptop, with a gamepad? "It works for me" is a useful report too.
3. **Listen.** Music, effects and the intro, compared with the original.
4. **Crashes and freezes.** Share only the `crash-*.txt` or `hang-*.txt` report, after reading it
   ([guide/reporting-crashes.md](guide/reporting-crashes.md)).

To report, [open an issue](https://github.com/Armandpro12/chibi-robo-pc/issues/new) with: the version (from the
installer's file name, or the release you downloaded), US or PAL, your Windows version, your graphics card and
driver, what you did and what happened. One problem per issue. A screenshot of a visual problem is welcome (crop it
to the problem); attach a save only if asked. **Never attach your disc image, files extracted from it, or memory
dumps.**

## Known problems

- An intermittent crash at the start of 3D rendering ([#11](https://github.com/Armandpro12/chibi-robo-pc/issues/11)):
  a race in the graphics library was reduced, but the fix is not proven.
- Repeated crashes at the same instruction on one player's machine, cause unknown
  ([#28](https://github.com/Armandpro12/chibi-robo-pc/issues/28)).
- Only part of the game is compared with the original; most video cutscenes are not yet
  ([#34](https://github.com/Armandpro12/chibi-robo-pc/issues/34)).
- On first launch, about 37 seconds (US) or 72 seconds (PAL) into a new or loaded game, the game stops for a while to
  compile graphics pipelines: about 10 seconds on the test machine, much more on a heavily loaded PC; later
  launches reuse the compiled pipelines. The development version compiles them on several threads, which brings
  that stop down to about 2 seconds on the test machine
  ([#47](https://github.com/Armandpro12/chibi-robo-pc/issues/47)).
- The installer does not accept compressed images (`.rvz`, `.ciso`, ZIP...).

All open issues: [github.com/Armandpro12/chibi-robo-pc/issues](https://github.com/Armandpro12/chibi-robo-pc/issues).
If the game crashes or freezes, share only the `crash-*.txt` or `hang-*.txt` report, after reading it
([guide/reporting-crashes.md](guide/reporting-crashes.md)).

## Guide

The details live in [`guide/`](guide), one file per topic:

| Topic | What is in it |
|---|---|
| [Installation](guide/installation.md) | The installer step by step, updating, failed installs, uninstalling, where saves and settings live |
| [Settings and controls](guide/settings-and-controls.md) | Setup menu, console, graphics backend, keyboard and gamepad, useful options |
| [Crash reports](guide/reporting-crashes.md) | Crash and freeze reports, what to share and what not, diagnostics tools |
| [Building from source](guide/building.md) | Requirements, build, installer, icon, PGO, test tools |
| [Technical notes](guide/technical.md) | How the port works, graphics backends, Aurora patches, repository layout, REA |
| [Distribution, licenses and credits](guide/distribution-and-license.md) | Distribution policy, licenses, third-party software |

Project status and plans: [ROADMAP.md](ROADMAP.md). What we learned about the game: [RESEARCH.md](games/chibi-robo/RESEARCH.md).

## License and credits

The code is **GPL-3.0** ([LICENSE](LICENSE)); some parts are CC0 1.0, or GPL-3.0 or MIT at your choice
([details](guide/distribution-and-license.md#license)). The recompiled game code inside the executable is derived from
the original game and is not licensed by this project. Built on [DolRecomp](https://github.com/ExpansionPak/DolRecomp),
[Aurora](https://github.com/encounter/aurora), [Dolphin](https://github.com/dolphin-emu/dolphin) (reference and audio
code) and more: [credits](guide/distribution-and-license.md#credits-and-third-party-software). Most of the code is
written with AI coding agents (Claude) under the direction of the project owner.

*Chibi-Robo!* is a trademark of its respective owners. This is an unofficial fan project, not affiliated with or
endorsed by Nintendo or skip Ltd. This repository contains no game code or assets, and requires a legally obtained
copy of the game.
