# Settings and controls

[Back to the README](../README.md)

- [Starting the game](#starting-the-game)
- [Setup menu](#setup-menu)
- [Console window](#console-window)
- [Graphics backend](#graphics-backend)
- [Controls](#controls)
- [Useful command-line options and environment variables](#useful-command-line-options-and-environment-variables)

## Starting the game

Run the game by launching the executable directly (or the Start menu shortcut made by the installer).
`chibi-robo.exe --help` lists the diagnostic options; `tools/run.py` runs it for automated tests (isolated user
folder, audio recorded to a WAV file instead of the speakers).

## Setup menu

A direct launch opens a small setup menu first: window size, fullscreen, render scale and graphics backend (Enter or
"Start game" starts the game; launches by the project's tools never show it; `GCRT_SETUP_MENU=0` disables it, `=1`
forces it). The choices are saved in `game_config.ini` in the same folder as `controls.ini` (see below). Tick "Skip
this menu next time" to start the game directly; hold Shift while the game starts to show the menu again, or set
`skipMenu=0` in `game_config.ini`. Values outside the menu's choices fall back to the defaults.

## Console window

The game no longer opens a console window by itself. "Show console" (checked by default) opens one with the game's
log when the game starts, never while the menu is up; the output written before that moment is shown in it. If the
game was launched from a terminal, it prints in that terminal instead of a new window (Windows does not wait for it,
so the output follows the prompt). The choice is saved as `console=1` or `console=0` in `game_config.ini`; with
"Skip this menu next time", the saved value applies. `--console` and `--no-console` decide for one launch only: the
box starts checked or unchecked and the file is changed only if you toggle the box. When the output is redirected
(to a file or a pipe, as `tools/run.py` and the test benches do), no console is ever opened.

## Graphics backend

The menu's "Graphics backend" list offers Auto (the default), Direct3D 12 and Vulkan, saved as
`backend=auto|d3d12|vulkan` in `game_config.ini`. The window is already open when the menu shows, so a change applies
the next time the game starts. `--backend d3d12|vulkan|auto` or `GCRT_BACKEND` choose for one launch only (not
saved). Auto uses Direct3D 12, with Vulkan as the alternative. An unknown name falls back to Auto with a message.

What happens when a backend cannot start, the software renderer used when no GPU can be used, and how the backends
compare: [technical.md](technical.md#graphics-backends).

## Controls

On first launch the game writes a commented `controls.ini` into `%APPDATA%\Chibi Robo PC\Chibi Robo\`. Each
GameCube button can have several keys or gamepad inputs, and dead zones are adjustable. Keys are named by their
physical position (US QWERTY layout), so the defaults work on AZERTY keyboards too. Default keyboard layout (QWERTY
names):

| GameCube | Key | | GameCube | Key |
|---|---|---|---|---|
| Control stick | Arrow keys | | A / B | X / Z |
| C-stick | I / J / K / L | | X / Y / Z | C / S / D |
| D-pad | T / F / G / H | | L / R (digital) | Q / W |
| Start | Enter | | L / R (analog) | A / E |

Gamepads use Aurora's mappings per controller family (Xbox, PlayStation, Switch, GameCube adapters).

## Useful command-line options and environment variables

`chibi-robo.exe --help` gives the full list of command-line options. The ones described in this guide:

| Option or variable | Effect | Details |
|---|---|---|
| `--console`, `--no-console` | Show or hide the console window for one launch | [Console window](#console-window) |
| `--backend d3d12\|vulkan\|auto`, `GCRT_BACKEND` | Graphics backend for one launch (not saved) | [Graphics backend](#graphics-backend) |
| `GCRT_SETUP_MENU=0` / `=1` | Disable / force the setup menu | [Setup menu](#setup-menu) |
| `GCRT_WATCHDOG=<seconds>` | Delay before a freeze report (`0` turns off both watchdogs) | [reporting-crashes.md](reporting-crashes.md#freezes) |
| `GCRT_WATCHDOG_STARTUP=<seconds>` | Delay of the start-up watchdog | [reporting-crashes.md](reporting-crashes.md#freezes) |
| `GCRT_CRASH_DIR` | Another folder for crash reports | [reporting-crashes.md](reporting-crashes.md#where-reports-are-written) |
| `GCRT_EXIT_BOOST=0` | Turn off the priority boost at exit (diagnosis) | [technical.md](technical.md#process-exit-priority) |
| `GCRT_FATAL_DIALOG` | Message box for a damaged installation (automated runs never get it) | [installation.md](installation.md#damaged-installation) |
| `GCRT_TEST_ALL_BACKENDS=1` | Tests only: lets `--backend` name `d3d11` or `null` | [technical.md](technical.md#graphics-backends) |
