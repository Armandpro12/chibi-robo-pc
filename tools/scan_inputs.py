"""Inputs shared by the static scanners (tools/rtti_scan.py, tools/extab_scan.py). Generic: every path and
version comes from games/<game>/game.toml; nothing here names a game.

  - program(...)        the DOL and REL files of a version (game.toml [code]: dol, rel_dir), loaded by gcimage;
  - read_map(...)       a symbol map "ADDRESS SIZE NAME" (games/<game>/<[symbols] map_dir>/<ID>.map), read only;
  - dtk_functions(...)  the functions of a decomp-toolkit symbols.txt ([symbols] dtk_dir; reference version
                        only), DOL and RELs, placed in the program's address space;
  - dtk_units(...)      the units of a decomp-toolkit splits.txt (DOL only);
  - ghidra_functions()  the function list exported by tools/ghidra/analyze.py
                        (generated/<game>/<ID>/ghidra/out/functions.tsv: "entry size min max name ...");
  - mangled_class(...)  the class qualifier of a CodeWarrior-mangled name ("Update__7CWidgetFv" -> "CWidget").

`root` is the checkout holding the untracked inputs (external/, generated/), as in tools/decomp_progress.py;
game.toml and the maps are always read from the checkout of the tools.
"""

from __future__ import annotations

import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gcimage  # noqa: E402
import toolchain as tc  # noqa: E402

DTK_RE = re.compile(r"^(\S+)\s*=\s*([^:\s]+):0x([0-9A-Fa-f]+);\s*//\s*(.*)$")
SIZE_RE = re.compile(r"size:0x([0-9A-Fa-f]+)")
SPLIT_UNIT_RE = re.compile(r"^(\S.*):\s*$")
SPLIT_SEC_RE = re.compile(r"^\s+(\S+)\s+start:0x([0-9A-Fa-f]+)\s+end:0x([0-9A-Fa-f]+)")
# Automatic names (no real name known), same family as tools/symbols.py AUTO_RE: they carry no information.
AUTO_RE = re.compile(r"^(fn_|zz_|lbl_|@|func_|sub_|FUN_|LAB_|DAT_)")


def version_ids(game: dict, arg: str | None) -> list[str]:
    known = [v["game_id"] for v in game.get("versions", [])]
    if not arg:
        return known
    ids = [x.strip() for x in arg.split(",") if x.strip()]
    for i in ids:
        if i not in known:
            print(f"unknown version {i} (game.toml knows {', '.join(known)})", file=sys.stderr)
            sys.exit(2)
    return ids


def code_paths(slug: str, game: dict, vid: str, root: Path) -> tuple[Path, Path]:
    code = game.get("code", {})
    base = root / "generated" / slug / vid / "disc"
    return base / code.get("dol", "sys/main.dol"), base / code.get("rel_dir", "rel")


def check_dol(dol: Path) -> None:
    """Exits with code 1 and a message when the file cannot be a DOL: shorter than its 0x100-byte header, a segment
    outside the file, no code segment, or an entry point outside the code segments (the header itself has no magic
    number, so this is a plausibility check, not a proof)."""
    data = dol.read_bytes()
    if len(data) < 0x100:
        sys.exit(f"not a DOL: {dol} is shorter than the 256-byte header")
    offs = struct.unpack(">18I", data[0x00:0x48])
    addrs = struct.unpack(">18I", data[0x48:0x90])
    sizes = struct.unpack(">18I", data[0x90:0xD8])
    entry = struct.unpack(">I", data[0xE0:0xE4])[0]
    for i in range(18):
        if sizes[i] and (offs[i] < 0x100 or offs[i] + sizes[i] > len(data)):
            sys.exit(f"not a DOL: segment {i} of {dol} lies outside the file")
    if not any(sizes[i] and addrs[i] <= entry < addrs[i] + sizes[i] for i in range(7)):
        sys.exit(f"not a DOL: the entry point of {dol} is outside its code segments")


def program(slug: str, game: dict, vid: str, root: Path, rels: bool = True) -> gcimage.Program:
    dol, rel_dir = code_paths(slug, game, vid, root)
    if not dol.exists():
        sys.exit(f"missing: {dol} (extract the disc first: tools/extract.py)")
    check_dol(dol)
    rel_files = sorted(rel_dir.glob("*.rel")) if rels and rel_dir.is_dir() else []
    base = game.get("recomp", {}).get("rel_base", gcimage.DEFAULT_REL_BASE)
    return gcimage.Program.load(dol, rel_files, base)


def map_path(game: dict, vid: str) -> Path:
    d = game.get("symbols", {}).get("map_dir", "symbols")
    return Path(game["_dir"]) / d / f"{vid}.map"


def read_map(path: Path) -> list[tuple[int, int, str]]:
    out = []
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        t = line.split()
        if len(t) < 3 or line.startswith("#"):
            continue
        try:
            out.append((int(t[0], 16), int(t[1], 16), t[2]))
        except ValueError:
            continue
    return out


def dtk_dir(game: dict, root: Path) -> Path | None:
    d = game.get("symbols", {}).get("dtk_dir")
    return (root / d) if d else None


def dtk_reference(game: dict) -> str | None:
    return game.get("symbols", {}).get("reference")


def _dtk_file_functions(path: Path) -> list[tuple[str, int, int, str]]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        m = DTK_RE.match(line)
        if not m or "type:function" not in m.group(4):
            continue
        sz = SIZE_RE.search(m.group(4))
        out.append((m.group(2), int(m.group(3), 16), int(sz.group(1), 16) if sz else 0, m.group(1)))
    return out


def dtk_functions(game: dict, root: Path, prog: gcimage.Program) -> dict[int, tuple[int, str]]:
    """start -> (size, name) of every dtk function, DOL and RELs. REL symbols are offsets in the module's code
    section (dtk numbers the module's .text as its section 1, see tools/symbols.py): they are placed at that
    segment's synthetic address. Empty when the dtk folder is not there."""
    d = dtk_dir(game, root)
    out: dict[int, tuple[int, str]] = {}
    if not d or not (d / "symbols.txt").exists():
        return out
    for _sec, a, sz, name in _dtk_file_functions(d / "symbols.txt"):
        out[a] = (sz, name)
    by_name = {m.path.stem: m for m in prog.modules.values() if m.id}
    for sym in d.rglob("symbols.txt"):
        if sym.parent == d:
            continue
        mod = by_name.get(sym.parent.name)
        if mod is None:
            continue
        code = [s for _i, s in sorted(mod.sections.items()) if s.code]
        if not code:
            continue
        base = code[0].addr
        for _sec, a, sz, name in _dtk_file_functions(sym):
            out[base + a] = (sz, name)
    return out


def dtk_objects(game: dict, root: Path) -> dict[int, tuple[int, str]]:
    """start -> (size, name) of the named data objects of the DOL in the dtk symbols.txt (automatic names left
    out). Used to cross-check vtable and RTTI addresses against names such as __vt__<n><Class>."""
    d = dtk_dir(game, root)
    out: dict[int, tuple[int, str]] = {}
    if not d or not (d / "symbols.txt").exists():
        return out
    for line in (d / "symbols.txt").read_text(encoding="utf-8").splitlines():
        m = DTK_RE.match(line)
        if not m or "type:object" not in m.group(4) or is_auto(m.group(1)):
            continue
        sz = SIZE_RE.search(m.group(4))
        out[int(m.group(3), 16)] = (int(sz.group(1), 16) if sz else 0, m.group(1))
    return out


def dtk_units(game: dict, root: Path) -> list[tuple[int, int, str]]:
    """(start, end, unit) of the code sections of the DOL units in the dtk splits.txt."""
    d = dtk_dir(game, root)
    out = []
    if not d or not (d / "splits.txt").exists():
        return out
    unit = None
    for line in (d / "splits.txt").read_text(encoding="utf-8").splitlines():
        m = SPLIT_UNIT_RE.match(line)
        if m and not line.startswith("\t") and not line.startswith(" "):
            unit = m.group(1) if m.group(1) != "Sections" else None
            continue
        m = SPLIT_SEC_RE.match(line)
        if m and unit and m.group(1) in (".text", ".init"):
            out.append((int(m.group(2), 16), int(m.group(3), 16), unit))
    return sorted(out)


def ghidra_functions(slug: str, vid: str, root: Path) -> dict[int, int]:
    tsv = root / "generated" / slug / vid / "ghidra" / "out" / "functions.tsv"
    out: dict[int, int] = {}
    if not tsv.exists():
        return out
    for line in tsv.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        t = line.split("\t")
        try:
            out[int(t[0], 16)] = int(t[1], 16)
        except (ValueError, IndexError):
            continue
    return out


def is_auto(name: str) -> bool:
    return bool(AUTO_RE.match(name))


def mangled_class(name: str) -> str | None:
    """Class qualifier of a CodeWarrior (MWCC) mangled member name: `<base>__<len><Class>F...`, or nested
    `<base>__Q<n><len><A><len><B>...` (written A::B). None for a free function or an unmangled name.
    Mangling scheme: the CodeWarrior/cfront-style one also decoded by decomp-toolkit's demangler
    (cwdemangle); read here only to compare class names, [hypothesis] on rarely used forms (templates)."""
    k = name.find("__", 1)
    while k > 0:
        rest = name[k + 2:]
        m = re.match(r"(\d+)", rest)
        if m:
            n = int(m.group(1))
            cls = rest[len(m.group(1)):len(m.group(1)) + n]
            if len(cls) == n and n and (cls[0].isalpha() or cls[0] == "_"):
                return cls
        m = re.match(r"Q(\d)", rest)
        if m:
            parts, pos = [], 2
            for _ in range(int(m.group(1))):
                mm = re.match(r"(\d+)", rest[pos:])
                if not mm:
                    break
                n = int(mm.group(1))
                pos += len(mm.group(1))
                parts.append(rest[pos:pos + n])
                pos += n
            if parts and len(parts) == int(m.group(1)):
                return "::".join(parts)
        k = name.find("__", k + 1)
    return None


def default_out(tool: str, vid: str) -> Path:
    return tc.ROOT / "build" / tool / vid
