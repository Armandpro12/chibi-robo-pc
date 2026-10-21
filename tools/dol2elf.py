"""Converts a GameCube DOL executable into an ELF32 big-endian PowerPC executable. Generic.

Usage:
  py -3 tools/dol2elf.py <in.dol> <out.elf> [--map FILE] [--sda auto|none|R13,R2] [--no-check]

Purpose: tools that read ELF but not DOL (for example REA's "PowerPC ELF through Ghidra" lane, see
tools/rea.py) can analyse the game's main executable. The ELF holds exactly the DOL's bytes, at the DOL's
addresses: nothing is relocated, decompiled or rewritten. Being derived from the game, the output belongs in
an ignored folder (`generated/`), never in Git.

Layout of the output:
  - header: ELFCLASS32, ELFDATA2MSB, ET_EXEC, EM_PPC, e_flags = 0 (no EF_PPC_EMB: the Gekko is not an
    embedded/Book-E core), e_entry = the DOL's entry point;
  - one PT_LOAD segment and one section per non-empty DOL text/data section: `.textN` (PROGBITS,
    ALLOC+EXECINSTR, R+X) and `.dataN` (PROGBITS, ALLOC+WRITE, R+W), N = the DOL slot;
  - the DOL's BSS range minus the text/data sections placed inside it (the DOL format lets the small-data
    sections sit in the middle of the BSS range): one NOBITS section `.bss`, `.bss.1`, ... and one PT_LOAD
    (file size 0) per remaining piece;
  - `.symtab`/`.strtab`: one global FUNC symbol (address, size) per line of the symbol map, if given.
    Map format: "ADDRESS SIZE NAME" in hexadecimal, `#` comments (the format of tools/symbols.py). Malformed
    lines are reported on stderr and skipped; an unreadable map, or one that gives no symbol inside the
    DOL's text sections, is an error.

Small-data bases (`--sda`, default `auto`): PowerPC EABI code reaches small globals through r13 (`.sdata`/
`.sbss`) and r2 (`.sdata2`/`.sbss2`), set once at boot. Without their values, Ghidra shows these accesses
as `unaff_r13 + off`. Ghidra's PowerPC ELF loader (Ghidra 12.1.4, Processors/PowerPC/lib/PowerPC-src.zip,
PowerPC_ElfRelocationContext.getSDABase/getSDA2Base) sets r13/r2 as register context over every executable
block, but only while it applies an R_PPC_EMB_SDA21 relocation, and it takes the base from a `_SDA_BASE_` /
`_SDA2_BASE_` symbol when one exists. A symbol alone has no effect. So with `--sda`, this tool:
  - finds r13 and r2 in the boot code (`auto`: the entry function and the functions it calls directly,
    `lis rN,hi` then `ori`/`addi rN,rN,lo`), or takes them from the command line;
  - names `.sdata` / `.sdata2` the DOL data sections reached through r13 / r2, and adds absolute
    `_SDA_BASE_` / `_SDA2_BASE_` symbols;
  - adds one R_PPC_EMB_SDA21 relocation per register, on an existing instruction that already addresses
    small data through that register, with an addend chosen so that the relocated word is the word already
    there (Ghidra's formula: (old & 0xFFE00000) | (reg << 16) | ((S + A - base) & 0xFFFF)). The check below
    recomputes it: no byte of the program changes.
`--sda none` writes no relocation, no `_SDA*_BASE_` symbol and keeps the `.dataN` names.

Check (unless --no-check): the ELF is read back by an independent parser; every PROGBITS section must equal
the DOL bytes at the same address, the NOBITS pieces plus the data sections must cover the BSS range exactly,
the header fields must match, every relocation must leave its word unchanged. If `llvm-readelf` or GNU
`readelf` is found (tools/toolchain.py: [paths] readelf, external/llvm/bin, PATH), its view of the header,
sections and segments is compared too.

ELF format references: System V ABI (ELF), PowerPC Processor Supplement (ELF32 EM_PPC = 20) and the
PowerPC Embedded ABI (R_PPC_EMB_SDA21 = 109). DOL format: wiki.tockdom.com "DOL (File Format)".
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gcinfo  # noqa: E402
import toolchain as tc  # noqa: E402

# ELF constants (System V ABI; PowerPC supplement).
EM_PPC = 20
ET_EXEC = 2
PT_LOAD = 1
PF_X, PF_W, PF_R = 1, 2, 4
SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA, SHT_NOBITS = 1, 2, 3, 4, 8
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
SHN_ABS = 0xFFF1
STB_LOCAL, STB_GLOBAL = 0, 1
STT_NOTYPE, STT_FUNC, STT_SECTION = 0, 2, 3
R_PPC_EMB_SDA21 = 109

EHDR = struct.Struct(">16sHHIIIIIHHHHHH")   # 52 bytes
PHDR = struct.Struct(">IIIIIIII")           # 32 bytes
SHDR = struct.Struct(">IIIIIIIIII")         # 40 bytes
SYM = struct.Struct(">IIIBBH")              # 16 bytes
RELA = struct.Struct(">IIi")                # 12 bytes

# D-form loads, stores and addi: the instructions that address small data as d(r13) / d(r2).
DFORM_OPS = {14, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 48, 49, 50, 51, 52, 53, 54, 55}
SDA_SPAN = 0x8000          # a signed 16-bit displacement reaches [base - 0x8000, base + 0x8000)


@dataclass
class Section:
    name: str
    addr: int
    size: int
    data: bytes | None          # None: NOBITS
    exec_: bool = False
    dol_slot: str = ""          # "text1", "data6"... (empty for the BSS pieces)
    index: int = 0              # section header index, set when laid out
    offset: int = 0             # file offset, set when laid out


@dataclass
class Reloc:
    addr: int
    reg: int
    section: Section            # section whose STT_SECTION symbol is the relocation symbol
    addend: int


@dataclass
class Plan:
    entry: int
    sections: list[Section]
    funcs: list[tuple[int, int, str]] = field(default_factory=list)
    sda: dict[int, int] = field(default_factory=dict)       # register -> base
    relocs: list[Reloc] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)


U32 = 0xFFFFFFFF


def read_map(path: Path) -> list[tuple[int, int, str]]:
    """Lines "ADDRESS SIZE NAME" (hexadecimal, 32-bit). Blank and `#` lines are ignored; any other line that
    does not parse is malformed: counted and reported (first line numbers) on stderr, then skipped. A map
    that cannot be read, or that yields no symbol at all, is an error."""
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as e:
        sys.exit(f"cannot read the symbol map {path}: {e}")
    out, bad = [], []
    for n, line in enumerate(text.splitlines(), start=1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split()
        try:
            if len(parts) < 3:
                raise ValueError
            addr, size = int(parts[0], 16), int(parts[1], 16)
            if not (0 <= addr <= U32 and 0 <= size <= U32):
                raise ValueError
        except ValueError:
            bad.append(n)
            continue
        out.append((addr, size, parts[2]))
    if bad:
        print(f"warning: symbol map {path.name}: {len(bad)} malformed line(s) skipped (line "
              f"{', '.join(map(str, bad[:5]))}{', ...' if len(bad) > 5 else ''}); expected "
              "\"ADDRESS SIZE NAME\", hexadecimal, 32-bit values", file=sys.stderr)
    if not out:
        sys.exit(f"symbol map {path}: no symbol read (expected lines \"ADDRESS SIZE NAME\"); "
                 "fix the map, or convert without one")
    return out


def parse_sda(sda: str) -> str | tuple[int, int]:
    """`auto`, `none`, or "R13,R2" (two 32-bit values, decimal or 0x-prefixed): a clean error otherwise."""
    if sda in ("auto", "none"):
        return sda
    parts = [v.strip() for v in sda.split(",")]
    try:
        if len(parts) != 2:
            raise ValueError
        r13, r2 = (int(v, 0) for v in parts)
        if not (0 <= r13 <= U32 and 0 <= r2 <= U32):
            raise ValueError
    except ValueError:
        sys.exit(f"--sda: expected auto, none or R13,R2 (two 32-bit values, e.g. 0x80661EC0,0x80663D40), "
                 f"got {sda!r}")
    return r13, r2


DOL_HEADER_SIZE = 0x100   # 18 offsets, 18 addresses, 18 sizes, BSS address/size, entry, padding (tockdom wiki)


class DolImage:
    def __init__(self, data: bytes):
        self.data = data
        # A clean error instead of a struct.error traceback for a truncated or non-DOL file.
        if len(data) < DOL_HEADER_SIZE:
            sys.exit(f"not a DOL: {len(data)} bytes, shorter than the {DOL_HEADER_SIZE:#x}-byte DOL header")
        try:
            self.info = gcinfo.describe_dol(data)
        except struct.error as e:
            sys.exit(f"not a DOL: unreadable header ({e})")
        self.segs = self.info["segments"]

    def bytes_at(self, addr: int, size: int) -> bytes | None:
        for s in self.segs:
            if s["addr"] <= addr and addr + size <= s["addr"] + s["size"]:
                off = s["offset"] + addr - s["addr"]
                return self.data[off:off + size]
        return None

    def word(self, addr: int) -> int | None:
        b = self.bytes_at(addr, 4)
        return struct.unpack(">I", b)[0] if b else None


def sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def find_sda_bases(img: DolImage) -> dict[int, int]:
    """r13 and r2 as set by the boot code: the entry function and the functions it calls with `bl`
    (on the SDK's crt0, __start calls __init_registers). Pattern: lis rN,hi then ori/addi rN,rN,lo."""
    entry = img.info["entry"]
    found: dict[int, int] = {}

    def scan(start: int, collect_calls: bool) -> list[int]:
        calls, pending = [], {}
        for i in range(512):
            a = start + 4 * i
            w = img.word(a)
            if w is None or w == 0x4E800020:          # end of section / blr
                break
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0 and rd in (2, 13):                 # lis rN, hi
                pending[rd] = imm << 16
            elif op == 24 and rd == ra and ra in pending:              # ori rN, rN, lo (rS = rA)
                found.setdefault(ra, (pending.pop(ra) | imm) & 0xFFFFFFFF)
            elif op == 14 and ra == rd and rd in pending:              # addi rN, rN, lo
                found.setdefault(rd, (pending.pop(rd) + sext16(imm)) & 0xFFFFFFFF)
            elif collect_calls and op == 18 and (w & 3) == 1:          # bl target
                disp = w & 0x03FFFFFC
                if disp & 0x02000000:
                    disp -= 0x04000000
                calls.append((a + disp) & 0xFFFFFFFF)
        return calls

    for target in scan(entry, True)[:32]:
        if 2 in found and 13 in found:
            break
        scan(target, False)
    return {r: v for r, v in found.items() if r in (2, 13)}


def sda_section(sections: list[Section], base: int) -> Section | None:
    """The data section reached through a small-data base: the lowest PROGBITS data section inside
    [base - 0x8000, base + 0x8000)."""
    lo, hi = base - SDA_SPAN, base + SDA_SPAN
    cands = [s for s in sections if s.data is not None and not s.exec_ and s.addr < hi and s.addr + s.size > lo]
    return min(cands, key=lambda s: s.addr) if cands else None


def sda_instruction(sections: list[Section], reg: int) -> tuple[int, int] | None:
    """First instruction of the text sections that addresses memory as d(reg): (address, displacement)."""
    for s in sections:
        if not s.exec_ or s.data is None:
            continue
        for off in range(0, len(s.data) - 3, 4):
            w = struct.unpack_from(">I", s.data, off)[0]
            if (w >> 26) in DFORM_OPS and ((w >> 16) & 31) == reg:
                return s.addr + off, sext16(w & 0xFFFF)
    return None


def plan(dol: bytes, map_path: Path | None, sda: str) -> Plan:
    sda_mode = parse_sda(sda)
    funcs_in = read_map(map_path) if map_path else []
    img = DolImage(dol)
    info = img.info
    sections: list[Section] = []
    for s in sorted(img.segs, key=lambda s: s["addr"]):
        text = s["name"].startswith("text")
        sections.append(Section(name="." + s["name"], addr=s["addr"], size=s["size"],
                                data=dol[s["offset"]:s["offset"] + s["size"]], exec_=text, dol_slot=s["name"]))
        if len(sections[-1].data) != s["size"]:
            sys.exit(f"DOL truncated: {s['name']} needs {s['size']:#x} bytes at file offset {s['offset']:#x}")
    # Overlaps between DOL sections would make the ELF ambiguous: refuse.
    for a, b in zip(sections, sections[1:]):
        if a.addr + a.size > b.addr:
            sys.exit(f"DOL sections {a.dol_slot} and {b.dol_slot} overlap")

    # BSS range minus the text/data sections placed inside it.
    pieces, cur, end = [], info["bss"], info["bss"] + info["bss_size"]
    for s in sections:
        if s.addr + s.size <= cur or s.addr >= end:
            continue
        if s.addr > cur:
            pieces.append((cur, s.addr))
        cur = max(cur, s.addr + s.size)
    if cur < end:
        pieces.append((cur, end))
    for i, (a, b) in enumerate(pieces):
        sections.append(Section(name=".bss" if i == 0 else f".bss.{i}", addr=a, size=b - a, data=None))
    sections.sort(key=lambda s: s.addr)

    p = Plan(entry=info["entry"], sections=sections)

    if sda_mode != "none":
        if sda_mode == "auto":
            p.sda = find_sda_bases(img)
            if set(p.sda) != {2, 13}:
                print(f"warning: small-data bases not found in the boot code (found: "
                      f"{', '.join(f'r{r}={v:#x}' for r, v in sorted(p.sda.items())) or 'none'}); "
                      "use --sda R13,R2 to give them", file=sys.stderr)
        else:
            r13, r2 = sda_mode
            p.sda = {13: r13, 2: r2}
        names = {13: ".sdata", 2: ".sdata2"}
        used: set[int] = set()
        for reg in (13, 2):
            if reg not in p.sda:
                continue
            sec = sda_section(sections, p.sda[reg])
            ins = sda_instruction(sections, reg)
            if sec is None or id(sec) in used or ins is None:
                print(f"warning: r{reg} = {p.sda[reg]:#x}: no data section or no instruction using it, "
                      "small-data base not set", file=sys.stderr)
                del p.sda[reg]
                continue
            used.add(id(sec))
            sec.name = names[reg]
            addr, disp = ins
            p.relocs.append(Reloc(addr=addr, reg=reg, section=sec, addend=p.sda[reg] + disp - sec.addr))

    for addr, size, name in funcs_in:
        if any(s.exec_ and s.addr <= addr < s.addr + s.size for s in sections):
            p.funcs.append((addr, size, name))
        else:
            p.skipped.append(name)
    if map_path and not p.funcs:
        sys.exit(f"symbol map {map_path}: none of its {len(funcs_in)} symbols is inside a text section of "
                 "this DOL (map of another version?)")
    return p


class StrTab:
    def __init__(self):
        self.data = bytearray(b"\0")
        self.index: dict[str, int] = {"": 0}

    def add(self, s: str) -> int:
        if s not in self.index:
            self.index[s] = len(self.data)
            self.data += s.encode("utf-8") + b"\0"
        return self.index[s]


def write_elf(p: Plan) -> bytes:
    alloc = p.sections
    loads = alloc                                    # one PT_LOAD per section (BSS pieces: file size 0)
    phoff = EHDR.size
    pos = phoff + PHDR.size * len(loads)

    def align(pos: int, addr: int, a: int = 32) -> int:
        """File offset >= pos, congruent to addr modulo a (PT_LOAD rule: p_offset = p_vaddr mod p_align)."""
        return pos + ((addr - pos) % a)

    for s in alloc:
        s.offset = align(pos, s.addr)
        if s.data is not None:          # NOBITS: an offset but no file bytes
            pos = s.offset + s.size

    # Section header indices: [0] null, alloc sections, rela tables, .symtab, .strtab, .shstrtab.
    for i, s in enumerate(alloc, start=1):
        s.index = i
    rela_targets: dict[int, list[Reloc]] = {}
    for r in p.relocs:
        tgt = next(s for s in alloc if s.exec_ and s.addr <= r.addr < s.addr + s.size)
        rela_targets.setdefault(tgt.index, []).append(r)
    n = len(alloc) + 1
    rela_idx = {t: n + k for k, t in enumerate(sorted(rela_targets))}
    n += len(rela_targets)
    symtab_idx, strtab_idx, shstrtab_idx = n, n + 1, n + 2
    nsec = n + 3

    # Symbols: null, one STT_SECTION per alloc section, _SDA*_BASE_ (local, absolute), then global FUNCs.
    strtab = StrTab()
    syms = [SYM.pack(0, 0, 0, 0, 0, 0)]
    sec_sym: dict[int, int] = {}
    for s in alloc:
        sec_sym[s.index] = len(syms)
        syms.append(SYM.pack(0, s.addr, 0, (STB_LOCAL << 4) | STT_SECTION, 0, s.index))
    for reg, name in ((13, "_SDA_BASE_"), (2, "_SDA2_BASE_")):
        if reg in p.sda and any(r.reg == reg for r in p.relocs):
            syms.append(SYM.pack(strtab.add(name), p.sda[reg], 0, (STB_LOCAL << 4) | STT_NOTYPE, 0, SHN_ABS))
    first_global = len(syms)
    for addr, size, name in p.funcs:
        sec = next(s for s in alloc if s.exec_ and s.addr <= addr < s.addr + s.size)
        syms.append(SYM.pack(strtab.add(name), addr, size, (STB_GLOBAL << 4) | STT_FUNC, 0, sec.index))
    symtab = b"".join(syms)

    relas = {}
    for t, rs in rela_targets.items():
        relas[t] = b"".join(RELA.pack(r.addr, (sec_sym[r.section.index] << 8) | R_PPC_EMB_SDA21, r.addend)
                            for r in rs)

    shstr = StrTab()
    out = bytearray(pos)
    for s in alloc:
        if s.data is not None:
            out[s.offset:s.offset + s.size] = s.data

    def append(blob: bytes, a: int = 4) -> int:
        nonlocal out
        while len(out) % a:
            out.append(0)
        off = len(out)
        out += blob
        return off

    shdrs = [SHDR.pack(*([0] * 10))]
    for s in alloc:
        flags = SHF_ALLOC | (SHF_EXECINSTR if s.exec_ else SHF_WRITE)
        stype = SHT_NOBITS if s.data is None else SHT_PROGBITS
        shdrs.append(SHDR.pack(shstr.add(s.name), stype, flags, s.addr, s.offset, s.size, 0, 0, 32, 0))
    for t in sorted(rela_targets):
        off = append(relas[t])
        name = ".rela" + alloc[t - 1].name
        shdrs.append(SHDR.pack(shstr.add(name), SHT_RELA, 0, 0, off, len(relas[t]), symtab_idx, t, 4, RELA.size))
    off = append(symtab)
    shdrs.append(SHDR.pack(shstr.add(".symtab"), SHT_SYMTAB, 0, 0, off, len(symtab), strtab_idx, first_global,
                           4, SYM.size))
    off = append(bytes(strtab.data), 1)
    shdrs.append(SHDR.pack(shstr.add(".strtab"), SHT_STRTAB, 0, 0, off, len(strtab.data), 0, 0, 1, 0))
    name = shstr.add(".shstrtab")
    off = append(bytes(shstr.data), 1)
    shdrs.append(SHDR.pack(name, SHT_STRTAB, 0, 0, off, len(shstr.data), 0, 0, 1, 0))
    shoff = append(b"".join(shdrs), 4)

    phdrs = []
    for s in loads:
        flags = PF_R | (PF_X if s.exec_ else PF_W)
        filesz = 0 if s.data is None else s.size
        phdrs.append(PHDR.pack(PT_LOAD, s.offset, s.addr, s.addr, filesz, s.size, flags, 32))
    ident = b"\x7fELF" + bytes([1, 2, 1, 0]) + b"\0" * 8      # ELFCLASS32, ELFDATA2MSB, EV_CURRENT, SYSV
    out[0:EHDR.size] = EHDR.pack(ident, ET_EXEC, EM_PPC, 1, p.entry, phoff, shoff, 0, EHDR.size,
                                 PHDR.size, len(phdrs), SHDR.size, nsec, shstrtab_idx)
    out[phoff:phoff + PHDR.size * len(phdrs)] = b"".join(phdrs)
    return bytes(out)


# ---------------------------------------------------------------------------------------------------------
# Check: independent re-read of the ELF.

def read_elf(data: bytes) -> dict:
    ident = data[:16]
    if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 2:
        raise ValueError("not an ELF32 big-endian file")
    (_, e_type, e_machine, _, e_entry, e_phoff, e_shoff, e_flags, _, e_phentsize, e_phnum, e_shentsize,
     e_shnum, e_shstrndx) = EHDR.unpack_from(data, 0)
    shdrs = [SHDR.unpack_from(data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    shstr = shdrs[e_shstrndx]

    def cstr(tab_off: int, idx: int) -> str:
        end = data.index(b"\0", tab_off + idx)
        return data[tab_off + idx:end].decode("utf-8")

    secs = []
    for i, h in enumerate(shdrs):
        secs.append({"index": i, "name": cstr(shstr[4], h[0]), "type": h[1], "flags": h[2], "addr": h[3],
                     "offset": h[4], "size": h[5], "link": h[6], "info": h[7], "entsize": h[9]})
    phdrs = [PHDR.unpack_from(data, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    syms, relas = [], []
    for s in secs:
        if s["type"] == SHT_SYMTAB:
            strsec = secs[s["link"]]
            for k in range(s["size"] // SYM.size):
                nm, val, sz, info, _, shndx = SYM.unpack_from(data, s["offset"] + k * SYM.size)
                syms.append({"name": cstr(strsec["offset"], nm), "value": val, "size": sz, "bind": info >> 4,
                             "type": info & 15, "shndx": shndx})
    for s in secs:
        if s["type"] == SHT_RELA:
            for k in range(s["size"] // RELA.size):
                off, info, add = RELA.unpack_from(data, s["offset"] + k * RELA.size)
                relas.append({"offset": off, "sym": info >> 8, "type": info & 0xFF, "addend": add,
                              "target": s["info"]})
    return {"type": e_type, "machine": e_machine, "entry": e_entry, "flags": e_flags, "sections": secs,
            "segments": phdrs, "symbols": syms, "relas": relas}


def check(elf_bytes: bytes, dol: bytes, p: Plan) -> list[str]:
    errors = []
    e = read_elf(elf_bytes)
    img = DolImage(dol)
    info = img.info
    if (e["type"], e["machine"], e["flags"]) != (ET_EXEC, EM_PPC, 0):
        errors.append(f"header: type {e['type']} machine {e['machine']} flags {e['flags']:#x}")
    if e["entry"] != info["entry"]:
        errors.append(f"entry {e['entry']:#x} != DOL {info['entry']:#x}")
    progbits = [s for s in e["sections"] if s["type"] == SHT_PROGBITS]
    nobits = [s for s in e["sections"] if s["type"] == SHT_NOBITS]
    if len(progbits) != len(img.segs):
        errors.append(f"{len(progbits)} PROGBITS sections for {len(img.segs)} DOL sections")
    for seg in img.segs:
        match = [s for s in progbits if s["addr"] == seg["addr"] and s["size"] == seg["size"]]
        if len(match) != 1:
            errors.append(f"DOL {seg['name']}: no ELF section at {seg['addr']:#x} size {seg['size']:#x}")
            continue
        s = match[0]
        if elf_bytes[s["offset"]:s["offset"] + s["size"]] != dol[seg["offset"]:seg["offset"] + seg["size"]]:
            errors.append(f"{s['name']}: bytes differ from DOL {seg['name']}")
        want = SHF_ALLOC | (SHF_EXECINSTR if seg["name"].startswith("text") else SHF_WRITE)
        if s["flags"] != want:
            errors.append(f"{s['name']}: flags {s['flags']:#x}, expected {want:#x}")
        seg_ok = [ph for ph in e["segments"] if ph[0] == PT_LOAD and ph[2] == s["addr"] and ph[1] == s["offset"]
                  and ph[4] == s["size"] and ph[5] == s["size"]]
        if len(seg_ok) != 1:
            errors.append(f"{s['name']}: no matching PT_LOAD")
    # BSS coverage: NOBITS pieces + the sections inside the range = the whole range, no overlap.
    lo, hi = info["bss"], info["bss"] + info["bss_size"]
    spans = sorted([(s["addr"], s["addr"] + s["size"]) for s in nobits] +
                   [(max(s["addr"], lo), min(s["addr"] + s["size"], hi)) for s in progbits
                    if s["addr"] < hi and s["addr"] + s["size"] > lo])
    cur = lo
    for a, b in spans:
        if a != cur:
            errors.append(f"BSS coverage: gap or overlap at {cur:#x} (next span {a:#x})")
            break
        cur = b
    if spans and cur != hi:
        errors.append(f"BSS coverage ends at {cur:#x}, expected {hi:#x}")
    for s in nobits:
        if not [ph for ph in e["segments"] if ph[0] == PT_LOAD and ph[2] == s["addr"] and ph[4] == 0
                and ph[5] == s["size"]]:
            errors.append(f"{s['name']}: no matching PT_LOAD")
    # Symbols.
    funcs = [s for s in e["symbols"] if s["type"] == STT_FUNC]
    if len(funcs) != len(p.funcs):
        errors.append(f"{len(funcs)} FUNC symbols, {len(p.funcs)} expected")
    for s, (addr, size, name) in zip(funcs, p.funcs):
        if (s["value"], s["size"], s["name"]) != (addr, size, name):
            errors.append(f"symbol {s['name']}: {s['value']:#x}+{s['size']:#x}, expected {name} {addr:#x}+{size:#x}")
            break
    # Relocations: Ghidra's R_PPC_EMB_SDA21 formula must give back the word already there.
    bases = {s["name"]: s["value"] for s in e["symbols"] if s["name"] in ("_SDA_BASE_", "_SDA2_BASE_")}
    for r in e["relas"]:
        sym = e["symbols"][r["sym"]]
        block = e["sections"][sym["shndx"]]["name"]
        reg = {".sdata": 13, ".sbss": 13, ".sdata2": 2, ".sbss2": 2}.get(block)
        base = bases.get("_SDA_BASE_" if reg == 13 else "_SDA2_BASE_")
        old = img.word(r["offset"] & ~3)
        if r["type"] != R_PPC_EMB_SDA21 or reg is None or base is None or old is None:
            errors.append(f"relocation at {r['offset']:#x}: unexpected type/symbol/base")
            continue
        new = (old & 0xFFE00000) | (reg << 16) | ((sym["value"] - base + r["addend"]) & 0xFFFF)
        if new != old:
            errors.append(f"relocation at {r['offset']:#x} would change {old:#010x} into {new:#010x}")
    return errors


def readelf_check(path: Path, elf_bytes: bytes) -> tuple[str, list[str]]:
    """Header, sections and segments as seen by llvm-readelf / GNU readelf, compared with our own reader."""
    exe = tc.find_readelf()
    if not exe:
        return "readelf not found (skipped)", []
    out = subprocess.run([str(exe), "--file-header", "--sections", "--segments", "--wide", str(path)],
                         capture_output=True, text=True, encoding="utf-8", errors="replace")
    if out.returncode != 0:
        return f"{exe.name} failed", [out.stderr.strip()[:500]]
    text = out.stdout
    errors = []
    e = read_elf(elf_bytes)

    def field_(label: str) -> str | None:
        m = re.search(rf"^\s*{label}:\s*(.+?)\s*$", text, re.M)
        return m.group(1) if m else None

    checks = [("Class", lambda v: v == "ELF32"), ("Data", lambda v: "big endian" in v),
              ("Type", lambda v: v.startswith("EXEC")), ("Machine", lambda v: "PowerPC" in v and "64" not in v),
              ("Entry point address", lambda v: int(v, 16) == e["entry"]),
              ("Flags", lambda v: int(v.split(",")[0].split()[0], 16) == 0)]
    for label, ok in checks:
        v = field_(label)
        if v is None or not ok(v):
            errors.append(f"readelf {label}: {v!r}")
    # Section rows: "[Nr] Name Type Address Off Size ES Flg Lk Inf Al".
    rows = {}
    for m in re.finditer(r"^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-fA-F]{8})\s+([0-9a-fA-F]{6,8})\s+([0-9a-fA-F]{6,8})",
                         text, re.M):
        rows[int(m.group(1))] = (m.group(2), m.group(3), int(m.group(4), 16), int(m.group(5), 16), int(m.group(6), 16))
    for s in e["sections"][1:]:
        r = rows.get(s["index"])
        if r is None or (r[0], r[2], r[3], r[4]) != (s["name"], s["addr"], s["offset"], s["size"]):
            errors.append(f"readelf section [{s['index']}] {s['name']}: {r}")
    loads = re.findall(r"^\s*LOAD\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)",
                       text, re.M)
    got = sorted((int(a, 16), int(b, 16), int(c, 16), int(d, 16), int(f, 16)) for a, b, c, d, f in loads)
    want = sorted((ph[1], ph[2], ph[3], ph[4], ph[5]) for ph in e["segments"] if ph[0] == PT_LOAD)
    if got != want:
        errors.append(f"readelf: {len(got)} LOAD rows, {len(want)} expected, or values differ")
    return f"{exe.name}: {len(rows)} sections, {len(got)} LOAD segments", errors


def convert(dol_path: Path, out_path: Path, map_path: Path | None, sda: str = "auto",
            verify: bool = True) -> Plan:
    try:
        dol = dol_path.read_bytes()
    except OSError as e:
        sys.exit(f"cannot read the DOL: {e}")
    p = plan(dol, map_path, sda)
    elf = write_elf(p)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_name(out_path.name + ".tmp")
    tmp.write_bytes(elf)
    if verify:
        errors = check(elf, dol, p)
        note, rerrors = readelf_check(tmp, elf)
        errors += rerrors
        if errors:
            tmp.unlink()
            sys.exit("dol2elf: check FAILED:\n  " + "\n  ".join(errors))
        nprog = sum(1 for s in p.sections if s.data is not None)
        print(f"dol2elf: check OK: {nprog} sections equal to the DOL, BSS covered, "
              f"{len(p.funcs)} symbols, {len(p.relocs)} relocations unchanged words; {note}", file=sys.stderr)
    tmp.replace(out_path)
    return p


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dol", type=Path)
    ap.add_argument("out", type=Path)
    ap.add_argument("--map", type=Path, help='symbol map, lines "ADDRESS SIZE NAME" (hexadecimal)')
    ap.add_argument("--sda", default="auto", help="small-data bases: auto (from the boot code), none, or R13,R2")
    ap.add_argument("--no-check", action="store_true", help="skip the read-back and readelf checks")
    a = ap.parse_args()
    p = convert(a.dol, a.out, a.map, a.sda, not a.no_check)
    print(f"{a.out}: entry {p.entry:#010x}, {len(p.sections)} sections, {len(p.funcs)} functions"
          + (f" ({len(p.skipped)} map entries outside the text sections skipped)" if p.skipped else "")
          + "".join(f", r{r} = {v:#010x}" for r, v in sorted(p.sda.items(), reverse=True)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
