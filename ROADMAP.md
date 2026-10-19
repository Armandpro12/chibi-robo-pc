# Roadmap

Where the project stands, what has been done and what remains. Updated at every milestone.

**Last update: 2026-10-16.**

Legend: ✅ done · 🟡 in progress or partly done · ⬜ not started

## The plan at a glance

The project moves through five stages, **strictly one after the other**. A stage starts only once the previous
one has been validated.

| # | Stage | Goal | State |
|---|---|---|---|
| 1 | Faithful native port | The original game running natively on PC, with no emulated hardware | 🟡 in progress |
| 2 | Full decompilation (D1–D5) | The whole game rewritten as readable C++; first public release | ⬜ |
| 3 | Multiplayer | Two players in the same house | ⬜ |
| 4 | Remaster | PC-style controls and saves, audio settings, remastered models, optional HD pack | ⬜ |
| 5 | Modding platform (M1–M8) | Every game file replaceable; levels, C++ plugins and Lua scripts | ⬜ |

From stage 3 onwards, every stage must keep multiplayer working (automated two-instance test).

**Release policy.** The only public release will be the fully decompiled C++ version (stage 2, milestone D3): an
installer with a precompiled executable that contains no game data, which asks for the player's own disc image.
Nothing is compiled on the player's machine. Until then, the game can only be built locally (see the
[README](README.md)).

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
| 5 | Rendering | ✅ | PAL: the five boot screens, from the 50/60 Hz prompt to the title screen, match Dolphin captures above the documented SSIM threshold. US: only the health and safety warning has been compared with Dolphin. |
| 6 | Audio | 🟡 | Music and effects work. Title music matches Dolphin (peak and RMS); still to do: intro and US comparison against Dolphin, and a listening test. |
| 7 | Saves | ✅ | Native PC saves: card always inserted, crash-safe writes (temporary file, then rename). |
| 8 | Playable game | 🟡 | Scripted play sessions of up to 50 minutes from late saves run without a crash; open issues from player reports are listed under "Known issues". |
| 9 | Comparison with Dolphin | ⬜ | Key scenes compared; differences listed. |
| 10 | No emulated hardware | ✅ | See below. |
| 11 | Installer (private) | ⬜ | Disc check, extraction, install and uninstall, tested on a clean Windows. It stays private until the C++ version exists. |

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
5. **Milestone 11**: the private installer.
6. Phase review, then validation by the project owner.

## Stage 2 — Full decompilation ⬜

Rewrite every function of the game (DOL and 16 REL modules) as readable C++ with meaningful names, verified
against the recompiled build by an automated oracle. The decompiled code contains no game data: tables, strings
and assets are read from the player's files.

| # | Milestone | Goal |
|---|---|---|
| D1 | Tooling | Mixed executable (decompiled and recompiled functions side by side), oracle, progress tracking |
| D2 | Batches | Decompile batch by batch (by unit or REL), each one checked by the oracle |
| D3 | No recompiled code, no virtual CPU | 100 % decompiled; DolRecomp no longer needed; the game runs on the PC clock. **First public release**: lightweight installer with a precompiled executable |
| D4 | Native memory | Native types, pointers and byte order; the 24 MB guest RAM disappears |
| D5 | No GameCube GPU commands | Models converted to native geometry; no GX command stream decoded at run time |

## Stage 3 — Multiplayer ⬜

1. Study what must be synchronized: player, objects, NPCs, scene state, area loading.
2. Network layer: host and client, tolerant of packet loss and latency.
3. A second Chibi-Robo that mirrors the other player's position and animation.
4. Interactions: picked-up objects, actions, area changes.
5. Multiplayer balancing.
6. Real test between two PCs.

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
    freezes. A second report, from the same kind of machine, faulted at the same instruction: no software cause
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
- An intermittent crash at the start of 3D rendering was seen early on; it has not been reproduced in more than
  1,200 runs since.
- A few TEV color registers get other values than in Dolphin, because the game builds them partly from
  uninitialized stack memory and the native SDK functions do not leave the same bytes on the stack (issue 9). Checked:
  the values are the same from one run to the next, and frames do not change at all when that stack memory is
  filled with other bytes (US: menus, loading and 3D scenes; US and PAL: backyard), so these channels are not used
  there. Left as is.
- The comparison with Dolphin is partial. PAL: the five boot screens and the title music. US: only the health and
  safety warning screen, and no audio measurement yet.

## How changes are validated

Every code change goes through an automated A/B bench (images, audio and saves compared with native reference runs,
in PAL and US), a check that no hardware access remains and save round-trip tests. Reviews: a separate AI review agent
before each merge into `main`, and the project owner's validation at the end of each stage. The bench references and
the regression tests live in the project's local `tests/` folder, which is not published (it holds output of the
game).
