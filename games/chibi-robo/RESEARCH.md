# Chibi-Robo! (GameCube): research notes

Verified facts about the PAL and US executables of *Chibi-Robo!*, for anyone who decompiles, ports or analyses the game.

This document is dedicated to the public domain under CC0 1.0 (see `/LICENSES/CC0-1.0.txt`); it contains no game code or data.

Date: 2026-10-13.

## Contents

1. [Conventions and provenance](#1-conventions-and-provenance)
2. [Disc images and versions](#2-disc-images-and-versions)
3. [Main executable (DOL)](#3-main-executable-dol)
4. [REL modules](#4-rel-modules)
5. [Differences between PAL and US](#5-differences-between-pal-and-us)
6. [Nintendo SDK and other libraries](#6-nintendo-sdk-and-other-libraries)
7. [How SDK functions were identified](#7-how-sdk-functions-were-identified)
8. [Boot sequence and main loop](#8-boot-sequence-and-main-loop)
9. [Threads, alarms, interrupts and busy-waits](#9-threads-alarms-interrupts-and-busy-waits)
10. [Audio](#10-audio)
11. [Drawing (GX)](#11-drawing-gx)
12. [Memory card](#12-memory-card)
13. [Disc file formats](#13-disc-file-formats)
14. [Pitfalls for reimplementations](#14-pitfalls-for-reimplementations)
15. [Sources and credits](#15-sources-and-credits)
- [Appendix A. SDK functions unnamed in cbr_decomp](#appendix-a-sdk-functions-unnamed-in-cbr_decomp)
- [Appendix B. Game and HAL library functions](#appendix-b-game-and-hal-library-functions)

---

## 1. Conventions and provenance

**Versions**: `GGTE01` revision 0 (US) and `GGTP01` revision 0 (PAL). The Japanese and Wii releases were not studied.

**Addresses**: virtual, hexadecimal. Tables give the US address first, because `eavpsp/cbr_decomp` targets the US version. "US / PAL" in running text gives both.

**Every entry is verified.** Unconfirmed findings were left out. The tag says how each entry was checked:

| tag | meaning |
|---|---|
| `[verified: file]` | read directly from the disc image, the DOL or REL headers |
| `[verified: Ghidra]` | disassembly read in Ghidra |
| `[verified: ref]` | same function order, sizes and calls as the CC0 reference SDK sources in `cbr_decomp/decomp_dtk_main`, cross-checked in Ghidra (call graph and hardware register accesses) |
| `[verified: trace]` | observed at run time in our static recompilation, which executes the game's own code translated instruction by instruction, on US and PAL |
| `[verified: Dolphin]` | observed in the Dolphin emulator, or read in its source |
| `[verified: signatures]` | masked-code signatures (section 7) compared between the US and PAL DOLs |
| `[verified: REL relocations]` | read from the relocation tables of the 16 REL files |

**Provenance**:
- Names from `cbr_decomp/decomp_dtk_main/config/GGTE01/symbols.txt` (CC0) are cited as such. The appendices list only functions that this file leaves as `fn_XXXXXXXX`.
- The unlicensed parts of `cbr_decomp` (repository root, `ChibiRobo_Dol_Inject/` and its Ghidra map) are not a source of this document; every name here was confirmed independently.
- No name comes from the Melee decompilation (unlicensed). HAL library functions are described by role, unless the game binary itself names them.
- No game data is reproduced: no code bytes, no tables, no text and no file contents from the game. Only names (including source file names and save file names), addresses, sizes and hashes, with layouts and behaviour described in our own words.

---

## 2. Disc images and versions

| | US (`GGTE01`) | PAL (`GGTP01`) |
|---|---|---|
| Revision | 0 | 0 |
| SHA-1 of the `.iso` | `9052f186411b49845323eaaeb014755bcf96321b` | `f321aa7fb1ae5101ec60f1bbca8184898142135a` |
| Files in the FST | 144 | 147 |
| SHA-1 of `sys/main.dol` | `103c68e107b691d905306ecb6bbc43ea44893157` | `d823451aab3e022eedde5bd0a9fac04aa7d51e2e` |
| Size of `main.dol` | 2,483,872 bytes | 2,512,000 bytes |
| Files in `qp.bin` (U8 archive) | 1,153 | 1,321 |
| Language folders in `qp.bin/bin/` | `e` | `e f g i s` |
| 50/60 Hz prompt at boot | no | yes |

All `[verified: file]`, except the 50/60 Hz prompt `[verified: Dolphin, trace]`.

---

## 3. Main executable (DOL)

Entry point `0x80005240` (`__start`) in both versions `[verified: file]`.

| segment | US address | US size | PAL address | PAL size |
|---|---|---|---|---|
| text0 (`.init`) | `0x80003100` | `0x2500` | `0x80003100` | `0x2500` |
| text1 (`.text`) | `0x80007880` | `0x1F5DE0` | `0x800078A0` | `0x1F87C0` |
| data0 | `0x80005600` | `0x1080` | `0x80005600` | `0x1080` |
| data1 | `0x80006680` | `0x1200` | `0x80006680` | `0x1220` |
| data2 | `0x801FD660` | `0x60` | `0x80200060` | `0x60` |
| data3 | `0x801FD6C0` | `0x20` | `0x802000C0` | `0x20` |
| data4 | `0x801FD6E0` | `0x124E0` | `0x802000E0` | `0x161E0` |
| data5 | `0x8020FBC0` | `0x4C640` | `0x802162C0` | `0x4CA60` |
| data6 (`.sdata`) | `0x80659EC0` | `0x1280` | `0x80660D40` | `0x12A0` |
| data7 (`.sdata2`) | `0x8065BD40` | `0x4220` | `0x80662BE0` | `0x44C0` |
| BSS (whole range) | `0x8025C200` | `0x4040F0` | `0x80262E00` | `0x404638` |

All `[verified: file]`.

**Register bases** set by `__init_registers` `[verified: Ghidra]`:

| | US | PAL |
|---|---|---|
| r1, initial stack top (end of BSS + 64 KiB) | `0x806702F0` | `0x80677438` |
| r2, `_SDA2_BASE_` (data7 + `0x8000`) | `0x80663D40` | `0x8066ABE0` |
| r13, `_SDA_BASE_` (data6 + `0x8000`) | `0x80661EC0` | `0x80668D40` |

**BSS zones** cleared by `__init_data` `[verified: Ghidra]`:
- US: `0x8025C200`+`0x3FDCB0`, `0x8065B140`+`0xBF8`, `0x8065FF60`+`0x390`;
- PAL: `0x80262E00`+`0x3FDF30`, `0x80661FE0`+`0xBF8`, `0x806670A0`+`0x398`.

**Inside text0** `[verified: Ghidra]`:
- `0x80003154`–`0x80005088` holds the TRK debugger's interrupt vector table, mostly zero padding. It is copied onto the exception vectors only if `InitMetroTRK` runs, which never happens on a retail console.
- `_rom_copy_info` at `0x80005544` has 10 entries, all with source = destination: `__init_data` copies nothing. `_bss_init_info` at `0x800055C8` lists the 3 BSS zones.

**OS tables in low memory** `[verified: Ghidra]`: exception handlers at `0x80003000` (15 pointers), interrupt handlers at `0x80003040` (32 pointers), linked-module list at `0x800030C8` (head, tail; name table at `0x800030D0`).

**Global game object** `[verified: Ghidra]`: `CGame` (constructor `__ct__5CGameFv`, named in `cbr_decomp`) is at US `0x8025C400` / PAL `0x80263000` (start of BSS + `0x200`). Its vtable pointer is at +`0x3334` (US) / +`0x333C` (PAL) and points to US `0x8020FC80` / PAL `0x8021638C`.

---

## 4. REL modules

The 16 modules are stored in `qp.bin` under `rel/` `[verified: file]`:
- REL version 3, one code section (section 1), four or five data sections.
- Each module imports only from itself and from the DOL (module 0): no module depends on another.
- Names, ids and BSS sizes are the same in both versions, but all 16 files differ.
- In every module, `_unresolved`, `_epilog` and `_prolog` are contiguous: epilog = prolog − `0x48`, unresolved = prolog − `0x100`.

| file | id | code size US / PAL | BSS | `_prolog` offset in section 1, US / PAL | relocations against (self, DOL), US / PAL |
|---|---|---|---|---|---|
| `ahk.rel` | 1 | `0x5CC` / `0x5CC` | `0xDD4` | `0x514` / `0x514` | 60, 16 / 60, 16 |
| `hori.rel` | 2 | `0x748` / `0x748` | `0x11C` | `0x690` / `0x690` | 127, 20 / 127, 20 |
| `shun.rel` | 3 | `0x3604` / `0x3618` | `0x8CC` | `0x1F00` / `0x1F14` | 536, 210 / 536, 212 |
| `stage01.rel` | 4 | `0x5DD0` / `0x5D44` | `0x708` | `0x48E4` / `0x48E4` | 790, 235 / 778, 230 |
| `stage02.rel` | 5 | `0x8944` / `0x8870` | `0x19EC` | `0x888C` / `0x87B8` | 1597, 528 / 1577, 520 |
| `stage03.rel` | 6 | `0x1878` / `0x1900` | `0x109C` | `0x17C0` / `0x1848` | 253, 99 / 263, 103 |
| `stage04.rel` | 7 | `0x2968` / `0x28DC` | `0x3C8` | `0x1F0` / `0x1F0` | 532, 102 / 520, 97 |
| `stage06.rel` | 8 | `0x808` / `0x808` | `0x3B0` | `0x414` / `0x414` | 109, 28 / 109, 28 |
| `stage07.rel` | 9 | `0x4F78` / `0x4F78` | `0x70` | `0x2748` / `0x2748` | 948, 296 / 948, 296 |
| `stage08.rel` | 10 | `0x1C0` / `0x1C0` | `0x0` | `0x108` / `0x108` | 17, 5 / 17, 5 |
| `stage09.rel` | 11 | `0x23A8` / `0x231C` | `0x408` | `0xD5C` / `0xD5C` | 370, 84 / 358, 79 |
| `stage11.rel` | 12 | `0x27C` / `0x27C` | `0x0` | `0x1C4` / `0x1C4` | 24, 7 / 24, 7 |
| `stage16.rel` | 13 | `0x2E4` / `0x2E4` | `0x4` | `0x22C` / `0x22C` | 29, 10 / 29, 10 |
| `stage17.rel` | 14 | `0x27C` / `0x27C` | `0x0` | `0x1C4` / `0x1C4` | 24, 7 / 24, 7 |
| `stage18.rel` | 15 | `0x600` / `0x600` | `0x368` | `0x20C` / `0x20C` | 83, 17 / 83, 17 |
| `stage20.rel` | 16 | `0x2DC` / `0x2DC` | `0x4` | `0x224` / `0x224` | 32, 9 / 32, 9 |

**How the game loads a module** `[verified: Ghidra, US and PAL]`:
- The **scene loader** (US `0x8006EE24` / PAL `0x80070C70`) is a state machine run from the `CGame` update method (vtable +`0x14`). For a scene it builds the path of a data file in the language folder (`bin<lang>/<scene>.bin`) and the path of the module (`rel/<module>.rel`). The language comes from a byte of `CGame` (PAL +`0x3320`).
- Module names come from a scene table (PAL `0x8022AE44`, 16-byte entries). Scenes 28 and 29 use the module of scene 7.
- The **module loader** (US `0x801A92C4` / PAL `0x801ABBC0`) is the only caller of `OSLink`:
  1. it reads the whole `.rel` file into a heap buffer through the game's file object;
  2. if the header's `bssSize` (+`0x20`) is not zero, it allocates the BSS separately, rounded up to 32 bytes;
  3. it calls `OSLink(module, bss)` (never `OSLinkFixed`: relocation tables are kept) and stores both pointers;
  4. it calls the prolog (header +`0x34`) with r3 = −1, r4 = 0, r5 = 0.
- The **unloader** (US `0x801A923C` / PAL `0x801ABB38`) calls the epilog (header +`0x38`), then `OSUnlink`, then frees the BSS and the module.
- Modules therefore live at heap addresses chosen at run time, not at fixed addresses.
- Only `stage02`, `stage03` and `stage09` call sound code, and always through the HAL sound library, never AX, AI or DSP directly `[verified: REL relocations]`.

---

## 5. Differences between PAL and US

**Address shifts** (PAL address = US address + shift). Code from `0x80003100` to the start of text1 is at the same address in both versions `[verified: Ghidra + identical masked code]`.

| area | shift |
|---|---|
| start of text1 | +`0x20` |
| `CGame` constructor | +`0x2C0` |
| `main` | +`0x2D4` |
| scene loader | +`0x1E4C` |
| save manager | +`0x1F5C` to +`0x1F9C` |
| object-loader threads | +`0x280C` |
| TRK, MSL, OS, EXI, SI, DVD, VI | +`0x28B0` |
| PAD, start of AI | +`0x2874` |
| rest of AI, AR, ARQ, AX, AXFX, DSP, CARD, GX | +`0x2868` |
| game C++ drawing framework around US `0x801A0000` | +`0x2878` |
| HAL graphics, module loader, memory-card wrappers, THPSimple | +`0x28FC` |
| HAL sound, microphone driver, `THPInit` | +`0x29F4` |

**Version-only functions** `[verified: Ghidra]`. They explain the shifts inside the SDK:
- US only:
  - `VIGetDTVStatus` (US `0x80172AF4`, `0x3C` bytes, called by the game at US `0x8001C2C4`). It moves the shift from +`0x28B0` (VI) to +`0x2874` (PAD).
  - `AIReset` (US `0x80174C90`, `0xC` bytes). It moves the shift from +`0x2874` to +`0x2868`.
  - `OSGetProgressiveMode` / `OSSetProgressiveMode` (US `0x80169808` / `0x80169878`).
- PAL only: `OSSetEuRgb60Mode` (PAL `0x8016C194`, `0xA4` bytes) and the `0x70`-byte function just before it (PAL `0x8016C124`). Same total size as the US progressive-mode pair, so no shift.

**SDK code is identical** in both versions once address-dependent fields are masked: the 390 SDK function signatures computed on both DOLs are equal `[verified: signatures]`.

**Transposing `cbr_decomp` names to PAL** (by masked code, neighbourhood and call graph) `[verified: Ghidra]`:
- 380 of the 381 named DOL functions were found in PAL; 486 of 495 REL functions.
- The 15,720 `bl` calls between transposed functions all point to the expected function in PAL.
- The resulting PAL maps are in `games/chibi-robo/symbols/` (names and addresses only).

**Behaviour** `[verified: Ghidra]`:
- PAL asks for 50 or 60 Hz at boot. Choosing 60 Hz switches the video mode to EuRGB60, 59.94 Hz `[verified: trace]`.
- The frame limiter divides by a variable in PAL (initial value 50) and by the constant 60 in US (section 8).

---

## 6. Nintendo SDK and other libraries

**Dolphin SDK `0x2301`** `[verified: file]`. Each library's init function registers a release banner with `OSRegisterVersion`. Banner build dates: OS and GX, 10 November 2004; EXI, SI, DVD, VI, PAD, AI, AR, ARQ, AX, DSP and CARD, April 2004; THP, January 2004.

**Library init functions**, named from their banner `[verified: Ghidra]`:

| function | US | PAL |
|---|---|---|
| `EXIInit` | `0x8015EC0C` | `0x801614BC` |
| `SIInit` | `0x8016024C` | `0x80162AFC` |
| `OSInit` | `0x801614DC` | `0x80163D8C` |
| `DVDInit` | `0x8016DC60` | `0x80170510` |
| `VIInit` | `0x80171180` | `0x80173A30` |
| `PADInit` | `0x80173848` | `0x801760BC` |
| `AIInit` | `0x80174B24` | `0x80177398` |
| `ARInit` | `0x8017524C` | `0x80177AB4` |
| `ARQInit` | `0x80176D84` | `0x801795EC` |
| `AXInitEx` | `0x80176F74` | `0x801797DC` |
| `DSPInit` | `0x8017CDCC` | `0x8017F634` |
| `CARDInit` | `0x8017E97C` | `0x801811E4` |
| `GXInit` | `0x801848D0` | `0x80187138` |
| `THPInit` | `0x801FD144` | `0x801FFB38` |

**Other libraries in the DOL**:
- AXFX: HAL calls `AXFXSetHooks` `[verified: Ghidra]`.
- The THP sample player (`THPSimple`), linked into the game `[verified: Ghidra]`.
- No SDK function is in the REL modules `[verified: REL relocations]`.

**Other code** `[verified: file]`:
- **Metrowerks CodeWarrior runtime**, MSL C library, and the TRK debug kernel (inactive on a retail console).
- **HAL Laboratory's sysdolphin library (HSD)**. Its strings name it and its source files, for example `displayfunc.c`, `initialize.c`, `objalloc.c`, `memory.c`, `particle.c`, `psdisp.c`, `pobj.c`, `shadow.c`, `axdriver.c`, `devcom.c` and `dvdsche.c`.
- **Game code in C++**, with source names such as `game.cpp`, `sound.cpp`, `xobjs.cpp`, `cfont.cpp`, `cfile.cpp`, `cobj.cpp` and `cparticle.cpp`.
- **A GameCube Microphone driver** that is not part of the reference SDK sources (section 9).

**Empty debug output in this release build** `[verified: Ghidra]`. `OSReport` (US `0x8001580C` / PAL `0x80015ACC`), `DBPrintf`, `MWTRACE` and the DSP library's debug printf (US `0x8017D010` / PAL `0x8017F878`) only save their argument registers and return. All four have the same code. `OSPanic` (US `0x80163F80` / PAL `0x80166830`) is real: it prints through MSL's `vprintf` and halts.

**SDK functions not linked into this DOL** (useful when splitting translation units) `[verified: Ghidra + ref]`:
- OS:
  - `OSJoinThread`, `OSDetachThread`, `OSSetThreadPriority`, `OSGetThreadPriority`, `OSSetSwitchThreadCallback`;
  - all of `OSMutex.c` except `__OSUnlockAllMutex`;
  - `OSJamMessage` and the condition-variable functions.

  `OSLinkFixed` is never called.
- CARD:
  - `CARDProbe`, `CARDGetResultCode`, `CARDGetEncoding`, `CARDGetMemSize`, `CARDGetSerialNo`;
  - `CARDFastOpen`, `CARDFastDelete*`, `CARDRename*`, `CARDGetAttributes`/`SetAttributes`, `CARDCancel`;
  - `CARDFormatAsync` (`CARDFormat` calls the region formatter directly), `CARDGetStatusEx`/`SetStatusEx`, `CARDGetXferredBytes`.
- GX:
  - `GXSetDrawSync`, `GXReadDrawSync`, `GXPeek*`, `GXPokeARGB`, `GXPokeZ`;
  - `GXGetFifoPtrs`, `GXGetFifoStatus`, `GXSaveCPUFifo`, `GXPreLoadEntireTexture`;
  - the `GXRead*Metric` functions.

  There is also no callable `GXPosition*`, `GXColor*`, `GXTexCoord*`, `GXNormal*`, `GXMatrixIndex*` or `GXEnd` (section 11).

---

## 7. How SDK functions were identified

1. **Starting point**: the dtk symbols of `cbr_decomp` (US, CC0): 4,506 function bounds, about 381 non-automatic names.
2. **Library inits**: named from their `OSRegisterVersion` banner (section 6).
3. **Rest of the SDK**: matched against the CC0 reference sources in `cbr_decomp/decomp_dtk_main/src/Dolphin/`. The linker keeps the order of the functions of each source file, so each file was aligned with the DOL by function order and size. Each match was then confirmed by the call graph, by the hardware registers the function touches, and often by reading the disassembly.
   - Static functions are written `name@file`, for example `AlarmHandler@dvdlow`.
   - All 427 function sizes we measured agree with the `cbr_decomp` bounds.
4. **AX** has no reference source in `cbr_decomp`. Only the AX names confirmed by behaviour are listed in appendix A: callbacks seen in the call chain, and the 16 `AXSetVoice*` functions, identified by the voice-parameter-block field and sync bit each one writes.
5. **Signatures** find the same functions in another version or another game. A signature is the function size plus a CRC-32 of its 32-bit words after masking every address-dependent field:
   - the displacement of `b`/`bl`, and of `bc` when it leaves the function;
   - the immediate of `lis`;
   - offsets relative to r2, r13 or a register loaded by `lis`, including `ori`/`oris` on such a register;
   - the displacement of `psq_l`/`psq_st`.
6. **Ambiguous signatures**: small functions often have identical masked code `[verified: signatures]`. The known groups are:
   - `OSGetArenaHi`, `OSGetArenaLo`, `VIGetRetraceCount` and `ARGetSize`;
   - `OSGetCurrentThread`, `OSGetCurrentContext` and `OSGetPhysicalMemSize`;
   - the callback setters `VISetPreRetraceCallback`, `VISetPostRetraceCallback`, `AIRegisterDMACallback`, `ARRegisterDMACallback`, `AXRegisterCallback`, `GXSetBreakPtCallback`, `GXSetDrawSyncCallback` and `GXSetDrawDoneCallback`;
   - `ExternalInterruptHandler` and `DecrementerExceptionHandler`;
   - `Config24MB` and `Config48MB` (they differ only by masked constants);
   - `OSReport` and the three other empty printf functions;
   - `__init_user`, `_ExitProcess` and `OSGetResetSwitchState`.

   Resolve them by anchoring on a unique neighbour from the same source file. For example, `VIGetRetraceCount` directly follows `VISetBlack`.

---

## 8. Boot sequence and main loop

**From `__start` to `main`** `[verified: Ghidra; identical in both versions]`:
1. `__init_registers`: r1, r2, r13 (section 3).
2. `__init_hardware`: enables floating point, then `__OSPSInit`, `__OSFPRInit`, `__OSCacheInit`.
3. `__init_data`: the no-op ROM copies and the 3 BSS zones.
4. Debugger check through BI2 (`0x800000F4`): `InitMetroTRK` runs only on development hardware.
5. Argument relocation if BI2 provides arguments (otherwise argc = argv = 0), then `DBInit`.
6. `OSInit` (US `0x801614DC` / PAL `0x80163D8C`), in this order:
   1. exception vectors (`OSExceptionInit`), system call vector;
   2. alarms, module list, interrupts, reset-button handler (interrupt 22);
   3. contexts, caches, `EXIInit`, `SIInit`, SRAM, threads;
   4. DSP bootstrap (`__OSInitAudioSystem`), memory protection, OS banner;
   5. `DVDInit`, `DVDInquiryAsync`.
7. `__init_user` → `__init_cpp`: static constructors, including the global `CGame`.
8. `main(argc, argv)`. `main` never returns.

`OSExceptionInit` is the only self-modifying code in the DOL `[verified: Ghidra]`. It writes the exception number into an instruction of its vector template inside the DOL, copies the template to `0x80000x00`, then restores the instruction.

**`CGame` vtable slots used by `main`** `[verified: Ghidra]`:

| slot | US | PAL | what `main` does with it |
|---|---|---|---|
| +`0x0C` | `0x80013580` | `0x8001375C` | `ParseStageData__5CGameFiPc` (`cbr_decomp`), called with (argc, argv) |
| +`0x10` | `0x80012A8C` | `0x80012C08` | called once: initialises every subsystem (below) |
| +`0x14` | `0x80011BD8` | `0x80011D00` | called every frame; runs the scene loader |
| +`0x18` | `0x8000FACC` | `0x8000FB30` | called every frame with the number of elapsed video fields |
| +`0x1C` | `0x8000DFA8` | `0x8000DFE4` | called every frame, after +`0x18` |
| +`0x28` | `0x8000A4B8` | `0x8000A4D8` | frame interval (below) |

**Initialisation** (vtable +`0x10`), in order `[verified: Ghidra]`:
1. System init (US `0x8001D8C0` / PAL `0x8001DCA4`):
   - sets the paired-single quantisation registers GQR2 to GQR5;
   - calls `srand(OSGetTick())`;
   - sets up the arena;
   - calls the HAL system init (US `0x801CA87C` / PAL `0x801CD178`). That function calls `DVDInit`, `GXInit`, the HAL video init (US `0x801CBE78` / PAL `0x801CE774`, which calls `VIInit` and `VIGetTvFormat`), then `VIWaitForRetrace`.
2. `ARInit`, `ARQInit`.
3. The HAL pad init (US `0x801CD714` / PAL `0x801D0010`), which calls `PADInit`.
4. Three OS stopwatches, used by `main`.
5. `AIInit`, then the HAL sound path to `AXInit`.
6. The save manager init (US `0x8008A2A8` / PAL `0x8008C244`): `CARDInit`, then a CARD library setting call (US `0x8017EE40` / PAL `0x801816A8`) with `TRUE`.
7. Many game inits: the microphone driver init, the game's AI DMA callback and `AIStartDMA`, and at the end the THP player init (`THPInit`).

`srand` and `rand` (MSL) are at US `0x80158A0C` / `0x80158A14` and PAL `0x8015B2BC` / `0x8015B2C4`. `rand` is the classic ANSI linear congruential generator returning 15 bits `[verified: Ghidra]`. The seed comes from the time base, so the random parts of the title screen depend on boot timing. Fix the seed at `srand` to get reproducible runs `[verified: trace]`.

**`main`** (US `0x80015644` / PAL `0x80015918`) `[verified: Ghidra, both versions]`:
1. Calls vtable +`0x0C`, then +`0x10`, then a HAL video function with 0.
2. Loop head (US `0x800156C8` / PAL `0x80015994`): `VIWaitForRetrace`.
3. Elapsed fields = `VIGetRetraceCount()` − the value saved at the previous frame (`CGame`+`0x1B08` US / +`0x1B0C` PAL), capped at 4.
4. Resets and starts the three stopwatches. Calls the HAL end-of-frame function (US `0x801CBAFC` / PAL `0x801CE3F8`), then reads stopwatch 1. Calls a game function with (`CGame`+`0x1904`, elapsed fields).
5. Calls vtable +`0x14` and +`0x18`, then reads stopwatch 2. Calls +`0x1C`, then reads stopwatch 3.
6. **Frame limiter, a busy-wait.** With interval = vtable +`0x28`, it spins on `VIGetRetraceCount()` until `count − start ≥ interval × 60 / divisor − 1` (unsigned).
   - US: the divisor is the constant 60. Loop at `0x800157F4`–`0x80015804`.
   - PAL: the divisor is the word at r13 − `0x7FF4` (PAL `0x80660D4C`), whose initial value is 50 `[verified: file]`. Loop at `0x80015AA8`–`0x80015AC4`.

**Frame interval**, vtable +`0x28` `[verified: Ghidra, both versions]`. It returns 1 if the word at `CGame`+`0x472C` is between 1 and 14, or if the word at `CGame`+`0x4930` is between 1 and 22; otherwise 2. The game runs at 30 frames per second, and at 60 while one of these fields is in range. Dolphin's "60 FPS" patch replaces the last instruction before the return (US `0x8000A518` / PAL `0x8000A538`) so that the function always returns 1 `[verified: Ghidra + Dolphin's GameSettings files]`.


### Stages and save restore

PAL addresses were found by masked instruction matching against US, then confirmed by runs.
- A stage N loads `bin/e/stageNN.bin` from `qp.bin` and, when it exists, `rel/stageNN.rel` `[verified: DVD trace
  matched against qp.bin, US and PAL]`. Normal stages are numbered 0 to 29 (numbering as in
  [unplug](https://github.com/adierking/unplug), MIT). Stage 0 is a developers' scene where each character opens a
  menu of story points that warps to the matching stage; stage 9 is the backyard; stage 14 is the living room during
  the birthday party `[verified: runs and screenshots, US and PAL, for stages 0, 9 and 14]`.
- **Stage change**, US `0x8000B394` `(game object, int stage, int mode)` `[verified: Ghidra + runs, US]`. `stage` is
  a stage number, or a code: 10000 = start stage, 20000 = the stage stored in the save (it also requests the save
  restore), negative = next stage. It writes the current stage at game object `+0x3444` and the previous one at
  `+0x3448`. PAL: `0x8000B3B4` (60 of its first 64 instructions match), current stage at `+0x344C`; the codes were
  not observed at run time on PAL.
- **Loading a save** calls the stage change with 20000 from US `0x80030C48` / PAL `0x8003158C` `[verified: GCRT_WATCH
  counters (US), runs (PAL)]`.
- **Save restore**, US `0x80087CC0` / PAL `0x80089C1C`, called only from US `0x80012704` / PAL `0x80012864`
  `[verified: Ghidra (US), runs (US and PAL)]`. It reads the stage stored at save offset `+0xC408` (same offset on
  both versions), copies the current stage into the previous one, writes the stored stage as the current one, then
  restores the counters, the battery and other blocks of the save.
- Right after that call, the caller sets bit `0x2000` in a flags word of the game object (US `+0x1B14`, PAL
  `+0x1B1C`). While it is set, the stage being entered places Chibi-Robo at the spot where the save was made; with
  the bit cleared, Chibi-Robo appears at the stage's own starting point `[verified: runs, US and PAL]`. Which code
  reads the bit is not established.
- The game object is a single instance at US `0x8025C400` / PAL `0x80263000` (`CGame` above).

---

## 9. Threads, alarms, interrupts and busy-waits

**Threads** (all detached) `[verified: trace, US and PAL]`:

| thread object US / PAL | created by | entry US / PAL | priority | stack | role |
|---|---|---|---|---|---|
| `0x805CA7D0` / `0x805D1650` | `__OSThreadInit` | `main` | 16 | 64 KiB above BSS | main loop; sleeps in `VIWaitForRetrace` |
| `0x80380748` / `0x80387548` | US `0x80132504` / PAL `0x80134D10` | `0x801320F4` / `0x80134900` | 16 | 8 KiB | object loader |
| `0x803803A8` / `0x803871A8` | same | `0x80132358` / `0x80134B64` | 16 | 8 KiB | object loader |
| `0x80380008` / `0x80386E08` | same | `0x801323DC` / `0x80134BE8` | 17 | 8 KiB | object loader |
| `0x8036AC10` / `0x80371A10` | save manager | one entry per operation | 16 | 4 KiB | one memory-card operation, then returns into `OSExitThread` |

- The three object-loader threads start after the title screen. Each one loops on a blocking `OSReceiveMessage` (queue of 8 messages), then calls a virtual method of the request. Requests are posted with a blocking `OSSendMessage` `[verified: Ghidra + trace]`.
- `main` polls `OSIsThreadTerminated` on the save thread every frame `[verified: Ghidra]`.
- The game uses no mutex and no condition variable `[verified: Ghidra]`.
- The scheduler is the SDK's: priorities 0–31, no time slicing, no pre-emption at equal priority. Threads switch only on explicit calls and at the end of interrupt and alarm handlers `[verified: Ghidra + ref]`.

**Alarms**: one periodic alarm of 5 ms (202,500 ticks), set by the microphone driver init `[verified: trace]`. Alarm object: US `0x80659AA0` / PAL `0x80660920`; handler: US `0x801F452C` / PAL `0x801F6F20`.

**Time base**: 40.5 MHz. The game never writes it: `mttb` appears only in TRK `[verified: Ghidra]`. Save dates come from `OSGetTime` through `OSTicksToCalendarTime` `[verified: Ghidra]`.

**Interrupt handlers** installed during initialisation `[verified: Ghidra + trace]`:

| number | handler (US / PAL when not in `cbr_decomp`) |
|---|---|
| 0–4 | `MEMIntrruptHandler` |
| 5 | `__AIDHandler` (US `0x80174D18` / PAL `0x80177580`) |
| 6 | `__ARHandler` (US `0x80175318` / PAL `0x80177B80`) |
| 7 | `__DSPHandler` (US `0x8017D060` / PAL `0x8017F8C8`) |
| 8 | AI library handler (US `0x80174C9C` / PAL `0x80177504`) |
| 9, 12, 15 | `EXIIntrruptHandler` |
| 10, 13, 16 | `TCIntrruptHandler` |
| 11, 14 | `EXTIntrruptHandler` |
| 17 | `GXCPInterruptHandler` (US `0x80185808` / PAL `0x80188070`) |
| 18 | `GXTokenInterruptHandler` (US `0x80187B34` / PAL `0x8018A39C`) |
| 19 | `GXFinishInterruptHandler` (US `0x80187C00` / PAL `0x8018A468`) |
| 20 | `SIInterruptHandler` |
| 21 | `__DVDInterruptHandler` |
| 22 | `__OSResetSWInterruptHandler` |
| 24 | `__VIRetraceHandler` (US `0x80170BD8` / PAL `0x80173488`) |

Exceptions: 4 → `ExternalInterruptHandler`, 7 → `OSSwitchFPUContext` (lazy FPU switch), 8 → `DecrementerExceptionHandler`, all others → `OSDefaultExceptionHandler`.

**Busy-waits and idle loops** `[verified: Ghidra + trace]`:

| loop US / PAL | where | exits when |
|---|---|---|
| `0x8016A40C` / `0x8016CCBC` | `SelectThread` idle loop (SDK) | an interrupt or alarm handler makes a thread ready |
| `0x800157F4` / `0x80015AA8` | `main` frame limiter | enough video fields have elapsed |
| in US `0x80131FAC`, `0x801321FC` / PAL `0x801347B8`, `0x80134A08` | `OSYieldThread` loop on a virtual method of the object (vtable +`0xA0` or +`0x9C`) | an object-loader thread has finished the object |
| `0x801A4090` / `0x801A6908` | US `0x801A3F18` / PAL `0x801A6790`, after a HAL `devcom.c` request | a state field leaves the value 2 |
| `0x801784E4` / `0x8017AD4C` | AX output init, waiting for the DSP | DSP interrupt (boot only) |
| several in `__OSInitAudioSystem` | DSP bootstrap, polling DSP registers | boot only |

During the introduction, the `OSYieldThread` loops run hundreds of thousands of times, each time saving and reloading the same thread `[verified: trace]`. `VIWaitForRetrace`, `GXDrawDone` and `OSReceiveMessage` sleep with `OSSleepThread` and are not busy-waits.

**Microphone driver** `[verified: Ghidra]`. Function names are unknown.
- The probe (US `0x801F2EFC` / PAL `0x801F58F0`) accepts EXI channel 0 or 1, otherwise returns −128. It calls `EXIProbeEx` (no device → −3, busy → −1), `EXIGetState`, then `EXIGetID`.
- It returns 0 only for EXI device ID `0x0A000000`, the ID that Dolphin gives its emulated GameCube Microphone `[verified: Dolphin source]`; any other device gives −2.
- A game state machine (US `0x8000A6D8` / PAL `0x8000A6F8`) probes channel 1 (memory-card slot B) every frame during gameplay `[verified: Ghidra + trace]`.
- The game's audio DMA callback also polls this driver (section 10).

---

## 10. Audio

**Layers** `[verified: Ghidra + trace]`:

```
game sound code (DOL; REL stage02, stage03, stage09)
  -> HAL sound library (axdriver.c, synthesizer, .hps streams, .ssm/.sem banks, devcom.c, dvdsche.c)
       -> AX: voices, frame callback, AUX ; AXFX hooks
       -> ARQ/AR: samples to ARAM ; DVD
            -> DSP (AX microcode) and AI (audio DMA)
THPSimple and the game each add their own AI DMA callback in front of AX's.
```

- The game talks only to the HAL sound library, never to AX.
- `HSD_AudioInit` (US `0x801F29A8` / PAL `0x801F539C`, named by its own assertion strings) calls `AXInit`, `AISetDSPSampleRate(0)` (32 kHz), `ARAlloc`, the devcom init, `AXRegisterCallback`, and sets stream volumes.
- **Microcode**: one standard AX microcode, `0x1F00` bytes, identical in both versions `[verified: file]`. Its hash with Dolphin's microcode hash function (`HashEctor`) is `0x3daf59b9`. It is stored at US `0x80233B40` / PAL `0x8023A660`. No other DSP microcode exists in the DOL or the RELs, apart from the `0x80`-byte init microcode of `__OSInitAudioSystem` (PAL `0x80236AF0`).
- **Cadence**: 32 kHz output, one AI DMA interrupt every 5 ms (`0x280` bytes, 160 stereo 16-bit frames), 200 per second `[verified: trace]`. The DSP receives one AX command list per 5 ms frame.

**AI DMA callback chain** `[verified: Ghidra + trace]`. AX registers its callback first, then the game, then THPSimple. On each AI DMA interrupt:
1. THPSimple's callback (US `0x801B5B1C` / PAL `0x801B8418`) calls the previous callback. It then mixes 160 frames into **its own** double buffer and restarts DMA from that buffer:
   - without a movie, it copies AX's output unchanged;
   - with a movie, it adds THP audio × volume, with saturation, and averages to mono when the OS sound mode is mono.

   The buffer actually played is always THPSimple's.
2. The game's callback (US `0x8000A678` / PAL `0x8000A698`) calls the previous callback, then polls the microphone driver into a buffer of its own. It never touches the output.
3. `__AXOutAiCallback` → `__AXOutNewFrame` → the HAL frame callback (US `0x801F07D8` / PAL `0x801F31CC`). The HAL callback frees finished voices, runs the synthesizer envelopes, and services `.hps` streams by re-chaining the ADPCM loop and end addresses of stream voices.

**Music by place** `[verified: native DVD trace + Dolphin audio dump, US, a 47-minute save, night]`:
- Inside the Chibi-House no `.hps` stream is read: the output is a mono low-frequency loop (main energy near 42 Hz, left = right), the same in Dolphin and in the native port.
- In the living room at night the game streams `qp/streaming/bgm_night.hps` (envelope correlation 0.80 native, 0.86 Dolphin, with sound effects mixed in).

**AUX effects**: the HAL effect setup (US `0x801E8D84` / PAL `0x801EB778`) takes an effect type from 0 to 4 and a bus (A or B); type 0 installs no effect. Its only caller passes type 0, and no pointer to it exists in the DOL or the RELs `[verified: Ghidra, REL relocations]`.

---

## 11. Drawing (GX)

**Library** `[verified: Ghidra + ref]`:
- 173 functions, starting at US `0x8018447C` / PAL `0x80186CE4`. The US library is the PAL one shifted by −`0x2868`, with identical sizes.
- `GXInit` is called once by HAL, with a 256 KiB FIFO allocated by HAL. The CPU and GP FIFOs are the same object (linked mode).

**Vertex data is written inline** `[verified: Ghidra + trace]`:
- There is no callable `GXPosition*`, `GXColor*`, `GXTexCoord*`, `GXNormal*`, `GXMatrixIndex*` or `GXEnd` in the DOL or the RELs. These are `static inline` functions of the SDK headers: the drawing code writes to the write-gather pipe (`0xCC008000`) itself.
- Every command header in the stream is written by the GX library: BP, CP and XF loads, `CALL_DL`, `INVL_VC`, draw headers and padding.
- Code outside the library writes only vertex data, and always after a call to `GXBegin` (US `0x80187D74` / PAL `0x8018A5DC`). Appendix B lists these 26 functions; three helper functions inside the GX library also do it (US `0x8018B22C`, `0x8018B8F8`, `0x8018BE8C` / PAL `0x8018DA94`, `0x8018E160`, `0x8018E6F4`).
- The heaviest inline writer is the game's textured sprite (US `0x801A00E0` / PAL `0x801A2958`): strips of 4 vertices with a float position, a 32-bit colour and 8-bit texture coordinates.
- HAL's `pobj.c` (US `0x801AD29C` / PAL `0x801AFB98`) reads a model's display list back and re-emits every vertex inline. Positions and normals come from arrays transformed by the CPU (skinning); the other attributes are copied from the list.
- The REL modules never form an address in `0xCC00xxxx`, `0xCC01xxxx` or the EFB (`0xC8xxxxxx`).

**Display lists** from model files, run by `GXCallDisplayList` from HAL's PObj code (PAL `0x801AE8F4`), contained only draw commands and padding in every scene we traced (boot, title, introduction, new game, load game) `[verified: trace]`. These are quads, triangles and triangle strips, with no BP, CP or XF loads, no nested call and no indexed load.

**Copies** `[verified: Ghidra + trace]`:
- One display copy per frame, made by HAL (US `0x801CB8A4` / PAL `0x801CE1A0`): copy setup, `GXSetDispCopyYScale`, `GXSetCopyClear`, `GXSetCopyFilter`, gamma, then `GXCopyDisp` with clear. On PAL the source is the full 640×528 EFB, pixel format RGBA6_Z24, gamma 1.0, with a vertical copy filter.
- EFB-to-texture copies (`GXCopyTex`) come from HAL's `shadow.c` and from the game function US `0x801A66A4` / PAL `0x801A8F1C`. There are none on the title screen.
- In 3D gameplay (US, living room at night, frame dump of the GX stream), each frame has two EFB-to-texture copies `[verified: trace]`:
  - at the start of the frame, a few fan and strip draws, then a copy of the red channel (`GX_CTF_R8`, source 640x480, with clear); later draws of the same frame sample that destination as an `I8` 640x480 texture (22 loads in the traced frame);
  - at the end of the frame, a copy of the alpha channel (`GX_CTF_A8`, 640x448, without clear), sampled as `I8` right after.
  The source of the red-channel copy (480 lines) is taller than the display copy (448 lines on US). This red-channel
  copy is the shadow map: an orthographic pass in a 640x480 viewport, sampled by the floors in their own draw. A
  renderer whose EFB is only as tall as the US display copy (448 lines) cuts and stretches it, which moves the
  shadows; the EFB must keep the console's 528 lines `[verified: GX stream, shadow positions predicted from the
  stream's matrices and measured, comparison with Dolphin]`. On PAL the display copy already reads the full
  640x528 EFB.
- The CPU never reads or writes the EFB `[verified: Ghidra]`.

**Synchronisation** `[verified: Ghidra + trace]`:
- HAL calls `GXSetDrawDone` once per frame, in its end-of-frame function (US `0x801CBAFC` / PAL `0x801CE3F8`).
- `GXSetDrawDoneCallback` installs a HAL callback (US `0x801CB850` / PAL `0x801CE14C`) that clears a HAL flag and calls an optional user callback.
- No draw-sync token is ever sent: `GXSetDrawSync` is not linked, and `GXSetDrawSyncCallback` is called only by `__GXShutdown`.

**Hardware accesses outside the pipe** `[verified: Ghidra + trace]`:
- During gameplay, the only GX register access (CP, PE, PI FIFO, MI) is `GXFinishInterruptHandler` acknowledging the PE finish interrupt, once per frame.
- All other accesses happen inside `GXInit` and its helpers.

**Immediate and deferred state** `[verified: Ghidra + ref]`:
- `GXSetArray` writes the CP array registers immediately.
- The vertex descriptor, vertex format, texgen, channel, TEV-stage and cull setters only update `__GXData` and its dirty bits. These are flushed at the next `GXBegin`, `GXCallDisplayList`, `GXFlush` or `GXSetDrawDone`.

---

## 12. Memory card

**CARD functions called by the game**, all synchronous `[verified: Ghidra]`: `CARDInit`, a CARD library setting call with `TRUE` (US `0x8017EE40` / PAL `0x801816A8`, unnamed), `CARDProbeEx`, `CARDMount`, `CARDCheck`, `CARDFormat`, `CARDUnmount`, `CARDOpen`, `CARDClose`, `CARDCreate`, `CARDRead`, `CARDWrite`, `CARDDelete`, `CARDGetStatus`, `CARDSetStatus`, `CARDFreeBlocks`, `CARDGetSectorSize`. The asynchronous versions are called only from inside the library.

**Wrapper class** (US `0x801A93C8`–`0x801A9AD4` / PAL `0x801ABCC4`–`0x801AC3D0`) `[verified: Ghidra]`:
- Layout: +`0x00` channel, +`0x04` `CARDFileInfo`, +`0x18` last result, +`0x20` work area (`0xA000` bytes).
- **Every wrapper retries for as long as the result is −1 (BUSY)**, except the per-frame probe (US `0x801A99F8` / PAL `0x801AC2F4`) and the `CARDInit` wrapper.
- The read wrapper calls `DCInvalidateRange` after `CARDRead`.

**Save manager** (global object at PAL `0x8034B400`; PAL code `0x80088E80`–`0x8008C2B0`) `[verified: Ghidra]`:
- Each operation runs in its own thread (section 9). The object's +`0x00` holds the current step and +`0x38` the last CARD result; the menus read both.
- Every operation thread starts with `CARDProbeEx`, then `CARDMount`, then `CARDCheck`. A sector size other than 8 KiB is treated as a wrong device. The thread then runs its operation and ends with `CARDUnmount`.
- Operations: delete slot *n*; check, and format only after −6 (BROKEN) or −13 (ENCODING); find whether any save exists; list slots 0–9; load slot *n*; save slot *n*, creating the file if needed.
- On the title screen and in the in-game menus, `CARDProbeEx(0)` is called every frame without retry. The first time a card is seen, the menu starts the "find any save" thread; a positive answer adds the load option to the title screen `[verified: Ghidra + Dolphin]`.

**Save files** `[verified: Ghidra + Dolphin]`:
- Name `CHIBIROBO(n)`; the code looks for n = 0 to 9. The menus offer three files `[verified: Dolphin]`.
- Size `0xC000` (6 blocks of 8 KiB). As a Dolphin GCI file: `0x40` + 6 × `0x2000` = 49,216 bytes, named `01-GGTP-CHIBIROBO(0).gci` for PAL slot 0.
- The game creates the file as soon as a new game is started, with no confirmation `[verified: Dolphin]`.
- Expected `CARDStat` values:

  | field | value |
  |---|---|
  | `length` | `0xC000` |
  | `bannerFormat` | 2 (RGB5A3) |
  | `iconAddr` | `0x40` |
  | `iconFormat` | 2 |
  | `iconSpeed` | 3 |
  | `commentAddr` | 0 |
  | `offsetBanner` | `0x40` |
  | `offsetIcon[0]` | `0x1840` |
  | `offsetData` | `0x2040` |

  The game calls `CARDSetStatus` only when one of these differs, that is right after creation.
- **Two copies** of `0x6000` bytes (file offsets 0 and `0x6000`) are written one after the other.
- On load (US `0x80087190` / PAL `0x800890EC`), the first valid copy is kept. A copy is valid when:
  1. a 32-bit magic word at +`0x2200` matches;
  2. the byte sum of the `0x2CF0` bytes from +`0x2200` equals the word at +`0x2040`;
  3. several header fields pass range checks.

  If both copies fail, the result is −128.

---

## 13. Disc file formats

What the disc contains `[verified: file]`. For the formats themselves, see `unplug` (section 15).
- `qp.bin`: a U8 archive (magic `0x55AA382D`) with:
  - models and scenes (`.dat`, HAL HSD archives);
  - textures (`.tpl`);
  - data under `bin/<lang>/`;
  - the 16 REL modules under `rel/`.
- `qp/*.hps`: music streams in HAL's DSP-ADPCM format (109 files in PAL).
- `qp/*.ssm`: sound-effect banks (26 files in PAL); `qp/sfx_sample.sem`: effect metadata.
- `qp/movie/*.thp`: THP movies. PAL also has `.thp25` variants.
- `opening.bnr`: the GameCube menu banner.
- `tmp/cbr.dol`: a byte-identical copy of `main.dol` in both versions (same SHA-1).

---

## 14. Pitfalls for reimplementations

- **Uninitialised stack read in TEV colours** `[verified: Ghidra]`. A HAL TEV-expression function (US `0x801D8F48` / PAL `0x801DB844`, called by US `0x801D9444` / PAL `0x801DBD40`) has a local array of 7 `GXColor` at sp+`0x14`, never cleared.
  - It walks a linked list of TEV constants. For each entry it fills only some channels of one element; in one case it reads back the element's old alpha byte and merges it.
  - At the end it passes elements 0–3 to `GXSetTevKColor` and elements 4–6 to `GXSetTevColor`, for every element the list touched.
  - Channels that no entry wrote, often alpha, keep whatever earlier calls left on the stack. A decompilation or native replacement that changes the stack contents of the preceding calls changes these register values.
- **CARD must never return −1 from a synchronous call**: the game's wrappers would retry forever (section 12).
- **The frame limiter is a busy-wait** on the retrace counter (section 8). The object-loading waits are `OSYieldThread` loops (section 9).
- **The audio buffer that reaches the AI is THPSimple's**, not AX's: THPSimple copies AX's output into its own buffer every 5 ms (section 10).
- **Vertex data reaches the pipe without any function call** (section 11): replacing the GX functions is not enough to capture geometry.

---

## 15. Sources and credits

- **eavpsp/cbr_decomp** — https://github.com/eavpsp/cbr_decomp. Only `decomp_dtk_main/` is licensed (CC0 1.0). We used:
  - `config/GGTE01/symbols.txt` and `splits.txt` (function bounds and names, cited as such);
  - the reference SDK sources and headers under `src/Dolphin/` and `include/Dolphin/` (function order, sizes, prototypes).

  The unlicensed parts were not used as a source.
- **adierking/unplug** (MIT) — https://github.com/adierking/unplug. The *Chibi-Robo!* modding toolkit, and the reference for the disc, U8, HSD, HPS, SSM, SEM and stage formats.
- **Dolphin** (GPL-2.0-or-later) — https://github.com/dolphin-emu/dolphin, commit `e6f3ae17`. Behavioural reference, read only: the microcode hash function, the GCI format, the microphone's EXI ID, and the `GameSettings` 60 FPS patch. No code copied.
- **EverydaySimpleDev/ChibiRobo-GBA-Link** (MIT) — its US hook on the `VIWaitForRetrace` call in `main` (US `0x800156C8`) agrees with our analysis.
- **Ghidra** (Apache 2.0), with Cuyler36's GameCube loader extension.
- **ExpansionPak/DolRecomp** (GPL-3.0) — the static recompiler used to run and trace the original code.

---

## Appendix A. SDK functions unnamed in cbr_decomp

Functions of the Nintendo SDK and the Metrowerks libraries that `cbr_decomp`'s `symbols.txt` leaves as `fn_XXXXXXXX`, or that exist only in PAL (marked "—" for US).
- All entries are `[verified: Ghidra + ref]`: same order and size as the reference source, plus at least one of call graph, hardware register accesses, runtime trace or disassembly read.
- Functions that the reference source lacks (for example `CARDGetSectorSize`, the `CARDDelete` family, `VIGetDTVStatus`) and the libraries without a reference source in `cbr_decomp` (AX, AXFX, THP, MSL) are listed only when their code and callers were read in Ghidra (section 7).
- Names are the SDK's public names; `name@file` is a `static` function of that SDK source file.
- Prototypes are those of the public SDK headers (see the CC0 headers in `cbr_decomp`).
- Sizes are the `cbr_decomp` function bounds at the US address.

### OS (64)

| US | PAL | size | name |
|---|---|---|---|
| `0x80161168` | `0x80163A18` | `0x8` | `PPCMthid0` |
| `0x80161188` | `0x80163A38` | `0x8` | `PPCSync` |
| `0x80161478` | `0x80163D28` | `0x28` | `OSGetConsoleType` |
| `0x80161DDC` | `0x8016468C` | `0x14` | `__OSGetDIConfig` |
| `0x8016213C` | `0x801649EC` | `0x7C` | `OSSetPeriodicAlarm` |
| `0x801621B8` | `0x80164A68` | `0x11C` | `OSCancelAlarm` |
| `0x80162554` | `0x80164E04` | `0xA0` | `OnReset@OSAlarm` |
| `0x80163864` | `0x80166114` | `0xC` | `OSGetCurrentContext` |
| `0x80163870` | `0x80166120` | `0x80` | `OSSaveContext` |
| `0x801639C8` | `0x80166278` | `0x8` | `OSGetStackPointer` |
| `0x801639D0` | `0x80166280` | `0x30` | `OSSwitchFiber` |
| `0x80163A24` | `0x801662D4` | `0xBC` | `OSInitContext` |
| `0x80163E54` | `0x80166704` | `0x12C` | `OSFillFPUContext` |
| `0x80163F80` | `0x80166830` | `0x12C` | `OSPanic` |
| `0x80165E84` | `0x80168734` | `0x64` | `OSGetFontEncode` |
| `0x80166EB8` | `0x80169768` | `0x14` | `__OSGetInterruptHandler` |
| `0x80166F40` | `0x801697F0` | `0x2D8` | `SetInterruptMask@OSInterrupt` |
| `0x80167328` | `0x80169BD8` | `0x344` | `__OSDispatchInterrupt` |
| `0x80167C60` | `0x8016A510` | `0x24` | `OSLink` |
| `0x80167EBC` | `0x8016A76C` | `0x1D4` | `OSUnlink` |
| `0x801680A8` | `0x8016A958` | `0x60` | `OSInitMessageQueue` |
| `0x80168108` | `0x8016A9B8` | `0xC8` | `OSSendMessage` |
| `0x801681D0` | `0x8016AA80` | `0xDC` | `OSReceiveMessage` |
| `0x801682AC` | `0x8016AB5C` | `0xC` | `OSGetPhysicalMemSize` |
| `0x801682B8` | `0x8016AB68` | `0x3C` | `OnReset@OSMemory` |
| `0x80168590` | `0x8016AE40` | `0x70` | `__OSUnlockAllMutex` |
| `0x80168670` | `0x8016AF20` | `0xC` | `OSSetSaveRegion` |
| `0x8016867C` | `0x8016AF2C` | `0x14` | `OSGetSaveRegion` |
| `0x80168894` | `0x8016B144` | `0x48` | `__OSDoHotReset` |
| `0x80168C08` | `0x8016B4B8` | `0x298` | `OSGetResetButtonState` |
| `0x80168EA0` | `0x8016B750` | `0x20` | `OSGetResetSwitchState` |
| `0x80168EC0` | `0x8016B770` | `0x60` | `WriteSramCallback@OSRtc` |
| `0x80168F20` | `0x8016B7D0` | `0x118` | `WriteSram@OSRtc` |
| `0x8016922C` | `0x8016BADC` | `0x33C` | `UnlockSram@OSRtc` |
| `0x801695C0` | `0x8016BE70` | `0x124` | `__OSReadROM` |
| `0x801696E4` | `0x8016BF94` | `0x80` | `OSGetSoundMode` |
| `0x80169764` | `0x8016C014` | `0xA4` | `OSSetSoundMode` |
| `0x80169808` | — | `0x70` | `OSGetProgressiveMode` |
| `0x80169878` | — | `0xA4` | `OSSetProgressiveMode` |
| `0x80169988` | `0x8016C238` | `0x84` | `OSGetWirelessID` |
| `0x80169A0C` | `0x8016C2BC` | `0xAC` | `OSSetWirelessID` |
| `0x80169BE0` | `0x8016C490` | `0x2C` | `OSInitStopwatch` |
| `0x80169C0C` | `0x8016C4BC` | `0x3C` | `OSStartStopwatch` |
| `0x80169C48` | `0x8016C4F8` | `0xCC` | `OSStopStopwatch` |
| `0x80169D14` | `0x8016C5C4` | `0x70` | `OSCheckStopwatch` |
| `0x80169D84` | `0x8016C634` | `0x28` | `OSResetStopwatch` |
| `0x80169F9C` | `0x8016C84C` | `0xC` | `OSGetCurrentThread` |
| `0x80169FA8` | `0x8016C858` | `0x34` | `OSIsThreadTerminated` |
| `0x8016A05C` | `0x8016C90C` | `0x68` | `UnsetRun@OSThread` |
| `0x8016A0C4` | `0x8016C974` | `0x3C` | `__OSGetEffectivePriority` |
| `0x8016A100` | `0x8016C9B0` | `0x1C0` | `SetEffectivePriority@OSThread` |
| `0x8016A518` | `0x8016CDC8` | `0x3C` | `OSYieldThread` |
| `0x8016A554` | `0x8016CE04` | `0x1E8` | `OSCreateThread` |
| `0x8016A73C` | `0x8016CFEC` | `0xE4` | `OSExitThread` |
| `0x8016A820` | `0x8016D0D0` | `0x1BC` | `OSCancelThread` |
| `0x8016A9DC` | `0x8016D28C` | `0x288` | `OSResumeThread` |
| `0x8016AC64` | `0x8016D514` | `0x170` | `OSSuspendThread` |
| `0x8016ADD4` | `0x8016D684` | `0xEC` | `OSSleepThread` |
| `0x8016AEC0` | `0x8016D770` | `0x104` | `OSWakeupThread` |
| `0x8016B0F4` | `0x8016D9A4` | `0x58` | `__OSTimeToSystemTime` |
| `0x8016B14C` | `0x8016D9FC` | `0x19C` | `GetDates@OSTime` |
| `0x8016B2E8` | `0x8016DB98` | `0x204` | `OSTicksToCalendarTime` |
| `0x8016B4EC` | `0x8016DD9C` | `0x2E0` | `OSCalendarTimeToTicks` |
| — | `0x8016C194` | `0xA4` | `OSSetEuRgb60Mode` |

### Cache (7)

| US | PAL | size | name |
|---|---|---|---|
| `0x80162FD8` | `0x80165888` | `0x30` | `DCStoreRange` |
| `0x80163034` | `0x801658E4` | `0x2C` | `DCZeroRange` |
| `0x801630B8` | `0x80165968` | `0xCC` | `__LCEnable` |
| `0x80163184` | `0x80165A34` | `0x38` | `LCEnable` |
| `0x801631E4` | `0x80165A94` | `0x24` | `LCStoreBlocks` |
| `0x80163208` | `0x80165AB8` | `0xAC` | `LCStoreData` |
| `0x801632B4` | `0x80165B64` | `0x14` | `LCQueueWait` |

### EXI (9)

| US | PAL | size | name |
|---|---|---|---|
| `0x8015DD14` | `0x801605C4` | `0xA0` | `EXIImmEx` |
| `0x8015E134` | `0x801609E4` | `0x7C` | `EXISetExiCallback` |
| `0x8015E324` | `0x80160BD4` | `0x80` | `EXIProbe` |
| `0x8015E3A4` | `0x80160C54` | `0xB4` | `EXIProbeEx` |
| `0x8015E458` | `0x80160D08` | `0x10C` | `EXIAttach` |
| `0x8015E564` | `0x80160E14` | `0xBC` | `EXIDetach` |
| `0x8015EFB0` | `0x80161860` | `0x18` | `EXIGetState` |
| `0x8015F3A0` | `0x80161C50` | `0x18C` | `ProbeBarnacle@EXIUart` |
| `0x8015F6E8` | `0x80161F98` | `0x70` | `InitializeUART` |

### SI / PAD (35)

| US | PAL | size | name |
|---|---|---|---|
| `0x8015F958` | `0x80162208` | `0x20` | `SIBusy` |
| `0x8015F978` | `0x80162228` | `0x3C` | `SIIsChanBusy` |
| `0x8015F9B4` | `0x80162264` | `0x2FC` | `CompleteTransfer@SIBios` |
| `0x8015FFF4` | `0x801628A4` | `0x98` | `SIEnablePollingInterrupt` |
| `0x8016008C` | `0x8016293C` | `0xCC` | `SIRegisterPollingHandler` |
| `0x80160158` | `0x80162A08` | `0xF4` | `SIUnregisterPollingHandler` |
| `0x80160300` | `0x80162BB0` | `0x20C` | `__SITransfer` |
| `0x8016050C` | `0x80162DBC` | `0x7C` | `SIGetStatus` |
| `0x80160588` | `0x80162E38` | `0x14` | `SISetCommand` |
| `0x8016059C` | `0x80162E4C` | `0x10` | `SITransferCommands` |
| `0x80160618` | `0x80162EC8` | `0x9C` | `SIEnablePolling` |
| `0x801606B4` | `0x80162F64` | `0x6C` | `SIDisablePolling` |
| `0x80160720` | `0x80162FD0` | `0xD4` | `SIGetResponseRaw@SIBios` |
| `0x801607F4` | `0x801630A4` | `0xC4` | `SIGetResponse` |
| `0x801608B8` | `0x80163168` | `0x8C` | `AlarmHandler@SIBios` |
| `0x80160F0C` | `0x801637BC` | `0x13C` | `SIGetTypeAsync` |
| `0x8016112C` | `0x801639DC` | `0x24` | `SIRefreshSamplingRate` |
| `0x80172DAC` | `0x80175620` | `0x1A4` | `UpdateOrigin@Pad` |
| `0x80172F50` | `0x801757C4` | `0xC4` | `PADOriginCallback@Pad` |
| `0x80173014` | `0x80175888` | `0xCC` | `PADOriginUpdateCallback@Pad` |
| `0x801730E0` | `0x80175954` | `0xD8` | `PADProbeCallback@Pad` |
| `0x801731B8` | `0x80175A2C` | `0x32C` | `PADTypeAndStatusCallback@Pad` |
| `0x801734E4` | `0x80175D58` | `0x140` | `PADReceiveCheckCallback@Pad` |
| `0x80173624` | `0x80175E98` | `0x110` | `PADReset` |
| `0x80173734` | `0x80175FA8` | `0x114` | `PADRecalibrate` |
| `0x80173848` | `0x801760BC` | `0x150` | `PADInit` |
| `0x80173998` | `0x8017620C` | `0x300` | `PADRead` |
| `0x80173C98` | `0x8017650C` | `0xB8` | `PADControlMotor` |
| `0x80173D50` | `0x801765C4` | `0x60` | `PADSetSpec` |
| `0x80173DB0` | `0x80176624` | `0x174` | `SPEC0_MakeStatus@Pad` |
| `0x80173F24` | `0x80176798` | `0x174` | `SPEC1_MakeStatus@Pad` |
| `0x80174098` | `0x8017690C` | `0x470` | `SPEC2_MakeStatus@Pad` |
| `0x80174508` | `0x80176D7C` | `0xBC` | `OnReset@Pad` |
| `0x801745C4` | `0x80176E38` | `0x60` | `SamplingHandler@Pad` |
| `0x80174624` | `0x80176E98` | `0x54` | `PADSetSamplingCallback` |

### VI (21)

| US | PAL | size | name |
|---|---|---|---|
| `0x80170BD8` | `0x80173488` | `0x274` | `__VIRetraceHandler` |
| `0x80170E4C` | `0x801736FC` | `0x44` | `VISetPreRetraceCallback` |
| `0x80170E90` | `0x80173740` | `0x44` | `VISetPostRetraceCallback` |
| `0x80170ED4` | `0x80173784` | `0xA8` | `getTiming@vi` |
| `0x80170F7C` | `0x8017382C` | `0x204` | `__VIInit@vi` |
| `0x80171180` | `0x80173A30` | `0x4B0` | `VIInit` |
| `0x80171630` | `0x80173EE0` | `0x54` | `VIWaitForRetrace` |
| `0x80171684` | `0x80173F34` | `0x2D4` | `setFbbRegs@vi` |
| `0x80171958` | `0x80174208` | `0x1A0` | `setVerticalRegs@vi` |
| `0x80171AF8` | `0x801743A8` | `0x808` | `VIConfigure` |
| `0x80172300` | `0x80174BB0` | `0x394` | `VIConfigurePan` |
| `0x80172694` | `0x80174F44` | `0x130` | `VIFlush` |
| `0x801727C4` | `0x80175074` | `0x6C` | `VISetNextFrameBuffer` |
| `0x80172830` | `0x801750E0` | `0x7C` | `VISetBlack` |
| `0x801728AC` | `0x8017515C` | `0x8` | `VIGetRetraceCount` |
| `0x801728B4` | `0x80175164` | `0x3C` | `GetCurrentDisplayPosition@vi` |
| `0x801728F0` | `0x801751A0` | `0x68` | `getCurrentFieldEvenOdd@vi` |
| `0x801729F4` | `0x801752A4` | `0x98` | `VIGetCurrentLine` |
| `0x80172AF4` | — | `0x3C` | `VIGetDTVStatus` |
| `0x80172B30` | `0x801753A4` | `0x21C` | `__VIDisplayPositionToXY` |
| `0x80172D4C` | `0x801755C0` | `0x60` | `__VIGetCurrentPosition` |

### DVD (61)

| US | PAL | size | name |
|---|---|---|---|
| `0x8016CBEC` | `0x8016F49C` | `0x84` | `AlarmHandler@dvdlow` |
| `0x8016CCE0` | `0x8016F590` | `0x110` | `Read@dvdlow` |
| `0x8016CDF0` | `0x8016F6A0` | `0x80` | `SeekTwiceBeforeRead@dvdlow` |
| `0x8016CE70` | `0x8016F720` | `0x298` | `DVDLowRead` |
| `0x8016D108` | `0x8016F9B8` | `0x94` | `DVDLowSeek` |
| `0x8016D1C8` | `0x8016FA78` | `0xA4` | `DVDLowReadDiskID` |
| `0x8016D2F8` | `0x8016FBA8` | `0x8C` | `DVDLowRequestError` |
| `0x8016D384` | `0x8016FC34` | `0x9C` | `DVDLowInquiry` |
| `0x8016D420` | `0x8016FCD0` | `0x98` | `DVDLowAudioStream` |
| `0x8016D4B8` | `0x8016FD68` | `0x8C` | `DVDLowRequestAudioStatus` |
| `0x8016D544` | `0x8016FDF4` | `0x9C` | `DVDLowAudioBufferConfig` |
| `0x8016D69C` | `0x8016FF4C` | `0x14` | `DVDLowBreak` |
| `0x8016D6B0` | `0x8016FF60` | `0x1C` | `DVDLowClearCallback` |
| `0x8016D710` | `0x8016FFC0` | `0x38` | `__DVDLowTestAlarm` |
| `0x8016D780` | `0x80170030` | `0x2F4` | `DVDConvertPathToEntrynum` |
| `0x8016DA74` | `0x80170324` | `0x74` | `DVDFastOpen` |
| `0x8016DAE8` | `0x80170398` | `0x24` | `DVDClose` |
| `0x8016DB0C` | `0x801703BC` | `0x60` | `DVDChangeDir` |
| `0x8016DB6C` | `0x8017041C` | `0xC0` | `DVDReadAsyncPrio` |
| `0x8016DC2C` | `0x801704DC` | `0x30` | `cbForReadAsync@dvdfs` |
| `0x8016DD38` | `0x801705E8` | `0x94` | `stateReadingFST@dvd` |
| `0x8016DDCC` | `0x8017067C` | `0x80` | `cbForStateReadingFST@dvd` |
| `0x8016DEF8` | `0x801707A8` | `0x34` | `stateTimeout@dvd` |
| `0x8016DF2C` | `0x801707DC` | `0x28` | `stateGettingError@dvd` |
| `0x8016DF54` | `0x80170804` | `0xB4` | `CategorizeError@dvd` |
| `0x8016E008` | `0x801708B8` | `0x264` | `cbForStateGettingError@dvd` |
| `0x8016E26C` | `0x80170B1C` | `0x5C` | `cbForUnrecoveredError@dvd` |
| `0x8016E2C8` | `0x80170B78` | `0x80` | `cbForUnrecoveredErrorRetry@dvd` |
| `0x8016E348` | `0x80170BF8` | `0x28` | `stateGoToRetry@dvd` |
| `0x8016E370` | `0x80170C20` | `0x140` | `cbForStateGoToRetry@dvd` |
| `0x8016E4B0` | `0x80170D60` | `0xE0` | `stateCheckID@dvd` |
| `0x8016E590` | `0x80170E40` | `0x34` | `stateCheckID3@dvd` |
| `0x8016E5C4` | `0x80170E74` | `0x34` | `stateCheckID2a@dvd` |
| `0x8016E5F8` | `0x80170EA8` | `0x68` | `cbForStateCheckID2a@dvd` |
| `0x8016E660` | `0x80170F10` | `0x38` | `stateCheckID2@dvd` |
| `0x8016E698` | `0x80170F48` | `0xFC` | `cbForStateCheckID1@dvd` |
| `0x8016E794` | `0x80171044` | `0xD8` | `cbForStateCheckID2@dvd` |
| `0x8016E86C` | `0x8017111C` | `0xF0` | `cbForStateCheckID3@dvd` |
| `0x8016E9A0` | `0x80171250` | `0xD4` | `stateCoverClosed@dvd` |
| `0x8016EA74` | `0x80171324` | `0x30` | `stateCoverClosed_CMD@dvd` |
| `0x8016EAA4` | `0x80171354` | `0x64` | `cbForStateCoverClosed@dvd` |
| `0x8016EB08` | `0x801713B8` | `0x28` | `stateMotorStopped@dvd` |
| `0x8016F244` | `0x80171AF4` | `0x658` | `cbForStateBusy@dvd` |
| `0x8016F89C` | `0x8017214C` | `0xDC` | `DVDReadAbsAsyncPrio` |
| `0x8016F978` | `0x80172228` | `0xD0` | `DVDReadAbsAsyncForBS` |
| `0x8016FB1C` | `0x801723CC` | `0xBC` | `DVDCancelStreamAsync` |
| `0x8016FCEC` | `0x8017259C` | `0x4C` | `DVDGetCommandBlockStatus` |
| `0x8016FDE4` | `0x80172694` | `0x10` | `DVDSetAutoInvalidation` |
| `0x8016FDF4` | `0x801726A4` | `0x50` | `DVDResume` |
| `0x8016FE44` | `0x801726F4` | `0x27C` | `DVDCancelAsync` |
| `0x801700C0` | `0x80172970` | `0xAC` | `DVDCancel` |
| `0x8017016C` | `0x80172A1C` | `0x24` | `cbForCancelSync@dvd` |
| `0x80170190` | `0x80172A40` | `0x8` | `DVDGetCurrentDiskID` |
| `0x80170198` | `0x80172A48` | `0xF8` | `DVDCheckDisk` |
| `0x80170290` | `0x80172B40` | `0x11C` | `__DVDPrepareResetAsync` |
| `0x801703AC` | `0x80172C5C` | `0x38` | `__DVDTestAlarm` |
| `0x8017057C` | `0x80172E2C` | `0x60` | `__DVDDequeueWaitingQueue` |
| `0x80170774` | `0x80173024` | `0xF8` | `DVDCompareDiskID` |
| `0x8017086C` | `0x8017311C` | `0x8C` | `__DVDShowFatalMessage` |
| `0x801708F8` | `0x801731A8` | `0x70` | `DVDSetAutoFatalMessaging` |
| `0x80170968` | `0x80173218` | `0x30` | `__DVDPrintFatalMessage` |

### AR / ARQ (16)

| US | PAL | size | name |
|---|---|---|---|
| `0x80175000` | `0x80177868` | `0x44` | `ARRegisterDMACallback` |
| `0x80175044` | `0x801778AC` | `0x3C` | `ARGetDMAStatus` |
| `0x80175080` | `0x801778E8` | `0xF0` | `ARStartDMA` |
| `0x80175170` | `0x801779D8` | `0x68` | `ARAlloc` |
| `0x801751D8` | `0x80177A40` | `0x74` | `ARFree` |
| `0x8017524C` | `0x80177AB4` | `0xC4` | `ARInit` |
| `0x80175310` | `0x80177B78` | `0x8` | `ARGetSize` |
| `0x80175318` | `0x80177B80` | `0x78` | `__ARHandler` |
| `0x80175390` | `0x80177BF8` | `0x20` | `__ARClearInterrupt` |
| `0x801753B0` | `0x80177C18` | `0x10` | `__ARGetInterruptStatus` |
| `0x801753C0` | `0x80177C28` | `0x17F4` | `__ARChecksize` |
| `0x80176BB4` | `0x8017941C` | `0x100` | `__ARQServiceQueueLo` |
| `0x80176CB4` | `0x8017951C` | `0x4` | `__ARQCallbackHack` |
| `0x80176CB8` | `0x80179520` | `0xCC` | `__ARQInterruptServiceRoutine` |
| `0x80176D84` | `0x801795EC` | `0x70` | `ARQInit` |
| `0x80176DF4` | `0x8017965C` | `0x15C` | `ARQPostRequest` |

### AI (11)

| US | PAL | size | name |
|---|---|---|---|
| `0x801746F4` | `0x80176F68` | `0x44` | `AIRegisterDMACallback` |
| `0x80174738` | `0x80176FAC` | `0x88` | `AIInitDMA` |
| `0x801747C0` | `0x80177034` | `0x18` | `AIStartDMA` |
| `0x801747D8` | `0x8017704C` | `0x18` | `AIStopDMA` |
| `0x801747F0` | `0x80177064` | `0x1C` | `AIGetDMAStartAddr` |
| `0x801748F4` | `0x80177168` | `0xE0` | `AISetDSPSampleRate` |
| `0x80174ACC` | `0x80177340` | `0x1C` | `AISetStreamVolLeft` |
| `0x80174AF8` | `0x8017736C` | `0x1C` | `AISetStreamVolRight` |
| `0x80174B24` | `0x80177398` | `0x16C` | `AIInit` |
| `0x80174C90` | — | `0xC` | `AIReset` |
| `0x80174D18` | `0x80177580` | `0xAC` | `__AIDHandler` |

### DSP (10)

| US | PAL | size | name |
|---|---|---|---|
| `0x8017CD80` | `0x8017F5E8` | `0x10` | `DSPCheckMailToDSP` |
| `0x8017CD90` | `0x8017F5F8` | `0x10` | `DSPCheckMailFromDSP` |
| `0x8017CDA0` | `0x8017F608` | `0x18` | `DSPReadMailFromDSP` |
| `0x8017CDB8` | `0x8017F620` | `0x14` | `DSPSendMailToDSP` |
| `0x8017CDCC` | `0x8017F634` | `0xC4` | `DSPInit` |
| `0x8017CE98` | `0x8017F700` | `0x70` | `DSPAddTask` |
| `0x8017CF48` | `0x8017F7B0` | `0xC8` | `DSPAssertTask` |
| `0x8017D060` | `0x8017F8C8` | `0x424` | `__DSPHandler` |
| `0x8017D484` | `0x8017FCEC` | `0x1A0` | `__DSP_exec_task` |
| `0x8017D624` | `0x8017FE8C` | `0x18C` | `__DSP_boot_task` |

### AX (26)

| US | PAL | size | name |
|---|---|---|---|
| `0x80176F50` | `0x801797B8` | `0x24` | `AXInit` |
| `0x80176F74` | `0x801797DC` | `0x4C` | `AXInitEx` |
| `0x80178134` | `0x8017A99C` | `0x1B8` | `__AXOutNewFrame` |
| `0x801782EC` | `0x8017AB54` | `0xBC` | `__AXOutAiCallback` |
| `0x801783A8` | `0x8017AC10` | `0xC` | `__AXDSPInitCallback` |
| `0x801783B4` | `0x8017AC1C` | `0x58` | `__AXDSPResumeCallback` |
| `0x8017840C` | `0x8017AC74` | `0x2C` | `__AXDSPDoneCallback` |
| `0x80178438` | `0x8017ACA0` | `0xCC` | `__AXOutInitDSP` |
| `0x80178504` | `0x8017AD6C` | `0x3A0` | `__AXOutInit` |
| `0x801788FC` | `0x8017B164` | `0x44` | `AXRegisterCallback` |
| `0x80179A2C` | `0x8017C294` | `0xC4` | `AXSetVoiceSrcType` |
| `0x80179AF0` | `0x8017C358` | `0x5C` | `AXSetVoiceState` |
| `0x80179B4C` | `0x8017C3B4` | `0x254` | `AXSetVoiceMix` |
| `0x80179DA0` | `0x8017C608` | `0x64` | `AXSetVoiceItdOn` |
| `0x80179E04` | `0x8017C66C` | `0x5C` | `AXSetVoiceItdTarget` |
| `0x80179E60` | `0x8017C6C8` | `0x58` | `AXSetVoiceVe` |
| `0x80179EB8` | `0x8017C720` | `0x4C` | `AXSetVoiceVeDelta` |
| `0x80179F04` | `0x8017C76C` | `0x110` | `AXSetVoiceAddr` |
| `0x8017A014` | `0x8017C87C` | `0x4C` | `AXSetVoiceLoop` |
| `0x8017A060` | `0x8017C8C8` | `0x54` | `AXSetVoiceLoopAddr` |
| `0x8017A0B4` | `0x8017C91C` | `0x54` | `AXSetVoiceEndAddr` |
| `0x8017A108` | `0x8017C970` | `0x54` | `AXSetVoiceCurrentAddr` |
| `0x8017A15C` | `0x8017C9C4` | `0xA4` | `AXSetVoiceAdpcm` |
| `0x8017A200` | `0x8017CA68` | `0x98` | `AXSetVoiceSrc` |
| `0x8017A298` | `0x8017CB00` | `0x98` | `AXSetVoiceSrcRatio` |
| `0x8017A330` | `0x8017CB98` | `0x6C` | `AXSetVoiceAdpcmLoop` |

### AXFX (1)

| US | PAL | size | name |
|---|---|---|---|
| `0x8017CD74` | `0x8017F5DC` | `0xC` | `AXFXSetHooks` |

### GX (113)

| US | PAL | size | name |
|---|---|---|---|
| `0x8018447C` | `0x80186CE4` | `0xFC` | `__GXDefaultTexRegionCallback` |
| `0x80184578` | `0x80186DE0` | `0x24` | `__GXDefaultTlutRegionCallback` |
| `0x8018459C` | `0x80186E04` | `0x190` | `__GXShutdown` |
| `0x80185808` | `0x80188070` | `0x134` | `GXCPInterruptHandler` |
| `0x80185CEC` | `0x80188554` | `0x44` | `GXSetBreakPtCallback` |
| `0x80185D7C` | `0x801885E4` | `0x24` | `__GXFifoReadEnable` |
| `0x80185DA0` | `0x80188608` | `0x24` | `__GXFifoReadDisable` |
| `0x80185DC4` | `0x8018862C` | `0x34` | `__GXFifoLink` |
| `0x80185DF8` | `0x80188660` | `0x30` | `__GXWriteFifoIntEnable` |
| `0x80185E28` | `0x80188690` | `0x30` | `__GXWriteFifoIntReset` |
| `0x80185E58` | `0x801886C0` | `0x100` | `__GXCleanGPFifo` |
| `0x80185F58` | `0x801887C0` | `0x8` | `GXGetCPUFifo` |
| `0x80185F60` | `0x801887C8` | `0x8` | `GXGetGPFifo` |
| `0x8018645C` | `0x80188CC4` | `0xBC` | `__GXSetVCD` |
| `0x80186D94` | `0x801895FC` | `0x88` | `__GXSetVAT` |
| `0x80187110` | `0x80189978` | `0x44` | `GXSetArray` |
| `0x80187154` | `0x801899BC` | `0x10` | `GXInvalidateVtxCache` |
| `0x80187164` | `0x801899CC` | `0x280` | `GXSetTexCoordGen2` |
| `0x801873E4` | `0x80189C4C` | `0x3C` | `GXSetNumTexGens` |
| `0x801874B4` | `0x80189D1C` | `0x5C` | `GXFlush` |
| `0x80187510` | `0x80189D78` | `0x16C` | `__GXAbort` |
| `0x8018767C` | `0x80189EE4` | `0x1C8` | `GXAbortFrame` |
| `0x80187844` | `0x8018A0AC` | `0x98` | `GXSetDrawDone` |
| `0x801878DC` | `0x8018A144` | `0x4C` | `GXWaitDrawDone` |
| `0x80187928` | `0x8018A190` | `0x80` | `GXDrawDone` |
| `0x801879A8` | `0x8018A210` | `0x24` | `GXPixModeSync` |
| `0x801879CC` | `0x8018A234` | `0x14` | `GXPokeAlphaMode` |
| `0x801879E0` | `0x8018A248` | `0x20` | `GXPokeAlphaRead` |
| `0x80187A00` | `0x8018A268` | `0x18` | `GXPokeAlphaUpdate` |
| `0x80187A18` | `0x8018A280` | `0x64` | `GXPokeBlendMode` |
| `0x80187A7C` | `0x8018A2E4` | `0x18` | `GXPokeColorUpdate` |
| `0x80187A94` | `0x8018A2FC` | `0x24` | `GXPokeDstAlpha` |
| `0x80187AB8` | `0x8018A320` | `0x18` | `GXPokeDither` |
| `0x80187AD0` | `0x8018A338` | `0x20` | `GXPokeZMode` |
| `0x80187AF0` | `0x8018A358` | `0x44` | `GXSetDrawSyncCallback` |
| `0x80187B34` | `0x8018A39C` | `0x88` | `GXTokenInterruptHandler` |
| `0x80187BBC` | `0x8018A424` | `0x44` | `GXSetDrawDoneCallback` |
| `0x80187C00` | `0x8018A468` | `0x80` | `GXFinishInterruptHandler` |
| `0x80187CF4` | `0x8018A55C` | `0x80` | `__GXSetDirtyState` |
| `0x80187D74` | `0x8018A5DC` | `0xD0` | `GXBegin` |
| `0x80187ECC` | `0x8018A734` | `0x40` | `GXSetLineWidth` |
| `0x80187F0C` | `0x8018A774` | `0x40` | `GXSetPointSize` |
| `0x80187F4C` | `0x8018A7B4` | `0x48` | `GXEnableTexOffsets` |
| `0x80187F94` | `0x8018A7FC` | `0x28` | `GXSetCullMode` |
| `0x80187FBC` | `0x8018A824` | `0x34` | `GXSetCoPlanar` |
| `0x80187FF0` | `0x8018A858` | `0x24` | `__GXSetGenMode` |
| `0x80188430` | `0x8018AC98` | `0xCC` | `GXSetDispCopyYScale` |
| `0x801884FC` | `0x8018AD64` | `0x78` | `GXSetCopyClear` |
| `0x80188574` | `0x8018ADDC` | `0x208` | `GXSetCopyFilter` |
| `0x8018877C` | `0x8018AFE4` | `0x14` | `GXSetDispCopyGamma` |
| `0x80188790` | `0x8018AFF8` | `0x168` | `GXCopyDisp` |
| `0x801888F8` | `0x8018B160` | `0x18C` | `GXCopyTex` |
| `0x80188A84` | `0x8018B2EC` | `0x38` | `GXClearBoundingBox` |
| `0x80188ABC` | `0x8018B324` | `0x1C` | `GXInitLightAttn` |
| `0x80188D38` | `0x8018B5A0` | `0x10` | `GXInitLightPos` |
| `0x80188D48` | `0x8018B5B0` | `0x1C` | `GXInitLightDir` |
| `0x80188D64` | `0x8018B5CC` | `0xC` | `GXInitLightColor` |
| `0x80188D70` | `0x8018B5D8` | `0x7C` | `GXLoadLightObjImm` |
| `0x80188DEC` | `0x8018B654` | `0xE8` | `GXSetChanAmbColor` |
| `0x80188ED4` | `0x8018B73C` | `0xE8` | `GXSetChanMatColor` |
| `0x80188FBC` | `0x8018B824` | `0x3C` | `GXSetNumChans` |
| `0x80188FF8` | `0x8018B860` | `0xB0` | `GXSetChanCtrl` |
| `0x801896E4` | `0x8018BF4C` | `0x8` | `GXGetTexObjFmt` |
| `0x801896EC` | `0x8018BF54` | `0x18` | `GXGetTexObjMipMap` |
| `0x80189704` | `0x8018BF6C` | `0x17C` | `GXLoadTexObjPreLoaded` |
| `0x80189880` | `0x8018C0E8` | `0x54` | `GXLoadTexObj` |
| `0x8018990C` | `0x8018C174` | `0x98` | `GXLoadTlut` |
| `0x80189AD0` | `0x8018C338` | `0x48` | `GXInvalidateTexAll` |
| `0x80189B40` | `0x8018C3A8` | `0xA0` | `__SetSURegs` |
| `0x80189BE0` | `0x8018C448` | `0x17C` | `__GXSetSUTexRegs` |
| `0x8018A0B0` | `0x8018C918` | `0x6C` | `GXSetTevIndirect` |
| `0x8018A11C` | `0x8018C984` | `0x144` | `GXSetIndTexCoordScale` |
| `0x8018A324` | `0x8018CB8C` | `0x8C` | `GXSetTevOp` |
| `0x8018A3B0` | `0x8018CC18` | `0x44` | `GXSetTevColorIn` |
| `0x8018A3F4` | `0x8018CC5C` | `0x44` | `GXSetTevAlphaIn` |
| `0x8018A438` | `0x8018CCA0` | `0x68` | `GXSetTevColorOp` |
| `0x8018A4A0` | `0x8018CD08` | `0x68` | `GXSetTevAlphaOp` |
| `0x8018A508` | `0x8018CD70` | `0x60` | `GXSetTevColor` |
| `0x8018A5CC` | `0x8018CE34` | `0x64` | `GXSetTevKColor` |
| `0x8018A630` | `0x8018CE98` | `0x5C` | `GXSetTevKColorSel` |
| `0x8018A68C` | `0x8018CEF4` | `0x5C` | `GXSetTevKAlphaSel` |
| `0x8018A6E8` | `0x8018CF50` | `0x48` | `GXSetTevSwapMode` |
| `0x8018A730` | `0x8018CF98` | `0x80` | `GXSetTevSwapModeTable` |
| `0x8018A7B0` | `0x8018D018` | `0x44` | `GXSetAlphaCompare` |
| `0x8018A7F4` | `0x8018D05C` | `0x8C` | `GXSetZTexture` |
| `0x8018A880` | `0x8018D0E8` | `0x19C` | `GXSetTevOrder` |
| `0x8018AA44` | `0x8018D2AC` | `0x214` | `GXSetFog` |
| `0x8018AE08` | `0x8018D670` | `0x124` | `GXSetFogRangeAdj` |
| `0x8018AF2C` | `0x8018D794` | `0x54` | `GXSetBlendMode` |
| `0x8018AF80` | `0x8018D7E8` | `0x2C` | `GXSetColorUpdate` |
| `0x8018AFAC` | `0x8018D814` | `0x2C` | `GXSetAlphaUpdate` |
| `0x8018AFD8` | `0x8018D840` | `0x34` | `GXSetZMode` |
| `0x8018B00C` | `0x8018D874` | `0x34` | `GXSetZCompLoc` |
| `0x8018B040` | `0x8018D8A8` | `0xD4` | `GXSetPixelFmt` |
| `0x8018B114` | `0x8018D97C` | `0x2C` | `GXSetDither` |
| `0x8018B140` | `0x8018D9A8` | `0x3C` | `GXSetDstAlpha` |
| `0x8018B17C` | `0x8018D9E4` | `0x38` | `GXSetFieldMask` |
| `0x8018B1B4` | `0x8018DA1C` | `0x78` | `GXSetFieldMode` |
| `0x8018C0DC` | `0x8018E944` | `0x70` | `GXCallDisplayList` |
| `0x8018C2C0` | `0x8018EB28` | `0xA4` | `GXSetProjection` |
| `0x8018C364` | `0x8018EBCC` | `0x8C` | `GXSetProjectionv` |
| `0x8018C438` | `0x8018ECA0` | `0x50` | `GXLoadPosMtxImm` |
| `0x8018C488` | `0x8018ECF0` | `0x50` | `GXLoadNrmMtxImm` |
| `0x8018C4D8` | `0x8018ED40` | `0x34` | `GXSetCurrentMtx` |
| `0x8018C50C` | `0x8018ED74` | `0xB4` | `GXLoadTexMtxImm` |
| `0x8018C5C0` | `0x8018EE28` | `0x90` | `__GXSetViewport` |
| `0x8018C6A8` | `0x8018EF10` | `0x48` | `GXSetViewport` |
| `0x8018C714` | `0x8018EF7C` | `0x78` | `GXSetScissor` |
| `0x8018C78C` | `0x8018EFF4` | `0x40` | `GXSetScissorBoxOffset` |
| `0x8018C7CC` | `0x8018F034` | `0x28` | `GXSetClipMode` |
| `0x8018C7F4` | `0x8018F05C` | `0x84` | `__GXSetMatrixIndex` |
| `0x8018C878` | `0x8018F0E0` | `0x848` | `GXSetGPMetric` |
| `0x8018D0C0` | `0x8018F928` | `0x10` | `GXClearGPMetric` |

### CARD (79)

| US | PAL | size | name |
|---|---|---|---|
| `0x8017D8E8` | `0x80180150` | `0x34` | `__CARDSyncCallback` |
| `0x8017D91C` | `0x80180184` | `0xD8` | `__CARDExtHandler` |
| `0x8017D9F4` | `0x8018025C` | `0x118` | `__CARDExiHandler` |
| `0x8017DB0C` | `0x80180374` | `0xA8` | `__CARDTxHandler` |
| `0x8017DBB4` | `0x8018041C` | `0x84` | `__CARDUnlockedHandler` |
| `0x8017DC38` | `0x801804A0` | `0xC0` | `__CARDEnableInterrupt` |
| `0x8017DCF8` | `0x80180560` | `0xF0` | `__CARDReadStatus` |
| `0x8017DED8` | `0x80180740` | `0xAC` | `__CARDClearStatus` |
| `0x8017DF84` | `0x801807EC` | `0xA4` | `TimeoutHandler@CARDBios` |
| `0x8017E2C8` | `0x80180B30` | `0x110` | `UnlockedCallback@CARDBios` |
| `0x8017E5FC` | `0x80180E64` | `0x134` | `__CARDReadSegment` |
| `0x8017E97C` | `0x801811E4` | `0xAC` | `CARDInit` |
| `0x8017EA28` | `0x80181290` | `0x8` | `__CARDGetFontEncode` |
| `0x8017EA30` | `0x80181298` | `0x38` | `__CARDSetDiskID` |
| `0x8017EA68` | `0x801812D0` | `0xB8` | `__CARDGetControlBlock` |
| `0x8017EB20` | `0x80181388` | `0x64` | `__CARDPutControlBlock` |
| `0x8017EB84` | `0x801813EC` | `0x150` | `CARDFreeBlocks` |
| `0x8017ECD4` | `0x8018153C` | `0x84` | `CARDGetSectorSize` |
| `0x8017ED58` | `0x801815C0` | `0x98` | `__CARDSync` |
| `0x8017EDF0` | `0x80181658` | `0x50` | `OnReset@CARDBios` |
| `0x8017EE90` | `0x801816F8` | `0x16C` | `bitrev@CARDUnlock` |
| `0x8017EFFC` | `0x80181864` | `0x144` | `ReadArrayUnlock@CARDUnlock` |
| `0x8017F140` | `0x801819A8` | `0xC4` | `DummyLen@CARDUnlock` |
| `0x8017F204` | `0x80181A6C` | `0xB58` | `__CARDUnlock` |
| `0x8017FD5C` | `0x801825C4` | `0x70` | `InitCallback@CARDUnlock` |
| `0x8017FDCC` | `0x80182634` | `0x324` | `DoneCallback@CARDUnlock` |
| `0x801800F0` | `0x80182958` | `0xDC` | `BlockReadCallback@CARDRdwr` |
| `0x801801CC` | `0x80182A34` | `0x64` | `__CARDRead` |
| `0x80180380` | `0x80182BE8` | `0x8` | `__CARDGetFatBlock` |
| `0x80180388` | `0x80182BF0` | `0xD4` | `WriteCallback@CARDBlock` |
| `0x8018045C` | `0x80182CC4` | `0xC8` | `EraseCallback@CARDBlock` |
| `0x80180524` | `0x80182D8C` | `0x118` | `__CARDAllocBlock` |
| `0x801806D8` | `0x80182F40` | `0xAC` | `__CARDUpdateFatBlock` |
| `0x80180784` | `0x80182FEC` | `0x8` | `__CARDGetDirBlock` |
| `0x8018078C` | `0x80182FF4` | `0xD0` | `WriteCallback@CARDDir` |
| `0x8018085C` | `0x801830C4` | `0xC8` | `EraseCallback@CARDDir` |
| `0x80180924` | `0x8018318C` | `0xC4` | `__CARDUpdateDir` |
| `0x801809E8` | `0x80183250` | `0x1B0` | `__CARDCheckSum` |
| `0x80180B98` | `0x80183400` | `0x284` | `VerifyID@CARDCheck` |
| `0x80180E1C` | `0x80183684` | `0x240` | `VerifyDir@CARDCheck` |
| `0x8018105C` | `0x801838C4` | `0x284` | `VerifyFAT@CARDCheck` |
| `0x801812E0` | `0x80183B48` | `0x8C` | `__CARDVerify` |
| `0x8018136C` | `0x80183BD4` | `0x590` | `CARDCheckExAsync` |
| `0x801818FC` | `0x80184164` | `0x54` | `CARDCheck` |
| `0x80181950` | `0x801841B8` | `0xCC` | `IsCard@CARDMount` |
| `0x80181A1C` | `0x80184284` | `0x17C` | `CARDProbeEx` |
| `0x80181FEC` | `0x80184854` | `0x138` | `__CARDMountCallback` |
| `0x80182124` | `0x8018498C` | `0x1A0` | `CARDMountAsync` |
| `0x801822C4` | `0x80184B2C` | `0x48` | `CARDMount` |
| `0x8018230C` | `0x80184B74` | `0x9C` | `DoUnmount@CARDMount` |
| `0x801823A8` | `0x80184C10` | `0xAC` | `CARDUnmount` |
| `0x80182454` | `0x80184CBC` | `0x144` | `FormatCallback@CARDFormat` |
| `0x80182598` | `0x80184E00` | `0x658` | `__CARDFormatRegionAsync` |
| `0x80182BF0` | `0x80185458` | `0x54` | `CARDFormat` |
| `0x80182C44` | `0x801854AC` | `0x68` | `__CARDCompareFileName` |
| `0x80182CAC` | `0x80185514` | `0x94` | `__CARDAccess` |
| `0x80182D40` | `0x801855A8` | `0x134` | `__CARDIsWritable` |
| `0x80182E74` | `0x801856DC` | `0xF4` | `__CARDIsReadable` |
| `0x80182F68` | `0x801857D0` | `0x150` | `__CARDGetFileNo` |
| `0x801830B8` | `0x80185920` | `0x11C` | `CARDOpen` |
| `0x801831D4` | `0x80185A3C` | `0x54` | `CARDClose` |
| `0x80183230` | `0x80185A98` | `0x130` | `CreateCallbackFat@CARDCreate` |
| `0x80183360` | `0x80185BC8` | `0x220` | `CARDCreateAsync` |
| `0x80183580` | `0x80185DE8` | `0x48` | `CARDCreate` |
| `0x801835C8` | `0x80185E30` | `0x1B8` | `__CARDSeek` |
| `0x80183780` | `0x80185FE8` | `0x130` | `ReadCallback@CARDRead` |
| `0x801838B0` | `0x80186118` | `0x144` | `CARDReadAsync` |
| `0x801839F4` | `0x8018625C` | `0x48` | `CARDRead` |
| `0x80183A3C` | `0x801862A4` | `0x170` | `WriteCallback@CARDWrite` |
| `0x80183BAC` | `0x80186414` | `0xB0` | `EraseCallback@CARDWrite` |
| `0x80183C5C` | `0x801864C4` | `0x114` | `CARDWriteAsync` |
| `0x80183D70` | `0x801865D8` | `0x48` | `CARDWrite` |
| `0x80183DB8` | `0x80186620` | `0xA4` | `DeleteCallback@CARDDelete` |
| `0x80183E5C` | `0x801866C4` | `0x110` | `CARDDeleteAsync` |
| `0x80183F6C` | `0x801867D4` | `0x48` | `CARDDelete` |
| `0x80183FB4` | `0x8018681C` | `0x1F8` | `UpdateIconOffsets@CARDStat` |
| `0x801841AC` | `0x80186A14` | `0x114` | `CARDGetStatus` |
| `0x801842C0` | `0x80186B28` | `0x174` | `CARDSetStatusAsync` |
| `0x80184434` | `0x80186C9C` | `0x48` | `CARDSetStatus` |

### THP (2)

| US | PAL | size | name |
|---|---|---|---|
| `0x801B70B4` | `0x801B99B0` | `0x114` | `THPSimpleInit` |
| `0x801FD144` | `0x801FFB38` | `0xA0` | `THPInit` |

### MSL (3)

| US | PAL | size | name |
|---|---|---|---|
| `0x801569E0` | `0x80159290` | `0x98` | `vprintf` |
| `0x80158A0C` | `0x8015B2BC` | `0x8` | `srand` |
| `0x80158A14` | `0x8015B2C4` | `0x20` | `rand` |

---

## Appendix B. Game and HAL library functions

Functions outside the SDK whose role was established. All are `cbr_decomp` `fn_XXXXXXXX` symbols. Names are unknown unless stated. US and PAL addresses were each read in the disassembly of their version, or matched by identical code after masking addresses. Sizes are the `cbr_decomp` bounds at the US address.

### Game core

| US | PAL | size | role |
|---|---|---|---|
| `0x8000A4B8` | `0x8000A4D8` | `0x68` | `CGame` vtable +`0x28`: frame interval (section 8) |
| `0x8000A678` | `0x8000A698` | `0x60` | AI DMA callback: calls the previous callback, then polls the microphone driver |
| `0x8000A6D8` | `0x8000A6F8` | `0x180` | microphone state machine: probes slot B every frame during gameplay |
| `0x8000DFA8` | `0x8000DFE4` | `0x194C` | `CGame` vtable +`0x1C`: called every frame after +`0x18` |
| `0x8000FACC` | `0x8000FB30` | `0x1204` | `CGame` vtable +`0x18`: called every frame with the elapsed fields; polls the save thread |
| `0x80011BD8` | `0x80011D00` | `0xE5C` | `CGame` vtable +`0x14`: called every frame; runs the scene loader |
| `0x80012A8C` | `0x80012C08` | `0xAB0` | `CGame` vtable +`0x10`: initialisation of all subsystems |
| `0x8001D8C0` | `0x8001DCA4` | `0xE8` | system init: GQR2–GQR5, `srand(OSGetTick())`, arena, HAL system init |
| `0x8006EE24` | `0x80070C70` | `0x514` | scene loader (state machine; builds the `.bin` and `.rel` paths) |
| `0x801A3F18` | `0x801A6790` | `0x244` | issues a HAL `devcom.c` request, then busy-waits until a state field leaves 2 |
| `0x801A923C` | `0x801ABB38` | `0x88` | REL module unloader: epilog, `OSUnlink`, frees BSS and module |
| `0x801A92C4` | `0x801ABBC0` | `0x104` | REL module loader: reads the file, allocates BSS, `OSLink`, calls the prolog |

All `[verified: Ghidra]`.

### Object-loader threads

| US | PAL | size | role |
|---|---|---|---|
| `0x80131FAC` | `0x801347B8` | `0x94` | waits for a loaded object: `OSYieldThread` loop on a virtual predicate |
| `0x80132040` | `0x8013484C` | `0xB4` | posts a request to a loader thread (one of 8 slots, blocking `OSSendMessage`) |
| `0x801320F4` | `0x80134900` | `0x4C` | loader thread body: `OSReceiveMessage`, then a virtual method |
| `0x801321FC` | `0x80134A08` | `0xA8` | waits for a loaded object (second variant) |
| `0x801322A4` | `0x80134AB0` | `0xB4` | posts a request (second variant) |
| `0x80132358` | `0x80134B64` | `0x4C` | loader thread body |
| `0x801323DC` | `0x80134BE8` | `0x4C` | loader thread body |
| `0x80132504` | `0x80134D10` | `0xC8` | creates a loader thread: queue of 8 messages, 8 KiB stack, `OSCreateThread` (priority 16 or 17, detached), `OSResumeThread` |

All `[verified: Ghidra + trace]`.

### Memory card

Wrappers around the CARD API (section 12). All `[verified: Ghidra]`.

| US | PAL | size | wraps |
|---|---|---|---|
| `0x801A93C8` | `0x801ABCC4` | `0x11C` | result code to name (debug output only) |
| `0x801A94E4` | `0x801ABDE0` | `0x54` | `CARDSetStatus` |
| `0x801A9538` | `0x801ABE34` | `0x54` | `CARDGetStatus` |
| `0x801A958C` | `0x801ABE88` | `0xB0` | `CARDFreeBlocks` + `CARDGetSectorSize`; returns free bytes / sector size |
| `0x801A963C` | `0x801ABF38` | `0x70` | `CARDWrite` |
| `0x801A96AC` | `0x801ABFA8` | `0x7C` | `CARDRead` + `DCInvalidateRange` |
| `0x801A9728` | `0x801AC024` | `0x50` | `CARDDelete` |
| `0x801A9778` | `0x801AC074` | `0x38` | `CARDClose` |
| `0x801A97B0` | `0x801AC0AC` | `0x40` | `CARDClose`, keeping the result |
| `0x801A97F0` | `0x801AC0EC` | `0x54` | `CARDOpen` |
| `0x801A9844` | `0x801AC140` | `0x64` | `CARDCreate` |
| `0x801A98A8` | `0x801AC1A4` | `0x40` | `CARDFormat` |
| `0x801A98E8` | `0x801AC1E4` | `0x40` | `CARDCheck` |
| `0x801A9928` | `0x801AC224` | `0x38` | `CARDUnmount` |
| `0x801A9960` | `0x801AC25C` | `0x40` | `CARDUnmount`, keeping the result |
| `0x801A99A0` | `0x801AC29C` | `0x58` | `CARDMount` |
| `0x801A99F8` | `0x801AC2F4` | `0x3C` | `CARDProbeEx`, no retry |
| `0x801A9A34` | `0x801AC330` | `0x6C` | `CARDProbeEx`, with retry |
| `0x801A9AA0` | `0x801AC39C` | `0x34` | `CARDInit` |

Save manager. Each "start" function creates the thread whose entry is listed with it. All `[verified: Ghidra]`.

| US | PAL | size | role |
|---|---|---|---|
| `0x80087190` | `0x800890EC` | `0x378` | reads both copies of a save and keeps the first valid one (section 12) |
| `0x80087854` | `0x800897B0` | `0x1C8` | creates `CHIBIROBO(n)` (`0xC000` bytes) or opens it if it exists, then checks its length |
| `0x80087A1C` | `0x80089978` | `0x24` | per-frame card probe, called by the title and in-game menus |
| `0x80088848` | `0x8008A7E4` | `0x58` | has the save thread finished (`OSIsThreadTerminated`) |
| `0x80088B4C` / `0x800888A0` | `0x8008AAE8` / `0x8008A83C` | `0xAC` / `0x278` | start / thread: delete slot *n* |
| `0x80088E78` / `0x80088BF8` | `0x8008AE14` / `0x8008AB94` | `0xA8` / `0x250` | start / thread: check the card, format only after −6 or −13 |
| `0x80089288` / `0x80088F20` | `0x8008B224` / `0x8008AEBC` | `0xB4` / `0x338` | start / thread: find whether any save exists |
| `0x80089890` / `0x8008933C` | `0x8008B82C` / `0x8008B2D8` | `0xEC` / `0x524` | start / thread: list slots 0–9 and free blocks |
| `0x80089C90` / `0x8008997C` | `0x8008BC2C` / `0x8008B918` | `0xAC` / `0x2E0` | start / thread: load slot *n* |
| `0x8008A1F4` / `0x80089D3C` | `0x8008C190` / `0x8008BCD8` | `0xAC` / `0x484` | start / thread: save slot *n* |
| `0x8008A2A8` | `0x8008C244` | `0x6C` | save manager init: `CARDInit`, a CARD library setting call with `TRUE` (US `0x8017EE40` / PAL `0x801816A8`), clears the manager object |

### HAL graphics

All `[verified: Ghidra + trace]`.

| US | PAL | size | role |
|---|---|---|---|
| `0x801AB6BC` | `0x801ADFB8` | `0x2A0` | `shadow.c`: writes vertex data inline |
| `0x801AD29C` | `0x801AFB98` | `0x31C` | `pobj.c`: re-emits a display list's vertices inline, with CPU-transformed positions and normals |
| `0x801B1C9C` | `0x801B4598` | `0xE78` | `psdisp.c` (particle display): writes vertex data inline |
| `0x801CA87C` | `0x801CD178` | `0x13C` | system init: `DVDInit`, `GXInit`, video init, `VIWaitForRetrace` |
| `0x801CB850` | `0x801CE14C` | `0x44` | draw-done callback: clears a HAL flag, calls an optional user callback |
| `0x801CB8A4` | `0x801CE1A0` | `0x258` | display copy, once per frame (`GXCopyDisp`) |
| `0x801CBAFC` | `0x801CE3F8` | `0x208` | end of frame, called by `main`: `GXSetDrawDone` once per frame |
| `0x801CBE78` | `0x801CE774` | `0x228` | video init: `VIInit`, `VIGetTvFormat`, `GXSetDrawDoneCallback` |
| `0x801CD714` | `0x801D0010` | `0x26C` | pad init: `PADInit` |
| `0x801D8F48` | `0x801DB844` | `0x4FC` | TEV constant colours: `GXSetTevKColor` and `GXSetTevColor` from an uninitialised stack array (section 14) |
| `0x801D9444` | `0x801DBD40` | `0x6C` | caller of the previous function |

### Other inline vertex writers

These functions write vertex data to the pipe after a call to `GXBegin` `[verified: Ghidra]`. With `shadow.c`, `pobj.c` and `psdisp.c` above, they are the 26 writers outside the SDK.

| US | PAL | size | note |
|---|---|---|---|
| `0x801943FC` | `0x80196C64` | `0xBD8` | |
| `0x8019BC60` | `0x8019E4D8` | `0x2A4` | game C++ drawing framework |
| `0x8019BF04` | `0x8019E77C` | `0x1FC` | game C++ drawing framework |
| `0x8019C258` | `0x8019EAD0` | `0x2F4` | game C++ drawing framework |
| `0x8019C54C` | `0x8019EDC4` | `0x200` | game C++ drawing framework |
| `0x8019C8A4` | `0x8019F11C` | `0x294` | game C++ drawing framework |
| `0x8019CB38` | `0x8019F3B0` | `0x284` | game C++ drawing framework |
| `0x8019CDBC` | `0x8019F634` | `0x2A4` | game C++ drawing framework |
| `0x8019D060` | `0x8019F8D8` | `0x29C` | game C++ drawing framework |
| `0x8019D2FC` | `0x8019FB74` | `0x284` | game C++ drawing framework |
| `0x8019D580` | `0x8019FDF8` | `0x264` | game C++ drawing framework |
| `0x8019D7E4` | `0x801A005C` | `0x280` | game C++ drawing framework |
| `0x8019DA64` | `0x801A02DC` | `0x298` | game C++ drawing framework |
| `0x8019DCFC` | `0x801A0574` | `0x260` | game C++ drawing framework |
| `0x8019FF28` | `0x801A27A0` | `0x1B8` | |
| `0x801A00E0` | `0x801A2958` | `0x224` | textured sprite: strips of 4 vertices (float position, 32-bit colour, 8-bit texture coordinates); the heaviest writer |
| `0x801A66A4` | `0x801A8F1C` | `0x159C` | also makes EFB-to-texture copies |
| `0x801A7FC4` | `0x801AA8B8` | `0x32C` | |
| `0x801A839C` | `0x801AAC5C` | `0x404` | |
| `0x801B3218` | `0x801B5B14` | `0xCDC` | |
| `0x801B4934` | `0x801B7230` | `0x464` | |
| `0x801B4D98` | `0x801B7694` | `0x6F4` | |
| `0x801B71D8` | `0x801B9AD4` | `0xD0` | |

### HAL sound, THP sample and microphone driver

| US | PAL | size | role |
|---|---|---|---|
| `0x801B5B1C` | `0x801B8418` | `0x178` | THPSimple AI DMA callback (section 10) `[verified: Ghidra + trace]` |
| `0x801B5D48` | `0x801B8644` | `0x3BC` | THPSimple audio mix: copy, or THP audio added with saturation `[verified: Ghidra]` |
| `0x801E8D84` | `0x801EB778` | `0x30C` | AUX effect setup (section 10) `[verified: Ghidra]` |
| `0x801EA5CC` | `0x801ECFC0` | `0x4D8` | `devcom.c` transfer: `DVDReadAsyncPrio`, then `ARQPostRequest` from the callbacks `[verified: Ghidra + trace]` |
| `0x801EAE54` | `0x801ED848` | `0x2A4` | `devcom.c` transfer (second) `[verified: Ghidra + trace]` |
| `0x801EB3A4` | `0x801EDD98` | `0x270` | `devcom.c` request: disc to RAM to ARAM `[verified: Ghidra]` |
| `0x801EC754` | `0x801EF148` | `0x17C` | loads a sound-effect bank `[verified: Ghidra + trace]` |
| `0x801F07D8` | `0x801F31CC` | `0x1D0` | AX frame callback, every 5 ms: frees voices, synthesizer, stream service `[verified: Ghidra + trace]` |
| `0x801F1078` | `0x801F3A6C` | `0x934` | `.hps` stream service: re-chains ADPCM loop and end addresses `[verified: Ghidra + trace]` |
| `0x801F29A8` | `0x801F539C` | `0x468` | `HSD_AudioInit` (name from its own assertion strings) `[verified: Ghidra]` |
| `0x801F2E10` | `0x801F5804` | `0xEC` | microphone driver init: 2 channels, 3 alarms, one of them periodic (5 ms) `[verified: Ghidra + trace]` |
| `0x801F2EFC` | `0x801F58F0` | `0x10C` | microphone probe (section 9) `[verified: Ghidra]` |
| `0x801F452C` | `0x801F6F20` | `0x120` | microphone periodic alarm handler `[verified: Ghidra + trace]` |
