# Installation (Windows, for players)

[Back to the README](../README.md)

- [Download](#download)
- [Requirements and what was tested](#requirements-and-what-was-tested)
- [Supported versions](#supported-versions)
- [What the installer does](#what-the-installer-does)
- [Updating (running the installer again)](#updating-running-the-installer-again)
- [When an installation does not complete](#when-an-installation-does-not-complete)
- [Damaged installation](#damaged-installation)
- [Uninstalling](#uninstalling)
- [Where saves and settings live](#where-saves-and-settings-live)
- [Silent install](#silent-install)

## Download

Get the installer from the **[latest release](https://github.com/Armandpro12/chibi-robo-pc/releases/latest)**
(`chibi-robo-pc-setup-<version>.exe`, about 30 MB) and check its SHA-256 against `SHA256SUMS.txt` on the same page
(for example `certutil -hashfile <file> SHA256`). Run it and pick **your own disc image** of the game (US or PAL; an
uncompressed `.iso` or `.gcm` file: other formats must be converted first, for example with Dolphin). The installer
checks the image, copies it into the installation folder (about 1.5 GB in total), and installs per user, without
administrator rights.

The installer contains the game executable of each version, its DLLs, the Visual C++ runtime DLLs, `gcextract.exe`,
`game.toml` and `THIRD_PARTY_LICENSES.txt`, and no game data. How it is built: [building.md](building.md#building-the-installer).

## Requirements and what was tested

Needs a 64-bit Windows and a graphics driver with Direct3D 12 or Vulkan. It was developed and tested on one machine
(Windows 11, NVIDIA graphics). **Not tested yet**: a clean Windows (including Windows Sandbox), Windows 10, other
graphics hardware, a PC without a GPU (the software renderer fallback was only checked by hiding the GPU with a
test switch, see [technical.md](technical.md#graphics-backends)), and playing through the whole game. See the
release notes for the known problems, and please report any crash with its `crash-*.txt` report only, after reading
it (see [reporting-crashes.md](reporting-crashes.md): the other files of a report hold game memory and must not be shared).

## Supported versions

| ID | Region | Revision | SHA-1 of the .iso image |
|---|---|---|---|
| GGTP01 | PAL (Europe) | 0 | `f321aa7fb1ae5101ec60f1bbca8184898142135a` |
| GGTE01 | NTSC-U | 0 | `9052f186411b49845323eaaeb014755bcf96321b` |

The installer reads uncompressed `.iso` / `.gcm` images only. When you build from source, `tools/extract.py` also
accepts `.rvz`, `.gcz`, `.wia` and `.ciso` images, after automatic conversion with DolphinTool
([building.md](building.md#extract-and-build)).

## What the installer does

What the player does: run the installer, choose the installation folder (per user, no administrator rights; a path
of ASCII characters only; a new or empty folder, or an earlier installation of this installer: a folder that holds
other files is refused, so that nothing of the player's is overwritten or later uninstalled), choose their own
`.iso` / `.gcm` dump. The image is checked (SHA-1 against `game.toml`); compressed formats (RVZ, WIA, GCZ, CISO,
NKit), modified or unknown dumps are refused with an explanation. The image is copied into the installation folder
as `game.iso`, `sys/` is extracted next to it, and a Start menu shortcut is created (desktop shortcut optional).
One installer holds both versions; the player's image selects which executable is installed.

A folder that exists but cannot be listed (access denied) is refused like a folder that holds other files. An
existing empty folder is accepted (tested: install, failed first installs, uninstall), and a folder that holds
other files of the player is refused with nothing changed (tested).

The game, its window and taskbar button, the shortcuts, the installer and the uninstaller use the project's own
icon, an original pictogram: nothing in it comes from the game ([building.md](building.md#game-icon)).

## Updating (running the installer again)

Running the installer again over an existing installation updates it (the image already installed is offered and
not copied again). Free disk space is checked before anything is written: an update needs as much free space as a
first installation (about 1.6 GB), because the previous disc image is kept until the new copy is in place.

## When an installation does not complete

- the new image is refused, there is not enough free space, or the extraction or the check of the copy fails, or
  Setup is cancelled or stopped by an error while it installs the program files: a previous installation is kept
  as it was (whatever Setup had already replaced is put back: the disc image, `disc/` and `manifest.json`, the
  executable, its DLLs and `BUILD-INFO.txt`; Setup keeps each previous file as `<name>.old` until the installation
  is complete, because Inno Setup's own rollback does not restore a file it has overwritten). So a failed update to
  the other version (US to PAL, or the reverse) is not expected to leave an executable of one version with the disc
  image of the other (tested with simulated failures only, see below; not tested with a real cancel, a locked file
  or a power loss). Its Programs-list entry is not expected to change (Setup does not write it when the installation
  failed; only checked in Setup's log so far, not in the registry). If a previous file cannot be put back (for
  example because it is open in another program), Setup keeps it as `<name>.old` and says so when it stops with an
  error; if Setup was cancelled or stopped in another way, the message may only be written to Setup's log. Run the
  installer again, which puts it back first;
- a first installation into a new or empty folder that does not complete in any of these ways leaves nothing
  behind in that folder: the folder is removed if Setup created it (an empty folder that existed before is left in
  place, empty), and so are the parent folders Setup created for it if they are empty; no uninstaller is left, and
  no Programs-list entry is expected (Setup does not write it for a failed installation; only checked in Setup's log
  so far);
- after a power loss or a crash during the installation, nothing is guaranteed: run the installer again over the
  same folder. While an installation is under way, the folder holds `setup-in-progress.txt` (deleted when it
  completes, or when everything was put back). The next run puts back each previous file left as `<name>.old`:
  when the file itself is missing, and also when both exist and `setup-in-progress.txt` is there (the file may have
  been cut while being written, or belong to a half-made installation, so it is deleted: this brings back the
  installation as it was before the interrupted run). Without `setup-in-progress.txt`, the run that left a
  `<name>.old` had completed, so the file is kept and the `<name>.old` deleted. Then it repairs the installation (a
  first installation interrupted that way has no uninstaller yet). Tested by simulating the files such an
  interruption leaves, not with a real power loss.

## Damaged installation

If the game cannot start because the installation is damaged, it shows a message box and exits instead of opening
a black window: the disc image or `manifest.json` is missing or unreadable, the game files (`disc/sys/main.dol`,
`boot.bin`) are missing, or the disc image or the game files are those of another version than the executable
("This executable is for GGTE01 but the disc image is GGTP01"). The message names the file the game looked for; run
the installer again over the same folder to repair it. Automated runs never get the box: the message goes to stderr
(and the trace) and the exit code is 1 (`GCRT_FATAL_DIALOG`, see `runtime/core/platform/gcrt_fatal.h`).

## Uninstalling

Uninstalling removes only the files the installer put there and the game's GPU cache, then the installation folder
if nothing else is left in it (also when it was an empty folder that existed before the installation; a folder that
still holds other files is kept). Your saves and settings stay in your user folder, unless you tick "Also delete my
saves and settings" in the uninstaller (unticked by default) and confirm it (default answer: No); a silent uninstall
keeps them unless `/DELETEUSERDATA=1` is given.

## Where saves and settings live

- Saves (memory card files, `.gci`, Dolphin-compatible): `%USERPROFILE%\Saved Games\Chibi Robo PC\Chibi Robo\`. A
  memory card is always inserted, like a PC game.
- Settings (`game_config.ini`, `controls.ini`, see [settings-and-controls.md](settings-and-controls.md)) and crash
  reports (`crashes\`, see [reporting-crashes.md](reporting-crashes.md)): `%APPDATA%\Chibi Robo PC\Chibi Robo\`.

## Silent install

`setup.exe /SP- /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR=<folder> /IMAGE=<image.iso>` (exit code 0 on
success, 1 if the image or the folder is refused, 7 if the image could not be extracted, the folder is refused at
the last check, there is not enough free disk space or a previous file left by an interrupted run could not be put
back, 9 if its copy could not be checked or put in place, 5 if the installation was cancelled or stopped by a file
error).
