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
  * follows the jump tables of the code (jump_analysis, comment "Jump tables"): the block hooks and the graph of paths
    see their cases, and a table-shaped bctr that is not proven refuses the function;
  * writes the registration table of the version (C++ file for runtime/core/decomp/gc_abi.h, build folder only).
tools/build.py calls it for the HLE hooks (DolRecomp --hle-hooks) and the registration table.

Usage: py -3 tools/decomp_functions.py <game> check [--version ID ...]     (default: every version of game.toml)
       py -3 tools/decomp_functions.py <game> list --version ID          (resolved addresses of one version)
Options of check:
  --parity      first compares the masked code of every row between the versions, all of them whatever --version says,
                disabled rows included: same, differs, unknown (tools/decomp_proof.py). Warnings, refuses nothing. It
                runs before the address checks, so that a refusal does not hide it
  --strict      with --parity: exit code 1 on an enabled row that differs with no stated reason and no version condition
                in its source unit, or that could not be compared. A disc not extracted is reported, not refused
  --table PATH  reads this table of functions instead of the configured one
"""

from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Callable
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


def code_blocks(ranges: list[tuple[int, bytes]], address: int, size: int,
                tables: dict[int, JumpTable] | None = None) -> tuple[list[int], list[int]]:
    """Basic blocks of the function [address, address + size) of `ranges`, from its own code: (start addresses of
    the blocks reachable from the entry, start addresses of the blocks no path from the entry reaches). A block
    starts at the entry, at every branch target inside the function and after every branch. Edges: a conditional
    branch goes to its target and to the next instruction; `b` (without link) to its target; blr, bctr, rfi and the
    always-taken forms of bclr/bcctr end the path, except the bctr of a table of `tables` (jump_analysis), which goes to
    every entry of its table; a call (link bit) goes on to the next instruction; a branch out of the function ends the
    path. BO & 0x14 == 0x14 is "branch always" (PowerPC UISA, bc: neither CTR nor condition tested)."""
    tables = tables or {}
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
            table = tables.get(address + 4 * i) if ((w >> 1) & 0x3FF) == 528 else None
            cases = sorted({(t - address) // 4 for t in table.entries}) if table else []
            succ.append(cases if table else nxt if (lk or not always) else [])
            leaders.update(cases)
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


# Jump tables. A dense `switch` compiles to a compare of the index, a conditional branch taken when it is out of range,
# then a word loaded from a table in the data sections and a bctr to it:
#     cmplwi r3, 8; bgt default; lis r4, table@ha; slwi r0, r3, 2; addi r3, r4, table@l; lwzx r0, r3, r0; mtctr r0; bctr
# jump_analysis follows such a bctr only on a proof made on this code, in the manner of the AArch64 proof of REA
# (MIT, its docs/native-investigation.md; REA proves nothing for PowerPC, and tools/ghidra/scripts/ExportJumpTables.java
# is our own cross-check of this proof):
#   * bound: the comparison is an unsigned compare with an immediate, of the very value that indexes the table (a
#     register evaluation from the start of the block, through copies, and with the scaling by 4 seen), and the branch
#     out of range tests that compare (cmplwi + bgt: immediate + 1 entries; cmplwi + bge: immediate entries);
#   * no other way in: no branch of the function, direct or through a table, lands after the compare;
#   * table: its address is a constant of the code, and exactly that many words are read from the image;
#   * targets: every entry is an instruction of the function other than its entry (a jump to the entry would be a
#     recursive tail call; the oracle's run-time check tells a case from a tail call by the same rule); two tables of
#     a function do not overlap.
# Any failure is a refusal with its reason (`refused`): the table is then not followed, and no case is ever guessed
# (an index out of range is the bound branch, kept apart as `default`). A bctr whose CTR plainly comes from something
# else than an indexed load has no table shape: an indirect tail call, listed in `plain`, ending the path as before.
@dataclass(frozen=True)
class JumpTable:
    site: int                  # the bctr
    base: int                  # address of the table
    entries: tuple[int, ...]   # target of each index, as many as the bound proves
    compare: int               # the compare of the index
    bound_branch: int          # the branch taken when the index is out of range
    default: int | None        # its target, None when it leaves the function
    index_register: int        # register compared


@dataclass
class JumpAnalysis:
    tables: dict[int, JumpTable] = field(default_factory=dict)  # bctr -> proven table
    refused: dict[int, str] = field(default_factory=dict)       # bctr -> why its table is not proven
    plain: list[int] = field(default_factory=list)              # bctr without table shape


class FunctionWords(list):
    """Instruction words of an original (resolve `words`) with what jump_analysis found in them: tools/decomp_path_cost.py
    reads `jump_tables` {bctr: targets by index} and `jump_refusals` {bctr: reason}."""

    def __init__(self, words: list[int], analysis: JumpAnalysis | None = None):
        super().__init__(words)
        self.jump_tables = {s: t.entries for s, t in analysis.tables.items()} if analysis else {}
        self.jump_refusals = dict(analysis.refused) if analysis else {}


SPR_CTR = 9
# Primary opcodes whose register result is in the rD field, in the rA field, and the update forms that also write rA.
_WRITES_RD = frozenset({7, 8, 12, 13, 14, 15, 32, 34, 40, 42})
_WRITES_RA = frozenset({20, 21, 23, 24, 25, 26, 27, 28, 29})
_UPDATE_RD_RA = frozenset({33, 35, 41, 43})
_UPDATE_RA = frozenset({37, 39, 45, 49, 51, 53, 55, 57, 61})
_NO_GPR = frozenset({4, 10, 11, 36, 38, 44, 47, 48, 50, 52, 54, 56, 59, 60, 63})
# Opcode 31: extended opcodes with the result in rA; without register result (compares, cache, indexed stores, FP
# indexed loads, mtspr, mtcrf); XO-form and other results in rD (9-bit field: bit 21 is OE in the XO forms).
_X31_RA = frozenset({24, 26, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 922, 954})
_X31_NONE = frozenset({0, 32, 86, 144, 151, 215, 407, 467, 535, 599, 663, 727})
_X31_RD = frozenset({8, 10, 11, 19, 23, 40, 75, 87, 104, 136, 138, 200, 202, 232, 234, 235, 266, 279, 339, 343, 459, 491})
_CR_LOGICAL = frozenset({0, 33, 129, 193, 225, 257, 289, 417, 449})


def _gpr_written(word: int) -> tuple[int, ...] | None:
    """General registers `word` writes (PowerPC UISA instruction forms), None for a form not modelled here (no proof
    goes through one). Branches are not asked here."""
    op, rd, ra, xo = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 1) & 0x3FF
    if op in _WRITES_RD:
        return (rd,)
    if op in _WRITES_RA:
        return (ra,)
    if op in _UPDATE_RD_RA:
        return (rd, ra)
    if op in _UPDATE_RA:
        return (ra,)
    if op in _NO_GPR:
        return ()
    if op == 46:  # lmw
        return tuple(range(rd, 32))
    if op == 19 and xo in _CR_LOGICAL:
        return ()
    if op == 31:
        if xo in _X31_RA:
            return (ra,)
        if xo in _X31_NONE:
            return ()
        if (xo & 0x1FF) in _X31_RD:
            return (rd,)
    return None


def _writes_cr(word: int) -> bool:
    """True if `word` may change a CR field: compares, the record forms, CR logical instructions, mcrf, mcrfs (63/64),
    mcrxr (31/512), mtcrf, stwcx. (31/150, whatever its Rc bit). Checked against the instruction list of Dolphin's
    PPCTables.cpp (tests/decomp/test_jump_tables.py)."""
    op, xo = word >> 26, (word >> 1) & 0x3FF
    if op in (10, 11, 13, 28, 29) or (op == 19 and xo in _CR_LOGICAL):
        return True
    if op in (20, 21, 23, 59, 63, 4):
        return bool(word & 1) or (op == 63 and xo in (0, 32, 64)) or (op == 4 and xo in (0, 32, 64, 96))
    return op == 31 and (bool(word & 1) or xo in (0, 32, 144, 150, 512))


def _is_branch(word: int) -> bool:
    op, xo = word >> 26, (word >> 1) & 0x3FF
    return op in (16, 17, 18) or (op == 19 and xo in (16, 50, 528))


def _is_mtctr(word: int) -> bool:
    return word >> 26 == 31 and (word >> 1) & 0x3FF == 467 and (((word >> 16) & 31) | (((word >> 11) & 31) << 5)) == SPR_CTR


def _direct_targets(words: list[int], address: int) -> set[int]:
    """Targets of the direct branches without link of `words` (function at `address`)."""
    return {_branch_target(w, address + 4 * i) for i, w in enumerate(words) if w >> 26 in (16, 18) and not w & 1}


def _table_of_index(memory, start: int, compare: int, site: int, index: int) -> int | str:
    """Address of the table the bctr at `site` reads, from a register evaluation of the instructions [start, site), or
    the reason there is none: `index` is the register compared at `compare`. Values: ("const", value), ("index",),
    ("scaled",) for the index times 4, ("entry", table address) for the word of the table; anything else is unknown
    (absent). The only branch in the range is the bound branch, not taken here."""
    regs: dict[int, tuple] = {}
    ctr = None

    def kind(r: int) -> str:
        return regs[r][0] if r in regs else ""

    for at in range(start, site, 4):
        w = memory.word(at)
        op, rs, ra, rb, xo = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 1) & 0x3FF
        if at == compare:
            regs[index] = ("index",)
            continue
        if _is_branch(w):
            continue
        if _is_mtctr(w):
            ctr = regs.get(rs)
            continue
        written = _gpr_written(w)
        if written is None:
            return f"instruction {w:08X} at {at:08X} is not modelled"
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        value = None
        if op == 15 and (ra == 0 or kind(ra) == "const"):  # lis, addis
            value = ("const", ((regs[ra][1] if ra else 0) + (imm << 16)) & 0xFFFFFFFF)
        elif op == 14 and (ra == 0 or kind(ra) == "const"):  # li, addi
            value = ("const", ((regs[ra][1] if ra else 0) + simm) & 0xFFFFFFFF)
        elif op == 24 and kind(rs) == "const":  # ori
            value = ("const", regs[rs][1] | imm)
        elif op == 31 and xo == 444 and rs == rb:  # mr
            value = regs.get(rs)
        elif op == 21 and kind(rs) == "index" and (rb, (w >> 6) & 31, (w >> 1) & 31) == (2, 0, 29):  # slwi 2
            value = ("scaled",)
        elif op == 31 and xo == 23 and ra and {kind(ra), kind(rb)} == {"const", "scaled"}:  # lwzx
            value = ("entry", regs[ra][1] if kind(ra) == "const" else regs[rb][1])
        for r in written:
            regs.pop(r, None)
        if value is not None:
            regs[written[0]] = value
    if ctr is None or ctr[0] != "entry":
        return "the bctr does not jump to the word read at the compared index, scaled by 4, from a constant address"
    return ctr[1]


def _ctr_source(memory, address: int, site: int) -> str:
    """How CTR reaches the bctr at `site`: "table" (the last mtctr of its block takes a word from an indexed load),
    "plain" (it takes something else), or the reason it cannot be told."""
    at = site - 4
    while at >= address and not _is_mtctr(memory.word(at)):
        if _is_branch(memory.word(at)):
            return f"no mtctr in the block of the bctr at {site:08X}"
        at -= 4
    if at < address:
        return f"no mtctr before the bctr at {site:08X}"
    source = (memory.word(at) >> 21) & 31
    at -= 4
    while at >= address:
        x = memory.word(at)
        if _is_branch(x):
            break
        written = _gpr_written(x)
        if written is None:
            return f"instruction {x:08X} at {at:08X} is not modelled"
        if source in written:
            return "table" if x >> 26 == 31 and (x >> 1) & 0x3FF == 23 else "plain"
        at -= 4
    return f"r{source} of the mtctr before the bctr at {site:08X} comes from another block"


def _prove_table(memory, address: int, size: int, site: int, targets: set[int]) -> JumpTable | str:
    """The table of the bctr at `site` of the function [address, address + size), or the reason it is not proven."""
    # The bound branch: the nearest branch before the bctr, a conditional one.
    bound = site - 4
    while bound >= address and not _is_branch(memory.word(bound)):
        bound -= 4
    if bound < address:
        return "no bound check before the table"
    b = memory.word(bound)
    bo, bi = (b >> 21) & 31, (b >> 16) & 31
    if b >> 26 != 16 or b & 1 or (bo & 0x14) == 0x14:
        return f"the branch before the table, at {bound:08X}, is not a conditional bound check"
    compare = bound - 4
    while compare >= address and not _writes_cr(memory.word(compare)):
        if _is_branch(memory.word(compare)):
            return f"no compare in the block of the bound branch at {bound:08X}"
        compare -= 4
    if compare < address:
        return f"no compare before the bound branch at {bound:08X}"
    c = memory.word(compare)
    if c >> 26 != 10 or (c >> 21) & 3 or (c >> 23) & 7 != bi >> 2:
        return (f"the bound branch at {bound:08X} does not test an unsigned compare of a register with an immediate "
                f"({c:08X} at {compare:08X})")
    index, limit = (c >> 16) & 31, c & 0xFFFF
    if bo == 12 and bi & 3 == 1:  # branch if greater: in range up to `limit` included
        count = limit + 1
    elif bo == 4 and bi & 3 == 0:  # branch if not less: in range below `limit`
        count = limit
    else:
        return f"the bound branch at {bound:08X} (BO {bo}, BI {bi}) does not leave on an index out of range"
    if count == 0:
        return f"the bound at {compare:08X} leaves no entry"
    inside = sorted(t for t in targets if compare < t <= site)
    if inside:
        return f"a branch lands at {inside[0]:08X}, after the compare at {compare:08X}: the bound may not hold there"
    start = compare
    while start > address and start not in targets and not _is_branch(memory.word(start - 4)):
        start -= 4
    base = _table_of_index(memory, start, compare, site, index)
    if isinstance(base, str):
        return base
    entries = []
    for i in range(count):
        t = memory.word((base + 4 * i) & 0xFFFFFFFF)
        if t is None:
            return f"entry {i} of {count} of the table at {base:08X} is outside the image"
        if t % 4 or not address <= t < address + size:
            return f"entry {i} of {count} of the table at {base:08X} ({t:08X}) is not an instruction of the function"
        if t == address:
            return f"entry {i} of {count} of the table at {base:08X} jumps to the function's entry"
        entries.append(t)
    target = _branch_target(b, bound)
    return JumpTable(site, base, tuple(entries), compare, bound,
                     target if address <= target < address + size else None, index)


def jump_analysis(memory, address: int, size: int) -> JumpAnalysis:
    """Every bcctr without link of the function [address, address + size) of `memory` (word(address) -> word or None;
    code and data, relocated for a REL: dol_memory, rel_memory): its table if proven, or why not, or `plain`. See
    "Jump tables" above."""
    out = JumpAnalysis()
    words = [memory.word(address + 4 * i) for i in range(size // 4)]
    if None in words:
        raise ValueError(f"{address:08X}+0x{size:X} is outside the image")
    targets = _direct_targets(words, address)
    for i, w in enumerate(words):
        if w >> 26 != 19 or (w >> 1) & 0x3FF != 528 or w & 1:
            continue
        site = address + 4 * i
        source = _ctr_source(memory, address, site)
        if (w >> 21) & 0x14 != 0x14:
            out.refused[site] = "conditional bcctr"
        elif source == "plain":
            out.plain.append(site)
        elif source != "table":
            out.refused[site] = source
        else:
            found = _prove_table(memory, address, size, site, targets)
            if isinstance(found, str):
                out.refused[site] = found
            else:
                out.tables[site] = found
    # A table entry is a way in too: none may land after the compare of a table.
    for site, table in list(out.tables.items()):
        inside = sorted({t for other in out.tables.values() for t in other.entries if table.compare < t <= site})
        if inside:
            del out.tables[site]
            out.refused[site] = (f"a table entry lands at {inside[0]:08X}, after the compare at {table.compare:08X}: the "
                                 f"bound may not hold there")
    spans = sorted(out.tables.values(), key=lambda t: t.base)
    for a, b in zip(spans, spans[1:]):
        if a.base + 4 * len(a.entries) > b.base:
            for t in (a, b):
                out.tables.pop(t.site, None)
                out.refused[t.site] = f"the tables at {a.base:08X} and {b.base:08X} overlap"
    return out


def has_indirect_jump(ranges: list[tuple[int, bytes]], address: int, size: int) -> bool:
    """True if the function holds a bcctr without link (only then is the data of its module read)."""
    base, blob = _code_of(ranges, address)
    if blob is None:
        return False
    return any(w >> 26 == 19 and (w >> 1) & 0x3FF == 528 and not w & 1
               for w in (_word(blob, address - base + 4 * i) for i in range(size // 4)))


def dol_memory(path: Path):
    """Code and data sections of a DOL by address (tools/symbols.py Image)."""
    import gcinfo  # noqa: E402  (generic tool of the repository)
    import symbols as sy  # noqa: E402
    data = path.read_bytes()
    return sy.Image([(s["addr"], data[s["offset"]:s["offset"] + s["size"]])
                     for s in gcinfo.describe_dol(data)["segments"] if s["size"]])


# REL relocation entries (Dolphin OS module format; type names in tools/gcinfo.py): an offset from the previous entry, a
# type, a section, an addend. Those that give the code the address of a table and the table its targets: 1 R_PPC_ADDR32,
# 4 R_PPC_ADDR16_LO, 5 R_PPC_ADDR16_HI, 6 R_PPC_ADDR16_HA; 201 R_DOLPHIN_NOP, 202 R_DOLPHIN_SECTION (start of a section),
# 203 R_DOLPHIN_END.
def rel_memory(path: Path, module: dict):
    """Sections of the REL file `path` at the addresses of `module` (tools/build.py rel_layout), with the relocations
    against itself and against the DOL applied, as OSLink does at load time: a REL file holds zeroes in the words a
    relocation fills. Relocations against another module are left as they are."""
    import struct
    import symbols as sy  # noqa: E402
    data = path.read_bytes()

    def be32(offset: int) -> int:
        return int.from_bytes(data[offset:offset + 4], "big")

    # BSS sections are listed with their address and no bytes: a relocation may point at them.
    addresses = {i: addr for i, addr, *_ in module["sections"]}
    sections = {i: bytearray(blob) for i, _, _, _, blob in module["sections"]}
    for k in range(be32(0x2C) // 8):
        wanted, offset = be32(be32(0x28) + 8 * k), be32(be32(0x28) + 8 * k + 4)
        if wanted not in (0, module["id"]):
            continue
        where = None
        here = 0
        while offset + 8 <= len(data):
            step, kind, section, addend = struct.unpack_from(">HBBI", data, offset)
            offset += 8
            here += step
            if kind == 203:
                break
            if kind == 202:
                where, here = section, 0
            elif kind in (1, 4, 5, 6) and where in sections:
                value = (addresses.get(section, 0) + addend if wanted else addend) & 0xFFFFFFFF
                if kind == 1:
                    sections[where][here:here + 4] = value.to_bytes(4, "big")
                else:
                    half = {4: value, 5: value >> 16, 6: (value + 0x8000) >> 16}[kind] & 0xFFFF
                    sections[where][here:here + 2] = half.to_bytes(2, "big")
    return sy.Image([(addresses[i], bytes(blob)) for i, blob in sections.items() if blob])


def module_memory(game_cfg: dict, disc_root: Path, module: str, rels: dict[str, dict]):
    """Code and data of a module of the table ("dol" or a REL name, `rels` by name as in resolve), for jump_analysis."""
    if module == "dol":
        return dol_memory(disc_root / game_cfg["code"]["dol"])
    code = game_cfg.get("code", {})
    rel_dir = disc_root / code["rel_dir"] if code.get("rel_dir") else None
    found = sorted(rel_dir.rglob(f"{module}.rel")) if rel_dir and rel_dir.exists() else []
    if not found or module not in rels:
        raise SystemExit(f"module {module}: no REL file under {rel_dir}")
    return rel_memory(found[0], rels[module])


def any_function_words(game_cfg: dict, disc_root: Path, module: str, offset: int, size: int,
                       layout: list[dict]) -> tuple[int, FunctionWords]:
    """(guest address, words with their jump tables) of a function that need not be in the table, for a diagnostic:
    `offset` is its address, or its offset in the code of a REL `module`."""
    rels = {m["name"]: m for m in layout}
    memory = module_memory(game_cfg, disc_root, module, rels)
    guest = offset
    if module != "dol":
        guest += next(a for (_, a, _, executable, _) in rels[module]["sections"] if executable)
    words = [memory.word(guest + 4 * i) for i in range(size // 4)]
    if None in words:
        raise SystemExit(f"{guest:08X}+0x{size:X} is outside {module}")
    return guest, FunctionWords(words, jump_analysis(memory, guest, size))


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
    its callee run once, and replayed to the decompiled version). blr and bclr (returns) are not calls. The bctr of a
    jump table stays listed: its hook reads CTR at run time (gc_oracle_branch.h decode_branch), so a target inside the
    function runs as compiled, and a target out of it would be seen as the tail call it is."""
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


def assertion_blocks(ranges: list[tuple[int, bytes]], address: int, size: int, no_return: set[int],
                     tables: dict[int, JumpTable] | None = None) -> list[int]:
    """Reachable basic blocks (starts from code_blocks) that only lead to a function that never returns (`no_return`:
    guest addresses in this version, [decomp] no_return of game.toml): a block whose last instruction is an
    unconditional call (bl) to one of them, or whose successors all are such blocks (fixed point). The oracle report
    lists them apart (tools/decomp_oracle.py): no scenario reaches them without stopping the game, so synthetic cases,
    whose calls the oracle simulates, cover them."""
    tables = tables or {}
    reach, _ = code_blocks(ranges, address, size, tables)
    base, blob = _code_of(ranges, address)
    if blob is None or not no_return:
        return []
    starts = sorted(reach)
    end = address + size
    leaders = set(starts)
    # A block runs up to the next leader of the whole function (reachable or not).
    every, dead = code_blocks(ranges, address, size, tables)
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
            table = tables.get(last) if xo == 528 else None
            succ[s] = sorted(set(table.entries)) if table else nxt if (lk or not always) else []
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


class ProvenTables:
    """Jump tables of a program's code, for switching_reach. `sections`: (start, size, load), `load` giving the memory
    of that module for jump_analysis (code and data, relocated for a REL). A section is only read when the walk meets
    a bctr in it, so a closure without one costs nothing.

    The walk starts at call targets and knows no function bounds, and a build has no function list it can always rely
    on. So each section goes through jump_analysis as ONE span. Its checks only get stricter on the branch targets (all
    of the section's, a superset of the function's). The one that gets looser is that an entry must be inside the
    function: here it must be inside the section. The compare, the bound, the table address and the indexed load are
    proven as for a function, and tests/decomp/run_thread_switch_refusal_test.py checks the result against the census
    per function.

    This relaxed proof serves switching_reach only, and it fails in the dangerous direction: a mis-recognised table
    (the pattern found in dead code or decoded data) would be taken for true with targets outside its function, the
    real callee behind the bctr would be missed, and the site would turn from `partial` into `complete` wrongly. Rerun
    run_thread_switch_refusal_test whenever jump_analysis or _prove_table changes."""

    def __init__(self, sections: list[tuple[int, int, Callable]]):
        self.sections = sections
        self.analysis: dict[int, JumpAnalysis] = {}

    def entries(self, site: int) -> tuple[int, ...] | None:
        """Targets of the bctr at `site` if its table is proven, by table index; None otherwise."""
        for start, size, load in self.sections:
            if start <= site < start + size:
                if start not in self.analysis:
                    self.analysis[start] = jump_analysis(load(), start, size)
                table = self.analysis[start].tables.get(site)
                return table.entries if table else None
        return None


@dataclass
class LocalScan:
    callees: set[int]        # targets of the direct calls and of the jumps that may be tail calls
    indirect: list[int]      # bctrl, bclrl, and bctr without a proven table: their targets are unknown
    unresolved: list[int]    # calls whose target is an unapplied REL relocation
    outside: list[int]       # addresses reached that are not code the walk can read
    tables: list[int]        # bctr whose proven table was followed


def _local_scan(ranges: list[tuple[int, bytes]], entry: int, rel_bases: set[int], natives: frozenset[int] = frozenset(),
                tables: ProvenTables | None = None) -> LocalScan:
    """Control flow of the code at `entry`, followed through its conditional and unconditional branches until every
    path ends (blr, bctr, rfi, or an address outside the code). A `b` is followed as a jump and also reported as a
    callee (a tail call is a call); a conditional branch to an address of `natives` too, so the native is seen by name.
    Both go on through the guest body of a native, which may not run: the walk may then find more than the native does,
    never less. A bctr with a proven table (`tables`) goes on at each entry. In a REL module (`rel_bases`: start of its
    code sections) a branch whose displacement is 0 is an unapplied relocation: its target is not known here.

    A blr is taken for a return. That is an assumption: mtlr of a computed value then blr is a jump this walk does not
    see. Counted in the DOL of both versions of the game (RELs not counted): about 4 such sites per version, all in the
    TRK debugger or __OSDBIntegrator."""
    seen: set[int] = set()
    todo = [entry]
    scan = LocalScan(set(), [], [], [], [])
    while todo:
        at = todo.pop()
        while at not in seen:
            base, blob = _code_of(ranges, at)
            if blob is None or at - base + 4 > len(blob):
                scan.outside.append(at)
                break
            seen.add(at)
            w = _word(blob, at - base)
            op, lk = w >> 26, w & 1
            if op == 18:
                if (w & 0x03FFFFFC) == 0 and not (w & 2) and base in rel_bases:
                    scan.unresolved.append(at)
                    if not lk:
                        break
                    at += 4
                    continue
                t = _branch_target(w, at)
                scan.callees.add(t)
                if lk:
                    at += 4
                    continue
                todo.append(t)  # a jump inside the function, or a tail call (also a callee above)
                break
            if op == 16:
                t = _branch_target(w, at)
                bo = (w >> 21) & 0x1F
                if lk or t in natives:
                    scan.callees.add(t)
                if not lk:
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
                        scan.indirect.append(at)  # bctrl, bclrl: indirect call
                        at += 4
                        continue
                    if xo == 528:  # bctr: jump table or indirect tail call
                        entries = tables.entries(at) if tables and (bo & 0x14) == 0x14 else None
                        if entries is None:
                            scan.indirect.append(at)
                        else:
                            scan.tables.append(at)
                            todo.extend(entries)
                    if (bo & 0x14) == 0x14:
                        break
                    at += 4
                    continue
                if xo == 50:  # rfi
                    break
            at += 4
    return scan


def switching_reach(ranges: list[tuple[int, bytes]], entries: list[int], hooks: dict[int, str],
                    switching: set[str], rel_bases: set[int] | None = None, tables: ProvenTables | None = None,
                    observed: dict[int, set[int]] | None = None) -> dict[int, dict]:
    """For each entry, the static closure of its calls (issue #65). "No callee that waits" is true or unknown, never
    assumed: `closure` is "complete" when every transitive call is known, "partial(n)" with n the distinct places
    where it is not ("sites": indirect calls and bctr without a proven table, plus addresses that are not readable
    code; "unresolved": calls that are an unapplied REL relocation, also counted in n). A complete closure without
    `path` means every call is known and no call reaches a native of `switching`; it does not mean nothing can wait:
    guest code that busy-waits (on a flag, on a DMA) calls no such native. A partial closure says nothing about what it
    could not follow. An unknown call blocks the claim instead of reading as no call, as in REA (MIT, its
    src/domain/functionCallCoverage.ts: the idea, no code).

    {entry: {"path": [address, ..., native name] to a thread-switching native, or None; "closure"; "indirect": distinct
    indirect sites; "sites"; "unresolved"; "tables": bctr followed through a proven table; "observed": {site: targets}
    of the unknown sites `observed` has targets for; "observed_path": the sites on `path` that are such a target}}.

    A hooked address whose name is a native replacement (any name of `hooks` except "decomp", "game", "oracle-block",
    "oracle-call", "extra") is a leaf: its guest body does not run; hle/thread_switching_natives.txt lists those that
    run a guest callback before they return. A decompiled function is followed through its original (functional
    decompilation: the same calls). `tables`: bctr whose table is proven are followed, without it they stay unknown.
    `observed`: {site: targets seen at run time}, followed too, but what was seen is not all that can happen: the site
    stays unknown and the function partial, and a path through one is reported as such (`observed_path`)."""
    rel_bases = rel_bases or set()
    observed = observed or {}
    followed = ("decomp", "game", "oracle-block", "oracle-call", "extra")
    natives = frozenset(a for a, name in hooks.items() if name not in followed)
    cache: dict[int, LocalScan] = {}
    out: dict[int, dict] = {}
    for entry in entries:
        parent: dict[int, int | None] = {entry: None}
        through: dict[int, int] = {}  # callee reached only through an observed target -> its site
        queue = [entry]
        found = None
        indirect: dict[int, None] = {}
        other: dict[int, None] = {}
        unresolved: dict[int, None] = {}
        table_sites: dict[int, None] = {}
        seen_at: dict[int, list[int]] = {}
        while queue:
            f = queue.pop(0)
            if f != entry and f in natives:
                if found is None and hooks[f] in switching:
                    found = f
                continue  # native: its guest body does not run
            if f not in cache:
                cache[f] = _local_scan(ranges, f, rel_bases, natives, tables)
            scan = cache[f]
            indirect.update(dict.fromkeys(scan.indirect))
            other.update(dict.fromkeys(scan.outside))
            unresolved.update(dict.fromkeys(scan.unresolved))
            table_sites.update(dict.fromkeys(scan.tables))
            for c in sorted(scan.callees):
                if c not in parent:
                    parent[c] = f
                    queue.append(c)
            for site in scan.indirect:
                seen_at[site] = sorted(observed.get(site, ()))
                for c in seen_at[site]:
                    if c not in parent:
                        parent[c] = f
                        through[c] = site
                        queue.append(c)
        path = None
        observed_path: list[int] = []
        if found is not None:
            path = [hooks[found]]
            node = found
            while node is not None:
                if node in through:
                    observed_path.insert(0, through[node])
                node = parent[node]
                if node is not None:
                    path.insert(0, f"{node:08X}")
        sites = sorted(set(indirect) | set(other))
        unknown = len(sites) + len(unresolved)
        out[entry] = {"path": path, "closure": f"partial({unknown})" if unknown else "complete",
                      "indirect": len(indirect), "sites": sites, "unresolved": list(unresolved),
                      "tables": len(table_sites), "observed": {s: t for s, t in seen_at.items() if t},
                      "observed_path": observed_path}
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


def hash_shared_inputs(h, game_cfg: dict, dolrecomp: str) -> None:
    """Adds to `h` the inputs of an oracle verdict that are not files: `dolrecomp` (dolrecomp_state), the functions
    that never return and the globals table. Also read by tools/decomp_proof.py common_hash."""
    h.update(b"dolrecomp " + dolrecomp.encode() + b"\n")
    # Functions that never return ([decomp.no_return]): they decide the assertion blocks of the report.
    for name, per_version in sorted((game_cfg.get("decomp", {}).get("no_return") or {}).items()):
        h.update(f"no_return {name} {sorted((per_version or {}).items())}\n".encode())
    # The globals table (tools/decomp_globals.py): the addresses the decompiled code reads and writes.
    import decomp_globals  # noqa: E402
    gpath = decomp_globals.table_path(game_cfg)
    if gpath is not None and gpath.exists():
        h.update(b"globals\0" + gpath.read_bytes().replace(b"\r\n", b"\n") + b"\0")


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
    hash_shared_inputs(h, game_cfg, dolrecomp_state())
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
    (oracle builds). `words`: if given, filled with {guest address: instruction words of the original} (FunctionWords,
    with its jump tables; graph of its paths, tools/decomp_path_cost.py). A table-shaped bctr that jump_analysis does
    not prove refuses the function: neither the block hooks nor the graph could see its cases."""
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
    memories: dict[str, object] = {}
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
        analysis = JumpAnalysis()
        if not reason and has_indirect_jump(ranges, guest, f.size):
            if f.module not in memories:
                memories[f.module] = module_memory(game_cfg, disc_root, f.module, rels)
            analysis = jump_analysis(memories[f.module], guest, f.size)
            if analysis.refused:
                site, why = sorted(analysis.refused.items())[0]
                reason = f"jump table of the bctr at {site:08X} not proven: {why}"
        if reason:
            errors.append(f"{f.name} ({version} {f.module} {addr:08X}, size 0x{f.size:X}): {reason}")
        if blocks is not None:
            blocks[guest] = code_blocks(ranges, guest, f.size, analysis.tables)
        if fpu is not None:
            fpu[guest] = fpu_instructions(ranges, guest, f.size)
        if calls is not None:
            calls[guest] = call_sites(ranges, guest, f.size)
        if asserts is not None:
            asserts[guest] = assertion_blocks(ranges, guest, f.size, no_return_addresses(game_cfg, version),
                                              analysis.tables)
        if words is not None and not reason:
            base, blob = _code_of(ranges, guest)
            words[guest] = FunctionWords([_word(blob, guest - base + 4 * i) for i in range(f.size // 4)], analysis)
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


def manifest_disc(path: Path) -> Path:
    """Extracted disc named by the manifest `path` of tools/extract.py."""
    import json
    disc = json.loads(path.read_text(encoding="utf-8")).get("disc_root")
    if not disc:
        raise SystemExit(f"{path} has no disc_root: extract the disc again (tools/extract.py)")
    return Path(disc)


def main() -> int:
    parser = argparse.ArgumentParser(description="Checks and resolves the table of decompiled functions.")
    parser.add_argument("game")
    parser.add_argument("action", choices=["check", "list"])
    parser.add_argument("--version", action="append", help="version ID (repeatable; default: all)")
    parser.add_argument("--parity", action="store_true", help="check: compare the masked code between the versions")
    parser.add_argument("--strict", action="store_true", help="check --parity: exit 1 on what the warnings of an enabled "
                        "row say, except a disc not extracted")
    parser.add_argument("--table", type=Path, help="table of functions to read instead of the configured one")
    args = parser.parse_args()
    if args.parity and args.action != "check":
        parser.error("--parity goes with check")
    if args.strict and not args.parity:
        parser.error("--strict goes with check --parity")
    game_cfg = tc.load_game(args.game)
    if args.table:
        game_cfg = dict(game_cfg, decomp=dict(game_cfg["decomp"], functions=str(args.table.resolve())))
    versions = args.version or [v["game_id"] for v in game_cfg["versions"]]
    functions = read_table(game_cfg)
    print(f"{len(functions)} functions in the table, "
          f"{sum(f.enabled for f in functions)} enabled")
    parity = None
    if args.parity:
        import decomp_proof  # noqa: E402  (imports this file: not at the top)
        discs = {}
        for version in (v["game_id"] for v in game_cfg["versions"]):
            manifest = tc.GENERATED / args.game / version / "manifest.json"
            discs[version] = manifest_disc(manifest) if manifest.exists() else None
        parity = decomp_proof.parity_report(game_cfg, discs)
        print("\n".join(decomp_proof.parity_lines(parity)))
        if args.strict and parity["not_extracted"]:
            print(f"--strict: {parity['not_extracted']} enabled rows not compared: disc not extracted, not refused")
        if args.strict:
            print("\n".join(f"--strict refused: {line}" for line in parity["refused"]) or "--strict: nothing refused")
    for version in versions:
        manifest = tc.GENERATED / args.game / version / "manifest.json"
        if not manifest.exists():
            raise SystemExit(f"{manifest} missing: extract the disc first (tools/extract.py)")
        disc_root = manifest_disc(manifest)
        fpu: dict[int, list[int]] = {}
        entries = resolve(game_cfg, version, disc_root, _layout(game_cfg, disc_root), fpu=fpu)
        print(f"{version}: {len(entries)} functions resolved, code checks passed")
        if args.action == "list":
            for a, f in entries:
                fp = f"fpu={len(fpu[a])}@{fpu[a][0]:08X}" if fpu.get(a) else "fpu=0"
                print(f"  {a:08X} {f.size:#06x} {f.module:8} {f.status:10} {fp:16} {f.name}")
    return 1 if args.strict and parity["refused"] else 0


if __name__ == "__main__":
    sys.exit(main())
