"""Inventory of the functions that touch the GameCube hardware (0xCC00xxxx registers, cache operations,
locked cache, special registers). Generic: no game-specific name or address.

Usage:
  py -3 tools/ghidra/hw_inventory.py <game> [--version ID] static
      Static scan of the DOL (function bounds: Ghidra output `functions.tsv`) and of the RELs
      (code sections), constant propagation per function (lis/addis/addi/ori/oris/mr).
      Output: generated/<game>/<ID>/ghidra/out/hw_static.tsv
  py -3 tools/ghidra/hw_inventory.py <game> [--version ID] dynamic <pclog.tsv>... [-o output.tsv]
      Aggregates one or more per-pc access logs (format: kind pc addr size count lr first last;
      produced by a local instrumentation of the runtime, see the internal SDK notes) by function and register.
  py -3 tools/ghidra/hw_inventory.py <game> [--version ID] report [--dynamic F]... [--blocks DI,SI,...]
      Table per function: registers read/written, cache operations, callers, dynamic presence.

What is detected statically:
  - D-form accesses (lwz/stw/lhz/sth/lbz/stb/psq…) whose base is a known constant;
  - X-form accesses (lwzx/stwx/…) where rA or rB is a known constant ("base + index");
  - hardware addresses materialized in a register (lis/addi), even without a visible access (ADDR);
  - dcbf, dcbi, dcbst, dcbz, dcbz_l, icbi, dcbt, dcbtst; mtspr/mfspr of HID0/HID2/WPAR/DMAU/DMAL/L2CR;
  - accesses to the locked cache (0xE0000000-0xE0003FFF).
What is not: accesses through a pointer stored in memory (e.g. GX register pointers), hence
the second, dynamic source.

Sources of the register names: Dolphin (Source/Core/Core/HW/*.h, GPL-2.0+, names only) and the
public "YAGCD" documentation.

License: GPL-3.0 only. Unlike the rest of tools/, this file is NOT offered under the MIT license, because its
register names come from Dolphin's headers (see the license table in guide/distribution-and-license.md).
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import gcinfo  # noqa: E402
import toolchain as tc  # noqa: E402

# --------------------------------------------------------------------------------------------------
# Hardware registers
# --------------------------------------------------------------------------------------------------
BLOCKS = [  # (base, size, name)
    (0xCC000000, 0x1000, "CP"), (0xCC001000, 0x1000, "PE"), (0xCC002000, 0x1000, "VI"),
    (0xCC003000, 0x1000, "PI"), (0xCC004000, 0x1000, "MI"), (0xCC005000, 0x1000, "DSP"),
    (0xCC006000, 0x400, "DI"), (0xCC006400, 0x400, "SI"), (0xCC006800, 0x400, "EXI"),
    (0xCC006C00, 0x400, "AI"), (0xCC008000, 0x8000, "GXFIFO"),
]
REG_NAMES = {
    "DI": {0x00: "DISR", 0x04: "DICVR", 0x08: "DICMDBUF0", 0x0C: "DICMDBUF1", 0x10: "DICMDBUF2", 0x14: "DIMAR",
           0x18: "DILENGTH", 0x1C: "DICR", 0x20: "DIIMMBUF", 0x24: "DICFG"},
    "SI": {0x00: "SIC0OUTBUF", 0x04: "SIC0INBUFH", 0x08: "SIC0INBUFL", 0x0C: "SIC1OUTBUF", 0x10: "SIC1INBUFH",
           0x14: "SIC1INBUFL", 0x18: "SIC2OUTBUF", 0x1C: "SIC2INBUFH", 0x20: "SIC2INBUFL", 0x24: "SIC3OUTBUF",
           0x28: "SIC3INBUFH", 0x2C: "SIC3INBUFL", 0x30: "SIPOLL", 0x34: "SICOMCSR", 0x38: "SISR",
           0x3C: "SIEXILK"},
    "VI": {0x00: "VTR", 0x02: "DCR", 0x04: "HTR0H", 0x06: "HTR0L", 0x08: "HTR1H", 0x0A: "HTR1L", 0x0C: "VTOH",
           0x0E: "VTOL", 0x10: "VTEH", 0x12: "VTEL", 0x14: "BBOIH", 0x16: "BBOIL", 0x18: "BBEIH", 0x1A: "BBEIL",
           0x1C: "TFBLH", 0x1E: "TFBLL", 0x20: "TFBRH", 0x22: "TFBRL", 0x24: "BFBLH", 0x26: "BFBLL",
           0x28: "BFBRH", 0x2A: "BFBRL", 0x2C: "DPV", 0x2E: "DPH", 0x30: "DI0H", 0x32: "DI0L", 0x34: "DI1H",
           0x36: "DI1L", 0x38: "DI2H", 0x3A: "DI2L", 0x3C: "DI3H", 0x3E: "DI3L", 0x40: "DL0H", 0x42: "DL0L",
           0x44: "DL1H", 0x46: "DL1L", 0x48: "HSW", 0x4A: "HSR", 0x6C: "VICLK", 0x6E: "VISEL", 0x70: "FBWIDTH",
           0x72: "HBE", 0x74: "HBS", **{0x4C + 2 * i: f"FCT{i // 2}{'HL'[i % 2]}" for i in range(14)}},
    "PI": {0x00: "INTSR", 0x04: "INTMSK", 0x0C: "FIFO_BASE", 0x10: "FIFO_END", 0x14: "FIFO_WPTR",
           0x18: "FIFO_RESET", 0x24: "RESET_CODE", 0x2C: "FLIPPER_REV"},
    "DSP": {0x00: "MBOX_IN_H", 0x02: "MBOX_IN_L", 0x04: "MBOX_OUT_H", 0x06: "MBOX_OUT_L", 0x0A: "DSPCSR",
            0x12: "AR_SIZE", 0x16: "AR_MODE", 0x1A: "AR_REFRESH", 0x20: "AR_DMA_MMADDR_H",
            0x22: "AR_DMA_MMADDR_L", 0x24: "AR_DMA_ARADDR_H", 0x26: "AR_DMA_ARADDR_L", 0x28: "AR_DMA_CNT_H",
            0x2A: "AR_DMA_CNT_L", 0x30: "AUDIO_DMA_START_H", 0x32: "AUDIO_DMA_START_L",
            0x36: "AUDIO_DMA_CONTROL_LEN", 0x3A: "AUDIO_DMA_BLOCKS_LEFT"},
    "EXI": {c * 0x14 + o: f"EXI{c}{n}" for c in range(3)
            for o, n in ((0x00, "CSR"), (0x04, "MAR"), (0x08, "LENGTH"), (0x0C, "CR"), (0x10, "DATA"))},
    "AI": {0x00: "AICR", 0x04: "AIVR", 0x08: "AISCNT", 0x0C: "AIIT"},
    "MI": {0x1C: "MI_INTMSK", 0x1E: "MI_INTSR", 0x20: "MI_ADDR_L", 0x22: "MI_ADDR_H"},
}
SPR_NAMES = {1008: "HID0", 1009: "HID1", 920: "HID2", 921: "WPAR", 922: "DMAU", 923: "DMAL", 1017: "L2CR",
             22: "DEC", 284: "TBL_W", 285: "TBU_W", 268: "TBL", 269: "TBU", 26: "SRR0", 27: "SRR1",
             1010: "IABR", 1013: "DABR", 952: "MMCR0", 956: "MMCR1", 953: "PMC1", 954: "PMC2", 957: "PMC3",
             958: "PMC4", 1019: "ICTC", 1020: "THRM1", 1021: "THRM2", 1022: "THRM3"}
SPR_INTERESTING = {1008, 920, 921, 922, 923, 1017}
CACHE_XO = {54: "dcbst", 86: "dcbf", 470: "dcbi", 982: "icbi", 1014: "dcbz", 278: "dcbt", 246: "dcbtst"}
LC_BASE, LC_SIZE = 0xE0000000, 0x4000


def block_of(addr: int) -> tuple[str, int] | None:
    a = addr & 0xFFFFFFFF
    if 0xC8000000 <= a < 0xC8010000 or 0x0C000000 <= a < 0x0C010000:  # physical / uncached aliases
        a = 0xCC000000 | (a & 0xFFFF)
    for base, size, name in BLOCKS:
        if base <= a < base + size:
            return name, a - base
    if 0xCC000000 <= a < 0xCC010000:
        return "HW?", a - 0xCC000000
    return None


def reg_label(addr: int) -> str:
    b = block_of(addr)
    if b is None:
        if LC_BASE <= addr < LC_BASE + LC_SIZE:
            return "LC"
        return f"{addr:08X}"
    name, off = b
    names = REG_NAMES.get(name, {})
    if name == "SI" and 0x80 <= off < 0x100:
        return "SI.SIIOBUF"
    if name == "GXFIFO":
        return "GXFIFO"
    r = names.get(off) or names.get(off & ~3) or names.get(off & ~1)
    return f"{name}.{r}" if r else f"{name}+{off:03X}"


# --------------------------------------------------------------------------------------------------
# Loading
# --------------------------------------------------------------------------------------------------
def paths(game: str, vid: str) -> dict:
    base = tc.GENERATED / game / vid
    return {"base": base, "out": base / "ghidra" / "out", "disc": base / "disc",
            "functions": base / "ghidra" / "out" / "functions.tsv"}


def load_functions(path: Path) -> list[tuple[int, int, str, list[int]]]:
    """Ghidra functions.tsv: (entry, exclusive end, name, callees)."""
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        entry, lo, hi = int(f[0], 16), int(f[2], 16), int(f[3], 16)
        callees = [int(x, 16) for x in f[6].split(",") if x] if len(f) > 6 else []
        out.append((entry, max(hi + 1, entry + int(f[1], 16)), f[4], callees, lo))
    out.sort()
    return out


def load_map(path: Path | None) -> dict[int, str]:
    names = {}
    if path and path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.strip() and not line.startswith("#"):
                a, _s, n = line.split()[:3]
                names[int(a, 16)] = n
    return names


class FuncIndex:
    def __init__(self, funcs, names):
        self.funcs = funcs
        self.starts = [f[0] for f in funcs]
        self.names = names
        self.by_entry = {f[0]: f for f in funcs}
        self.callers = defaultdict(set)
        for e, _end, _n, callees, _lo in funcs:
            for c in callees:
                self.callers[c].add(e)

    def find(self, addr: int):
        import bisect
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            e, end, n, _c, _lo = self.funcs[i]
            if e <= addr < end:
                return e
        return None

    def name(self, entry: int | None) -> str:
        if entry is None:
            return "-"
        return self.names.get(entry) or self.by_entry.get(entry, (0, 0, f"FUN_{entry:08x}"))[2]


# --------------------------------------------------------------------------------------------------
# Static scan
# --------------------------------------------------------------------------------------------------
def sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def scan_code(words, start: int, end: int, emit, module: str = "dol") -> None:
    """Linear constant propagation over [start, end); emits the hardware accesses."""
    regs: dict[int, int] = {}

    def known(r):
        return regs.get(r)

    def hw(a):
        return block_of(a) is not None or LC_BASE <= a < LC_BASE + LC_SIZE

    for addr in range(start, end, 4):
        w = words(addr)
        if w is None:
            regs.clear()
            continue
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        rb = (w >> 11) & 31
        imm = w & 0xFFFF
        dest, val = None, None
        if op == 15:  # addis / lis
            base = 0 if ra == 0 else known(ra)
            dest = rd
            if base is not None:
                val = (base + (sext16(imm) << 16)) & 0xFFFFFFFF
        elif op == 14:  # addi / li
            base = 0 if ra == 0 else known(ra)
            dest = rd
            if base is not None:
                val = (base + sext16(imm)) & 0xFFFFFFFF
                if ra != 0 and hw(val):
                    emit(module, addr, "ADDR", val, 0, w)
        elif op in (24, 25):  # ori / oris
            dest = ra
            s = known(rd)
            if s is not None:
                val = s | (imm << 16 if op == 25 else imm)
                if hw(val):
                    emit(module, addr, "ADDR", val, 0, w)
        elif 32 <= op <= 55 or op in (56, 57, 60, 61):
            if op in (56, 57, 60, 61):
                disp = (w & 0xFFF) - 0x1000 if w & 0x800 else w & 0xFFF
            else:
                disp = sext16(imm)
            base = known(ra) if ra != 0 else 0
            store = op in (36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 60, 61)
            size = {32: 4, 33: 4, 34: 1, 35: 1, 36: 4, 37: 4, 38: 1, 39: 1, 40: 2, 41: 2, 42: 2, 43: 2, 44: 2,
                    45: 2, 46: 4, 47: 4, 48: 4, 49: 4, 50: 8, 51: 8, 52: 4, 53: 4, 54: 8, 55: 8}.get(op, 8)
            if base is not None and ra != 0:
                a = (base + disp) & 0xFFFFFFFF
                if hw(a):
                    emit(module, addr, "W" if store else "R", a, size, w)
            if not store or op in (37, 39, 45, 53, 55, 61):
                if op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55, 57, 61):  # update of rA
                    if ra in regs and base is not None:
                        regs[ra] = (base + disp) & 0xFFFFFFFF
                    else:
                        regs.pop(ra, None)
                if not store and op not in (48, 49, 50, 51, 56, 57):
                    if op == 46:
                        for r in range(rd, 32):
                            regs.pop(r, None)
                    else:
                        regs.pop(rd, None)
            continue
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            if xo in CACHE_XO:
                eab = known(ra) if ra else 0
                ea = None
                if eab is not None and known(rb) is not None:
                    ea = (eab + known(rb)) & 0xFFFFFFFF
                emit(module, addr, "CACHE", ea if ea is not None else 0, 0, w, CACHE_XO[xo])
                continue
            if xo in (339, 467):  # mfspr / mtspr
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                if spr in SPR_INTERESTING:
                    emit(module, addr, "MTSPR" if xo == 467 else "MFSPR", spr, 4, w, SPR_NAMES.get(spr, str(spr)))
                if xo == 339:
                    regs.pop(rd, None)
                continue
            xmem = {23: (4, 0), 55: (4, 0), 87: (1, 0), 119: (1, 0), 151: (4, 1), 183: (4, 1), 215: (1, 1),
                    247: (1, 1), 279: (2, 0), 311: (2, 0), 343: (2, 0), 375: (2, 0), 407: (2, 1), 439: (2, 1),
                    534: (4, 0), 662: (4, 1), 790: (2, 0), 918: (2, 1), 535: (4, 0), 567: (4, 0), 599: (8, 0),
                    631: (8, 0), 663: (4, 1), 695: (4, 1), 727: (8, 1), 759: (8, 1), 983: (4, 1)}
            if xo in xmem:
                size, store = xmem[xo]
                for r in (ra, rb):
                    if r and known(r) is not None and hw(known(r)):
                        emit(module, addr, "WX" if store else "RX", known(r), size, w)
                        break
                if not store and xo not in (535, 567, 599, 631):
                    regs.pop(rd, None)
                if xo in (55, 119, 183, 247, 311, 375, 439, 567, 631, 695, 759):
                    regs.pop(ra, None)
                continue
            if xo == 444 and rd == rb:  # mr rA, rS
                if known(rd) is not None:
                    regs[ra] = known(rd)
                else:
                    regs.pop(ra, None)
                continue
            # other X forms: invalidate the probable destination register
            if xo in (28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954, 986):
                regs.pop(ra, None)  # logical / shifts: destination rA
            else:
                regs.pop(rd, None)
            continue
        elif op == 4 and ((w >> 1) & 0x3FF) == 1014:  # dcbz_l
            emit(module, addr, "CACHE", 0, 0, w, "dcbz_l")
            continue
        elif op == 18:
            if w & 1:  # bl: volatile registers lost
                for r in [0] + list(range(3, 13)):
                    regs.pop(r, None)
            continue
        elif op == 19:
            continue
        elif op in (10, 11, 16, 17, 2, 3):
            continue
        elif op in (20, 21, 23, 26, 27, 28, 29):
            dest = ra
        elif op in (7, 8, 12, 13):
            dest = rd
        elif op in (48, 49, 50, 51, 52, 53, 54, 55, 59, 63, 4):
            continue
        else:
            dest = rd
        if dest is not None:
            if val is None:
                regs.pop(dest, None)
            else:
                regs[dest] = val


def cmd_static(ns, game_cfg, vid, p) -> int:
    from symbols import Image  # noqa: E402

    code = game_cfg.get("code", {})
    dol = p["disc"] / code.get("dol", "sys/main.dol")
    rel_dir = p["disc"] / code["rel_dir"] if code.get("rel_dir") else None
    img = Image.from_dol(dol)
    names = load_map(game_cfg["_dir"] / "symbols" / f"{vid}.map")
    funcs = load_functions(p["functions"])
    idx = FuncIndex(funcs, names)
    rows = []

    def emit(module, addr, kind, target, size, word, extra=""):
        rows.append((module, addr, kind, target, size, word, extra))

    # DOL: function by function (Ghidra bounds), then the code outside functions (raw scan).
    spans = []
    for entry, end, _n, _c, lo in funcs:
        scan_code(img.word, min(entry, lo), end, emit)
        spans.append((min(entry, lo), end))
    # Code that Ghidra did not put in a function (e.g. functions only called through a pointer):
    # each non-empty gap between two functions of a text segment is scanned as a function.
    spans.sort()
    for base, blob in img.ranges:
        cur = base
        for lo, hi in spans + [(base + len(blob), base + len(blob))]:
            if lo < base or lo > base + len(blob):
                continue
            if lo > cur:
                scan_code(img.word, cur, lo, emit)
            cur = max(cur, hi)
    # REL: executable sections, without function bounds (linear scan, rare in the scene code).
    if rel_dir and rel_dir.exists():
        for rel in sorted(rel_dir.rglob("*.rel")):
            data = rel.read_bytes()
            info = gcinfo.describe_rel(data)
            for s in info["sections"]:
                if not (s["exec"] and s["size"]):
                    continue
                blob = data[s["offset"]:s["offset"] + s["size"]]

                def word(a, blob=blob):
                    return struct.unpack_from(">I", blob, a)[0] if 0 <= a <= len(blob) - 4 else None
                scan_code(word, 0, len(blob), emit, module=f"{rel.stem}:s{s['index']}")
    out = p["out"] / "hw_static.tsv"
    out.parent.mkdir(parents=True, exist_ok=True)
    lines = ["# module\tfunction\tname\tinstruction\ttype\ttarget\tregister\tsize\tdetail"]
    for module, addr, kind, target, size, word, extra in rows:
        if module == "dol":
            fe = idx.find(addr)
            fn, nm = (f"{fe:08X}" if fe is not None else "-"), (idx.name(fe) if fe is not None else "(outside any Ghidra function)")
        else:
            fn, nm = "-", "-"
        reg = SPR_NAMES.get(target, str(target)) if kind in ("MTSPR", "MFSPR") else (
            extra if kind == "CACHE" else reg_label(target))
        lines.append(f"{module}\t{fn}\t{nm}\t{addr:08X}\t{kind}\t{target:08X}\t{reg}\t{size}\t{extra or f'{word:08X}'}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows)} accesses -> {out}")
    return 0


# --------------------------------------------------------------------------------------------------
# Dynamic aggregation
# --------------------------------------------------------------------------------------------------
def load_rel_layout(path: Path) -> list[tuple[int, int, str]]:
    out = []
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("#") or not line.strip():
                continue
            f = line.split()
            out.append((int(f[1], 16), int(f[7], 16), f[9]))
    return out


def read_pclog(paths_: list[Path]):
    agg = defaultdict(lambda: [0, None, None, None])  # (kind, pc, addr, size) -> [count, lr, first, last]
    for path in paths_:
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line or line.startswith("#"):
                continue
            kind, pc, addr, size, count, lr, first, last = line.split("\t")
            k = (kind, int(pc, 16), int(addr, 16), int(size))
            e = agg[k]
            e[0] += int(count)
            e[1] = e[1] or int(lr, 16)
            e[2] = e[2] if e[2] is not None else first
            e[3] = last
    return agg


def cmd_dynamic(ns, game_cfg, vid, p) -> int:
    names = load_map(game_cfg["_dir"] / "symbols" / f"{vid}.map")
    idx = FuncIndex(load_functions(p["functions"]), names)
    layout = load_rel_layout(p["base"] / "recomp" / "rel_layout.txt")
    agg = read_pclog([Path(x) for x in ns.logs])
    lines = ["# function\tname\tpc\ttype\tregister\taddress\tsize\tcount\tlr\tlr_function\tfirst\tlast"]
    kinds = {"R": "R", "W": "W", "r": "R?", "w": "W?", "L": "LC-R", "M": "LC-W", "c": "CACHE", "S": "MTSPR",
             "s": "MFSPR"}
    cache_ops = {1: "dcbst", 2: "dcbf", 3: "dcbi", 4: "icbi"}

    def where(pc):
        fe = idx.find(pc)
        if fe is not None:
            return f"{fe:08X}", idx.name(fe)
        for base, end, mod in layout:
            if base <= pc < end:
                return "-", f"rel:{mod}+{pc - base:X}"
        if pc < 0x80003100:
            return "-", "low memory (copied vector or apploader)"
        return "-", "?"

    rows = []
    for (kind, pc, addr, size), (count, lr, first, last) in agg.items():
        fe, nm = where(pc)
        lfe, lnm = where(lr or 0)
        if kind == "c":
            reg = cache_ops.get(size, str(size))
        elif kind in ("S", "s"):
            reg = SPR_NAMES.get(addr, f"SPR{addr}")
        else:
            reg = reg_label(addr)
        rows.append((nm, fe, pc, kinds.get(kind, kind), reg, addr, size, count, lr or 0, lnm, first, last))
    rows.sort(key=lambda r: (r[4].split(".")[0], r[1], r[2]))
    for r in rows:
        lines.append(f"{r[1]}\t{r[0]}\t{r[2]:08X}\t{r[3]}\t{r[4]}\t{r[5]:08X}\t{r[6]}\t{r[7]}\t{r[8]:08X}\t{r[9]}\t{r[10]}\t{r[11]}")
    out = Path(ns.output) if ns.output else p["out"] / "hw_dynamic.tsv"
    out.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"{len(rows)} lines -> {out}")
    return 0


# --------------------------------------------------------------------------------------------------
# Report per function
# --------------------------------------------------------------------------------------------------
def cmd_report(ns, game_cfg, vid, p) -> int:
    names = load_map(game_cfg["_dir"] / "symbols" / f"{vid}.map")
    idx = FuncIndex(load_functions(p["functions"]), names)
    want = set(ns.blocks.split(",")) if ns.blocks else None
    per = defaultdict(lambda: {"static": set(), "dyn": defaultdict(int)})
    for line in (p["out"] / "hw_static.tsv").read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            continue
        module, fn, nm, ins, kind, target, reg, size, extra = line.split("\t")
        blk = reg.split(".")[0].split("+")[0] if kind not in ("CACHE", "MTSPR", "MFSPR") else kind
        if want and blk not in want:
            continue
        key = fn if fn != "-" else f"{module}:{ins}"
        per[key]["static"].add(f"{kind}:{reg}")
    for d in ns.dynamic or []:
        for line in Path(d).read_text(encoding="utf-8").splitlines():
            if line.startswith("#"):
                continue
            fe, nm, pc, kind, reg, addr, size, count, *_ = line.split("\t")
            blk = reg.split(".")[0].split("+")[0] if kind not in ("CACHE", "MTSPR", "MFSPR") else kind
            if want and blk not in want:
                continue
            key = fe if fe != "-" else nm
            per[key]["dyn"][f"{kind}:{reg}"] += int(count)
    print("# function\tname\tstatic\tdynamic (count)\tcallers")
    for key in sorted(per):
        st = ",".join(sorted(per[key]["static"]))
        dy = ",".join(f"{k}={v}" for k, v in sorted(per[key]["dyn"].items()))
        try:
            fe = int(key, 16)
            nm = idx.name(fe)
            callers = ",".join(f"{c:08X}:{idx.name(c)}" for c in sorted(idx.callers.get(fe, ())))
        except ValueError:
            nm, callers = key, ""
        print(f"{key}\t{nm}\t{st}\t{dy}\t{callers}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--version")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("static")
    dy = sub.add_parser("dynamic")
    dy.add_argument("logs", nargs="+")
    dy.add_argument("-o", "--output")
    rp = sub.add_parser("report")
    rp.add_argument("--dynamic", action="append")
    rp.add_argument("--blocks", help="e.g. DI,SI,VI,DSP,CACHE,MTSPR,MFSPR,LC")
    ns = ap.parse_args()
    game_cfg = tc.load_game(ns.game)
    vid = ns.version or game_cfg["game"]["default_version"]
    p = paths(ns.game, vid)
    return {"static": cmd_static, "dynamic": cmd_dynamic, "report": cmd_report}[ns.cmd](ns, game_cfg, vid, p)


if __name__ == "__main__":
    sys.exit(main())
