# Roadmap

Where the project stands, what has been done and what remains. Updated at every milestone.

**Last update: 2026-10-14.**

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
| 8 | Playable game | ⬜ | Replayed inputs must progress through the first level, then the whole game. |
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
  removed (2026-10-13), and for the rare one-pixel difference of the US version listed under
  [Known issues](#known-issues). The save scenarios were recorded again at step 10b, because save dates now come
  from the console clock.

Performance in the heaviest scene measured so far (the birthday party), as frame time:

| | Without PGO | With PGO | Budget |
|---|---|---|---|
| PAL (50 Hz) | 27.8 ms | 22.4 ms | 40 ms |
| US (60 Hz) | 27.9 ms | 21.9 ms | 33.3 ms |

### Remaining work before stage 1 is complete

1. **Close milestone 6.** Compare the intro audio with a Dolphin capture, measure the US version, and get a
   listening test.
2. **Fix a rendering race.** Aurora's render thread reads game memory when it processes a command, not when the
   game issues it, so one frame of the party scene can differ from run to run. Once the data is captured at issue
   time, re-enable the batched render-thread wake-ups (about 8 to 11 % faster).
3. **Remove the last compatibility shims of milestone 10.** They exist only to stay bit-identical with the old
   emulated build: a cycle-dated audio event queue and an interrupt delivery order. Removing them changes the
   outputs, so the native reference recordings will be made again. The boot-cycle offset is already gone: since
   2026-10-13 every reference recording of the bench is native.
4. **Milestone 8**: input scenarios that play through the first level, real REL loading paths.
5. **Milestone 9**: key-scene comparison with Dolphin.
6. **Milestone 11**: the private installer.
7. Phase review, then validation by the project owner.

Small tooling fixes are also pending: PGO staleness warnings for generated code, and a misleading message in
`tools/pgo.py`.

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

- An intermittent crash at the start of 3D rendering was seen early on; it has not been reproduced in more than
  1,200 runs since.
- A few TEV color registers differ from the reference because the game reads uninitialized stack memory; no
  visible effect has been measured.
- The comparison with Dolphin is partial. PAL: the five boot screens and the title music. US: only the health and
  safety warning screen, and no audio measurement yet.
- In the US version, one pixel of a frame next to the loading screen of a save scenario sometimes differs from
  one run to the next. It already happened before the latest changes and depends on the memory layout of the run;
  its exact cause is not established yet.

## How changes are validated

Every code change goes through an automated A/B bench (images, audio and saves compared with native reference runs,
in PAL and US), a check that no hardware access remains and save round-trip tests. Reviews: a separate AI review agent
before each merge into `main`, and the project owner's validation at the end of each stage. The bench references and
the regression tests live in the project's local `tests/` folder, which is not published (it holds output of the
game).
