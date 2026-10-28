# Technical notes

[Back to the README](../README.md)

- [How the port works](#how-the-port-works)
- [Current state in detail (October 2026)](#current-state-in-detail-october-2026)
- [Graphics backends](#graphics-backends)
- [Aurora patches](#aurora-patches)
- [Process exit priority](#process-exit-priority)
- [Repository layout](#repository-layout)
- [Optional: reverse engineering with REA](#optional-reverse-engineering-with-rea)

## How the port works

*Chibi-Robo!* was released on the Nintendo GameCube in 2005/2006. The game's PowerPC code is statically recompiled to C (with [DolRecomp](https://github.com/ExpansionPak/DolRecomp)),
and every Nintendo SDK function that talks to the console hardware is replaced by a native PC implementation.
**Nothing is emulated at the hardware level any more**: no hardware registers, no cycle-driven devices, no
interpreter. Drawing goes through [Aurora](https://github.com/encounter/aurora) (GX on top of Dawn, the WebGPU
implementation, and SDL3 for the window and input). The long-term goal is a fully decompiled, readable C++ game,
then multiplayer, a PS Vita port, a remaster and a modding platform ([ROADMAP.md](../ROADMAP.md)). Our findings about the game
itself: [RESEARCH.md](../games/chibi-robo/RESEARCH.md).

## Current state in detail (October 2026)

- Runs on Windows, in real time with music and sound effects, from boot through the title screen, a new game, the
  prologue up to and including the birthday party, the first save, and the first exit from the Chibi-House with the
  first module load and unload, on both versions. Scripted game sessions run without a crash: 50 game minutes from a
  late save (US) and 30 minutes from a new-game save started in the backyard (PAL).
- Native saves, like a PC game: a memory card is always inserted; save files (`.gci`, Dolphin-compatible) live in
  `%USERPROFILE%\Saved Games\Chibi Robo PC\Chibi Robo\`.
- Configurable keyboard and gamepads (`controls.ini`, see [settings-and-controls.md](settings-and-controls.md#controls)).
- Validated automatically by the project against native reference runs (images, audio, saves), for both the PAL
  and the US versions. The bench references and the regression tests live in a local `tests/` folder that is not
  published (it holds output of the game), so these checks cannot be rerun as is from this repository;
  `tools/ab.py record` records references from your own build.
- Compared with Dolphin on the key scenes, in both versions: images of the boot screens, title screen, menus,
  loading screen, birthday party, living room and backyard, and the sound of the title screen, party, living room and
  backyard (music loops checked). Known differences are listed in the ROADMAP
  ([Known differences with Dolphin](../ROADMAP.md#known-differences-with-dolphin)). Of the video cutscenes, the
  credits video and the basement video are compared so far; the others, including the second basement video, are not.
- Not yet verified: playing through the whole game (milestone 8 of the roadmap), the other video cutscenes,
  listening and playing by a human, and a clean Windows (milestone 11).

## Graphics backends

The player chooses Auto, Direct3D 12 or Vulkan in the setup menu ([settings-and-controls.md](settings-and-controls.md#graphics-backend)).
Auto uses Direct3D 12, with Vulkan as the alternative. Direct3D 12 compiles its shaders with FXC
(`d3dcompiler_47.dll`, taken from the Windows system folder; present on the development machine, not established on
every Windows), so no extra DLL is needed next to the executable.

If the backend asked for cannot start, Aurora goes on by itself with its own order (Direct3D 12, Vulkan, then its
last resort Direct3D 11), and the game says so on stderr and in the trace.

**Software renderer.** When no GPU can be used at all, a software (CPU) adapter is tried as a last resort, before
null (no picture): WARP, Microsoft's software renderer for Direct3D 12 (Aurora patch 0009). The game then says "no
usable GPU: using the software renderer ..., the game will be slow" on stderr and in the trace. This was checked on
the development machine with the test switch `AURORA_TEST_SKIP_HARDWARE=1` (hardware adapters ignored): both
versions reach the title screen with the same sound, and the title-boot captures differ by 1 or 2 colour levels on
under 1 % of the pixels (plus two isolated pixels on one PAL capture); a real PC without a GPU has not been tested.

**How the backends compare.** On the development machine, Direct3D 12 and Vulkan give the same picture up to tiny
rounding differences: the title-boot captures of both versions are identical, and in some 3D scenes Vulkan differs by
1 or 2 colour levels on at most 0.05 % of the pixels. Direct3D 11 is not offered: it is only Aurora's last resort and
is less tested (its US picture was stretched until Aurora patch 0008; the title-boot captures are now identical on
both versions). For tests only, `GCRT_TEST_ALL_BACKENDS=1` lets `--backend` and `GCRT_BACKEND` name `d3d11` or
`null`. The trace line `graphics backend: ... in use, requested ...` and crash reports name the backend actually
used.

## Aurora patches

[Aurora](https://github.com/encounter/aurora) (MIT) is used with eleven small patches, kept in
`tools/patches/aurora/` and applied by `tools/build.py`:

1. `0001` lets the render target be taller than the displayed image (the console's 528-line EFB).
2. `0002` snaps the viewport, scissor and copy edges to whole target pixels, so that a non-integer window scale does
   not leave a black line at the bottom of the shadow map.
3. `0003` moves the buffer of very large texture uploads (more than 24 MiB in one frame) from the game thread to
   Aurora's render worker, so that creating and unmapping it no longer races with the worker on the Dawn device.
4. `0004` sets a busy timeout on the SQLite connections of Aurora's pipeline and Dawn caches, so that two game
   processes sharing one cache folder mostly wait for each other's locks instead of failing with "database is
   locked" (hardening only: it is not a fix for the intermittent crash of issue 11).
5. `0005` creates Aurora's cached bind groups on the render worker instead of the GX command thread, which removes
   one identified race on Dawn's unsynchronized D3D12 sampler descriptor cache (other Dawn calls, such as sampler and
   texture creation, still happen on the GX thread, and the crash of issue 11 is not shown to be fixed).
6. `0006` turns off Dawn's `use_dxc` toggle: the Direct3D 12 backend compiles its shaders with FXC
   (`d3dcompiler_47.dll`, found in the Windows system folder on the development machine; its presence on every
   Windows, and in Windows Sandbox, is not established) instead of DXC, so `dxcompiler.dll` and `dxil.dll` are
   neither needed nor copied next to the executable.
7. `0007` requests Dawn's `ImplicitDeviceSynchronization` feature when the adapter offers it, so that Dawn
   serializes the calls made on its device by the game thread, Aurora's GX FIFO processor and its render worker with
   an internal lock (command encoding excepted; a warning is logged if the adapter does not offer the feature;
   `AURORA_IMPLICIT_DEVICE_SYNC=0` leaves it off, for comparisons). It was measured to cost nothing visible on the
   development machine and is not shown to fix issue 11 by itself.
8. `0008` clamps the viewport depth range to [0, 1], as WebGPU requires: the SDK's depth scale puts the far plane a
   hair past 1, and Dawn's Direct3D 11 backend then ignored the whole viewport, which stretched the US picture
   (issue 38).
9. `0009` tries a software (CPU) adapter, such as WARP, only after every hardware adapter failed, instead of falling
   back to the null backend (no picture).
10. `0010` keeps the copied descriptor of a deferred bind group (fifth patch) when Dawn returns no bind group, so
    that the next use tries again instead of creating an empty one (it also passes the entries through Dawn's own
    conversion operator instead of a size-checked cast), and brings the comments of the third and fifth patches up
    to date with the seventh.
11. `0011` logs at debug level only, instead of as a warning, the staging buffer maps that Dawn aborts because Aurora
    is shutting down ("Buffer mapping Aborted: Buffer was destroyed before mapping was resolved", seen at the end of
    most runs); an abort while the game runs is still a warning.

`tools/build.py` finds how many of the patches a checkout already has by rebuilding each step of the series in a
temporary Git index (with its own temporary object directory, so the Aurora repository is not written to), since a
later patch may change lines that an earlier one added.

## Process exit priority

Once the game is over, the process switches to the high priority class for the rest of its life, and a helper thread
raises each of its threads to the highest priority level until the game's main function returns, so that a busy
computer does not keep it waiting for its low-priority threads (it could take minutes). `GCRT_EXIT_BOOST=0` turns
this off, for diagnosis; the trace's `exit:` lines give the times of the end in host seconds.

## Repository layout

- `runtime/core/` — generic GameCube runtime, reusable for other games: native replacements of the SDK
  (OS, DVD, VI, PAD, ARAM, CARD, GX through Aurora, DSP/AI/AX audio), CPU support for the recompiled code,
  platform layer (window, input, audio output, user folders).
- `games/chibi-robo/` — everything specific to Chibi-Robo!: game description (`game.toml`), recompiler
  configuration and hooks, symbol maps, reference scenarios.
- `tools/` — generic scripts: environment check, tool installation, extraction, build, run, A/B comparison,
  Dolphin reference capture, PGO, safe removal of worktrees that contain junctions (`tools/worktree.py`).
- `guide/` — this documentation.
- `orig/`, `generated/`, `external/`, `build/` — your disc image, locally generated files, third-party tools and
  build outputs. Never committed.

Code comments sometimes refer to the project's private working notes (`notes/…`, `docs/…`); these are not
published.

## Optional: class and exception-table scanners

Two read-only scanners help the symbol maps, the Ghidra/REA analysis and the decompilation. They read the extracted
DOL and REL files (pure Python, no Ghidra, about a second per version) and write their reports under
`build/rtti/<ID>/` and `build/extab/<ID>/` (derived from your copy of the game: never committed). Both run on every
version of `game.toml` by default; `--help` gives every option.

```
py -3 tools/rtti_scan.py chibi-robo            # classes, inheritance, vtables, virtual functions
py -3 tools/extab_scan.py chibi-robo           # exception tables, function bounds, cleanup actions
```

- `tools/rtti_scan.py` walks the CodeWarrior RTTI objects (class name and base list) and the vtables that point to
  them, in the DOL and in the relocated REL modules (`tools/gcimage.py` applies the modules' relocations). It gives
  the class hierarchy, every vtable with its slots, and for each virtual function the class that defines it (the
  topmost class with the same function in the same slot). Names it proposes (`__vt__<n><Class>`,
  `__dt__<n><Class>Fv` for a slot-0 function that stores its own vtable, `<Class>::vfunc_<slot>` otherwise) are
  hypotheses: they are never written into the symbol maps. It cross-checks existing names (symbol map, dtk symbols of
  the reference version) against the class of their mangled name, and checks that every slot is a function start.
- `tools/extab_scan.py` reads `_eti_init_info`, `extabindex` and `extab`: the exact bounds of every function that has
  an entry, its saved registers (checked against its prologue) and its cleanup actions (with the destructors they
  call). It compares those bounds with the symbol map, the dtk symbols (reference version), the transposed and
  gap-filled functions (other versions) and Ghidra's function list, and lists the functions of the covered range that
  have no entry. Read an agreement as a check of the reading of the index, not as an independent proof of the bounds:
  the symbol map covers only a few entries, the dtk symbols name every entry (so they probably come from the same
  index), and the gap filler of the transposition is itself fed with the exception index; only Ghidra's list is
  potentially independent.

Formats: the extab entry layout, the action names, their numbering and their sizes follow the decoder of
[cwextab](https://github.com/CelestialAmber/cwextab) (MIT OR Apache-2.0; no code copied); the RTTI and vtable layout
was measured on this game's two versions (the header of `tools/rtti_scan.py` says which parts are hypotheses). Known
limits:

- Virtual inheritance, templates and secondary vtables do not occur in this game, so their decoding is untested.
- A vtable's last zero words cannot be told apart from padding, so they are reported separately.
- Only 7 of the 17 action types of the exception tables occur in this game's tables (`destroy-local`,
  `destroy-local-cond`, `branch`, `specification`, `active-catch-block`, `terminate`, `catch-block-32`). The sizes and
  offsets of the other 10 come from cwextab and are not tested on the game's data. Of the flags, only bits 6-15 (the
  saved register counts) are cross-checked, against the function prologues.
- The REL modules of this game have no exception tables.

Exit codes: 0 when every version was scanned, 2 for an unknown game or version, 1 when an input is missing or is not a
DOL. Local tests: synthetic DOL and REL files built by the test (no game data) plus checks on both versions, among
them that every relocated call of the REL modules reaches a function start.

## Optional: reverse engineering with REA

[REA](https://github.com/morluto/rea) (Reverse Engineer Anything, MIT) answers questions about a binary through
Ghidra: overview, functions, pseudo-C, callers and references, from the command line or as an MCP server for an
AI agent. It is not needed to build or play. REA reads 32-bit big-endian PowerPC ELF executables, on Linux or
macOS only, and does not read GameCube DOL files, so:

- `tools/dol2elf.py` converts the extracted DOL (plus the version's symbol map) into such an ELF, byte for byte,
  under `generated/<game>/<ID>/rea/` (derived from your copy of the game: never committed). It checks the result
  against the DOL, and with `llvm-readelf` when available;
- `tools/rea.py` runs REA in a Linux container (Docker), with the Ghidra of `external/ghidra` mounted read-only.
  The container has no network, no Linux capabilities and runs as a non-root user. The image is built from
  `tools/rea/Dockerfile` (REA 6.7.0, Node.js 22, JDK 21): the base image, Node.js and REA are pinned; REA's
  transitive npm dependencies and the build stage's apt packages are not, and npm install scripts are not run.
  On a Linux or macOS host with REA installed, `--native` runs it directly.

Requirements: Docker, Ghidra with the GameCube loader (`tools/bootstrap.py`), an extracted game
(`tools/extract.py`). Build the image once (it downloads the base image, Node.js, REA and its npm
dependencies; `run` and `mcp` never build it themselves). Both versions work the same way:

```
py -3 tools/rea.py chibi-robo image
py -3 tools/rea.py chibi-robo --version GGTE01 run -- analyze {elf} --provider ghidra --json
py -3 tools/rea.py chibi-robo --version GGTP01 run -- function {elf} OSInit --provider ghidra --json
py -3 tools/rea.py chibi-robo --version GGTE01 mcp-config      # registration for an MCP client (`rea mcp`)
```

`{elf}` stands for the ELF as REA sees it. The Ghidra language is the Gekko/Broadway one of the GameCube loader
(`[rea]` in `games/chibi-robo/game.toml`), so paired-single instructions are decoded. The small-data registers
(r13, r2) are scanned from the entry point and its first calls and given to Ghidra, so small globals show as
addresses rather than `unaff_r13 + offset`. Limits: each command-line call imports the program again (about
2 minutes); the MCP server keeps it open between questions. Only the main executable is converted, not the REL modules.
