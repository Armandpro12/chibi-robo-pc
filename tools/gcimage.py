"""Loaded view of a GameCube program: the DOL and, optionally, its REL modules with their relocations applied.
Generic (no game-specific parameter). Shared by the static scanners (tools/rtti_scan.py, tools/extab_scan.py).

What it gives:
  - one flat 32-bit address space: the DOL segments at their own addresses, each REL section placed at a
    synthetic address (modules one after the other from `rel_base`, 64 KiB aligned, sections aligned on 32 bytes,
    the module's BSS after its sections), so that pointers between the DOL and the modules, and inside a module,
    can be followed like in the running game;
  - the relocations of every module applied to its sections (the module is linked against the DOL and against
    the other loaded modules), plus the set of relocated sites: in a REL a data word is a pointer only if a
    relocation writes it;
  - helpers: word/half/byte reads, C strings, code or data classification, readable locations
    ("0x80001234" in the DOL, "<module>.rel s1+0x120" in a module).

Formats (sources): DOL and REL layouts as read by tools/gcinfo.py (wiki.tockdom.com "DOL (File Format)" and
"REL (File Format)"). REL relocations: list per imported module of 8-byte entries (u16 offset from the previous
entry, u8 type, u8 section, u32 addend); R_DOLPHIN_NOP (201) only advances, R_DOLPHIN_SECTION (202) selects the
section patched next and restarts at its offset 0, R_DOLPHIN_END (203) ends the list (tockdom wiki). The target
is `addend` for module 0 (the DOL: absolute address) and `section base + addend` for a module. Patch formulas of
the R_PPC types (ADDR32, ADDR24, ADDR16, ADDR16_LO/HI/HA, ADDR14, REL24, REL14): System V ABI PowerPC Processor
Supplement (relocation table, `#lo`, `#hi`, `#ha`). Checked by a local test (every relocated `bl` of the modules
reaches a function start: US 1,217 of 1,217, PAL 1,194 of 1,194, against the dtk symbols (US), the transposition
(PAL), the symbol maps and Ghidra's function list; the list alone misses 6 functions that only the modules call).
REL24 and REL14 displacements are computed against the synthetic addresses above (modules from 0x81800000 on) and
masked to 26 and 16 bits: they are reliable only while a module lies within about 32 MiB (REL24) of its targets,
which holds for this game (the test above would catch a wrapped displacement), and are not checked otherwise.

A REL section with offset 0 and a non-zero size is the module's BSS (zero-filled): the file holds no bytes for it.
"""

from __future__ import annotations

import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gcinfo  # noqa: E402

R_NONE, R_ADDR32, R_ADDR24, R_ADDR16, R_ADDR16_LO, R_ADDR16_HI, R_ADDR16_HA, R_ADDR14 = 0, 1, 2, 3, 4, 5, 6, 7
R_REL24, R_REL14 = 10, 11
R_DOLPHIN_NOP, R_DOLPHIN_SECTION, R_DOLPHIN_END = 201, 202, 203
DEFAULT_REL_BASE = 0x81800000  # just after the 24 MiB of MEM1: overlaps nothing of the DOL
MODULE_ALIGN = 0x10000
SECTION_ALIGN = 32


@dataclass
class Segment:
    module: int          # 0 = DOL, otherwise the REL module id
    module_name: str     # "main.dol" or the REL file name
    index: int           # DOL: 0-17 (text0-6, data0-10); REL: section index
    name: str            # "text1", "data4", "s1"...
    addr: int
    data: bytearray
    code: bool
    bss: bool = False

    @property
    def end(self) -> int:
        return self.addr + len(self.data)


@dataclass
class Module:
    id: int
    name: str
    path: Path | None
    sections: dict[int, Segment] = field(default_factory=dict)
    unresolved: int = 0          # relocations towards a module that is not loaded
    relocations: int = 0
    info: dict = field(default_factory=dict)


class Program:
    def __init__(self) -> None:
        self.segments: list[Segment] = []
        self.modules: dict[int, Module] = {}
        self.pointer_sites: dict[int, int] = {}   # REL only: address of a relocated 32-bit word -> its target
        self.code_sites: set[int] = set()         # REL only: instruction words patched by a relocation
        self.entry = 0
        self.dol_bss: tuple[int, int] = (0, 0)
        self._last: Segment | None = None

    # ------------------------------------------------------------------ loading
    @classmethod
    def load(cls, dol: Path, rels: list[Path] | None = None, rel_base: int = DEFAULT_REL_BASE) -> "Program":
        p = cls()
        p._load_dol(dol)
        if rels:
            p._load_rels(sorted(rels), rel_base)
        p.segments.sort(key=lambda s: s.addr)
        return p

    def _load_dol(self, path: Path) -> None:
        data = path.read_bytes()
        info = gcinfo.describe_dol(data)
        mod = Module(0, path.name, path, info=info)
        for i, s in enumerate(info["segments"]):
            code = s["name"].startswith("text")
            idx = int(s["name"][4:]) if code else 7 + int(s["name"][4:])
            seg = Segment(0, path.name, idx, s["name"], s["addr"], bytearray(data[s["offset"]:s["offset"] + s["size"]]),
                          code)
            mod.sections[idx] = seg
            self.segments.append(seg)
        self.modules[0] = mod
        self.entry = info["entry"]
        self.dol_bss = (info["bss"], info["bss"] + info["bss_size"])

    def _load_rels(self, paths: list[Path], base: int) -> None:
        raw: dict[int, bytes] = {}
        addr = base
        for path in paths:
            data = path.read_bytes()
            info = gcinfo.describe_rel(data)
            mid = info["id"]
            if mid in self.modules:
                raise ValueError(f"{path.name}: module id {mid} already loaded")
            mod = Module(mid, path.name, path, info=info)
            addr = (addr + MODULE_ALIGN - 1) & ~(MODULE_ALIGN - 1)
            for s in info["sections"]:
                if not s["size"]:
                    continue
                addr = (addr + SECTION_ALIGN - 1) & ~(SECTION_ALIGN - 1)
                bss = s["offset"] == 0
                blob = bytearray(s["size"]) if bss else bytearray(data[s["offset"]:s["offset"] + s["size"]])
                seg = Segment(mid, path.name, s["index"], f"s{s['index']}", addr, blob, s["exec"], bss)
                mod.sections[s["index"]] = seg
                self.segments.append(seg)
                addr += s["size"]
            self.modules[mid] = mod
            raw[mid] = data
        for mid, data in raw.items():
            self._relocate(self.modules[mid], data)

    def _relocate(self, mod: Module, data: bytes) -> None:
        hdr = struct.unpack(">12I", data[:0x30])
        impoff, impsize = hdr[10], hdr[11]
        for i in range(impsize // 8):
            imp_mod, rel_off = struct.unpack_from(">II", data, impoff + 8 * i)
            pos, cur = rel_off, None
            where = 0
            while pos + 8 <= len(data):
                off, typ, sec, add = struct.unpack_from(">HBBI", data, pos)
                pos += 8
                if typ == R_DOLPHIN_END:
                    break
                if typ == R_DOLPHIN_SECTION:
                    cur, where = mod.sections.get(sec), 0
                    continue
                where += off
                if typ == R_DOLPHIN_NOP or cur is None:
                    continue
                if imp_mod == 0:
                    target = add
                else:
                    tm = self.modules.get(imp_mod)
                    tseg = tm.sections.get(sec) if tm else None
                    if tseg is None:
                        mod.unresolved += 1
                        continue
                    target = (tseg.addr + add) & 0xFFFFFFFF
                site = cur.addr + where
                self._patch(cur, where, typ, target, site)
                mod.relocations += 1

    def _patch(self, seg: Segment, o: int, typ: int, t: int, site: int) -> None:
        b = seg.data
        if typ == R_ADDR32:
            struct.pack_into(">I", b, o, t)
            self.pointer_sites[site] = t
            return
        if typ in (R_ADDR16, R_ADDR16_LO, R_ADDR16_HI, R_ADDR16_HA):
            v = {R_ADDR16: t, R_ADDR16_LO: t, R_ADDR16_HI: t >> 16,
                 R_ADDR16_HA: (t >> 16) + (1 if t & 0x8000 else 0)}[typ]
            struct.pack_into(">H", b, o, v & 0xFFFF)
            self.code_sites.add(site & ~3)
            return
        w = struct.unpack_from(">I", b, o)[0]
        if typ == R_ADDR24:
            w = (w & 0xFC000003) | (t & 0x03FFFFFC)
        elif typ == R_ADDR14:
            w = (w & 0xFFFF0003) | (t & 0xFFFC)
        elif typ == R_REL24:
            w = (w & 0xFC000003) | ((t - site) & 0x03FFFFFC)
        elif typ == R_REL14:
            w = (w & 0xFFFF0003) | ((t - site) & 0xFFFC)
        else:
            return
        struct.pack_into(">I", b, o, w)
        self.code_sites.add(site)

    # ------------------------------------------------------------------ queries
    def seg(self, addr: int) -> Segment | None:
        s = self._last
        if s is not None and s.addr <= addr < s.end:
            return s
        lo, hi = 0, len(self.segments)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.segments[mid].addr <= addr:
                lo = mid + 1
            else:
                hi = mid
        if lo:
            s = self.segments[lo - 1]
            if s.addr <= addr < s.end:
                self._last = s
                return s
        return None

    def word(self, addr: int) -> int | None:
        s = self.seg(addr)
        if s is None or addr + 4 > s.end:
            return None
        return struct.unpack_from(">I", s.data, addr - s.addr)[0]

    def half(self, addr: int) -> int | None:
        s = self.seg(addr)
        if s is None or addr + 2 > s.end:
            return None
        return struct.unpack_from(">H", s.data, addr - s.addr)[0]

    def bytes_at(self, addr: int, n: int) -> bytes | None:
        s = self.seg(addr)
        if s is None or addr + n > s.end:
            return None
        return bytes(s.data[addr - s.addr:addr - s.addr + n])

    def cstr(self, addr: int, limit: int = 256) -> str | None:
        s = self.seg(addr)
        if s is None or s.code:
            return None
        o = addr - s.addr
        chunk = s.data[o:o + limit]
        n = chunk.find(b"\0")
        if n <= 0:
            return None
        try:
            return chunk[:n].decode("ascii")
        except UnicodeDecodeError:
            return None

    def is_code(self, addr: int) -> bool:
        s = self.seg(addr)
        return s is not None and s.code

    def is_data(self, addr: int) -> bool:
        s = self.seg(addr)
        return s is not None and not s.code

    def is_pointer_site(self, addr: int) -> bool:
        """True when the word at `addr` may hold a pointer: always in the DOL (no relocations to tell), only on a
        relocated word in a REL (an unrelocated REL word is a plain value, never an address)."""
        s = self.seg(addr)
        if s is None:
            return False
        return s.module == 0 or addr in self.pointer_sites

    def data_segments(self) -> list[Segment]:
        return [s for s in self.segments if not s.code and not s.bss]

    def code_segments(self) -> list[Segment]:
        return [s for s in self.segments if s.code]

    def where(self, addr: int) -> str:
        s = self.seg(addr)
        if s is None:
            return f"0x{addr:08X} (unmapped)"
        if s.module == 0:
            return f"0x{addr:08X}"
        return f"{s.module_name} {s.name}+0x{addr - s.addr:X}"

    def module_of(self, addr: int) -> str:
        s = self.seg(addr)
        return s.module_name if s else "?"


def sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def branch_target(word: int, site: int) -> int | None:
    """Target of an unconditional relative `b`/`bl` (opcode 18, AA = 0)."""
    if word >> 26 != 18 or word & 2:
        return None
    disp = word & 0x03FFFFFC
    if disp & 0x02000000:
        disp -= 0x04000000
    return (site + disp) & 0xFFFFFFFF


# Opcodes whose instruction writes the GPR in the rD field (addi, addis, addic, mulli, subfic, integer loads), the
# rA field (ori, oris, xori, andi., rlwinm, rlwimi, rlwnm), or rA as the update form of a load or store.
GPR_WRITES_RD = {7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43}
GPR_WRITES_RA = {20, 21, 23, 24, 25, 26, 27, 28, 29}
UPDATE_FORMS = {33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55}
DFORM_OPS = {14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55}


def code_references(p: Program, sda: dict[int, int] | None = None, window: int = 16,
                    sites: dict[int, list[int]] | None = None) -> dict[int, int]:
    """Addresses formed by the code: `lis rD,hi` followed (within `window` instructions, rD not redefined) by
    `addi`/d-form load or store `x,lo(rD)`, plus `addi`/d-form accesses relative to r13/r2 when their bases are
    given. Returns address -> number of references; when `sites` is given, also fills it with address -> the
    instruction addresses that form it. Generic heuristic: it misses addresses built in other ways
    (ori, computed offsets) and never invents one that the code does not form."""
    refs: dict[int, int] = {}
    sda = sda or {}
    for s in p.code_segments():
        n = len(s.data) // 4
        words = struct.unpack_from(f">{n}I", s.data, 0)
        hi: dict[int, tuple[int, int]] = {}
        for i, w in enumerate(words):
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:                       # lis rD, imm
                hi[rd] = (imm << 16, i)
                continue
            if op == 24 and rd in hi and i - hi[rd][1] <= window:   # ori rA, rS, imm (rS in the rD field)
                t = (hi[rd][0] | imm) & 0xFFFFFFFF
                refs[t] = refs.get(t, 0) + 1
                if sites is not None:
                    sites.setdefault(t, []).append(s.addr + 4 * i)
            elif op in DFORM_OPS:
                t = None
                if ra and ra in hi and i - hi[ra][1] <= window:   # rA = 0 reads as 0 (li), never a base
                    t = (hi[ra][0] + sext16(imm)) & 0xFFFFFFFF
                elif ra in sda:
                    t = (sda[ra] + sext16(imm)) & 0xFFFFFFFF
                if t is not None:
                    refs[t] = refs.get(t, 0) + 1
                    if sites is not None:
                        sites.setdefault(t, []).append(s.addr + 4 * i)
            if op == 18 or op == 16 or op == 19 or op == 46:   # branches, blr/bctr, lmw: forget every pending lis
                hi.clear()
            elif op in GPR_WRITES_RD:
                hi.pop(rd, None)
                if op in UPDATE_FORMS:
                    hi.pop(ra, None)
            elif op in UPDATE_FORMS:
                hi.pop(ra, None)
            elif op in GPR_WRITES_RA:
                hi.pop(ra, None)
            elif op == 31:                                 # X-form: rD (or rA for logical ops) may be written
                hi.pop(rd, None)
                hi.pop(ra, None)
    return refs
