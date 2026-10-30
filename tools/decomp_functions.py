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


def guest_cost_path(game_cfg: dict) -> Path | None:
    """Guest cost table of [decomp] guest_cost (relative to games/<game>/), None if the key is missing."""
    rel = game_cfg.get("decomp", {}).get("guest_cost")
    return game_cfg["_dir"] / rel if rel else None


GUEST_COST_FRACTION = 256  # registration::guest_cost_256ths: the gateway charges 1/256 cycle units (gc_abi.h)
GUEST_COST_CELL = re.compile(r"^(\d+)(?:\.(\d{1,4}))?$")


def guest_cost_256ths(cell: str) -> int:
    """1/256-cycle units of a cell of the guest cost table: a whole number of cycles (a constant cost) or a mean with at
    most 4 decimals ("6.6049"), rounded half up to the nearest 1/256 cycle (exact arithmetic, no float). The gateway
    carries the remainder from call to call, so N calls charge N x the mean to within 1/512 cycle per call."""
    m = GUEST_COST_CELL.match(cell)
    if not m:
        raise ValueError(f"{cell!r}: number of cycles expected (digits, at most 4 decimals)")
    decimals = m.group(2) or ""
    scale = 10 ** len(decimals)
    value = int(m.group(1)) * scale + (int(decimals) if decimals else 0)  # cycles x scale
    return (2 * value * GUEST_COST_FRACTION + scale) // (2 * scale)


def format_guest_cost(cycles: int, calls: int) -> str:
    """Cell of the guest cost table for a mean of `cycles` / `calls`: the whole number when it is exact, otherwise the
    mean rounded half up to 4 decimals (guest_cost_256ths reads it back)."""
    if calls <= 0:
        raise ValueError("no call")
    if cycles % calls == 0:
        return str(cycles // calls)
    q = (2 * cycles * 10000 + calls) // (2 * calls)  # mean x 10^4, rounded half up
    return f"{q // 10000}.{q % 10000:04d}"


def read_guest_costs(game_cfg: dict) -> dict[str, dict[str, int]]:
    """{function name: {version ID: guest cost per call, in 1/256 cycle}} of the guest cost table (tools/decomp_oracle.py
    cost writes it): what the gateway charges to the cycle clock for each call of the decompiled function, the mean
    exclusive cost of its recompiled original measured by the oracle. Missing table, row or cell ("-"): no cost for
    that function and version (tools/build.py refuses a production build that enables it)."""
    path = guest_cost_path(game_cfg)
    if not path or not path.exists():
        return {}
    out: dict[str, dict[str, int]] = {}
    header: list[str] | None = None
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cells = line.split("\t")
        if header is None:
            header = cells
            if header[0] != "name":
                raise SystemExit(f"{path}:{n}: header 'name<TAB><version>...' expected")
            continue
        if len(cells) != len(header):
            raise SystemExit(f"{path}:{n}: {len(header)} columns expected")
        row: dict[str, int] = {}
        for version, cell in zip(header[1:], cells[1:]):
            if cell not in ("", "-"):
                try:
                    row[version] = guest_cost_256ths(cell)
                except ValueError as e:
                    raise SystemExit(f"{path}:{n}: {e}")
        if cells[0] in out:
            raise SystemExit(f"{path}:{n}: {cells[0]} listed twice")
        out[cells[0]] = row
    return out


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


def _locate(f: Function, addr: int, dol, rels: dict) -> tuple[list[tuple[int, bytes]], int] | str:
    """(code ranges of the function's module, guest address of `f`), or the reason it cannot be located."""
    if f.module == "dol":
        return dol.ranges, addr
    if f.module in rels:
        code = [(a, blob) for (_, a, _, ex, blob) in rels[f.module]["sections"] if ex]
        if not code:
            return f"{f.name}: module {f.module} has no code section"
        return code, code[0][0] + addr  # offset in the first code section, as in the dtk symbols
    return f"{f.name}: unknown module {f.module!r} (dol, or one of {', '.join(sorted(rels))})"


def code_blocks(ranges: list[tuple[int, bytes]], address: int, size: int) -> tuple[list[int], list[int]]:
    """Basic blocks of the function [address, address + size) of `ranges`, from its own code: (start addresses of
    the blocks reachable from the entry, start addresses of the blocks no path from the entry reaches). A block
    starts at the entry, at every branch target inside the function and after every branch. Edges: a conditional
    branch goes to its target and to the next instruction; `b` (without link) to its target; blr, bctr, rfi and the
    always-taken forms of bclr/bcctr end the path; a call (link bit) goes on to the next instruction; a branch out of
    the function ends the path. BO & 0x14 == 0x14 is "branch always" (PowerPC UISA, bc: neither CTR nor condition
    tested)."""
    blob = base = None
    for b, data in ranges:
        if b <= address < b + len(data):
            base, blob = b, data
            break
    if blob is None:
        return [address], []
    n = size // 4
    words = [_word(blob, address - base + 4 * i) for i in range(n)]
    leaders = {0}
    succ: list[list[int]] = []
    for i, w in enumerate(words):
        op, lk = w >> 26, w & 1
        nxt = [i + 1] if i + 1 < n else []
        if op in (16, 18):
            if op == 18:
                off = w & 0x03FFFFFC
                off -= 0x04000000 if off & 0x02000000 else 0
            else:
                off = w & 0xFFFC
                off -= 0x10000 if off & 0x8000 else 0
            t = off if (w & 2) else address + 4 * i + off
            target = (t - address) // 4 if address <= t < address + size and (t - address) % 4 == 0 else None
            always = op == 18 or ((w >> 21) & 0x14) == 0x14
            edges = [target] if target is not None else []
            if lk:
                edges = nxt  # a call comes back to the next instruction
            elif not always:
                edges = edges + nxt
            succ.append(edges)
            if target is not None:
                leaders.add(target)
            if i + 1 < n:
                leaders.add(i + 1)
        elif op == 19 and ((w >> 1) & 0x3FF) in (16, 50, 528):  # bclr, rfi, bcctr
            always = ((w >> 1) & 0x3FF) == 50 or ((w >> 21) & 0x14) == 0x14
            succ.append(nxt if (lk or not always) else [])
            if i + 1 < n:
                leaders.add(i + 1)
        else:
            succ.append(nxt)
    seen = {0}
    todo = [0]
    while todo:
        for j in succ[todo.pop()]:
            if j not in seen:
                seen.add(j)
                todo.append(j)
    reachable = sorted(address + 4 * i for i in leaders if i in seen)
    dead = sorted(address + 4 * i for i in leaders if i not in seen)
    return reachable, dead


def _branch_target(word: int, at: int) -> int:
    """Target of the relative or absolute branch `word` (b/bl or bc/bcl) found at `at`."""
    if word >> 26 == 18:
        off = word & 0x03FFFFFC
        off -= 0x04000000 if off & 0x02000000 else 0
    else:
        off = word & 0xFFFC
        off -= 0x10000 if off & 0x8000 else 0
    return (off & 0xFFFFFFFF) if (word & 2) else (at + off) & 0xFFFFFFFF


def _code_of(ranges: list[tuple[int, bytes]], address: int) -> tuple[int, bytes] | tuple[None, None]:
    for b, data in ranges:
        if b <= address < b + len(data):
            return b, data
    return None, None


def call_sites(ranges: list[tuple[int, bytes]], address: int, size: int) -> list[int]:
    """Addresses of the instructions of [address, address + size) that leave the function for another one: calls
    (bl, bcl, bclrl, bcctrl) and tail branches out of the function (b, and bc without link, whose target is outside
    it; bcctr). The oracle build compiles a hook at each one (runtime/core/decomp/gc_oracle.cpp: the call is recorded,
    its callee run once, and replayed to the decompiled version). blr and bclr (returns) are not calls."""
    base, blob = _code_of(ranges, address)
    if blob is None:
        return []
    out = []
    for i in range(size // 4):
        at = address + 4 * i
        w = _word(blob, at - base)
        op, lk, xo = w >> 26, w & 1, (w >> 1) & 0x3FF
        if op in (16, 18):
            t = _branch_target(w, at)
            if lk or not (address <= t < address + size):
                out.append(at)
        elif op == 19 and ((xo == 16 and lk) or xo == 528):
            out.append(at)
    return out


def assertion_blocks(ranges: list[tuple[int, bytes]], address: int, size: int, no_return: set[int]) -> list[int]:
    """Reachable basic blocks (starts from code_blocks) that only lead to a function that never returns (`no_return`:
    guest addresses in this version, [decomp] no_return of game.toml): a block whose last instruction is an
    unconditional call (bl) to one of them, or whose successors all are such blocks (fixed point). The oracle report
    lists them apart (tools/decomp_oracle.py): no scenario reaches them without stopping the game, so synthetic cases,
    whose calls the oracle simulates, cover them."""
    reach, _ = code_blocks(ranges, address, size)
    base, blob = _code_of(ranges, address)
    if blob is None or not no_return:
        return []
    starts = sorted(reach)
    end = address + size
    leaders = set(starts)
    # A block runs up to the next leader of the whole function (reachable or not).
    every, dead = code_blocks(ranges, address, size)
    bounds = sorted(set(every) | set(dead))
    end_of = {s: next((b for b in bounds if b > s), end) for s in starts}

    def block_of(at: int) -> int | None:
        best = None
        for s in starts:
            if s <= at:
                best = s
        return best

    succ: dict[int, list[int]] = {}
    panic: set[int] = set()
    for s in starts:
        last = end_of[s] - 4
        w = _word(blob, last - base)
        op, lk, xo = w >> 26, w & 1, (w >> 1) & 0x3FF
        nxt = [end_of[s]] if end_of[s] < end and end_of[s] in leaders else []
        if op in (16, 18):
            t = _branch_target(w, last)
            inside = address <= t < end
            always = op == 18 or ((w >> 21) & 0x14) == 0x14
            if lk:
                if t in no_return and always:
                    panic.add(s)
                    succ[s] = []
                else:
                    succ[s] = nxt
            else:
                succ[s] = ([t] if inside else []) + ([] if always else nxt)
        elif op == 19 and xo in (16, 50, 528):
            always = xo == 50 or ((w >> 21) & 0x14) == 0x14
            succ[s] = nxt if (lk or not always) else []
        else:
            succ[s] = nxt
    found = set(panic)
    changed = True
    while changed:
        changed = False
        for s in starts:
            if s in found or not succ[s]:
                continue
            if all(block_of(t) in found for t in succ[s]):
                found.add(s)
                changed = True
    return sorted(found)


def no_return_addresses(game_cfg: dict, version: str) -> set[int]:
    """[decomp.no_return] of game.toml, {name: {version: "address"}}: the guest addresses, in `version`, of the
    functions that never return (a failed assertion, a panic)."""
    out = set()
    for per_version in (game_cfg.get("decomp", {}).get("no_return") or {}).values():
        a = per_version.get(version) if isinstance(per_version, dict) else None
        if a:
            out.add(int(str(a), 16))
    return out


# Floating-point instructions as DolRecomp guards them (src/frontend/decoder.h ppc_op_uses_fpu: every opcode from
# PPC_OP_STFIWX to PPC_OP_PS_SEL; src/backend/emitter.c emits `if (!ppc_fp_available_inline(ctx, cia)) return;` before
# each one, the "FPU unavailable" exception when MSR[FP] = 0). Primary opcodes: 48-55 lfs, lfsu, lfd, lfdu, stfs,
# stfsu, stfd, stfdu; 56, 57 psq_l, psq_lu; 60, 61 psq_st, psq_stu; 59 single-precision arithmetic; 63 double-precision
# arithmetic, compares and the FPSCR instructions; 4 paired singles and psq_lx/stx/lux/stux, except extended opcode
# 1014 (dcbz_l); 31 with the extended opcodes of lfsx, lfsux, lfdx, lfdux, stfsx, stfsux, stfdx, stfdux and stfiwx.
# This is a superset: an encoding of these opcodes that DolRecomp's decoder rejects (PPC_OP_UNKNOWN, not guarded) is
# counted too, which only makes the gateway decline more often (tests/decomp: checked against the guards of the whole
# generated code of both versions).
FPU_PRIMARY = frozenset({48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 59, 60, 61, 63})
FPU_X31 = frozenset({535, 567, 599, 631, 663, 695, 727, 759, 983})


def is_fpu_instruction(word: int) -> bool:
    """True if `word` is a floating-point instruction (one that raises "FPU unavailable" when MSR[FP] = 0)."""
    op, xo = word >> 26, (word >> 1) & 0x3FF
    if op in FPU_PRIMARY:
        return True
    if op == 4:
        return xo != 1014
    return op == 31 and xo in FPU_X31


def fpu_instructions(ranges: list[tuple[int, bytes]], address: int, size: int) -> list[int]:
    """Addresses of the floating-point instructions (is_fpu_instruction) of [address, address + size) of `ranges`."""
    for base, blob in ranges:
        if base <= address < base + len(blob):
            return [address + 4 * i for i in range(size // 4)
                    if is_fpu_instruction(_word(blob, address - base + 4 * i))]
    return []


# Files outside the decompiled sources that decide what an oracle run measures, relative to the repository root
# (folders: every file below them): the basic-block analysis and the table checks (this file, decomp_common.py, the
# globals table checks and generation decomp_globals.py, the
# code reader symbols.py), the generation of the block hooks and of the registration table (build.py), the
# DolRecomp patch that compiles those hooks, the chassis that runs the original (runtime/core/cpu), the native OS
# whose idle detection detaches a write journal during the original (runtime/core/hle/native_os.c), and the CMake
# files that compile the oracle in (runtime/core/cmake/GcrtDecomp.cmake, the root CMakeLists.txt). The DolRecomp
# checkout itself (commit and local changes, toolchain.git_head) is hashed too: it generates the code of the original.
ORACLE_HASHED_TOOLS = ["tools/decomp_functions.py", "tools/decomp_common.py", "tools/decomp_globals.py",
                       "tools/symbols.py", "tools/build.py",
                       "tools/patches/dolrecomp", "runtime/core/cpu", "runtime/core/hle/native_os.c",
                       "runtime/core/cmake/GcrtDecomp.cmake", "CMakeLists.txt"]


THREAD_SWITCHING_NATIVES = tc.ROOT / "runtime" / "core" / "hle" / "thread_switching_natives.txt"


def thread_switching_natives(path: Path = THREAD_SWITCHING_NATIVES) -> set[str]:
    """Names of the native replacements that can switch threads (runtime/core/hle/thread_switching_natives.txt)."""
    out = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            out.add(line)
    return out


def _local_scan(ranges: list[tuple[int, bytes]], entry: int, rel_bases: set[int]) -> tuple[set[int], int, list[int]]:
    """Control flow of the code at `entry`, followed through its conditional and unconditional branches until every
    path ends (blr, bctr, rfi, or an address outside the code): (targets of its direct calls and of its jumps that
    may be tail calls, number of indirect calls, addresses of calls whose target is not resolved). A `b` is followed
    as a jump and also reported as a callee (a tail call is a call). In a REL module (`rel_bases`: start of its code
    sections) a branch whose displacement is 0 is an unapplied relocation: its target is not known here."""
    seen: set[int] = set()
    todo = [entry]
    callees: set[int] = set()
    indirect = 0
    unresolved: list[int] = []
    while todo:
        at = todo.pop()
        while at not in seen:
            base, blob = _code_of(ranges, at)
            if blob is None or at - base + 4 > len(blob):
                break
            seen.add(at)
            w = _word(blob, at - base)
            op, lk = w >> 26, w & 1
            if op == 18:
                if (w & 0x03FFFFFC) == 0 and not (w & 2) and base in rel_bases:
                    unresolved.append(at)
                    if not lk:
                        break
                    at += 4
                    continue
                t = _branch_target(w, at)
                callees.add(t)
                if lk:
                    at += 4
                    continue
                todo.append(t)  # a jump inside the function, or a tail call (also a callee above)
                break
            if op == 16:
                t = _branch_target(w, at)
                bo = (w >> 21) & 0x1F
                if lk:
                    callees.add(t)
                else:
                    todo.append(t)
                if not lk and (bo & 0x14) == 0x14:  # branch always
                    break
                at += 4
                continue
            if op == 19:
                xo = (w >> 1) & 0x3FF
                bo = (w >> 21) & 0x1F
                if xo in (16, 528):  # bclr, bcctr
                    if lk:
                        indirect += 1  # bctrl, bclrl: indirect call
                        at += 4
                        continue
                    if xo == 528:
                        indirect += 1  # bctr: jump table or indirect tail call, its targets are not followed
                    if (bo & 0x14) == 0x14:
                        break
                    at += 4
                    continue
                if xo == 50:  # rfi
                    break
            at += 4
    return callees, indirect, unresolved


def switching_reach(ranges: list[tuple[int, bytes]], entries: list[int], hooks: dict[int, str],
                    switching: set[str], rel_bases: set[int] | None = None) -> dict[int, dict]:
    """For each entry, the static closure of its direct calls (anomaly A17): {entry: {"path": [address, ...,
    native name] to a thread-switching native or None, "indirect": indirect calls met, "unresolved": calls whose target
    is unknown}}. A hooked address whose name is a native replacement (any name of `hooks` except "decomp", "game",
    "oracle-block", "oracle-call", "extra") is a leaf: its guest body does not run; a decompiled function is followed
    through its original (functional decompilation: the same calls). Indirect calls (bctrl, bclrl) and jump tables
    are not followed: the closure is a lower bound."""
    rel_bases = rel_bases or set()
    followed = ("decomp", "game", "oracle-block", "oracle-call", "extra")
    cache: dict[int, tuple[set[int], int, list[int]]] = {}
    out: dict[int, dict] = {}
    for entry in entries:
        parent: dict[int, int | None] = {entry: None}
        queue = [entry]
        found = None
        indirect = 0
        unresolved: list[int] = []
        while queue and found is None:
            f = queue.pop(0)
            name = hooks.get(f)
            if f != entry and name is not None and name not in followed:
                if name in switching:
                    found = f
                continue  # native: its guest body does not run
            if f not in cache:
                cache[f] = _local_scan(ranges, f, rel_bases)
            callees, ind, unres = cache[f]
            indirect += ind
            unresolved += unres
            for c in sorted(callees):
                if c not in parent:
                    parent[c] = f
                    queue.append(c)
        path = None
        if found is not None:
            path = [hooks[found]]
            node = parent[found]
            while node is not None:
                path.insert(0, f"{node:08X}")
                node = parent[node]
        out[entry] = {"path": path, "indirect": indirect, "unresolved": unresolved}
    return out


def files_below(root: Path) -> list[Path]:
    """Every file below `root`, sorted by its relative path as a case-sensitive string (Path ordering is
    case-insensitive on Windows only: sorting by the string gives the same order, so the same hash, everywhere)."""
    return sorted((p for p in Path(root).rglob("*") if p.is_file()), key=lambda p: p.relative_to(root).as_posix())


def dolrecomp_state() -> str:
    """Commit and local changes of the DolRecomp checkout (external/DolRecomp), "?" if there is none."""
    return tc.git_head(tc.EXTERNAL / "DolRecomp")


def oracle_hash_inputs(game_cfg: dict, negative: Path | None = None) -> list[tuple[str, Path]]:
    """(name, file) of every file oracle_sources_hash reads, in hash order."""
    roots = [game_cfg["_dir"] / "decomp" / "src", tc.ROOT / "runtime" / "core" / "decomp"]
    tests = game_cfg["_dir"] / "decomp" / "oracle_tests"
    if tests.is_dir():
        roots.append(tests)  # oracle test functions (rows enabled only by --decomp-enable of an oracle build)
    if negative:
        roots.append(Path(negative))
    out: list[tuple[str, Path]] = []
    for root in roots:
        out += [(p.relative_to(root).as_posix(), p) for p in files_below(root)]
    for rel in ORACLE_HASHED_TOOLS:
        path = tc.ROOT / rel
        if path.is_dir():
            out += [(p.relative_to(tc.ROOT).as_posix(), p) for p in files_below(path)]
        elif path.is_file():
            out.append((rel, path))
        else:
            raise SystemExit(f"oracle_sources_hash: {rel} not found (ORACLE_HASHED_TOOLS, tools/decomp_functions.py)")
    return out


def oracle_sources_hash(game_cfg: dict, negative: Path | None = None) -> str:
    """SHA-1 of what an oracle verdict is about: the decompiled sources (games/<game>/decomp/src, every file), the
    oracle and gateway code (runtime/core/decomp), the wrong variants of a negative build, the tools and runtime
    files that decide the block analysis, the block hooks and how the original runs (ORACLE_HASHED_TOOLS), the state
    of the DolRecomp checkout (dolrecomp_state), the globals table ([decomp] globals, tools/decomp_globals.py),
    and the name, module, addresses and size of every row of the functions table (not its status columns, which the
    promotion writes).
    Line ends are normalized (a checkout with CRLF gives the same hash). tools/build.py --oracle compiles it into the
    oracle executable (header of its jsonl files); tools/decomp_oracle.py report --promote refuses results whose hash
    is not the current one."""
    import hashlib
    h = hashlib.sha1()
    for name, path in oracle_hash_inputs(game_cfg, negative):
        h.update(name.encode() + b"\0")
        h.update(path.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    h.update(b"dolrecomp " + dolrecomp_state().encode() + b"\n")
    # Functions that never return ([decomp.no_return]): they decide the assertion blocks of the report.
    for name, per_version in sorted((game_cfg.get("decomp", {}).get("no_return") or {}).items()):
        h.update(f"no_return {name} {sorted((per_version or {}).items())}\n".encode())
    # The globals table (tools/decomp_globals.py): the addresses the decompiled code reads and writes.
    import decomp_globals  # noqa: E402
    gpath = decomp_globals.table_path(game_cfg)
    if gpath is not None and gpath.exists():
        h.update(b"globals\0" + gpath.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    _, columns = config(game_cfg)
    for f in read_table(game_cfg):
        h.update("\t".join([f.name, f.module, *(f"{f.addresses.get(v) or 0:08X}" for v in sorted(columns)),
                             f"{f.size:X}"]).encode() + b"\n")
    return h.hexdigest()


FORCED_SECTIONS = (("run", "force"), ("stage", "start"))


def forced_entries(game_cfg: dict, version: str) -> dict[int, list[str]]:
    """{function entry: [where]} of the GCRT_FORCE_ARG items that tools/run.py passes for `version` (game.toml
    [run.force]: fixed random seed of every scenario; [stage.start]: --stage). Item syntax (runtime/core/hle/os_report.c):
    "ADDRESS[@RETURN]:TARGET...", ADDRESS hexadecimal. The runtime installs these items before the game's replacements
    (runtime/core/boot/boot.c: gcrt_hle_install_defaults, then game->install) and gcrt_hle_register refuses a second
    function at one address: a decompiled function at a forced entry would be refused at startup, and the gateway
    stops the executable (runtime/core/decomp/gc_abi.cpp, install)."""
    out: dict[int, list[str]] = {}
    for section, key in FORCED_SECTIONS:
        for item in (game_cfg.get(section, {}).get(key, {}) or {}).get(version, []) or []:
            head = str(item).split(":", 1)[0].split("@", 1)[0].strip()
            try:
                address = int(head, 16)
            except ValueError:
                raise SystemExit(f"game.toml [{section}.{key}] {version}: item {item!r} does not start with a "
                                 "hexadecimal function address (GCRT_FORCE_ARG syntax ADDRESS[@RETURN]:TARGET=VALUE)")
            out.setdefault(address, []).append(f'[{section}.{key}] "{item}"')
    return out


def resolve(game_cfg: dict, version: str, disc_root: Path, layout: list[dict],
            blocks: dict[int, tuple[list[int], list[int]]] | None = None,
            fpu: dict[int, list[int]] | None = None, calls: dict[int, list[int]] | None = None,
            asserts: dict[int, list[int]] | None = None,
            words: dict[int, list[int]] | None = None) -> list[tuple[int, Function]]:
    """Enabled functions of `version` with their guest address, after the code checks. `layout`: tools/build.py
    rel_layout (virtual addresses of the RELs). `blocks`: if given, filled with {guest address: code_blocks()}.
    `fpu`: if given, filled with {guest address: fpu_instructions()} (empty list: no floating-point instruction).
    `calls`, `asserts`: if given, filled with {guest address: call_sites()} and {guest address: assertion_blocks()}
    (oracle builds). `words`: if given, filled with {guest address: instruction words of the original} (graph of its
    paths, tools/decomp_path_cost.py)."""
    import symbols as sy  # noqa: E402  (generic tool of the repository)
    functions = read_table(game_cfg)
    if not functions:
        return []
    dol = sy.Image.from_dol(disc_root / game_cfg["code"]["dol"])
    rels = {m["name"]: m for m in layout}
    out: list[tuple[int, Function]] = []
    errors: list[str] = []
    known = known_entries(game_cfg, version) if any(f.enabled for f in functions) else None
    forced = forced_entries(game_cfg, version)
    for f in functions:
        if not f.enabled:
            continue
        addr = f.addresses[version]
        assert addr is not None  # read_table refuses an enabled function with an absent address
        located = _locate(f, addr, dol, rels)
        # Guest address of the entry (a REL row gives an offset in its module); a DOL row whose code check fails is
        # still checked here, the clash depends only on the address.
        entry = located[1] if not isinstance(located, str) else (addr if f.module == "dol" else None)
        if entry in forced:
            errors.append(f"{f.name} ({version} {f.module} {entry:08X}): this entry is forced by game.toml "
                          f"{', '.join(forced[entry])}; the runtime installs GCRT_FORCE_ARG items first, so the "
                          "decompiled function would be refused at startup and the executable would stop. Move the "
                          "forced point (another function the game calls at the same moment, e.g. the caller) or "
                          "disable the row")
        if isinstance(located, str):
            errors.append(located)
            continue
        ranges, guest = located
        reason = check_sources(game_cfg, version, f, addr, known) or check_code(ranges, guest, f.size)
        if reason:
            errors.append(f"{f.name} ({version} {f.module} {addr:08X}, size 0x{f.size:X}): {reason}")
        if blocks is not None:
            blocks[guest] = code_blocks(ranges, guest, f.size)
        if fpu is not None:
            fpu[guest] = fpu_instructions(ranges, guest, f.size)
        if calls is not None:
            calls[guest] = call_sites(ranges, guest, f.size)
        if asserts is not None:
            asserts[guest] = assertion_blocks(ranges, guest, f.size, no_return_addresses(game_cfg, version))
        if words is not None and not reason:
            base, blob = _code_of(ranges, guest)
            words[guest] = [_word(blob, guest - base + 4 * i) for i in range(f.size // 4)]
        out.append((guest, f))
    addresses = [a for a, _ in out]
    for a in sorted({a for a in addresses if addresses.count(a) > 1}):
        errors.append(f"two functions at {a:08X}: {', '.join(f.name for b, f in out if b == a)}")
    if errors:
        raise SystemExit(f"Decompiled functions refused for {version}:\n  " + "\n  ".join(errors))
    return sorted(out, key=lambda e: e[0])


def registry_source(version: str, entries: list[tuple[int, Function]], oracle: bool = False,
                    records: dict[str, str] | None = None,
                    blocks: dict[int, tuple[list[int], list[int]]] | None = None, sources_hash: str = "",
                    fpu: dict[int, list[int]] | None = None, calls: dict[int, list[int]] | None = None,
                    asserts: dict[int, list[int]] | None = None, globals: list[str] | None = None,
                    costs: dict[str, int] | None = None, paths: list[str] | None = None,
                    path_names: dict[int, str] | None = None, no_return: list[int] | None = None) -> str:
    """C++ registration table of `version` (build folder only: it holds addresses). `oracle`: oracle build
    (tools/build.py --oracle), the table also gives the oracle each function's size, its basic blocks (`blocks`,
    code_blocks(): reachable starts, then unreachable ones marked by bit 0) and the hash of the sources
    (`sources_hash`, oracle_sources_hash()); `records`: export record to use instead of a function's own (negative
    test of the oracle: name -> record name). `fpu`: fpu_instructions() of each function (resolve); a function whose
    original holds a floating-point instruction is registered with gc::abi::registration_uses_fpu, so that the gateway
    lets the original run when MSR[FP] = 0 (runtime/core/decomp/gc_abi.h). Without `fpu`, every function is flagged
    (the safe side: declining only lets the original run). `globals`: lines of tools/decomp_globals.py table_source
    (the address array of the game globals in `version`, k_globals / k_global_count), installed by
    gc::set_global_table before the functions; None: no globals table. `costs`: {function name: guest cycles per
    call in `version`, 1/256 cycle} (read_guest_costs), registration::guest_cost_256ths; a missing name is 0 (nothing
    charged: tools/build.py refuses that for a production build unless --allow-no-guest-cost). `paths`: C++ lines of
    the path graphs (tools/decomp_path_cost.py registry_lines) and `path_names` {guest address: graph name}
    (registration::path of a function whose original's cost depends on its path, gc_path.h). `no_return`: oracle
    builds, guest addresses in `version` of the functions that do not return (no_return_addresses), given to the
    oracle by gcrt_decomp_oracle_no_return (a synthetic case ends at a call to one of them)."""
    records = records or {}
    blocks = blocks or {}
    # Oracle builds: call_sites() (the oracle records the calls made at these instructions) and assertion_blocks()
    # (bit 1 of a block start) of each function.
    calls = calls or {}
    asserts = asserts or {}
    lines = [f"// Registration table of the decompiled functions for {version}: generated by tools/decomp_functions.py",
             "// (tools/build.py) from the game's functions table. Build folder only, never in Git.",
             '#include "decomp/gc_abi.h"', '#include "decomp/gc_abi_c.h"']
    if globals is not None:
        lines += ['#include "decomp/gc_mem.h"', "", "#include <cstddef>", "#include <cstdint>"]
    lines.append("")
    for _, f in entries:
        lines.append(f"extern const ::gc::abi::export_record gc_decomp_export_{records.get(f.name, f.name)};")
    if oracle:
        lines += ["", 'extern "C" void gcrt_decomp_oracle_configure(struct GcrtCpu* g, const char* version, '
                      "const unsigned* addresses, const unsigned* sizes, const char* const* names, unsigned count, "
                      "const unsigned* block_first, const unsigned* blocks, const unsigned* call_first, const unsigned* calls, "
                      "const char* sources_hash);",
                  'extern "C" void gcrt_decomp_oracle_no_return(const unsigned* addresses, unsigned count);']
    lines += ["", "namespace {"]
    if globals is not None:
        lines += globals
    if paths:
        lines += paths
    if entries:
        lines.append("const ::gc::abi::registration k_table[] = {")
        for a, f in entries:
            uses_fpu = bool(fpu.get(a)) if fpu is not None else True
            flags = "::gc::abi::registration_uses_fpu" if uses_fpu else "::gc::abi::registration_none"
            first = f"first floating-point instruction {fpu[a][0]:08X}" if fpu and fpu.get(a) else (
                "no floating-point instruction" if fpu is not None else "not scanned")
            cost = (costs or {}).get(f.name, 0)
            graph = (path_names or {}).get(a)
            path = f", &{graph}" if graph else ""
            note = "; cost by path, mean" if graph else "; guest cost"
            lines.append(f"    {{0x{a:08X}u, &gc_decomp_export_{records.get(f.name, f.name)}, {flags}, {cost}u{path}}}, "
                         f"// {first}{note} {cost / GUEST_COST_FRACTION:.4f} cycles")
        lines += ["};", "constexpr unsigned k_count = sizeof(k_table) / sizeof(k_table[0]);"]
        if oracle:
            lines.append("const unsigned k_addresses[] = {" + ", ".join(f"0x{a:08X}u" for a, _ in entries) + "};")
            lines.append("const unsigned k_sizes[] = {" + ", ".join(f"0x{f.size:X}u" for _, f in entries) + "};")
            lines.append("const char* const k_names[] = {" + ", ".join(f'"{f.name}"' for _, f in entries) + "};")
            # Basic blocks of each function (code_blocks): k_blocks[k_block_first[i] .. k_block_first[i + 1]), the
            # starts of the reachable blocks (bit 1 set for an assertion block), then the unreachable ones with bit 0
            # set.
            flat: list[int] = []
            first = []
            for a, _ in entries:
                first.append(len(flat))
                reach, dead = blocks.get(a, ([a], []))
                marked = set(asserts.get(a, []))
                flat += [b | (2 if b in marked else 0) for b in reach] + [d | 1 for d in dead]
            first.append(len(flat))
            lines.append("const unsigned k_block_first[] = {" + ", ".join(str(x) for x in first) + "};")
            lines.append("const unsigned k_blocks[] = {" + ", ".join(f"0x{x:08X}u" for x in flat) + "};")
            # Call instructions of each function (call_sites), same layout (a final 0 keeps the array non-empty).
            cflat: list[int] = []
            cfirst = []
            for a, _ in entries:
                cfirst.append(len(cflat))
                cflat += calls.get(a, [])
            cfirst.append(len(cflat))
            lines.append("const unsigned k_call_first[] = {" + ", ".join(str(x) for x in cfirst) + "};")
            lines.append("const unsigned k_calls[] = {" + ", ".join(f"0x{x:08X}u" for x in cflat + [0]) + "};")
            # Functions that do not return (a final 0 keeps the array non-empty; the count leaves it out).
            nr = sorted(no_return or [])
            lines.append("const unsigned k_no_return[] = {" + ", ".join(f"0x{x:08X}u" for x in nr + [0]) + "};")
            lines.append(f"constexpr unsigned k_no_return_count = {len(nr)};")
    else:
        lines += ["const ::gc::abi::registration* const k_table = nullptr;", "constexpr unsigned k_count = 0;"]
    lines += ["} // namespace", "",
              'extern "C" void gcrt_decomp_install_generated(struct GcrtCpu* g) {']
    if globals is not None:
        # Before the functions: a decompiled function may run as soon as its hook is installed.
        lines.append("    ::gc::set_global_table(k_globals, k_global_count);")
    if oracle and entries:
        lines.append(f'    gcrt_decomp_oracle_configure(g, "{version}", k_addresses, k_sizes, k_names, k_count, '
                     f'k_block_first, k_blocks, k_call_first, k_calls, "{sources_hash}");')
        lines.append("    gcrt_decomp_oracle_no_return(k_no_return, k_no_return_count);")
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
        fpu: dict[int, list[int]] = {}
        entries = resolve(game_cfg, version, disc_root, _layout(game_cfg, disc_root), fpu=fpu)
        print(f"{version}: {len(entries)} functions resolved, code checks passed")
        if args.action == "list":
            for a, f in entries:
                fp = f"fpu={len(fpu[a])}@{fpu[a][0]:08X}" if fpu.get(a) else "fpu=0"
                print(f"  {a:08X} {f.size:#06x} {f.module:8} {f.status:10} {fp:16} {f.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
