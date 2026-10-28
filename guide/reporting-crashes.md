# Crash and freeze reports

[Back to the README](../README.md)

- [Where reports are written](#where-reports-are-written)
- [What you may share, and what you must not](#what-you-may-share-and-what-you-must-not)
- [Freezes](#freezes)
- [Reporting a problem](#reporting-a-problem)
- [Telling an unstable machine from a bug](#telling-an-unstable-machine-from-a-bug)
- [For developers: reading a minidump](#for-developers-reading-a-minidump)

## Where reports are written

If the game crashes, it writes a report into the `crashes` folder of the user folder
(`%APPDATA%\Chibi Robo PC\Chibi Robo\crashes\`), or into `%TEMP%\gcrt-crashes` if the crash happens before that
folder is known. Only the 5 most recent reports are kept. `GCRT_CRASH_DIR` chooses another folder.

## What you may share, and what you must not

A report has three files:

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

## Freezes

If the game loop stops progressing for 60 seconds (a freeze), a `hang-<date>-<time>-<pid>.txt` report is written in
the same folder, followed by a `.dmp` if the game is still stopped by then. The game is not killed: it is paused
while the report copies the state of its threads (at most a few seconds), and it may also be paused while a
snapshot of the process is taken for the `.dmp` (a fraction of a second); the `.dmp` is then written from that
snapshot without pausing the game. The same sharing rules apply: the `.txt` only, after reading it.

`GCRT_WATCHDOG=<seconds>` changes the delay (`0` turns off both watchdogs). The start-up (window, setup menu, game
boot up to its first frame) is watched too, with `GCRT_WATCHDOG_STARTUP=<seconds>` (default: `GCRT_WATCHDOG`, at
least 60; `0` turns it off); a start-up report is a `.txt` only, never a `.dmp`.

## Reporting a problem

Open an issue at <https://github.com/Armandpro12/chibi-robo-pc/issues> with your Windows and graphics driver version
and what you did. If the game crashed or froze, attach only the `crash-*.txt` or `hang-*.txt` report, after reading
it. A screenshot of a visual problem is welcome (crop it to the problem), and a save only if asked. Never attach
game files, your disc image, a `.dmp` or a `-guest-ram.bin` file.

## Telling an unstable machine from a bug

Two tools of this repository (not included in the installer) help players and testers tell an unstable machine from
a bug:

- `tools/collect_diagnostics.cmd` (read-only, anonymized: hardware error events, BIOS, microcode, drivers, overlays,
  recent crash reports; nothing is sent). What it reads and how it anonymizes: [tools/diag/README.md](../tools/diag/README.md).
- `tools/cpustress/` (runs the recompiled-code instruction patterns on every core and checks every result; a PASS
  does not prove the processor is sound). See [tools/cpustress/README.md](../tools/cpustress/README.md).

## For developers: reading a minidump

`tools/crash_inspect.py <report.dmp>` decodes the faulting instruction of a minidump, checks whether it explains the
fault and lists the memory regions, the threads and the last game events stored in the dump (`--help` lists its
options, verdicts and exit codes).
