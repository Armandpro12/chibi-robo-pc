# cpustress: stress test of recompiled-code instruction patterns

A small Windows x64 program, separate from the game, that helps decide whether an unexplained crash in statically
recompiled code (for example a general-protection fault on `mov dword ptr [rsi+0x280], imm32`, an instruction that
cannot fault with a valid base register) comes from an unstable processor.

It generates at run time x86-64 code with the shapes a static recompiler emits for guest function prologues, then
runs it on every logical processor for a few minutes and checks every result.

```
cpustress.exe                  (3 minutes, all logical processors)
cpustress.exe --seconds 600    (longer)
```

The machine is fully loaded while it runs: do not work on it at the same time. Ctrl+C stops early and still prints the
verdict. No system setting is changed (no priority class, power or affinity setting outside the program's own
threads).

## What it runs

| Pattern | Shape (base register = a 0x400-byte "CPU state" block) |
| --- | --- |
| `pc-run` | 8 byte-swapped guest-memory stores guarded by a never-taken `cmp byte [rsi+0x344],0; jne`, then N groups `mov dword [rsi+0x280], imm32; mov eax,[rsi+a]; mov [rsi+b],eax` (16 bytes each), then `mov dword [rsi+0x10],0`, more pc stores, `movabs rax; mov [rsi+0x280], rax`. N = 1, 2, 4, 8, 10, 12, 16. |
| `pc-only` | N back-to-back `mov dword [rsi+0x280], imm32`, same tail. N = 2 to 32. |
| `stack-reload` | the context pointer reloaded from `[rsp+0x38]` before each copy (`mov rcx,[rsp+0x38]; mov eax,[rcx+a]; mov [rcx+b],eax`), one `mov dword [rcx+0x280], imm32`, `mov dword [rax+0x284], imm32`, `mov dword [rsp+0x44], imm32`. N = 1, 4, 7, 10. |
| `mixed` | a seeded pseudo-random mix of the forms above. |

Each (pattern, length) is generated 64 times, with its first pc store at every offset modulo 64 bytes; the 1216
instances are spread over 32 MiB of code (`--code-mb`), so execution alternates between cold code (instruction cache
and TLB misses) and hot bursts of 8 and 64 calls. The state block is placed at 4 page offsets, one of which crosses a
4 KiB page. Each thread is pinned to one logical processor (`SetThreadGroupAffinity`, every processor group); the
summary shows the physical core and the efficiency class of hybrid processors
(`GetLogicalProcessorInformationEx`).

The `pc-run` encodings are byte for byte those of a recompiled prologue (`C7 86 80 02 00 00 imm32`, `8B 46 xx`,
`89 46 xx`); `stack-reload` reproduces the code generated when the compiler keeps the context pointer on the stack
(`48 8B 4C 24 38`, `C7 81 80 02 00 00 imm32`). `--dump` prints the bytes.

## Detection

- **Faults**: the generated code is registered with `RtlAddFunctionTable` (one shared `UNWIND_INFO`: `push rsi`,
  `sub rsp,0x50`), so a fault inside it is caught by structured exception handling (`__try/__except`) in the calling
  thread. The report gives the exception code, the faulting address, the offset of `rip` in the instance and the
  base register value.
- **Wrong results**: before each call the state block and guest memory are reset from one of 4 random input sets;
  after it, the whole block and memory are compared with the result computed once by a reference model.
- **Verdict**: `PASS`, or `FAIL` with, for each failing logical processor, its core, the first error, the pattern,
  length, alignment and the number of iterations before it. Exit code 0 PASS, 1 FAIL, 2 usage error or self-test not
  done.

## Self-test (proves that detection works, without touching the hardware)

```
cpustress.exe --seconds 5 --inject gp      (one call with a non-canonical base register: a real #GP)
cpustress.exe --seconds 5 --inject value   (one result corrupted on purpose after a correct call)
```

Both must print `VERDICT: FAIL` naming the chosen thread (`--inject-thread N`, default 0) and exit with code 1.
The `gp` case shows how Windows reports a general-protection fault on a store: `0xC0000005`, "read" of address
`0xFFFFFFFFFFFFFFFF`.

## Limits (read before concluding anything)

- A **PASS does not prove the processor is sound**. It only shows that these patterns ran correctly for this long,
  at this temperature, with these BIOS settings. Marginal processors may fail only under a specific load, voltage
  transition, temperature or after hours. A FAIL, on the other hand, is strong evidence of a hardware or settings
  problem (the code and its expected results are deterministic).
- The patterns imitate a recompiled game's code but not the game's whole workload (GPU driver, audio, large memory
  traffic). Run it at least as long as the time the game usually takes to crash, ideally several times that.
- It tests the processor core only, not the memory (use a memory tester for that).

## Build

Windows x64 only; any of:

```
cl /O2 /EHsc /W4 /MT cpustress.cpp                      (x64 Native Tools Command Prompt, Visual Studio)
clang-cl /O2 /EHsc /W4 /MT /std:c++17 cpustress.cpp    (same environment, LLVM)
cmake -S tools/cpustress -B build/cpustress -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/cpustress
```

`/MT` links the C++ runtime statically, so the `.exe` runs on a machine without the Visual C++ redistributable.
The program is not part of the game build.

## Options

`--seconds N` (180), `--threads N` (all), `--code-mb N` (32), `--patterns pc-run,pc-only,stack-reload,mixed`,
`--seed N`, `--inject gp|value`, `--inject-thread N`, `--list`, `--dump`.

## Sources

- Unwind data format (`UNWIND_INFO`, `UWOP_PUSH_NONVOL`, `UWOP_ALLOC_SMALL`) and `RtlAddFunctionTable`: Microsoft
  Learn, *x64 exception handling* and *RtlAddFunctionTable*. Verified in practice by `--inject gp`, whose fault
  inside generated code is caught by the caller's `__except`.
- `SetThreadGroupAffinity`, `GetActiveProcessorGroupCount`, `GetActiveProcessorCount`,
  `GetLogicalProcessorInformationEx`: Microsoft Learn (Win32 API).
