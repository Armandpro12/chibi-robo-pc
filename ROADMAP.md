# Roadmap

Where the project stands, what has been done and what remains. Updated at every milestone.

**Last update: 2026-10-28.**

Legend: ✅ done · 🟡 in progress or partly done · ⬜ not started

## The plan at a glance

The project moves through six stages, **strictly one after the other**. A stage starts only once the previous
one has been validated.

| # | Stage | Goal | State |
|---|---|---|---|
| 1 | Faithful native port | The original game running natively on PC, with no emulated hardware | 🟡 in progress |
| 2 | Full decompilation (D1–D5) | The whole game rewritten as readable C++; first public release | 🟡 in progress |
| 3 | Multiplayer | Two players in the same house | ⬜ |
| 3b | PS Vita port | Planned, after multiplayer, before the remaster ([tracking issue](https://github.com/Armandpro12/chibi-robo-pc/issues/43)) | ⬜ |
| 4 | Remaster | PC-style controls and saves, audio settings, remastered models, optional HD pack | ⬜ |
| 5 | Modding platform (M1–M8) | Every game file replaceable; levels, C++ plugins and Lua scripts | ⬜ |

From stage 3 onwards, every stage must keep multiplayer working (automated two-instance test).

**Release policy.** The player installer of milestone 11 (decision of the project owner, 2026-10-19) is
published as a GitHub release (the first one, 0.1.0, before the clean-Windows test): it contains the
recompiled executable and its libraries but no game data, asks for the player's own disc image, and compiles
nothing on the player's machine. The first release (0.1.0) and the second (0.1.1) are published on GitHub. The fully decompiled C++ version (stage 2,
milestone D3) will later replace the recompiled executable in that installer. The game can also be built locally
(see [Building from source](guide/building.md)).

## Stage 1 — Faithful native port, with no emulated hardware 🟡

The PowerPC code is statically recompiled to C with [DolRecomp](https://github.com/ExpansionPak/DolRecomp).
Instead of simulating the console's hardware (registers, interrupts, a cycle-driven scheduler), every SDK function
that touches the hardware is replaced, identified by signature, with a native implementation. Both the PAL
(GGTP01) and the US (GGTE01) versions are validated at every step.

| # | Milestone | State | Notes |
|---|---|---|---|
| 1 | Environment | ✅ | `tools/doctor.py` checks every tool. Existing projects surveyed. |
| 2 | Loading | ✅ | DOL and the 16 REL modules extracted, loaded into DolRecomp and Ghidra. |
| 3 | Recompilation | ✅ | Whole game recompiled to C, builds without errors. |
| 4 | Minimal runtime | ✅ | Window, main loop, keyboard and gamepads. |
| 5 | Rendering | ✅ | PAL: the five boot screens, from the 50/60 Hz prompt to the title screen, match Dolphin captures above the documented SSIM threshold. US: the health and safety warning was compared first; the logos, title screen, file menus, loading screen, party, living room and backyard were compared later (milestone 9). |
| 6 | Audio | 🟡 | Music and effects work. On the title screen, the party and the living room (PAL and US) the music has the same content as in Dolphin at a constant offset (matched block by block, many blocks identical to the sample) and it loops with the same period; the differences that remain are listed under "Known differences with Dolphin". The credits video has the same duration and the same sound in 99.8 to 99.9 % of the 5 ms blocks. Still to do: the second basement video cutscene (not reached yet) and a listening test. |
| 7 | Saves | ✅ | Native PC saves: card always inserted, crash-safe writes (temporary file, then rename). |
| 8 | Playable game | 🟡 | Scripted play sessions of up to 50 minutes from late saves run without a crash; the prologue (new game through the birthday party), the first save, and the first load and unload of a REL module (`stage07.rel`, Chibi-House to living room and back) are replayed with scripted inputs on both US and PAL, three identical replays per version; open issues from player reports are listed under "Known issues". |
| 9 | Comparison with Dolphin | 🟡 | Images of the key scenes (boot screens, title, menus, loading, party, living room, backyard) compared with Dolphin in PAL and US: above the SSIM threshold except for the cases explained under "Known differences with Dolphin". The credits video is compared (PAL: SSIM 0.937 to 0.95 because Dolphin dumps it at another height); the basement video is compared in US and PAL (see the video entries below); the second basement video is not compared yet. |
| 10 | No emulated hardware | ✅ | See below. |
| 11 | Player installer | 🟡 | Inno Setup installer (`tools/installer/`): the player picks their own disc image, checked by SHA-1 against `game.toml` and refused with a clear message otherwise; per-user install without administrator rights, progress bar, shortcut, uninstall that keeps saves and settings, reinstall to update; free disk space checked first; a failed or cancelled reinstall puts the previous installation back (disc image files, executable and DLLs, so a failed switch to the other region is expected to keep a consistent installation), and a failed first install leaves nothing behind; all of this was tested with simulated failures only (not with a real cancel, a locked file or a power loss). Installed and run to the title screen on US and PAL from a developer machine with no development tools in `PATH`. `dxcompiler.dll` and `dxil.dll` are not needed (Direct3D 12 compiles shaders with FXC, identical captures on the development machine); a PC without `d3dcompiler_47.dll` is untested; without a usable GPU, the game falls back to Microsoft's software renderer (WARP, slow), checked only by hiding the GPU with a test switch. The setup menu lets the player choose the graphics backend (Auto, Direct3D 12 or Vulkan; same picture up to tiny rounding differences). A damaged installation (missing or unreadable disc image or manifest, missing game files, or a disc image or game files of the other version) gives a clear message box and exit code 1 instead of a black window; automated runs get the message on stderr only. The dialog was only tested on a hidden desktop (not with a real double-click), and non-Windows builds have never been compiled (stderr only). A run interrupted by a power loss or a crash is repaired by the next run (a progress file tells it whether the interrupted run had completed; tested by simulating the files such an interruption leaves). The final installer (31.5 MB) is built from the release-candidate executables (PGO, clean build of the current `main`) and passed the automated installer checks on US and PAL (installs, refusals, simulated failures, reinstalls, uninstall). The first release (0.1.0) was published before the clean-Windows test and the human installation test, which are still pending. Installer 0.1.1 (second release): the game has its own original icon (executable, game window and taskbar, shortcuts, installer and uninstaller), and the uninstaller deletes the saves and settings only if the player ticks a box (unticked by default) and confirms; checked automatically (icon read back from the executable and from the running game's window; the uninstall dialog driven on a hidden desktop), not yet looked at by a person. |

### Milestone 10 — no emulated hardware ✅

| Step | Subsystems replaced natively | State |
|---|---|---|
| 10a | Disc (DVD), gamepads (PAD/SI → SDL, configurable), audio RAM (AR/ARQ), video and frame pacing (VI), cache operations | ✅ |
| — | Saves (CARD → files), done right after 10a as milestone 7 | ✅ |
| 10b | OS threads and priority scheduling, alarms and time, interrupts, boot without the apploader or exception vectors | ✅ |
| 10c | Drawing: GX command stream → Aurora; vertex writes to the write-gather pipe are only accepted inside a draw | ✅ |
| 10d | Audio: DSP and AI native, AX voice processing ported from Dolphin | ✅ |
| — | Performance: inline RAM fast paths, optional local PGO | ✅ |

Result, checked automatically for both versions:
- the executable no longer contains the hardware layer, the cycle scheduler or the PowerPC interpreter;
- a counter of accesses to hardware addresses (`0xCC00xxxx`) stays at zero, except vertex writes during draws;
- against the last build that still emulated the hardware: the 3D intro is identical to the pixel; on the boot
  sequence, 4 of the 5 checkpoints are identical and the title screen appears one frame later (the native memory
  card is mounted during boot). The audio is the same signal,
  but in the two boot scenarios it now starts one video field earlier (20 ms in PAL, 16.7 ms in US), because the
  game no longer runs the disc's apploader;
- from step 10c on, every step has produced the same images, audio and saves as the step before, byte for byte,
  except for that one-field audio start, which appeared when the last alignment with the emulated build was
  removed (2026-10-13), and for a rare one-pixel difference of the US version near a loading screen, which came
  from the rendering race below (fixed). The save scenarios were recorded again at step 10b, because save dates now come
  from the console clock.
- the last two alignments with the emulated build were removed on 2026-10-19: the audio events are plain native
  timers in time base ticks, and interrupts due at the same moment are delivered in the order of the SDK priority
  table, like the console. Images, audio and saves of the five bench scenarios stayed the same, byte for byte, in
  both versions.

Performance in the heaviest scene measured so far (the birthday party), as frame time:

| | Without PGO | With PGO | Budget |
|---|---|---|---|
| PAL (50 Hz) | 27.8 ms | 22.4 ms | 40 ms |
| US (60 Hz) | 27.9 ms | 21.9 ms | 33.3 ms |

### Remaining work before stage 1 is complete

1. **Close milestone 6.** Compare the intro audio with a Dolphin capture, measure the US version, and get a
   listening test.
2. **Rendering race: fixed.** Aurora's command thread used to read game memory (vertex arrays, textures,
   palettes) when it processed a command, not when the game issued it, so the same commands could give different
   frames (backyard flicker, issue 2). The GX bridge now hands that thread copies taken at issue time, and the
   batched wake-ups are back on (party scene: about 5 to 6 % less host time, about 20 % less CPU time). Checked
   against Dolphin on the backyard (US and PAL): the new frames match it, while the old ones sometimes lost a house-wall
   shadow that Dolphin always shows. The same race explains the intermittent one-pixel difference of the US
   `overwrite-save` scenario (issue 10): that pixel shows a palette-textured plant behind the wall through a
   one-pixel gap, and late reads probably gave it other colors. Since the fix, 30 to 32 runs with three output-path
   lengths gave the same frames byte for byte; the native references, recorded before the fix, hold a different
   color for that pixel (the bench does not compare these neighbouring frames).
3. **Milestone 8**: input scenarios that play through the first level, real REL loading paths.
4. **Milestone 9**: key-scene comparison with Dolphin.
5. **Milestone 11**: the player installer (clean-Windows test, review, then GitHub release).
6. Phase review, then validation by the project owner.

### Optional tooling (outside the milestones)

- **Reverse engineering with REA** (`tools/rea.py`, `tools/dol2elf.py`): REA over Ghidra on the main executable of
  both versions, from the command line or as an MCP server, in a container with no network that runs as a non-root
  user. Not needed to build or play; see [Technical notes](guide/technical.md#optional-reverse-engineering-with-rea).

## Stage 2 — Full decompilation 🟡

Rewrite every function of the game (DOL and 16 REL modules) as readable C++ with meaningful names, verified
against the recompiled build by an automated oracle. The decompiled code contains no game data: tables, strings
and assets are read from the player's files.

| # | Milestone | Goal |
|---|---|---|
| D1 | Tooling | Mixed executable (decompiled and recompiled functions side by side), oracle, progress tracking |
| D2 | Batches | Decompile batch by batch (by unit or REL), each one checked by the oracle |
| D3 | No recompiled code, no virtual CPU | 100 % decompiled; DolRecomp no longer needed; the game runs on the PC clock. **First release of the C++ version**: the installer ships the decompiled executable instead of the recompiled one |
| D4 | Native memory | Native types, pointers and byte order; the 24 MB guest RAM disappears |
| D5 | No GameCube GPU commands | Models converted to native geometry; no GX command stream decoded at run time |

Progress: 12 functions decompiled and verified by the oracle, on both US and PAL ("verified" as defined below). Two
more, `hsd_hermite_interpolate` and `hsd_mtx_get_translation`, are decompiled but disabled: they start with a
floating-point instruction, and the gateway does not yet handle a call made while the floating-point unit is off (see
"Remaining work in D1" below).

- **Memory type layer** (D1 started 2026-10-27): `runtime/core/decomp/gc_mem.h` (big-endian scalars, 32-bit guest
  pointers, bit-fields, per-version globals, native-memory switch), checked by unit and compile-refusal tests in the
  three memory modes (local tests, not in the repository).
- **Gateway** between recompiled and decompiled functions (`runtime/core/decomp/gc_abi.h`: PowerPC EABI arguments and
  results, nested calls back into recompiled code) and per-version table `games/chibi-robo/decomp/functions.tsv`,
  checked against each version's code by `tools/build.py`. Twelve decompiled leaf functions replace their recompiled
  versions in US and PAL builds; the first four (three of them are called during the boot, through both paths a call can
  take: a direct call within the same block of generated code, and a call through the dispatcher). A source file whose
  functions are all disabled is not built. A store of decompiled code reaches the CPU layer exactly as a store of
  recompiled code does (the write journal used by the idle detection, and the clearing of a `lwarx` reservation),
  through a store hook of the type layer; checked by a local test against the real idle detection and by the oracle.
- **Single-precision arithmetic** (`runtime/core/decomp/gc_float.h`): one helper per instruction (`fadds`, `fsubs`,
  `fmuls`, `fmadds`, `fneg`) that gives the recompiled code's results bit for bit where plain host `float` arithmetic
  does not: the 25-bit rounding of the multiplier, the fused multiply-add with its half-way correction, the NaN
  rules, and the flush of tiny results in the non-IEEE mode (FPSCR[NI], which the SDK's `OSInit` sets and which is on
  in every state the oracle captured). The mode is passed explicitly by the caller. The native GX replacements of the
  runtime do not flush; a diagnostic build found no tiny result in them on the reference scenarios (issue 48).
  Adapted from DolRecomp's CPU runtime (`src/cpu/cpu.c`, GPL-3.0-or-later),
  whose helpers follow Dolphin's interpreter (`Interpreter_FPUtils.h`, Copyright 2009 Dolphin Emulator Project,
  GPL-2.0-or-later); it stays under GPL-3.0. Checked by a local differential test against DolRecomp's helpers with
  the non-IEEE mode off and on (no difference on 2.66 million cases per mode, at two optimization levels). Equal to
  DolRecomp is what the oracle needs; that every result equals the console is not proven (DolRecomp's `fmadds`, for
  example, does not flush tiny results in the non-IEEE mode, while Dolphin's interpreter does).
- **Oracle** (`tools/build.py --oracle`, `tools/decomp_oracle.py`, `runtime/core/decomp/gc_oracle.cpp`; not in the
  normal build): on every call of a decompiled function, an oracle build runs the recompiled original and then the
  decompiled version from the same state and compares the result registers, the registers the PowerPC EABI preserves
  and the bytes written, keeping the original's results. It also records which basic blocks of the recompiled
  original ran, and runs hand-written synthetic cases (`games/chibi-robo/decomp/oracle_cases/`, invented values) to
  reach branches the scenarios do not. A function is **verified** when, in every version, it showed no difference on
  every compared call and synthetic case, every basic block of the original reachable from its entry was exercised,
  and enough distinct real starting states were seen (and every call was compared). This is basic-block coverage (not
  edge or path coverage), not a proof on states that were never compared. Results from a run that did not finish,
  from a failing bench run, from other sources than the current ones, or from an oracle whose settings are not the
  defaults are refused, as are accidental copies of a run (forged evidence is out of scope: results are not signed).
- **Results**: over the five reference scenarios, the twelve verified functions were compared 7,402,837 times in US
  and 7,774,037 in PAL (every call compared), without a difference, together with their synthetic cases; all their
  blocks were covered (some branches only by the synthetic cases). The oracle also checks that every byte stored by
  the decompiled version reached the write journal and that the `lwarx` reservation ends in the same state; with the
  store hook removed (test mode), every compared call of each storing function is reported, and no read-only one.
  Every reference scenario stays identical to the bench references with the oracle on, and a deliberately wrong
  variant of each function is reported on every compared call in both versions (except a variant of the Hermite
  interpolation written with plain host float arithmetic, which differs on 12 % (US) and 14 % (PAL) of its calls:
  the share of calls where the console's single-precision rounding and fused multiply-add change the result).
- **Measure**: `tools/decomp_progress.py` (per version and per module; partition in the `[decomp]` section of
  `games/chibi-robo/game.toml`) counts 3,772 functions to decompile (DOL 3,297 and 16 RELs 475; 1,910,996 bytes of
  code): 12 verified (0.31 %; 468 bytes, 0.02 %), on both US and PAL. Left out: 1,209 DOL functions (14 of the
  `.init` start-up section and 1,195 of the libraries: native replacement, or decision pending for THP) and 20 REL
  functions (the Metrowerks runtime's global destructor chain, in 10 modules).
- **PAL addresses**: 97 of the functions to decompile have no established PAL address yet, so they cannot count as
  decompiled in PAL: in the DOL, 22 placed by position only and 65 whose PAL size comes only from the code partition
  (both enabled per version, once the oracle has passed on PAL), and 1 ambiguous match; 9 in the RELs. The other PAL
  addresses come from the transposition of the US symbols and from the gap filling of
  `games/chibi-robo/symbols/GGTP01-gaps.tsv` (levels strong and graph); `tools/build.py` refuses to enable a function
  whose address and size are not established that way in each version.
- **HAL library (sysdolphin) names** (#46): 67 of the 742 functions of the HSD range are now named in the symbol maps,
  in US and PAL. Before, only 2 were named: the cbr_decomp names `THPSimpleAudioStop__Fv` and
  `THPSimpleAudioResume__Fv` (US `0x801B66EC` / `0x801B6700`), which sit in the middle of `dobj.c` in the Melee
  decompilation and are doubtful. 64 of the 65 new names come from the Melee decompilation (names only, with its
  maintainers' permission), checked against xd-decomp (CC0), plus `HSD_AudioInit` (named by one of the game's own
  error messages). A name is kept only when this game's code confirms it: an assertion found only in that function, or two calls to functions
  already named; 15 of them were read by hand in the US and PAL disassembly. 382 more matches are recorded as
  hypotheses in `games/chibi-robo/symbols/supplement.tsv`, outside the maps.
- A function counts as decompiled only once it is `verified`; one that already replaces its recompiled version but is
  not verified yet (`unverified`) is shown in a separate column.
- **Class and exception-table scanners** (`tools/rtti_scan.py`, `tools/extab_scan.py`,
  [#42](https://github.com/Armandpro12/chibi-robo-pc/issues/42) points 3 and 4; see [Technical
  notes](guide/technical.md#optional-class-and-exception-table-scanners)): in both US and PAL, 241 vtables (203 in the
  DOL, 38 in 11 of the 16 REL modules), 227 classes with their inheritance, and about 1,040 virtual functions, each
  attached to the class that defines it (proposed names stay hypotheses, outside the symbol maps); 187 of the DOL's
  virtual functions per version are missing from Ghidra's function list. The exception tables give the exact bounds of
  380 (US) and 382 (PAL) functions, and none disagrees with the dtk symbols (US), the transposition (PAL) or Ghidra's
  function list. That confirms the reading of the index, not the bounds themselves: the checks are only partly
  independent (the symbol maps cover 3 entries; dtk names all the entries, which suggests it reads the same index;
  the PAL gap filler is fed with the index; only Ghidra's list is potentially independent, and whether its analysis
  uses the tables is not shown). 14 functions have cleanup actions. The REL modules have no exception tables.

### Remaining work in D1

1. **Floating-point unit off.** The game's OS turns the floating-point unit off when it switches threads and restores
   a thread's floating-point registers on that thread's first floating-point instruction. The recompiled original
   does this; the gateway does not, so a decompiled function that uses floating-point registers could read another
   thread's values (deduced from the mechanism, not measured). Planned: for a function whose original contains a
   floating-point instruction, the gateway lets the original run when the unit is off, and the oracle counts those
   calls apart. Then `hsd_hermite_interpolate` and `hsd_mtx_get_translation` are enabled again and rechecked by the
   oracle. `hsd_pobj_set_shape_blend`, verified, also moves a float with floating-point loads and stores; the
   scenarios never called it with the unit off, and it is part of the same fix.
2. Full A/B bench of the production build of this batch, then review and merge.

## Stage 3 — Multiplayer ⬜

1. Study what must be synchronized: player, objects, NPCs, scene state, area loading.
2. Network layer: host and client, tolerant of packet loss and latency.
3. A second Chibi-Robo that mirrors the other player's position and animation.
4. Interactions: picked-up objects, actions, area changes.
5. Multiplayer balancing.
6. Real test between two PCs.

## Stage 3b — PS Vita port ⬜

A faithful port to the console (same behavior as the PC version, no remaster features), started once the game is decompiled
(from milestone D3 at the earliest) and kept after the multiplayer stage so that the two-instance multiplayer test keeps
passing. Tracking: [#43](https://github.com/Armandpro12/chibi-robo-pc/issues/43). Free tools only, nothing from the game in the
package, and no protection is bypassed.

1. Feasibility note: CPU and memory budget, size of the extracted game, storage, which graphics API to use (GXM or vitaGL) and the
   estimated effort. Every figure is sourced or marked as a hypothesis (the console's specifications are not yet verified here).
2. Keep the core portable during the decompilation: platform, rendering, audio, input and saves are separate, with no dependency on
   Dawn, SDL or Windows outside their own backends.
3. A graphics backend of its own for the console (the PC one, Aurora on Dawn, does not exist there), prototyped on the title screen.
4. Build chain and a guided installation with the player's own disc image.
5. The game itself: input, audio, saves, rendering, loading, and native memory on the console's 512 MB.
6. Validation: reference scenarios, frame rate measured on the console, the two-instance multiplayer test, and a real test on a
   PS Vita.

## Stage 4 — Remaster ⬜

- **PC-style controls**: an in-game controls menu, actions named by what they do, mouse camera, button icons
  matching the device in use.
- **PC-style saves**: no more memory card wording or screens, several save slots, backup copies.
- **Separate audio volumes**: master, music, effects (and voices if possible).
- **Remastered 3D models**: format documented, glTF export and import, replacement models loaded by the runtime.
- **Optional HD pack**: distributed separately (never in this repository), installed and verified by the installer.

## Stage 5 — Modding platform ⬜

| # | Milestone |
|---|---|
| M1 | Mod loader and mod manifests |
| M2 | Extractor: every game file to open formats |
| M3 | Replace any game file (textures, models, sounds, music, fonts, UI) |
| M4 | Text and translations, including new languages |
| M5 | Levels and event scripts |
| M6 | C++ plugins with a stable, versioned C API |
| M7 | Lua scripts in a sandbox |
| M8 | Documentation and example mods for modders |

## Known differences with Dolphin

Measured on the native build against Dolphin 5.0 (same inputs, scenes aligned by content). None of them is a
rendering or sound defect of the port that we know of; most come from the game's own random numbers or from the
timing of the simulated disc.

- **Disc timing.** The native disc model has no seek time. Verified: the silence the game leaves at the loop point of
  the title music follows the simulated disc speed, and it is 60 to 85 ms shorter than in Dolphin, which simulates a
  seek. Loading is also 2 to 3.6 times faster in the port, probably for the same reason. Which model is closer to the
  real console is unknown; a seek-time model could be added as an option.
- **Random sounds and animations.** Verified: the background of the title screen depends on the game's random
  generator (with the same seed on both sides the title scores 0.990 to 0.996 in SSIM), and so do the backyard
  ambient sounds, which depend on what happened before the scene. With the same generator state on both sides the US
  backyard plays the same sounds at the same instants (72 of 118 anchors identical to the sample, one periodic sound
  5 ms off); in PAL the gap is strongly reduced but only partly analysed. The ambient sounds of the living room still
  differ in small ways whose cause is not established. For the first seconds of the party, the random generator state
  was forced to the same value on both sides when the party module is loaded (US and PAL): the captured images and
  the audio did not change, so the random generator is not the cause there. The cause is not established; the
  remaining candidates are the game clock and the moment the loading ends.
- **Menus.** The scrolling background and the bouncing letters of the file menus are not in the same phase as in
  Dolphin; text and frames are identical. The cause is not established (probably an animation tied to the time since
  start-up).
- **Stretched viewport.** In the default `stretch` window mode the 3D image scores about 0.015 lower in SSIM than
  with `fit`; probably resampling, since Dolphin's frames are resized to compare them.
- **Video (credits).** Compared in both versions: same duration, and the sound is bit-identical in 99.8 to 99.9 % of
  the 5 ms blocks; both sides insert a few very short runs of zero samples (at most 1.2 ms) at different moments.
  In PAL the picture scores 0.937 to 0.95 in SSIM with the same picture and geometry; the gap is probably resampling
  (Dolphin dumps it at 640x468 instead of 640x528).
- **Video (basement, US and PAL).** Reached from a new game (scenario `basement-movie`): every frame of the silent movie
  is shown (PAL: 1150 frames in 46.0 s), picture SSIM 0.96 to 1.0 in the body of the movie against Dolphin (the fade-in pairs are unreliable). In Dolphin the movie repeats frames
  during its first ~23 s, and the music over it is close to, not identical with, the native one. Second movie: not yet.

## Known issues

- Player reports with late saves (US version):
  - **crashes** after a few minutes to 45 minutes of play (access violations). Not reproduced yet in scripted
    sessions of up to 50 minutes. In the first full crash report, the checks of `tools/crash_inspect.py` find no
    cause in the faulting instruction (a valid write that had succeeded at the same address four instructions
    earlier). This suggests, as a hypothesis only, a fault outside the program; more reports are needed before
    drawing any conclusion. Crash reports now identify the exact executable (file name, SHA-1, build commit), the
    processor and the faulting code bytes, without any local path, and `tools/crash_inspect.py` checks whether a
    fault is explained by its instruction. They also give the microcode and firmware versions, the graphics driver,
    the memory regions, the call stacks of every thread at the time of the fault, the last native events and log
    lines, and whether the same crash already happened on that computer; a watchdog writes a report when the game
    freezes. Its minidump is written from a snapshot of the process, so no thread stays suspended while it is
    written, which removes the lock deadlock found in tests (issue 36). A second report, from the same kind of
    machine, faulted at the same instruction: no software cause
    was found, and the crashing build was our current code. Two tools help players and testers tell an unstable
    machine from a bug: `tools/collect_diagnostics.cmd` (read-only, anonymized: hardware error events, BIOS,
    microcode, drivers, overlays, recent crash reports; nothing is sent) and `tools/cpustress/` (runs the
    recompiled-code instruction patterns on every core and checks every result; a PASS does not prove the
    processor is sound);
  - **large translucent shapes on the living-room floor**: checked, not a bug. They are the carpet texture plus a
    floor reflection the game draws on purpose; Dolphin shows the same in a matched view (US checked, PAL draw
    parameters confirmed but not compared);
  - **missing music**: in the places checked so far (the Chibi-House, the living room at night) the port plays
    exactly what Dolphin plays.
- An intermittent crash at the start of 3D rendering was seen early on; it has not been reproduced in 2,400
  launches of the release executables (1,200 per version), which does not prove it fixed.
- A few TEV color registers get other values than in Dolphin, because the game builds them partly from
  uninitialized stack memory and the native SDK functions do not leave the same bytes on the stack (issue 9). Checked:
  the values are the same from one run to the next, and frames do not change at all when that stack memory is
  filled with other bytes (US: menus, loading and 3D scenes; US and PAL: backyard), so these channels are not used
  there. Left as is.
- First-launch pause to compile graphics pipelines (issue 47): with an empty pipeline cache, about 37 s (US) or
  72 s (PAL) into a new or loaded game, the game asks for 143 new pipelines before one EFB-to-texture copy and waits
  for all of them, so that no draw is skipped. Aurora patch `0012` compiles them on several threads at high
  priority: on the 20-thread development machine the wait went from about 10 s to about 1.8 s (US and PAL), and
  under an artificial CPU load on 16 of the 20 threads from about 16 s to about 2 s (US and PAL, one run each);
  frames, sound and saves unchanged. A kept cache (later launches) avoids the wait, as before. Not measured on a
  machine with few cores or with the Vulkan backend. That Dawn is safe for concurrent pipeline creation under
  `ImplicitDeviceSynchronization` is assumed, not checked against its documentation (no crash in the measurement
  runs).
- The comparison with Dolphin is partial: images of the key scenes and sound of the title screen, party, living room
  and backyard in both versions, with the differences listed in "Known differences with Dolphin"; only the credits
  video and the basement video are compared, and the rest of the game is not.

## How changes are validated

Every code change goes through an automated A/B bench (images, audio and saves compared with native reference runs,
in PAL and US), a check that no hardware access remains and save round-trip tests. Reviews: a separate AI review agent
before each merge into `main`, and the project owner's validation at the end of each stage. The bench references and
the regression tests live in the project's local `tests/` folder, which is not published (it holds output of the
game).
