"""Table of the decompiled functions of a game (generic; parameters in games/<game>/game.toml, [decomp]).

The table (`[decomp] functions`, a TSV file in games/<game>/) lists, for every decompiled function, the identifier of
its export record (GC_DECOMP_EXPORT in the decompiled code), its module, its address in each version (one column per
version, named in `[decomp.columns]`), its size and its status. Only names, addresses and sizes: no game data.

This tool:
  * reads and checks the table (columns, one line per name, statuses);
  * resolves the addresses of one version, after checking in that version's DOL or REL that each address is the start
    of a function of the expected size (the instruction before it ends a function or is padding, the last one is
    blr/b/bctr); a function enabled for the build must have an address in EVERY version (parity);
  * writes the registration table of the version (C++ file for runtime/core/decomp/gc_abi.h, build folder only).
tools/build.py calls it for the HLE hooks (DolRecomp --hle-hooks) and the registration table.

Usage: py -3 tools/decomp_functions.py <game> check [--version ID ...]     (default: every version of game.toml)
       py -3 tools/decomp_functions.py <game> list --version ID          (resolved addresses of one version)
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402

# Statuses: `disabled` = listed, not built (no hook, no registration); `unverified` = replaces its recompiled version,
# not yet checked by the oracle; `verified` = replaces it and passed the oracle on its captured cases.
STATUSES = ("disabled", "unverified", "verified")
ENABLED = ("unverified", "verified")
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ABSENT = ("", "-")

BLR, BCTR, RFI = 0x4E800020, 0x4E800420, 0x4C000064


@dataclass
class Function:
    name: str
    module: str  # "dol", or the REL file name without extension
    addresses: dict[str, int | None]  # version ID -> address (DOL) or offset in the REL code section; None = absent
    size: int
    status: str
    line: int


def config(game_cfg: dict) -> tuple[Path, dict[str, str]]:
    """(table path, {version ID: column name}) from [decomp] of game.toml."""
    d = game_cfg.get("decomp", {})
    if "functions" not in d or "columns" not in d:
        raise SystemExit("game.toml: [decomp] functions and [decomp.columns] are required for decompiled functions")
    columns = dict(d["columns"])
    versions = {v["game_id"] for v in game_cfg.get("versions", [])}
    if set(columns) != versions:
        raise SystemExit(f"game.toml [decomp.columns]: one column per version expected ({sorted(versions)}), "
                         f"got {sorted(columns)}")
    return game_cfg["_dir"] / d["functions"], columns


def read_table(game_cfg: dict) -> list[Function]:
    path, columns = config(game_cfg)
    if not path.exists():
        return []
    header = None
    out: list[Function] = []
    seen: dict[str, int] = {}
    errors: list[str] = []
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        cells = raw.split("\t")
        if header is None:
            header = cells
            need = ["name", "module", *columns.values(), "size", "status"]
            missing = [c for c in need if c not in header]
            if missing:
                raise SystemExit(f"{path}:{n}: missing columns {missing} (header: {header})")
            continue
        if len(cells) != len(header):
            errors.append(f"{path}:{n}: {len(cells)} cells, {len(header)} columns")
            continue
        row = dict(zip(header, cells))
        name, status = row["name"], row["status"]
        if not IDENT.match(name):
            errors.append(f"{path}:{n}: name {name!r} is not a C identifier")
        if name in seen:
            errors.append(f"{path}:{n}: {name} already listed line {seen[name]}")
        seen[name] = n
        if status not in STATUSES:
            errors.append(f"{path}:{n}: status {status!r} (expected one of {', '.join(STATUSES)})")
        addresses: dict[str, int | None] = {}
        for version, column in columns.items():
            value = row[column].strip()
            try:
                addresses[version] = None if value in ABSENT else int(value, 16)
            except ValueError:
                errors.append(f"{path}:{n}: {column} {value!r} is not a hexadecimal address")
                addresses[version] = None
        try:
            size = int(row["size"], 0)
        except ValueError:
            errors.append(f"{path}:{n}: size {row['size']!r}")
            size = 0
        if size <= 0 or size % 4:
            errors.append(f"{path}:{n}: size {row['size']!r} must be a positive multiple of 4")
        if status in ENABLED:
            absent = [v for v, a in addresses.items() if a is None]
            if absent:
                errors.append(f"{path}:{n}: {name} is {status} but has no address for {', '.join(absent)}: a "
                              f"function without an address in every version cannot be enabled (status disabled)")
        out.append(Function(name, row["module"].strip(), addresses, size, status, n))
    if errors:
        raise SystemExit("Table of decompiled functions refused:\n  " + "\n  ".join(errors))
    return out


def _word(blob: bytes, offset: int) -> int:
    return int.from_bytes(blob[offset:offset + 4], "big")


def _ends_function(word: int) -> bool:
    """blr, bctr, rfi, or an unconditional relative branch without link (tail call)."""
    return word in (BLR, BCTR, RFI) or ((word >> 26) == 18 and (word & 3) == 0)


def check_code(ranges: list[tuple[int, bytes]], address: int, size: int) -> str | None:
    """Reason why [address, address + size) is not a whole function of `ranges` (code sections), None if it is."""
    for base, blob in ranges:
        if base <= address < base + len(blob):
            off = address - base
            if off % 4:
                return "address not aligned on 4"
            if off + size > len(blob):
                return f"size 0x{size:X} goes past the end of its code section"
            if off and not (_ends_function(_word(blob, off - 4)) or _word(blob, off - 4) == 0):
                return (f"the instruction before it ({_word(blob, off - 4):08X}) does not end a function: not a "
                        f"function start")
            last = _word(blob, off + size - 4)
            if not _ends_function(last):
                return f"its last instruction ({last:08X}) is not blr/bctr/b: size 0x{size:X} is not the function's"
            return None
    return "address outside the code sections"


def resolve(game_cfg: dict, version: str, disc_root: Path, layout: list[dict]) -> list[tuple[int, Function]]:
    """Enabled functions of `version` with their guest address, after the code checks. `layout`: tools/build.py
    rel_layout (virtual addresses of the RELs)."""
    import symbols as sy  # noqa: E402  (generic tool of the repository)
    functions = read_table(game_cfg)
    if not functions:
        return []
    dol = sy.Image.from_dol(disc_root / game_cfg["code"]["dol"])
    rels = {m["name"]: m for m in layout}
    out: list[tuple[int, Function]] = []
    errors: list[str] = []
    for f in functions:
        if f.status not in ENABLED:
            continue
        addr = f.addresses[version]
        assert addr is not None  # read_table refuses an enabled function with an absent address
        if f.module == "dol":
            ranges, guest = dol.ranges, addr
        elif f.module in rels:
            code = [(a, blob) for (_, a, _, ex, blob) in rels[f.module]["sections"] if ex]
            if not code:
                errors.append(f"{f.name}: module {f.module} has no code section")
                continue
            ranges, guest = code, code[0][0] + addr  # offset in the first code section, as in the dtk symbols
        else:
            errors.append(f"{f.name}: unknown module {f.module!r} (dol, or one of {', '.join(sorted(rels))})")
            continue
        reason = check_code(ranges, guest, f.size)
        if reason:
            errors.append(f"{f.name} ({version} {f.module} {addr:08X}, size 0x{f.size:X}): {reason}")
        out.append((guest, f))
    addresses = [a for a, _ in out]
    for a in sorted({a for a in addresses if addresses.count(a) > 1}):
        errors.append(f"two functions at {a:08X}: {', '.join(f.name for b, f in out if b == a)}")
    if errors:
        raise SystemExit(f"Decompiled functions refused for {version}:\n  " + "\n  ".join(errors))
    return sorted(out, key=lambda e: e[0])


def registry_source(version: str, entries: list[tuple[int, Function]]) -> str:
    """C++ registration table of `version` (build folder only: it holds addresses)."""
    lines = [f"// Registration table of the decompiled functions for {version}: generated by tools/decomp_functions.py",
             "// (tools/build.py) from the game's functions table. Build folder only, never in Git.",
             '#include "decomp/gc_abi.h"', '#include "decomp/gc_abi_c.h"', ""]
    for _, f in entries:
        lines.append(f"extern const ::gc::abi::export_record gc_decomp_export_{f.name};")
    lines += ["", "namespace {"]
    if entries:
        lines.append("const ::gc::abi::registration k_table[] = {")
        for a, f in entries:
            lines.append(f"    {{0x{a:08X}u, &gc_decomp_export_{f.name}}},")
        lines += ["};", "constexpr unsigned k_count = sizeof(k_table) / sizeof(k_table[0]);"]
    else:
        lines += ["const ::gc::abi::registration* const k_table = nullptr;", "constexpr unsigned k_count = 0;"]
    lines += ["} // namespace", "",
              'extern "C" void gcrt_decomp_install_generated(struct GcrtCpu* g) {',
              "    gcrt_decomp_install_table(g, k_table, k_count);", "}", ""]
    return "\n".join(lines)


def write_if_changed(path: Path, text: str) -> bool:
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    return True


def _layout(game_cfg: dict, disc_root: Path) -> list[dict]:
    import build  # noqa: E402  (tools/build.py: rel_layout)
    code = game_cfg.get("code", {})
    rel_dir = disc_root / code["rel_dir"] if code.get("rel_dir") else None
    rels = sorted(rel_dir.rglob("*.rel")) if rel_dir and rel_dir.exists() else []
    rc = game_cfg.get("recomp", {})
    base = build.parse_int(rc["rel_base"]) if "rel_base" in rc else 0x80500000
    return build.rel_layout(rels, base) if rels else []


def main() -> int:
    import json
    parser = argparse.ArgumentParser(description="Checks and resolves the table of decompiled functions.")
    parser.add_argument("game")
    parser.add_argument("action", choices=["check", "list"])
    parser.add_argument("--version", action="append", help="version ID (repeatable; default: all)")
    args = parser.parse_args()
    game_cfg = tc.load_game(args.game)
    versions = args.version or [v["game_id"] for v in game_cfg["versions"]]
    functions = read_table(game_cfg)
    print(f"{len(functions)} functions in the table, "
          f"{sum(f.status in ENABLED for f in functions)} enabled")
    for version in versions:
        manifest = tc.GENERATED / args.game / version / "manifest.json"
        if not manifest.exists():
            raise SystemExit(f"{manifest} missing: extract the disc first (tools/extract.py)")
        disc_root = Path(json.loads(manifest.read_text(encoding="utf-8"))["disc_root"])
        entries = resolve(game_cfg, version, disc_root, _layout(game_cfg, disc_root))
        print(f"{version}: {len(entries)} functions resolved, code checks passed")
        if args.action == "list":
            for a, f in entries:
                print(f"  {a:08X} {f.size:#06x} {f.module:8} {f.status:10} {f.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
