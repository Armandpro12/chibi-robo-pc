"""Table of the decompiled functions of a game (generic; parameters in games/<game>/game.toml, [decomp]).

The table (`[decomp] functions`, a TSV file in games/<game>/) lists, for every decompiled function, the identifier of
its export record (GC_DECOMP_EXPORT in the decompiled code), its module, its address in each version (one column per
version, named in `[decomp.columns]`), its size and its status. Only names, addresses and sizes: no game data.

This tool:
  * reads and checks the table (columns, one line per name, statuses). Statuses are read as tools/decomp_progress.py
    reads them (tools/decomp_common.py: whole word, case-insensitive, surrounding "[]"/backquotes ignored; lists in
    [decomp] done_statuses, replaced_statuses, listed_statuses). A done or replaced status (verified, unverified)
    enables the function; a listed one (disabled) does not; any other status refuses the table;
  * resolves the addresses of one version after two checks, both required for every enabled function:
    1. activation lock (`check_sources`): the address is ESTABLISHED in that version's symbol sources, all of them
       files in Git next to the symbol maps ([symbols] map_dir), so that a build needs neither the dtk symbols nor
       generated/ symbol files. Reference version: the reference columns of the transposition reports
       (<ref>-to-<ID>.tsv of tools/symbols.py transpose, [decomp] transposition: every reference function with its
       size), <ID>.map, <ID>-rel.tsv. Other versions: the target column of the transposition report, <ID>.map,
       <ID>-rel.tsv and <ID>-gaps.tsv.
       (a) an address that <ID>-gaps.tsv gives at the level position or sized is refused unless the row's oracle
           column for that version ([decomp.oracle_columns]) is "yes" (the oracle has passed on that version); a
           level other than strong, graph, position or sized is refused;
       (b) the row's size must equal the size of the function in that version (gap target size first, otherwise the
           sizes the other sources give, which must agree; a transposition size only for an identical match): an
           unknown or different size is refused, and so is a range holding another known function start of that
           version strictly inside it;
       (c) an address found in none of the sources is refused;
       (d) pairing: in a version other than the reference, the address must be the one that version's
           transposition report or <ID>-gaps.tsv gives to the row's REFERENCE address (tools/decomp_common.py
           paired_address), so that both columns name the same function; for a REL row, both <ID>-rel.tsv files
           must give that function the same non-automatic name at the two offsets;
    2. code check: in that version's DOL or REL the address is the start of a function of the expected size (the
       instruction before it ends a function or is padding, the last one is blr/b/bctr).
    A function enabled for the build must have an address in EVERY version (parity);
  * writes the registration table of the version (C++ file for runtime/core/decomp/gc_abi.h, build folder only).
tools/build.py calls it for the HLE hooks (DolRecomp --hle-hooks) and the registration table.

Usage: py -3 tools/decomp_functions.py <game> check [--version ID ...]     (default: every version of game.toml)
       py -3 tools/decomp_functions.py <game> list --version ID          (resolved addresses of one version)
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import decomp_common as dc  # noqa: E402
import toolchain as tc  # noqa: E402

# Status kinds (tools/decomp_common.py, lists in [decomp] of game.toml): done (`verified` = replaces its recompiled
# version and passed the oracle) and replaced (`unverified` = replaces it, not yet checked by the oracle) are built;
# listed (`disabled`) is not (no hook, no registration).
ENABLED_KINDS = ("done", "replaced")
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
ABSENT = ("", "-")

BLR, BCTR, RFI = 0x4E800020, 0x4E800420, 0x4C000064


@dataclass
class Function:
    name: str
    module: str  # "dol", or the REL file name without extension
    addresses: dict[str, int | None]  # version ID -> address (DOL) or offset in the REL code section; None = absent
    size: int
    status: str  # normalized (tools/decomp_common.py)
    line: int
    oracle: dict[str, bool] = field(default_factory=dict)  # version -> oracle column "yes" ([decomp.oracle_columns])
    kind: str = "listed"  # done, replaced or listed

    @property
    def enabled(self) -> bool:
        return self.kind in ENABLED_KINDS


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
    sets = dc.status_sets(game_cfg.get("decomp", {}))
    oracle_cols = dc.oracle_columns(game_cfg.get("decomp", {}))
    header = None
    out: list[Function] = []
    seen: dict[str, int] = {}
    errors: list[str] = []
    # Oracle builds only (tools/build.py --decomp-enable): disabled rows built as if they were unverified.
    forced = set(game_cfg.get("_decomp_enable", ()))
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
        name, status = row["name"], dc.normalize_status(row["status"])
        kind = dc.status_kind(status, sets)
        if kind == "listed" and name in forced:
            kind = "replaced"  # tools/build.py --decomp-enable (oracle builds only)
        if not IDENT.match(name):
            errors.append(f"{path}:{n}: name {name!r} is not a C identifier")
        if name in seen:
            errors.append(f"{path}:{n}: {name} already listed line {seen[name]}")
        seen[name] = n
        if kind is None:
            errors.append(f"{path}:{n}: status {row['status']!r} is not a recognized status (expected one of "
                          f"{', '.join(dc.all_statuses(sets))}: [decomp] done_statuses, replaced_statuses, "
                          f"listed_statuses; compared whole, case-insensitive)")
            kind = "listed"
        oracle: dict[str, bool] = {}
        for version, column in oracle_cols.items():
            try:
                oracle[version] = dc.parse_oracle(row.get(column, ""))
            except ValueError:
                errors.append(f"{path}:{n}: {column} {row[column]!r} must be yes, no or - (yes = the oracle has "
                              f"passed on {version})")
                oracle[version] = False
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
        if kind in ENABLED_KINDS:
            absent = [v for v, a in addresses.items() if a is None]
            if absent:
                errors.append(f"{path}:{n}: {name} is {status} but has no address for {', '.join(absent)}: a "
                              f"function without an address in every version cannot be enabled (status disabled)")
        out.append(Function(name, row["module"].strip(), addresses, size, status, n, oracle, kind))
    unknown = sorted(forced - set(seen))
    if unknown:
        errors.append(f"--decomp-enable: {', '.join(unknown)} not in {path.name}")
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


@dataclass
class Known:
    """Function starts of one version from its symbol sources."""
    sizes: dict[tuple[str, int], dict[str, int | None]]  # (module, address or REL offset) -> {source: size or None}
    gaps: dict[int, dc.Gap]  # DOL address in the version -> row of <ID>-gaps.tsv (non-reference versions)
    sources: list[str]
    ref: str = ""
    trans: dict[int, dc.Transposed] = field(default_factory=dict)  # reference address -> report row (other versions)
    gaps_by_ref: dict[int, dc.Gap] = field(default_factory=dict)  # reference address -> gap row (other versions)
    trans_name: str = ""
    rel_names: dict[str, dict[tuple[str, int], str]] = field(default_factory=dict)  # version -> (module, off) -> name


def _rel_names(path: Path) -> dict[tuple[str, int], str]:
    """<ID>-rel.tsv: (module, offset) -> name."""
    out: dict[tuple[str, int], str] = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8").splitlines():
            p = line.split("\t")
            if line.startswith("#") or len(p) < 6:
                continue
            try:
                out[(p[1], int(p[3], 16))] = p[5].strip()
            except ValueError:
                pass
    return out


def _map_entries(path: Path) -> dict[tuple[str, int], int]:
    """<ID>.map ("ADDRESS SIZE NAME") and <ID>-rel.tsv ("module_id module section offset size name"), hexadecimal:
    (module, address or offset) -> size (0 = unknown)."""
    out: dict[tuple[str, int], int] = {}
    if not path.exists():
        return out
    rel = path.suffix == ".tsv"
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        p = line.split("\t") if rel else line.split()
        try:
            if rel and len(p) >= 6:
                out[(p[1], int(p[3], 16))] = int(p[4], 16)
            elif not rel and len(p) == 3:
                out[("dol", int(p[0], 16))] = int(p[1], 16)
        except ValueError:
            pass
    return out


def known_entries(game_cfg: dict, version: str) -> Known:
    """Every known function start of `version`, with the size each source gives there (None = unknown)."""
    import symbols as sy  # noqa: E402  (generic tool of the repository)
    sym = game_cfg.get("symbols", {})
    d = game_cfg.get("decomp", {})
    slug = game_cfg["_dir"].name
    ref = d.get("reference") or sym.get("reference")
    map_dir = game_cfg["_dir"] / sym.get("map_dir", "symbols")
    sizes: dict[tuple[str, int], dict[str, int | None]] = {}
    used: list[str] = []

    def add(key: tuple[str, int], source: str, size: int | None) -> None:
        sizes.setdefault(key, {})[source] = size

    others = [v["game_id"] for v in game_cfg.get("versions", []) if v["game_id"] != ref]

    def report(vid: str) -> tuple[Path, dict[int, dc.Transposed]]:
        path = dc.transposition_path(game_cfg, tc.ROOT, vid)
        shown = path.relative_to(tc.ROOT).as_posix() if path.is_relative_to(tc.ROOT) else str(path)
        if not path.exists():
            raise SystemExit(f"Decompiled functions refused for {version}: transposition report {shown} missing (a "
                             f"file in Git, next to the symbol maps: restore it with `git checkout -- {shown}`, or "
                             f"write it again with `py -3 tools/symbols.py {slug} transpose --to {vid}`, which needs "
                             f"the dtk symbols and the Ghidra analysis of {vid}); the {version} addresses cannot be "
                             f"established without it")
        return path, dc.read_transposition(path)

    trans: dict[int, dc.Transposed] = {}
    trans_name = ""
    if version == ref and others:
        # Every reference function with its size: the reference columns of the transposition reports (written from
        # the dtk symbols by tools/symbols.py transpose); two reports that disagree refuse the size (rule (b)).
        for vid in others:
            path, rows = report(vid)
            for t in rows.values():
                add(("dol", t.ref), path.name, t.ref_size)
            used.append(path.name)
    elif version == ref:
        dtk = tc.ROOT / sym["dtk_dir"] / "symbols.txt" if sym.get("dtk_dir") else None
        if dtk is None or not dtk.exists():
            raise SystemExit(f"Decompiled functions refused for {version}: dtk symbols of the reference version "
                             f"missing ({dtk or '[symbols] dtk_dir'}): its addresses cannot be established")
        for s in sy.functions(sy.read_dtk(dtk)):
            add(("dol", s.addr), "dtk", s.size)
        used.append("dtk symbols")
    else:
        path, trans = report(version)
        trans_name = path.name
        for t in trans.values():
            if t.target:  # size of the reference: the version's size only for an identical match
                add(("dol", t.target), path.name, t.ref_size if t.identical else None)
        used.append(path.name)
    for key, size in _map_entries(map_dir / f"{version}.map").items():
        add(key, f"{version}.map", size or None)
    for key, size in _map_entries(map_dir / f"{version}-rel.tsv").items():
        add(key, f"{version}-rel.tsv", size or None)
    used += [f"{version}.map", f"{version}-rel.tsv"]
    gaps: dict[int, dc.Gap] = {}
    gaps_by_ref: dict[int, dc.Gap] = {}
    gpath = dc.gaps_path(game_cfg, version)
    for g in dc.read_gaps(gpath):
        if g.target:
            gaps[g.target] = g
            gaps_by_ref[g.ref] = g
            add(("dol", g.target), gpath.name, g.size or None)
    if gpath.exists():
        used.append(gpath.name)
    rel_names = {v: _rel_names(map_dir / f"{v}-rel.tsv") for v in {ref, version}}
    return Known(sizes, gaps, used, ref, trans, gaps_by_ref, trans_name, rel_names)


def check_sources(game_cfg: dict, version: str, f: Function, addr: int, known: Known) -> str | None:
    """Activation lock: reason why `addr` (DOL address or REL offset) of `f` is not established in `version`'s symbol
    sources with the row's size, None if it is (rules (a), (b), (c) of the header)."""
    gname = dc.gaps_path(game_cfg, version).name
    gap = known.gaps.get(addr) if f.module == "dol" else None
    size: int | None
    if gap is not None:
        if gap.level in dc.ORACLE_LEVELS:
            # An oracle build (tools/build.py --oracle) may enable it: that executable only compares the function
            # with its original and keeps the original's results; the mark is set by tools/decomp_oracle.py --promote.
            if not f.oracle.get(version) and not game_cfg.get("_oracle_build"):
                col = dc.oracle_columns(game_cfg.get("decomp", {})).get(version)
                return (f"{version} address placed by {gname} at level {gap.level}: enabled only once the oracle has "
                        f"passed on {version} (column {col} = yes)" if col else
                        f"{version} address placed by {gname} at level {gap.level}: enabled only once the oracle has "
                        f"passed on {version} ([decomp.oracle_columns] has no column for {version})")
        elif gap.level not in dc.ESTABLISHED_LEVELS:
            return f"{version} address placed by {gname} at level {gap.level}: not established"
        size = gap.size or None
        if size is None:
            return f"no {version} size in {gname}"
    else:
        found = known.sizes.get((f.module, addr))
        if found is None:
            return (f"not a known function start of {version} (sources: {', '.join(known.sources)}): establish it "
                    f"through tools/symbols.py first")
        given = {s for s in found.values() if s is not None}
        if not given:
            return f"size not established in {version} (known there with an unknown size: {', '.join(found)})"
        if len(given) > 1:
            return (f"the {version} sources disagree on its size ("
                    f"{', '.join(f'{k} 0x{v:X}' for k, v in found.items() if v is not None)})")
        size = given.pop()
    inside = sorted(a for (m, a) in known.sizes if m == f.module and addr < a < addr + f.size)
    if inside:
        return (f"range 0x{addr:X}+0x{f.size:X} holds another known {version} function start "
                f"({', '.join(f'{a:X}' for a in inside[:4])})")
    if f.size != size:
        return (f"size 0x{f.size:X} differs from the {version} size 0x{size:X} (one size column: a function whose "
                f"size differs between versions cannot be enabled)")
    return check_pairing(game_cfg, version, f, addr, known)


def check_pairing(game_cfg: dict, version: str, f: Function, addr: int, known: Known) -> str | None:
    """Rule (d): reason why `addr` is not, in `version`, the same function as the row's reference address."""
    ref = known.ref
    if version == ref:
        return None
    us = f.addresses.get(ref)
    if us is None:
        return f"no {ref} address: the {version} address cannot be paired with a {ref} function"
    if f.module != "dol":
        import symbols as sy  # noqa: E402  (automatic names, as tools/symbols.py writes them)
        a = known.rel_names.get(ref, {}).get((f.module, us))
        b = known.rel_names.get(version, {}).get((f.module, addr))
        if not a or not b or a != b or sy.AUTO_RE.match(a):
            return (f"{version} offset {addr:X} of {f.module} not paired with {ref} offset {us:X}: {ref}-rel.tsv and "
                    f"{version}-rel.tsv must give both the same non-automatic name (found {a or 'none'} and "
                    f"{b or 'none'}; the build does not run the REL matcher)")
        return None
    want, how = dc.paired_address(us, known.trans, known.gaps_by_ref)
    source = dc.gaps_path(game_cfg, version).name if how.startswith("gap-") else known.trans_name
    if want is None:
        gname = dc.gaps_path(game_cfg, version).name
        return (f"{ref} {us:08X} has no {version} address in {known.trans_name} nor in {gname} ({how}): the two "
                f"columns cannot be shown to name the same function")
    if want != addr:
        return (f"not the same function as {ref} {us:08X}: {source} gives that function the {version} address "
                f"{want:08X} ({how}), not {addr:08X}")
    return None


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
    known = known_entries(game_cfg, version) if any(f.enabled for f in functions) else None
    for f in functions:
        if not f.enabled:
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
        reason = check_sources(game_cfg, version, f, addr, known) or check_code(ranges, guest, f.size)
        if reason:
            errors.append(f"{f.name} ({version} {f.module} {addr:08X}, size 0x{f.size:X}): {reason}")
        out.append((guest, f))
    addresses = [a for a, _ in out]
    for a in sorted({a for a in addresses if addresses.count(a) > 1}):
        errors.append(f"two functions at {a:08X}: {', '.join(f.name for b, f in out if b == a)}")
    if errors:
        raise SystemExit(f"Decompiled functions refused for {version}:\n  " + "\n  ".join(errors))
    return sorted(out, key=lambda e: e[0])


def registry_source(version: str, entries: list[tuple[int, Function]], oracle: bool = False,
                    records: dict[str, str] | None = None) -> str:
    """C++ registration table of `version` (build folder only: it holds addresses). `oracle`: oracle build
    (tools/build.py --oracle), the table also gives the oracle each function's size; `records`: export record to use
    instead of a function's own (negative test of the oracle: name -> record name)."""
    records = records or {}
    lines = [f"// Registration table of the decompiled functions for {version}: generated by tools/decomp_functions.py",
             "// (tools/build.py) from the game's functions table. Build folder only, never in Git.",
             '#include "decomp/gc_abi.h"', '#include "decomp/gc_abi_c.h"', ""]
    for _, f in entries:
        lines.append(f"extern const ::gc::abi::export_record gc_decomp_export_{records.get(f.name, f.name)};")
    if oracle:
        lines += ["", 'extern "C" void gcrt_decomp_oracle_configure(const char* version, const unsigned* addresses, '
                      "const unsigned* sizes, const char* const* names, unsigned count);"]
    lines += ["", "namespace {"]
    if entries:
        lines.append("const ::gc::abi::registration k_table[] = {")
        for a, f in entries:
            lines.append(f"    {{0x{a:08X}u, &gc_decomp_export_{records.get(f.name, f.name)}}},")
        lines += ["};", "constexpr unsigned k_count = sizeof(k_table) / sizeof(k_table[0]);"]
        if oracle:
            lines.append("const unsigned k_addresses[] = {" + ", ".join(f"0x{a:08X}u" for a, _ in entries) + "};")
            lines.append("const unsigned k_sizes[] = {" + ", ".join(f"0x{f.size:X}u" for _, f in entries) + "};")
            lines.append("const char* const k_names[] = {" + ", ".join(f'"{f.name}"' for _, f in entries) + "};")
    else:
        lines += ["const ::gc::abi::registration* const k_table = nullptr;", "constexpr unsigned k_count = 0;"]
    lines += ["} // namespace", "",
              'extern "C" void gcrt_decomp_install_generated(struct GcrtCpu* g) {']
    if oracle and entries:
        lines.append(f'    gcrt_decomp_oracle_configure("{version}", k_addresses, k_sizes, k_names, k_count);')
    lines += ["    gcrt_decomp_install_table(g, k_table, k_count);", "}", ""]
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
          f"{sum(f.enabled for f in functions)} enabled")
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
