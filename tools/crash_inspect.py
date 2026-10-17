"""Reads a Windows minidump (crash report .dmp) and explains the faulting instruction. Generic: any x86-64 program.

Usage:
  py -3 tools/crash_inspect.py <crash.dmp> [--find-in EXE [EXE ...]] [--window N] [--regions N] [--events N]
  py -3 tools/crash_inspect.py [<crash.dmp>] --report <crash.txt>
  py -3 tools/crash_inspect.py --self-test

Prints:
  - system (Windows version, processor family/model/stepping, logical processors) and the faulting module (path, PE time
    stamp, image size), so that the exact build can be found again;
  - the memory regions (MemoryInfoList stream, when the dump has it): committed and reserved bytes per type (private,
    mapped, image) and the largest committed private regions (--regions N);
  - the thread information (ThreadInfoList stream): start address (module + offset), creation time, CPU time, exit
    status of every thread;
  - with --events N: the last N native events stored by the gcrt crash handler in the comment stream;
  - with --report TXT: the key lines of a gcrt text report (cause, recurrence, signature, executable, processor,
    microcode, firmware, graphics adapter, memory) and whether every section is present (alone: exit 0 if so, 1 if
    not);
  - the exception and the registers of the faulting thread;
  - the code around the faulting instruction (disassembled with Capstone if it is installed: `py -3 -m pip install
    capstone`; hex otherwise);
  - for the faulting instruction, each explicit memory operand with its effective address computed from the registers,
    and a verdict (below);
  - with --find-in: where the same code bytes are in local executables (RVA), to map the fault onto a build that has
    symbols or a map file.

Windows reports a general protection fault as an access violation (0xC0000005) of address 0xFFFFFFFFFFFFFFFF; the
real address must be recomputed from the instruction, which this tool does.

Verdicts. The tool only checks what it can compute: the explicit memory operands decoded by Capstone (non-canonical or
kernel address, operand crossing the end of the canonical range, misaligned access for an instruction that requires
alignment, address equal to the one Windows reports), the target of `ret` and of `jmp`/`call` through a register or a
captured memory operand, and the stack pointer of an instruction that uses the stack implicitly.
  - EXPLAINED / consistent: one of these checks accounts for the fault.
  - NOT EXPLAINED: a general protection fault on an instruction with at least one explicit memory operand, all of them
    valid, and none of the cases the tool does not check (system or privileged instruction, port I/O, segment register
    load, state restore that checks the loaded values, implicit stack use, branch, instruction longer than 15 bytes).
    This is a hypothesis to confirm with other reports, not a proof: it suggests a fault outside the program (unstable
    machine, thread context altered by another component).
  - UNDETERMINED: anything else (one of the unchecked cases, unknown operand address, no decoding, not an access
    violation, access violation that no operand matches).

Exit codes (for scripts):
  0  explained or consistent;
  1  usage or read error (bad arguments, unreadable file, not a minidump, no exception stream); with --self-test: a case
     failed;
  2  NOT EXPLAINED;
  3  UNDETERMINED, including Capstone not installed and code bytes not captured in the dump.

--self-test runs the verdict on synthetic instruction bytes (needs Capstone; exit 0 if every case passes).

Minidump format: MINIDUMP_HEADER, MINIDUMP_DIRECTORY and the stream structures of minidumpapiset.h (Windows SDK).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

THREAD_LIST, MODULE_LIST, MEMORY_LIST, EXCEPTION, SYSTEM_INFO, MEMORY64_LIST = 3, 4, 5, 6, 7, 9
COMMENT_A, MEMORY_INFO_LIST, THREAD_INFO_LIST = 10, 16, 17
MEM_COMMIT, MEM_RESERVE, MEM_FREE = 0x1000, 0x2000, 0x10000
MEM_TYPES = {0x20000: "private", 0x40000: "mapped", 0x1000000: "image"}
# Sections of the gcrt text report (runtime/core/platform/crash_handler.cpp), checked by --report.
REPORT_SECTIONS = ["== gcrt ", "== Guest state", "== Threads at the time of the report", "== Recent native events",
                   "== Recent log lines", "== Environment", "== Modules", "== end of report"]
REPORT_KEYS = ["cause:", "exception ", "recurrence:", "signature ", "executable sha1", "cpu ", "windows ",
               "cpu microcode", "bios", "gpu ", "process memory:", "address space", "minidump:"]
CTX_NAMES = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15 rip".split()

# Legacy SSE instructions whose 16-byte memory operand does not have to be aligned (all others do).
SSE_UNALIGNED_OK = {"movups", "movupd", "movdqu", "lddqu", "movsd", "movss", "movq", "movd", "movlps", "movhps",
                    "movlpd", "movhpd", "pinsrb", "pinsrw", "pinsrd", "pinsrq", "pextrb", "pextrw", "pextrd", "pextrq",
                    "insertps", "extractps", "maskmovdqu"}
# VEX/EVEX instructions that still require alignment to the operand size.
VEX_ALIGNED = {"vmovaps", "vmovapd", "vmovdqa", "vmovdqa32", "vmovdqa64", "vmovntps", "vmovntpd", "vmovntdq",
               "vmovntdqa"}
OTHER_ALIGNED = {"cmpxchg16b": 16, "fxsave": 16, "fxsave64": 16, "fxrstor": 16, "fxrstor64": 16, "xsave": 64,
                 "xsave64": 64, "xrstor": 64, "xrstor64": 64, "xsaveopt": 64, "xsavec": 64, "xsaves": 64,
                 "movdir64b": 64}
# Instructions that can raise a general protection fault for reasons the tool does not check (privilege level, port
# I/O permission, model-specific register number, descriptor or segment loads, values loaded by a state restore,
# far transfers). Capstone 5 puts some of them in its "privilege" or "int" groups, not all (rdmsr, in and out are in
# none), hence this list; the groups are checked as well.
SYSTEM_MNEMONICS = {
    "rdmsr", "wrmsr", "rdpmc", "rdtsc", "rdtscp", "rdpid", "xgetbv", "xsetbv", "cpuid",
    "in", "out", "ins", "insb", "insw", "insd", "outs", "outsb", "outsw", "outsd",
    "int", "int1", "int3", "into", "iret", "iretd", "iretq", "syscall", "sysenter", "sysexit", "sysret", "sysretq",
    "hlt", "cli", "sti", "clts", "lgdt", "lidt", "lldt", "ltr", "sgdt", "sidt", "sldt", "smsw", "str", "lmsw",
    "invd", "wbinvd", "wbnoinvd", "invlpg", "invlpga", "invpcid", "swapgs", "rdfsbase", "rdgsbase", "wrfsbase",
    "wrgsbase", "lar", "lsl", "verr", "verw", "monitor", "monitorx", "mwait", "mwaitx", "umonitor", "umwait",
    "tpause", "lds", "les", "lfs", "lgs", "lss", "ljmp", "lcall", "retf", "retfq", "lret", "lretq",
    "ldmxcsr", "vldmxcsr", "fxrstor", "fxrstor64", "xrstor", "xrstor64", "xrstors", "xrstors64",
    "enqcmd", "enqcmds", "wrpkru", "wrssd", "wrssq", "wrussd", "wrussq", "rstorssp", "saveprevssp", "incsspd",
    "incsspq", "setssbsy", "clrssbsy", "encls", "enclu", "enclv", "getsec", "vmcall", "vmlaunch", "vmresume",
    "vmxoff", "vmxon", "vmptrld", "vmptrst", "vmclear", "vmread", "vmwrite", "vmfunc", "invept", "invvpid",
}
SEGMENT_REGS = {"es", "cs", "ss", "ds", "fs", "gs"}
STACK_REGS = {"rsp", "esp", "sp", "spl"}
MASK64 = 0xFFFFFFFFFFFFFFFF
# Exit codes (see the module docstring).
EXIT_EXPLAINED, EXIT_ERROR, EXIT_NOT_EXPLAINED, EXIT_UNDETERMINED = 0, 1, 2, 3
STATUS_NAMES = {EXIT_EXPLAINED: "EXPLAINED or consistent", EXIT_NOT_EXPLAINED: "NOT EXPLAINED",
                EXIT_UNDETERMINED: "UNDETERMINED"}


class Minidump:
    def __init__(self, path: Path):
        self.d = path.read_bytes()
        sig, _ver, count, rva = struct.unpack_from("<4sIII", self.d, 0)
        if sig != b"MDMP":
            raise SystemExit(f"{path}: not a minidump")
        self.streams: dict[int, tuple[int, int]] = {}
        for i in range(count):
            kind, size, srva = struct.unpack_from("<III", self.d, rva + 12 * i)
            self.streams.setdefault(kind, (size, srva))
        self.ranges: list[tuple[int, int, int]] = []
        if MEMORY_LIST in self.streams:
            _, srva = self.streams[MEMORY_LIST]
            (n,) = struct.unpack_from("<I", self.d, srva)
            for i in range(n):
                start, size, drva = struct.unpack_from("<QII", self.d, srva + 4 + 16 * i)
                self.ranges.append((start, size, drva))
        if MEMORY64_LIST in self.streams:
            _, srva = self.streams[MEMORY64_LIST]
            n, cur = struct.unpack_from("<QQ", self.d, srva)
            for i in range(n):
                start, size = struct.unpack_from("<QQ", self.d, srva + 16 + 16 * i)
                self.ranges.append((start, size, cur))
                cur += size

    def string(self, rva: int) -> str:
        (n,) = struct.unpack_from("<I", self.d, rva)
        return self.d[rva + 4: rva + 4 + n].decode("utf-16-le", errors="replace")

    def read(self, addr: int, n: int) -> bytes | None:
        for start, size, drva in self.ranges:
            if start <= addr and addr + n <= start + size:
                return self.d[drva + addr - start: drva + addr - start + n]
        return None

    def readable_window(self, addr: int, before: int, after: int) -> tuple[int, bytes]:
        """Largest captured window [addr - b, addr + a) with b <= before, a <= after."""
        for start, size, drva in self.ranges:
            if start <= addr < start + size:
                lo = max(start, addr - before)
                hi = min(start + size, addr + after)
                return lo, self.d[drva + lo - start: drva + hi - start]
        return addr, b""

    def modules(self) -> list[tuple[int, int, int, str]]:
        out = []
        if MODULE_LIST in self.streams:
            _, srva = self.streams[MODULE_LIST]
            (n,) = struct.unpack_from("<I", self.d, srva)
            for i in range(n):
                base, size, _cks, stamp, name_rva = struct.unpack_from("<QIIII", self.d, srva + 4 + 108 * i)
                out.append((base, size, stamp, self.string(name_rva)))
        return out

    def memory_info(self) -> list[tuple[int, int, int, int, int, int]]:
        """MemoryInfoListStream (MiniDumpWithFullMemoryInfo): (base, allocation base, size, state, protect, type)."""
        out = []
        if MEMORY_INFO_LIST in self.streams:
            _, srva = self.streams[MEMORY_INFO_LIST]
            hsize, esize, n = struct.unpack_from("<IIQ", self.d, srva)
            for i in range(n):
                base, abase, _aprot, _a1, size, state, prot, typ = struct.unpack_from(
                    "<QQIIQIII", self.d, srva + hsize + esize * i)
                out.append((base, abase, size, state, prot, typ))
        return out

    def thread_info(self) -> list[tuple[int, int, int, int, int, int, int, int]]:
        """ThreadInfoListStream (MiniDumpWithThreadInfo): (thread id, dump flags, dump error, exit status, create time,
        kernel time, user time, start address); times in 100 ns units (create time: FILETIME)."""
        out = []
        if THREAD_INFO_LIST in self.streams:
            _, srva = self.streams[THREAD_INFO_LIST]
            hsize, esize, n = struct.unpack_from("<III", self.d, srva)
            for i in range(n):
                tid, flags, err, status, create, _exit, kern, user, start = struct.unpack_from(
                    "<IIIIQQQQQ", self.d, srva + hsize + esize * i)
                out.append((tid, flags, err, status, create, kern, user, start))
        return out

    def comment(self) -> str:
        """CommentStreamA: the gcrt crash handler stores the whole native event ring there."""
        if COMMENT_A not in self.streams:
            return ""
        size, srva = self.streams[COMMENT_A]
        return self.d[srva: srva + size].split(b"\0", 1)[0].decode("utf-8", errors="replace")

    def threads(self) -> dict[int, tuple[int, int, int]]:
        """thread id -> (TEB, stack start, stack size)."""
        out = {}
        if THREAD_LIST in self.streams:
            _, srva = self.streams[THREAD_LIST]
            (n,) = struct.unpack_from("<I", self.d, srva)
            for i in range(n):
                tid, _s, _c, _p, teb, stk, stksz, _r, _cs, _cr = struct.unpack_from("<IIIIQQIIII", self.d,
                                                                                       srva + 4 + 48 * i)
                out[tid] = (teb, stk, stksz)
        return out


def canonical(addr: int) -> bool:
    top = addr >> 47  # 48-bit virtual addresses (4-level paging)
    return top == 0 or top == 0x1FFFF


def describe_system(md: Minidump) -> None:
    if SYSTEM_INFO not in md.streams:
        return
    _, rva = md.streams[SYSTEM_INFO]
    arch, level, rev, nproc, _pt, major, minor, build = struct.unpack_from("<HHHBBIII", md.d, rva)
    model, stepping = rev >> 8, rev & 0xFF
    print(f"system: Windows {major}.{minor}.{build}, processor architecture {arch}, family {level} "
          f"model 0x{model:X} stepping {stepping}, {nproc} logical processors")


def module_of(mods, addr: int):
    for base, size, stamp, name in mods:
        if base <= addr < base + size:
            return base, size, stamp, name
    return None


def operand_addresses(ins, regs: dict[str, int], gs_base: int | None):
    """[(text, effective address or None, size)] for the memory operands of a Capstone instruction (detail on)."""
    from capstone import x86

    out = []
    for op in ins.operands:
        if op.type != x86.X86_OP_MEM:
            continue
        mem = op.mem
        ea = mem.disp
        known = True
        if mem.base != 0:
            name = ins.reg_name(mem.base)
            if name == "rip":
                ea += ins.address + ins.size
            else:
                value = reg_value(name, regs)
                known = known and value is not None
                ea += value or 0
        if mem.index != 0:
            value = reg_value(ins.reg_name(mem.index), regs)
            known = known and value is not None
            ea += (value or 0) * mem.scale
        if mem.segment != 0:
            seg = ins.reg_name(mem.segment)
            if seg == "gs" and gs_base is not None:
                ea += gs_base
            elif seg in ("fs", "gs"):
                known = False
        out.append((f"{op.size}-byte operand", (ea & 0xFFFFFFFFFFFFFFFF) if known else None, op.size))
    return out


def reg_value(name: str, regs: dict[str, int]) -> int | None:
    if name in regs:
        return regs[name]
    # 32-bit forms (address-size override): eax..edi, r8d..r15d
    full = {"eax": "rax", "ecx": "rcx", "edx": "rdx", "ebx": "rbx", "esp": "rsp", "ebp": "rbp", "esi": "rsi",
            "edi": "rdi"}.get(name)
    if full is None and name.startswith("r") and name.endswith("d"):
        full = name[:-1]
    if full in regs:
        return regs[full] & 0xFFFFFFFF
    return None


def required_alignment(ins, size: int) -> int:
    m = ins.mnemonic
    if m in OTHER_ALIGNED:
        return OTHER_ALIGNED[m]
    if m in VEX_ALIGNED:
        return size
    if not m.startswith("v") and size == 16 and m not in SSE_UNALIGNED_OK and m != "cmpxchg16b":
        return 16  # legacy SSE with a 128-bit memory operand
    return 1


def uses_stack_implicitly(ins) -> bool:
    """push, pop, call, ret, enter, leave, pushf...: Capstone lists the stack pointer among the implicit registers."""
    return any(ins.reg_name(r) in STACK_REGS for r in list(ins.regs_read) + list(ins.regs_write))


def unchecked_cases(ins) -> list[str]:
    """Why a general protection fault may come from this instruction even with valid explicit memory operands: cases
    the tool does not check, which forbid the NOT EXPLAINED verdict."""
    import capstone

    out = []
    if ins.size > 15:
        out.append(f"instruction longer than 15 bytes ({ins.size})")
    groups = set(ins.groups)
    _read, written = ins.regs_access()
    if ins.mnemonic in SYSTEM_MNEMONICS or groups & {capstone.CS_GRP_PRIVILEGE, capstone.CS_GRP_INT,
                                                     capstone.CS_GRP_IRET}:
        out.append("system or privileged instruction")
    elif any(ins.reg_name(r) in SEGMENT_REGS for r in written):
        out.append("segment register load")
    if uses_stack_implicitly(ins):
        out.append("implicit stack use")
    if groups & {capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL, capstone.CS_GRP_RET}:
        out.append("branch (its target is not fully checked)")
    return out


def verdict(ins, code: int, params: list[int], regs, gs_base, read) -> tuple[int, str, list[str]]:
    """(exit code, verdict text, operand lines) for the faulting instruction. `read(address, size)` returns the bytes
    captured in the dump, or None."""
    from capstone import x86

    gp = code == 0xC0000005 and len(params) >= 2 and params[1] == MASK64
    ops = operand_addresses(ins, regs, gs_base)
    lines: list[str] = []
    reasons: list[str] = []
    unknown = False
    for text, ea, size in ops:
        if ea is None:
            unknown = True
            lines.append(f"  memory operand ({text}): address unknown (segment base or register not in the context)")
            continue
        align = required_alignment(ins, size)
        last = (ea + max(size, 1) - 1) & MASK64
        notes = []
        if not canonical(ea):
            notes.append("NON-CANONICAL")
            reasons.append(f"non-canonical address 0x{ea:016X}")
        elif not canonical(last):
            notes.append("CROSSES THE END OF THE CANONICAL RANGE")
            reasons.append(f"operand 0x{ea:016X}..0x{last:016X} crosses the end of the canonical range")
        elif ea >= 0xFFFF800000000000:
            notes.append("kernel half")
            reasons.append(f"kernel address 0x{ea:016X}")
        if align > 1 and ea % align:
            notes.append(f"MISALIGNED (this instruction requires {align}-byte alignment)")
            reasons.append(f"misaligned {align}-byte access at 0x{ea:016X}")
        if not gp and len(params) >= 2 and ea <= params[1] < ea + max(size, 1):
            notes.append("matches the address reported by Windows")
            reasons.append("reported address")
        region = "captured in the dump" if read(ea, 1) is not None else "not captured in the dump"
        lines.append(f"  memory operand ({text}): 0x{ea:016X} [{', '.join(notes) or 'canonical'}; {region}]")
    # Implicit stack use with a non-canonical stack pointer faults (stack fault) on the instruction itself.
    if uses_stack_implicitly(ins) and "rsp" in regs:
        lines.append(f"  stack pointer (implicit stack use): 0x{regs['rsp']:016X}")
        if not canonical(regs["rsp"]):
            reasons.append(f"non-canonical stack pointer 0x{regs['rsp']:016X}")
    # Control transfers: a non-canonical target faults on the branch itself.
    if ins.mnemonic in ("ret", "retn") and "rsp" in regs:
        target = read(regs["rsp"], 8)
        if target is not None:
            t = struct.unpack("<Q", target)[0]
            lines.append(f"  return address at [rsp]: 0x{t:016X}")
            if not canonical(t):
                reasons.append(f"return to non-canonical 0x{t:016X}")
    if ins.mnemonic in ("jmp", "call") and len(ins.operands) == 1:
        op = ins.operands[0]
        t = None
        if op.type == x86.X86_OP_REG:
            t = reg_value(ins.reg_name(op.reg), regs)
        elif op.type == x86.X86_OP_MEM and ops and ops[0][1] is not None:
            target = read(ops[0][1], 8)
            t = struct.unpack("<Q", target)[0] if target is not None else None
        if t is not None:
            lines.append(f"  branch target: 0x{t:016X}")
            if not canonical(t):
                reasons.append(f"branch to non-canonical 0x{t:016X}")
    if gp:
        if reasons:
            return EXIT_EXPLAINED, "general protection fault EXPLAINED by the instruction: " + "; ".join(reasons), lines
        unchecked = unchecked_cases(ins)
        if not ops:
            unchecked.append("no explicit memory operand")
        if unknown:
            unchecked.append("a memory operand address is unknown")
        if unchecked:
            return (EXIT_UNDETERMINED, "general protection fault UNDETERMINED: the checks of this tool do not cover this "
                    "instruction (" + "; ".join(unchecked) + "); no conclusion can be drawn", lines)
        return (EXIT_NOT_EXPLAINED,
                "general protection fault NOT EXPLAINED by the checks of this tool: the explicit memory operands are "
                "canonical and aligned as required, and the instruction is not one of the cases the tool does not check "
                "(system, stack, branch). This suggests, as a hypothesis to confirm with more reports, a fault outside "
                "the program: unstable machine (processor, memory, overclocking, temperature: look for WHEA-Logger "
                "events in the Windows System log at the time of the crash) or a thread context altered by another "
                "component. Compare with other crash reports of the same machine.", lines)
    if code == 0xC0000005:
        if reasons:
            return EXIT_EXPLAINED, "access violation consistent with the instruction: " + "; ".join(reasons), lines
        return (EXIT_UNDETERMINED, "access violation UNDETERMINED: no memory operand matches the reported address "
                "(check the decoding)", lines)
    return EXIT_UNDETERMINED, "UNDETERMINED: not an access violation, which this tool does not analyse", lines


def self_test() -> int:
    """Verdicts on synthetic instruction bytes (general protection fault unless stated)."""
    try:
        import capstone
    except ImportError:
        print("self-test: Capstone not installed (py -3 -m pip install capstone)")
        return EXIT_UNDETERMINED
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    rip = 0x00007FF600001000
    base = {name: 0x1000 for name in CTX_NAMES}
    base.update(rip=rip, rsp=0x000000E12345F000, rsi=0x000001D234560000)
    gp = (0xC0000005, [1, MASK64])
    cases = [
        # (name, code bytes, register changes, (code, parameters), expected exit code)
        ("mov dword ptr [rsi+0x280], imm, valid address", "c7868002000001000000", {}, gp, EXIT_NOT_EXPLAINED),
        ("mov dword ptr [rsi+0x280], imm, non-canonical address", "c7868002000001000000",
         {"rsi": 0x8000000000000000}, gp, EXIT_EXPLAINED),
        ("push rax", "50", {}, gp, EXIT_UNDETERMINED),
        ("push rax, non-canonical rsp", "50", {"rsp": 0x8000000000001000}, gp, EXIT_EXPLAINED),
        ("rdmsr", "0f32", {}, gp, EXIT_UNDETERMINED),
        ("int 3", "cd03", {}, gp, EXIT_UNDETERMINED),
        ("int3", "cc", {}, gp, EXIT_UNDETERMINED),
        ("in al, dx", "ec", {}, gp, EXIT_UNDETERMINED),
        ("mov ds, eax", "8ed8", {}, gp, EXIT_UNDETERMINED),
        ("xgetbv", "0f01d0", {}, gp, EXIT_UNDETERMINED),
        ("ldmxcsr dword ptr [rsi]", "0fae16", {}, gp, EXIT_UNDETERMINED),
        ("jmp qword ptr [rsi], target not captured", "ff26", {}, gp, EXIT_UNDETERMINED),
        ("push qword ptr [rsi]", "ff36", {}, gp, EXIT_UNDETERMINED),
        ("movaps xmmword ptr [rsi+8], xmm0, misaligned", "0f294608", {}, gp, EXIT_EXPLAINED),
        ("mov qword ptr [rsi], rax across the end of the canonical range", "488906",
         {"rsi": 0x00007FFFFFFFFFFC}, gp, EXIT_EXPLAINED),
        ("mov eax, dword ptr [rsi], read fault at the reported address", "8b06", {},
         (0xC0000005, [0, 0x000001D234560000]), EXIT_EXPLAINED),
        ("mov eax, dword ptr [rsi], read fault elsewhere", "8b06", {}, (0xC0000005, [0, 0x10]), EXIT_UNDETERMINED),
        ("illegal instruction exception", "0f0b", {}, (0xC000001D, []), EXIT_UNDETERMINED),
    ]
    failed = 0
    for name, hexcode, changes, (code, params), expected in cases:
        regs = dict(base, **changes)
        ins = next(dis.disasm(bytes.fromhex(hexcode), rip), None)
        if ins is None:
            status, text = None, "could not decode"
        else:
            status, text, _lines = verdict(ins, code, params, regs, None, lambda _a, _n: None)
        ok = status == expected
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'} {name}: {STATUS_NAMES.get(status, status)}"
              + ("" if ok else f" (expected {STATUS_NAMES[expected]}: {text})"))
    print(f"self-test: {len(cases) - failed}/{len(cases)} cases passed")
    return EXIT_EXPLAINED if failed == 0 else EXIT_ERROR


def find_in_executables(window_start: int, window: bytes, rip: int, crash_rva: int | None, exes: list[str]) -> None:
    if not window:
        print("find-in: no code bytes captured around rip")
        return
    key_off = rip - window_start
    for exe in exes:
        try:
            b = Path(exe).read_bytes()
        except OSError as e:
            print(f"find-in {exe}: {e}")
            continue
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        nsec = struct.unpack_from("<H", b, pe + 6)[0]
        optsz = struct.unpack_from("<H", b, pe + 20)[0]
        stamp = struct.unpack_from("<I", b, pe + 8)[0]
        secs = []
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            vsz, va, rsz, rptr = struct.unpack_from("<IIII", b, o + 8)
            secs.append((va, vsz, rptr, rsz))
        # Exact window first, then shrinking windows centred on rip (relocated or rip-relative bytes may differ).
        hits: list[int] = []
        for half in (len(window), 48, 32, 24, 16):
            lo = max(0, key_off - half)
            pat = window[lo: key_off + half]
            pos = b.find(pat)
            while pos >= 0 and len(hits) < 4:
                for va, vsz, rptr, rsz in secs:
                    if rptr <= pos < rptr + rsz:
                        hits.append(pos - rptr + va + (key_off - lo))
                pos = b.find(pat, pos + 1)
            if hits:
                print(f"find-in {exe} (PE time stamp 0x{stamp:08X}): rip at RVA "
                      + ", ".join(f"0x{h:X}" + (f" (delta {h - crash_rva:+#x})" if crash_rva is not None else "")
                                  for h in hits) + f" [{len(pat)}-byte match]")
                break
        else:
            print(f"find-in {exe}: code not found")


class ArgumentParser(argparse.ArgumentParser):
    """Usage errors exit with code 1 (argparse uses 2, which here means NOT EXPLAINED)."""

    def error(self, message: str):
        self.print_usage(sys.stderr)
        self.exit(EXIT_ERROR, f"{self.prog}: error: {message}\n")


def result(status: int, text: str) -> int:
    print(f"verdict: {text}")
    print(f"result: {STATUS_NAMES[status]} (exit code {status})")
    return status


def describe_memory(md: Minidump, largest: int) -> None:
    info = md.memory_info()
    if not info:
        print("memory regions: no MemoryInfoList stream (dump written without MiniDumpWithFullMemoryInfo)")
        return
    totals: dict[tuple[str, str], list[int]] = {}
    for base, _abase, size, state, prot, typ in info:
        if state == MEM_FREE:
            continue
        key = (MEM_TYPES.get(typ, f"type {typ:X}"), "committed" if state == MEM_COMMIT else "reserved")
        t = totals.setdefault(key, [0, 0])
        t[0] += size
        t[1] += 1
    parts = [f"{k[0]} {k[1]} {v[0] / 1048576:.1f} MiB in {v[1]}" for k, v in sorted(totals.items())]
    print(f"memory regions: {len(info)} entries; " + "; ".join(parts))
    private = sorted((r for r in info if r[3] == MEM_COMMIT and r[5] == 0x20000), key=lambda r: -r[2])
    for base, abase, size, _state, prot, _typ in private[:largest]:
        print(f"  private committed 0x{base:016X} {size / 1048576:8.1f} MiB protect 0x{prot:X} "
              f"(allocation 0x{abase:016X})")


def describe_threads(md: Minidump, mods) -> None:
    info = md.thread_info()
    if not info:
        print("thread information: no ThreadInfoList stream (dump written without MiniDumpWithThreadInfo)")
        return
    first = min(r[4] for r in info if r[4]) if any(r[4] for r in info) else 0
    print(f"thread information: {len(info)} threads (created = seconds after the first thread; CPU = user + kernel)")
    for tid, flags, err, status, create, kern, user, start in info:
        mod = module_of(mods, start)
        where = f"{os.path.basename(mod[3])}+0x{start - mod[0]:X}" if mod else f"0x{start:X}"
        extra = f", dump flags 0x{flags:X} error 0x{err:X}" if flags or err else ""
        print(f"  thread {tid:6}: start {where}, created +{(create - first) / 1e7:.3f} s, CPU "
              f"{(user + kern) / 1e7:.3f} s, exit status 0x{status:X}{extra}")


def print_events(md: Minidump, last: int) -> None:
    text = md.comment()
    if not text:
        print("native events: no comment stream in this dump")
        return
    lines = text.splitlines()
    print(f"native events (comment stream): {lines[0] if lines else ''}")
    for line in lines[1:][-last:]:
        print(line)


def check_report(path: Path) -> int:
    """Summary of a gcrt text report: key lines and sections present. Exit 0 if every section is there, 1 otherwise."""
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    print(f"text report {path.name}: {len(text)} bytes, {len(lines)} lines")
    for key in REPORT_KEYS:
        hit = next((line for line in lines if line.startswith(key)), None)
        if hit is not None:
            print(f"  {hit[:200]}")
    missing = [s for s in REPORT_SECTIONS if not any(line.startswith(s) for line in lines)]
    threads = sum(1 for line in lines if line.startswith("thread "))
    events = sum(1 for line in lines if line.startswith("  -"))
    print(f"  {threads} thread lines, {events} event lines")
    if missing:
        print(f"  missing sections: {missing}")
        return EXIT_ERROR
    print("  every section present")
    return EXIT_EXPLAINED


def inspect_dump(args) -> int:
    md = Minidump(args.dump)
    describe_system(md)
    mods = md.modules()
    describe_memory(md, args.regions)
    describe_threads(md, mods)
    if args.events:
        print_events(md, args.events)
    if EXCEPTION not in md.streams:
        print("no exception stream: a hang report (gcrt watchdog) or not a crash dump")
        return EXIT_ERROR
    _, rva = md.streams[EXCEPTION]
    tid = struct.unpack_from("<I", md.d, rva)[0]
    code, flags, _rec, addr, nparams = struct.unpack_from("<IIQQI", md.d, rva + 8)
    params = list(struct.unpack_from(f"<{nparams}Q", md.d, rva + 8 + 32)) if nparams else []
    ctx_size, ctx_rva = struct.unpack_from("<II", md.d, rva + 8 + 152)
    c = md.d[ctx_rva: ctx_rva + ctx_size]
    cs_, ds_, es_, fs_, gs_, ss_ = struct.unpack_from("<6H", c, 56)
    eflags = struct.unpack_from("<I", c, 68)[0]
    regs = dict(zip(CTX_NAMES, struct.unpack_from("<17Q", c, 120)))
    mods = md.modules()
    rip = regs["rip"]
    mod = module_of(mods, addr)
    where = f"{os.path.basename(mod[3])}+0x{addr - mod[0]:X}" if mod else "(no module)"
    print(f"exception 0x{code:08X} flags 0x{flags:X} at 0x{addr:016X} = {where}, thread {tid}, "
          f"parameters {[hex(p) for p in params]}")
    crash_rva = None
    if mod:
        crash_rva = addr - mod[0]
        print(f"module {mod[3]}: base 0x{mod[0]:X}, image size 0x{mod[1]:X}, PE time stamp 0x{mod[2]:08X}")
    print(" ".join(f"{n}={v:016X}" for n, v in regs.items()))
    print(f"eflags {eflags:08X} cs {cs_:X} ds {ds_:X} es {es_:X} fs {fs_:X} gs {gs_:X} ss {ss_:X}")
    teb = md.threads().get(tid, (None,))[0]
    start, window = md.readable_window(rip, args.window, 64)
    if not window:
        print("code bytes around rip: not captured in the dump")
        return result(EXIT_UNDETERMINED, "UNDETERMINED: no code bytes to decode")
    try:
        import capstone
    except ImportError:
        print(f"code from 0x{start:X} (rip = +0x{rip - start:X}): {window.hex()}")
        print("Capstone not installed: no decoding (py -3 -m pip install capstone)")
        find_in_executables(start, window, rip, crash_rva, args.find_in)
        return result(EXIT_UNDETERMINED, "UNDETERMINED: Capstone not installed, the instruction was not decoded")
    dis = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    dis.detail = True
    listing, fault = [], None
    for skip in range(min(16, rip - start + 1)):  # resynchronise so that rip is an instruction boundary
        listing = list(dis.disasm(window[skip:], start + skip))
        hit = [i for i in listing if i.address == rip]
        if hit:
            fault = hit[0]
            break
    if fault is None:
        print("could not decode an instruction at rip")
        return result(EXIT_UNDETERMINED, "UNDETERMINED: no instruction decoded at rip (an instruction longer than "
                      "15 bytes, or invalid bytes, also ends up here)")
    idx = listing.index(fault)
    for ins in listing[max(0, idx - 12): idx + 6]:
        mark = ">>" if ins is fault else "  "
        print(f"{mark}{ins.address:016X}: {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}")
    print(f"faulting instruction: {fault.mnemonic} {fault.op_str}")
    status, text, lines = verdict(fault, code, params, regs, teb, md.read)
    for line in lines:
        print(line)
    if args.find_in:
        find_in_executables(start, window, rip, crash_rva, args.find_in)
    return result(status, text)


def main() -> int:
    ap = ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", type=Path, nargs="?")
    ap.add_argument("--find-in", nargs="+", default=[], metavar="EXE", help="local executables to search")
    ap.add_argument("--window", type=int, default=96, help="bytes disassembled before rip (default 96)")
    ap.add_argument("--self-test", action="store_true", help="check the verdicts on synthetic instructions")
    ap.add_argument("--regions", type=int, default=8, metavar="N",
                    help="largest committed private regions listed (default 8)")
    ap.add_argument("--events", type=int, default=0, metavar="N",
                    help="last N native events of the comment stream (gcrt dumps)")
    ap.add_argument("--report", type=Path, metavar="TXT",
                    help="gcrt text report to summarize (key lines, sections present); alone or with a dump")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.report is not None:
        try:
            status = check_report(args.report)
        except OSError as e:
            print(f"{args.report}: cannot read the report: {e}", file=sys.stderr)
            return EXIT_ERROR
        if args.dump is None:
            return status
    if args.dump is None:
        ap.error("a minidump is required (or --self-test, or --report)")
    try:
        return inspect_dump(args)
    except (OSError, struct.error, IndexError, ValueError) as e:
        print(f"{args.dump}: cannot read the minidump: {e}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
