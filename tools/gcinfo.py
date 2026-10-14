"""Describes the layout of a GameCube DOL executable and of REL modules. Generic.

Usage: py -3 tools/gcinfo.py <file.dol | file.rel | directory> [--markdown]

Formats (sources: wiki.tockdom.com "DOL (File Format)" and "REL (File Format)";
the SDK's OSModuleHeader header as used by Dolphin and the dtk decompilations):
  DOL: 7 text segments + 11 data segments (offsets, addresses, sizes), BSS, entry point.
  REL: OSModuleHeader header (id, sections, bss, relocations, imports, prolog/epilog/unresolved),
       section table (offset | executable flag in bit 0, size), import table
       (module, offset of the relocations), 8-byte relocations (u16 offset, u8 type, u8 section, u32 addend).
"""

from __future__ import annotations

import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain  # noqa: E402,F401  (console encoding)

R_PPC = {0: "NONE", 1: "ADDR32", 2: "ADDR24", 3: "ADDR16", 4: "ADDR16_LO", 5: "ADDR16_HI", 6: "ADDR16_HA",
         7: "ADDR14", 10: "REL24", 11: "REL14", 201: "DOLPHIN_NOP", 202: "DOLPHIN_SECTION", 203: "DOLPHIN_END"}


def describe_dol(data: bytes) -> dict:
    offs = struct.unpack(">18I", data[0x00:0x48])
    addrs = struct.unpack(">18I", data[0x48:0x90])
    sizes = struct.unpack(">18I", data[0x90:0xD8])
    bss, bss_size, entry = struct.unpack(">III", data[0xD8:0xE4])
    segs = []
    for i in range(18):
        if sizes[i]:
            kind, idx = ("text", i) if i < 7 else ("data", i - 7)
            segs.append({"name": f"{kind}{idx}", "offset": offs[i], "addr": addrs[i], "size": sizes[i]})
    return {"entry": entry, "bss": bss, "bss_size": bss_size, "segments": segs}


def describe_rel(data: bytes) -> dict:
    (mid, _nxt, _prv, nsec, secoff, nameoff, namesize, ver, bss_size, reloff, impoff, impsize,
     prolog_s, epilog_s, unres_s, bss_s, prolog, epilog, unresolved) = struct.unpack(">12I4B3I", data[:0x40])
    align = bss_align = fix_size = None
    if ver >= 2:
        align, bss_align = struct.unpack(">II", data[0x40:0x48])
    if ver >= 3:
        fix_size = struct.unpack(">I", data[0x48:0x4C])[0]
    sections = []
    for i in range(nsec):
        off, size = struct.unpack(">II", data[secoff + i * 8:secoff + i * 8 + 8])
        sections.append({"index": i, "offset": off & ~1, "exec": bool(off & 1), "size": size})
    imports = []
    for i in range(impsize // 8):
        imp_mod, rel_off = struct.unpack(">II", data[impoff + i * 8:impoff + i * 8 + 8])
        types: Counter = Counter()
        pos = rel_off
        count = 0
        while pos + 8 <= len(data):
            _o, typ, _sec, _add = struct.unpack(">HBBI", data[pos:pos + 8])
            pos += 8
            if typ == 203:
                break
            if typ not in (201, 202):
                types[R_PPC.get(typ, str(typ))] += 1
                count += 1
        imports.append({"module": imp_mod, "relocs": count, "types": dict(types)})
    return {"id": mid, "version": ver, "num_sections": nsec, "sections": sections, "bss_size": bss_size,
            "name_offset": nameoff, "name_size": namesize,
            "prolog": (prolog_s, prolog), "epilog": (epilog_s, epilog), "unresolved": (unres_s, unresolved),
            "bss_section": bss_s, "align": align, "bss_align": bss_align, "fix_size": fix_size, "imports": imports}


def print_dol(path: Path, md: bool) -> None:
    d = describe_dol(path.read_bytes())
    print(f"### {path.name}\n" if md else f"== {path}")
    print(f"- entry point: `{d['entry']:#010x}`")
    print(f"- BSS: `{d['bss']:#010x}` + `{d['bss_size']:#x}`")
    if md:
        print("\n| segment | file offset | address | size |\n|---|---|---|---|")
    for s in d["segments"]:
        if md:
            print(f"| {s['name']} | `{s['offset']:#x}` | `{s['addr']:#010x}` | `{s['size']:#x}` |")
        else:
            print(f"  {s['name']:<6} off {s['offset']:#08x} addr {s['addr']:#010x} size {s['size']:#08x}")


def print_rels(paths: list[Path], md: bool) -> None:
    if md:
        print("| file | id | ver | sections (exec / data) | code size | BSS | prolog | epilog | unresolved | imports (module: relocs) |")
        print("|---|---|---|---|---|---|---|---|---|---|")
    for p in paths:
        r = describe_rel(p.read_bytes())
        code = sum(s["size"] for s in r["sections"] if s["exec"])
        nexec = sum(1 for s in r["sections"] if s["exec"])
        ndata = sum(1 for s in r["sections"] if s["size"] and not s["exec"])
        imps = ", ".join(f"{i['module']}:{i['relocs']}" for i in r["imports"])
        fmt = lambda t: f"s{t[0]}+{t[1]:#x}" if t[0] else "—"  # noqa: E731
        if md:
            print(f"| {p.name} | {r['id']} | {r['version']} | {nexec} / {ndata} | `{code:#x}` | `{r['bss_size']:#x}` | "
                  f"{fmt(r['prolog'])} | {fmt(r['epilog'])} | {fmt(r['unresolved'])} | {imps} |")
        else:
            print(f"== {p.name} id={r['id']} v{r['version']} code={code:#x} bss={r['bss_size']:#x} "
                  f"prolog={fmt(r['prolog'])} epilog={fmt(r['epilog'])} unresolved={fmt(r['unresolved'])}")
            for s in r["sections"]:
                if s["size"] or s["offset"]:
                    print(f"   sec{s['index']:<2} {'X' if s['exec'] else ' '} off {s['offset']:#07x} size {s['size']:#07x}")
            for i in r["imports"]:
                print(f"   import module {i['module']}: {i['relocs']} relocs {i['types']}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("path", type=Path)
    ap.add_argument("--markdown", action="store_true")
    a = ap.parse_args()
    if a.path.is_dir():
        print_rels(sorted(a.path.glob("*.rel")), a.markdown)
    elif a.path.suffix.lower() == ".dol":
        print_dol(a.path, a.markdown)
    else:
        print_rels([a.path], a.markdown)
    return 0


if __name__ == "__main__":
    sys.exit(main())
