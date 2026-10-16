"""Reads a Windows minidump (crash report .dmp) and explains the faulting instruction. Generic: any x86-64 program.

Usage:
  py -3 tools/crash_inspect.py <crash.dmp> [--find-in EXE [EXE ...]] [--window N]

Prints:
  - system (Windows version, processor family/model/stepping, logical processors) and the faulting module (path, PE time
    stamp, image size), so that the exact build can be found again;
  - the exception and the registers of the faulting thread;
  - the code around the faulting instruction (disassembled with Capstone if it is installed: `py -3 -m pip install
    capstone`; hex otherwise);
  - for the faulting instruction, each memory operand with its effective address computed from the registers, and a
    verdict: is the fault explained by the instruction (non-canonical or kernel address, misaligned access for an
    instruction that requires alignment, address equal to the one Windows reports), or not? An unexplained general
    protection fault on a valid instruction points outside the program (unstable machine, altered thread context).
  - with --find-in: where the same code bytes are in local executables (RVA), to map the fault onto a build that has
    symbols or a map file.

Windows reports a general protection fault as an access violation (0xC0000005) of address 0xFFFFFFFFFFFFFFFF; the
real address must be recomputed from the instruction, which this tool does.

Minidump format: MINIDUMP_HEADER, MINIDUMP_DIRECTORY and the stream structures of minidumpapiset.h (Windows SDK).
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

THREAD_LIST, MODULE_LIST, MEMORY_LIST, EXCEPTION, SYSTEM_INFO, MEMORY64_LIST = 3, 4, 5, 6, 7, 9
CTX_NAMES = "rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15 rip".split()

# Legacy SSE instructions whose 16-byte memory operand does not have to be aligned (all others do).
SSE_UNALIGNED_OK = {"movups", "movupd", "movdqu", "lddqu", "movsd", "movss", "movq", "movd", "movlps", "movhps",
                    "movlpd", "movhpd", "pinsrb", "pinsrw", "pinsrd", "pinsrq", "pextrb", "pextrw", "pextrd", "pextrq",
                    "insertps", "extractps", "maskmovdqu"}
# VEX/EVEX instructions that still require alignment to the operand size.
VEX_ALIGNED = {"vmovaps", "vmovapd", "vmovdqa", "vmovdqa32", "vmovdqa64", "vmovntps", "vmovntpd", "vmovntdq",
               "vmovntdqa"}
OTHER_ALIGNED = {"cmpxchg16b": 16, "fxsave": 16, "fxsave64": 16, "fxrstor": 16, "fxrstor64": 16, "xsave": 64,
                 "xsave64": 64, "xrstor": 64, "xrstor64": 64, "xsaveopt": 64, "xsavec": 64, "xsaves": 64}


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


def verdict(ins, code: int, params: list[int], regs, gs_base, md: Minidump) -> str:
    gp = code == 0xC0000005 and len(params) >= 2 and params[1] == 0xFFFFFFFFFFFFFFFF
    ops = operand_addresses(ins, regs, gs_base)
    lines = []
    reasons = []
    for text, ea, size in ops:
        if ea is None:
            lines.append(f"  memory operand ({text}): address unknown (segment base or register not in the context)")
            continue
        align = required_alignment(ins, size)
        notes = []
        if not canonical(ea):
            notes.append("NON-CANONICAL")
            reasons.append(f"non-canonical address 0x{ea:016X}")
        elif ea >= 0xFFFF800000000000:
            notes.append("kernel half")
            reasons.append(f"kernel address 0x{ea:016X}")
        if align > 1 and ea % align:
            notes.append(f"MISALIGNED (this instruction requires {align}-byte alignment)")
            reasons.append(f"misaligned {align}-byte access at 0x{ea:016X}")
        if not gp and len(params) >= 2 and ea <= params[1] < ea + max(size, 1):
            notes.append("matches the address reported by Windows")
            reasons.append("reported address")
        region = "captured in the dump" if md.read(ea, 1) is not None else "not captured in the dump"
        lines.append(f"  memory operand ({text}): 0x{ea:016X} [{', '.join(notes) or 'canonical'}; {region}]")
    # Control transfers: a non-canonical target faults on the branch itself.
    if ins.mnemonic in ("ret", "retn") and "rsp" in regs:
        target = md.read(regs["rsp"], 8)
        if target is not None:
            t = struct.unpack("<Q", target)[0]
            lines.append(f"  return address at [rsp]: 0x{t:016X}")
            if not canonical(t):
                reasons.append(f"return to non-canonical 0x{t:016X}")
    if ins.mnemonic in ("jmp", "call") and len(ins.operands) == 1:
        from capstone import x86
        op = ins.operands[0]
        if op.type == x86.X86_OP_REG:
            t = reg_value(ins.reg_name(op.reg), regs)
            if t is not None and not canonical(t):
                reasons.append(f"branch to non-canonical 0x{t:016X}")
    for line in lines:
        print(line)
    if gp:
        if reasons:
            return "general protection fault EXPLAINED by the instruction: " + "; ".join(reasons)
        return ("general protection fault NOT EXPLAINED by the instruction: with these registers it cannot raise one. "
                "Suspect a fault outside the program: unstable machine (processor, memory, overclocking, temperature: "
                "look for WHEA-Logger events in the Windows System log at the time of the crash) or a thread context "
                "altered by another component. Compare with other crash reports of the same machine.")
    if code == 0xC0000005:
        if reasons:
            return "access violation consistent with the instruction: " + "; ".join(reasons)
        return "access violation: no memory operand matches the reported address (check the decoding)"
    return "not an access violation: see the exception code"


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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dump", type=Path)
    ap.add_argument("--find-in", nargs="+", default=[], metavar="EXE", help="local executables to search")
    ap.add_argument("--window", type=int, default=96, help="bytes disassembled before rip (default 96)")
    args = ap.parse_args()
    md = Minidump(args.dump)
    describe_system(md)
    if EXCEPTION not in md.streams:
        print("no exception stream")
        return 1
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
        return 0
    try:
        import capstone
    except ImportError:
        print(f"code from 0x{start:X} (rip = +0x{rip - start:X}): {window.hex()}")
        print("Capstone not installed: no decoding (py -3 -m pip install capstone)")
        find_in_executables(start, window, rip, crash_rva, args.find_in)
        return 0
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
        return 0
    idx = listing.index(fault)
    for ins in listing[max(0, idx - 12): idx + 6]:
        mark = ">>" if ins is fault else "  "
        print(f"{mark}{ins.address:016X}: {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}")
    print(f"faulting instruction: {fault.mnemonic} {fault.op_str}")
    print("verdict: " + verdict(fault, code, params, regs, teb, md))
    if args.find_in:
        find_in_executables(start, window, rip, crash_rva, args.find_in)
    return 0


if __name__ == "__main__":
    sys.exit(main())
