"""Table of the game globals that decompiled code reads or writes (generic; parameters in games/<game>/game.toml,
[decomp]).

Decompiled code never holds a guest address (docs: "never a raw guest RAM address in decompiled code"). A global is
named by an identifier; its address in the running version comes from a table installed at start-up
(gc::set_global_table, runtime/core/decomp/gc_mem.h, which knows only "address table indexed by identifier"). This
tool reads that table (`[decomp] globals`, a TSV file in games/<game>/), checks every address in the DOL of each
version, and writes the identifier header and the address array of the version being built (build folder only).
Only names, addresses and sizes are in the table: never the contents of the data.

Columns (tab-separated; header line first, `#` lines are comments):
  id        C++ identifier of the global in decompiled code (enum value of `global_id`), or "-" for a row that is
            only documented and checked (a constant written as a literal in the code has no identifier)
  name      descriptive or CodeWarrior-style name (free text without tabs)
  <version columns of [decomp.columns]>   guest address in that version, or "-" when not established there
  size      size in bytes (hexadecimal, with or without 0x), the same in every version
  section   DOL section of the address in every version: a name of [decomp.dol_sections] or of
            [decomp] dol_bss_sections (e.g. .data, .sbss, .sdata2)
  kind      data      a variable (any data or BSS section); in an initialized section its bytes must be the same
                      in every version, except words that are pointers into the DOL in both versions (relocated
                      pointers: a code pointer must pair through the transposition, a data pointer is masked)
            const     a constant (initialized section): the same bytes in every version, no masking
            vtable    a virtual table (initialized section): slot by slot, the same word, or code pointers that pair
                      through the transposition, or pointers to the same row of this table, or masked data pointers
            callback  a code address (a function taken by its address): a known function start of the version that
                      pairs with the reference address through the transposition (tools/decomp_common.py)
  used_by   functions whose ORIGINAL code references the global: names of the functions table, or reference-version
            addresses (hexadecimal) of functions known to the symbol sources; "," separated; "-" for none
  status    verified (role and name cross-checked in both versions) or hypothesis (name or role not certain); both
            are built: the address checks below are applied to every row
  evidence  free text

Checks, for each version (`check_version`; tools/build.py refuses to build when one fails):
  1. the identifier is a C identifier, unique, not a placeholder ("todo..."); no two rows overlap;
  2. [address, address + size) lies inside the row's section, as the DOL of that version lays it out (sections named
     by [decomp.dol_sections]; the BSS range minus the data sections inside it, in address order, named by
     [decomp] dol_bss_sections). The section names are checked against the small-data bases of the boot code
     (tools/dol2elf.py find_sda_bases): r13 - 0x8000 must be the start of .sdata and r2 - 0x8000 that of .sdata2;
  3. the contents, compared with the reference version, by kind (above);
  4. for every function of used_by: its original code in that version references an address inside the row
     (r13/r2 + displacement, an absolute address built by lis then a D-form instruction, as the code scanner below
     finds them), and the references fall at the same instruction offsets in every version (so the two addresses
     are the same global of the same function, the displacements compared per version).
  A row with an address that fails a check refuses the version; a row without an address in a version ("-") is
  absent there (address 0 in the array: gc::global faults if it is used). tools/build.py also refuses a version where
  an enabled function of the functions table lists, in used_by, a row absent there, or where a built source names an
  identifier absent there.

Usage: py -3 tools/decomp_globals.py <game> check [--version ID ...]
       py -3 tools/decomp_globals.py <game> propose <function> [...]   (name of the functions table or reference
                                                                       address: candidate rows, US and PAL paired
                                                                       by instruction position)
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import decomp_common as dc  # noqa: E402
import toolchain as tc  # noqa: E402

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
PLACEHOLDER = re.compile(r"^todo", re.IGNORECASE)
ABSENT = ("", "-")
KINDS = ("data", "const", "vtable", "callback")
STATUSES = ("verified", "hypothesis")
SDA_SPAN = 0x8000  # a signed 16-bit displacement reaches [base - 0x8000, base + 0x8000)

# D-form instructions (primary opcode) that address memory or form an address as d(rA): addi (14), the integer and
# floating-point loads and stores, with their update forms.
DFORM = frozenset({14, *range(32, 56)})
UPDATE_FORMS = frozenset({33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55})  # lwzu lbzu stwu stbu lhzu lhau sthu lfsu...
LOADS_TO_GPR = frozenset({14, 32, 33, 34, 35, 40, 41, 42, 43, 46})  # rD written (46 lmw: from rD on)
ACCESS_SIZE = {32: 4, 33: 4, 34: 1, 35: 1, 36: 4, 37: 4, 38: 1, 39: 1, 40: 2, 41: 2, 42: 2, 43: 2, 44: 2, 45: 2,
               46: 4, 47: 4, 48: 4, 49: 4, 50: 8, 51: 8, 52: 4, 53: 4, 54: 8, 55: 8}
MNEMONIC = {14: "addi", 32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
            40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw", 48: "lfs",
            49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu"}


@dataclass
class Row:
    id: str            # "" = no identifier
    name: str
    addresses: dict[str, int | None]
    size: int
    section: str
    kind: str
    used_by: list[str]
    status: str
    evidence: str
    line: int


@dataclass
class Ref:
    """One reference of a function's original code to a global address."""
    offset: int        # instruction offset from the function start
    address: int       # target address
    how: str           # "r13", "r2", "abs"
    text: str          # e.g. "lwz r13-0x6530"
    group: int = -1    # absolute references from the same lis (one object reached through one base): same group
    width: int = 0     # bytes accessed (0: addi, an address only)


@dataclass
class Layout:
    """Sections of one version's DOL: name -> (start, end, bytes or None for BSS), and the small-data bases."""
    sections: dict[str, tuple[int, int, bytes | None]]
    r13: int | None
    r2: int | None
    text: list[tuple[int, bytes]] = field(default_factory=list)

    def section_of(self, address: int) -> str | None:
        for name, (lo, hi, _) in self.sections.items():
            if lo <= address < hi:
                return name
        return None

    def read(self, address: int, size: int) -> bytes | None:
        for lo, hi, blob in self.sections.values():
            if blob is not None and lo <= address and address + size <= hi:
                return blob[address - lo:address - lo + size]
        return None

    def in_image(self, address: int) -> bool:
        return self.section_of(address) is not None


# ---------------------------------------------------------------------------------------------------------------
# Reading
# ---------------------------------------------------------------------------------------------------------------

def table_path(game_cfg: dict) -> Path | None:
    rel = game_cfg.get("decomp", {}).get("globals")
    return game_cfg["_dir"] / rel if rel else None


def version_columns(game_cfg: dict) -> dict[str, str]:
    """{version ID: column name}: the address columns of the functions table ([decomp.columns])."""
    columns = dict(game_cfg.get("decomp", {}).get("columns", {}))
    versions = {v["game_id"] for v in game_cfg.get("versions", [])}
    if set(columns) != versions:
        raise SystemExit(f"game.toml [decomp.columns]: one column per version expected ({sorted(versions)}), "
                         f"got {sorted(columns)}")
    return columns


def read_table(game_cfg: dict) -> list[Row]:
    """Rows of the globals table ([] if the game has none). Refuses a malformed table, or a table named by game.toml
    ([decomp] globals) that does not exist (SystemExit): read as empty, it would let a build pass without the checks
    and give every global the address 0."""
    path = table_path(game_cfg)
    if path is None:
        return []
    if not path.exists():
        raise SystemExit(f"{path}: globals table named by game.toml ([decomp] globals) not found")
    columns = version_columns(game_cfg)
    header = None
    rows: list[Row] = []
    errors: list[str] = []
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        cells = raw.split("\t")
        if header is None:
            header = cells
            need = ["id", "name", *columns.values(), "size", "section", "kind", "used_by", "status", "evidence"]
            missing = [c for c in need if c not in header]
            if missing:
                raise SystemExit(f"{path}:{n}: missing columns {missing} (header: {header})")
            continue
        if len(cells) != len(header):
            errors.append(f"{path.name}:{n}: {len(cells)} cells, {len(header)} columns")
            continue
        r = dict(zip(header, cells))
        ident = r["id"].strip()
        ident = "" if ident in ABSENT else ident
        addresses: dict[str, int | None] = {}
        for version, column in columns.items():
            value = r[column].strip()
            try:
                addresses[version] = None if value in ABSENT else int(value, 16)
            except ValueError:
                errors.append(f"{path.name}:{n}: {column} {value!r} is not a hexadecimal address")
                addresses[version] = None
        try:
            size = int(r["size"], 16)
        except ValueError:
            errors.append(f"{path.name}:{n}: size {r['size']!r} is not hexadecimal")
            size = 0
        if size <= 0:
            errors.append(f"{path.name}:{n}: size must be positive")
        kind, status = r["kind"].strip(), r["status"].strip().lower()
        if kind not in KINDS:
            errors.append(f"{path.name}:{n}: kind {kind!r} (expected one of {', '.join(KINDS)})")
        if status not in STATUSES:
            errors.append(f"{path.name}:{n}: status {r['status']!r} (expected one of {', '.join(STATUSES)})")
        used = [u.strip() for u in r["used_by"].split(",") if u.strip() and u.strip() not in ABSENT]
        rows.append(Row(ident, r["name"].strip(), addresses, size, r["section"].strip(), kind, used, status,
                        r["evidence"].strip(), n))
    seen: dict[str, int] = {}
    for row in rows:
        if not row.id:
            continue
        if not IDENT.match(row.id):
            errors.append(f"{path.name}:{row.line}: id {row.id!r} is not a C identifier")
        elif PLACEHOLDER.match(row.id) or row.id.startswith("__"):
            errors.append(f"{path.name}:{row.line}: id {row.id!r} is a placeholder or a reserved name: give the "
                          f"global a meaningful name")
        if row.id in seen:
            errors.append(f"{path.name}:{row.line}: id {row.id} already used line {seen[row.id]}")
        seen[row.id] = row.line
    if errors:
        raise SystemExit("Table of globals refused:\n  " + "\n  ".join(errors))
    return rows


def identifiers(rows: list[Row]) -> list[Row]:
    """Rows with an identifier, in table order: the enum value of a row is its position in this list."""
    return [r for r in rows if r.id]


# ---------------------------------------------------------------------------------------------------------------
# DOL layout
# ---------------------------------------------------------------------------------------------------------------

def layout(game_cfg: dict, dol_path: Path) -> Layout:
    """Sections of a DOL, named by [decomp.dol_sections] and [decomp] dol_bss_sections, and its small-data bases.
    Refuses a configuration that does not fit this DOL (SystemExit)."""
    import gcinfo  # noqa: E402
    import dol2elf  # noqa: E402  (find_sda_bases: r13 and r2 as the boot code sets them)
    d = game_cfg.get("decomp", {})
    names = dict(d.get("dol_sections", {}))
    bss_names = list(d.get("dol_bss_sections", []))
    if not names:
        raise SystemExit("game.toml: [decomp.dol_sections] is required for the globals table")
    data = dol_path.read_bytes()
    info = gcinfo.describe_dol(data)
    segs = {s["name"]: s for s in info["segments"]}
    unknown = sorted(set(names) - set(segs))
    if unknown:
        raise SystemExit(f"[decomp.dol_sections]: {', '.join(unknown)} not in {dol_path.name}")
    sections: dict[str, tuple[int, int, bytes | None]] = {}
    for slot, s in sorted(segs.items(), key=lambda kv: kv[1]["addr"]):
        name = names.get(slot, slot)
        sections[name] = (s["addr"], s["addr"] + s["size"], data[s["offset"]:s["offset"] + s["size"]])
    # BSS range minus the data sections inside it, in address order.
    lo, hi = info["bss"], info["bss"] + info["bss_size"]
    pieces, cur = [], lo
    for s in sorted(segs.values(), key=lambda s: s["addr"]):
        a, b = s["addr"], s["addr"] + s["size"]
        if b <= cur or a >= hi:
            continue
        if a > cur:
            pieces.append((cur, a))
        cur = max(cur, b)
    if cur < hi:
        pieces.append((cur, hi))
    if bss_names and len(pieces) != len(bss_names):
        raise SystemExit(f"[decomp] dol_bss_sections names {len(bss_names)} BSS pieces, {dol_path.name} has "
                         f"{len(pieces)} ({', '.join(f'{a:08X}-{b:08X}' for a, b in pieces)})")
    for i, (a, b) in enumerate(pieces):
        sections[bss_names[i] if bss_names else f"bss{i}"] = (a, b, None)
    bases = dol2elf.find_sda_bases(dol2elf.DolImage(data))
    out = Layout(sections, bases.get(13), bases.get(2),
                 [(s["addr"], data[s["offset"]:s["offset"] + s["size"]])
                  for s in info["segments"] if s["name"].startswith("text")])
    # The small-data bases point 0x8000 past the start of .sdata / .sdata2 (CodeWarrior's _SDA_BASE_ /
    # _SDA2_BASE_): a configuration that names other sections .sdata / .sdata2 is refused.
    for reg, base, want in ((13, out.r13, ".sdata"), (2, out.r2, ".sdata2")):
        if want in sections and (base is None or sections[want][0] + SDA_SPAN != base):
            raise SystemExit(f"[decomp.dol_sections] of {dol_path.name}: r{reg} = "
                             f"{'not found' if base is None else f'{base:08X}'} in the boot code, but {want} starts "
                             f"at {sections[want][0]:08X}: expected r{reg} = start + 0x8000")
    return out


# ---------------------------------------------------------------------------------------------------------------
# Code scanner
# ---------------------------------------------------------------------------------------------------------------

def _words(text: list[tuple[int, bytes]], address: int, size: int) -> list[int] | None:
    for base, blob in text:
        if base <= address and address + size <= base + len(blob):
            o = address - base
            return list(struct.unpack_from(f">{size // 4}I", blob, o))
    return None


def _sext16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def references(lay: Layout, address: int, size: int) -> list[Ref]:
    """References of the code [address, address + size) to global addresses: d(r13), d(r2), and absolute addresses
    built by `lis rX, hi` then a D-form instruction on rX (addi, load, store, with update forms followed). The scan is
    linear (branches are not followed): a register set by lis stays known until a D-form instruction overwrites it
    or another lis does; any other instruction writing it is not seen [hypothesis: enough for the compiler's
    lis/addi and lis/lwz pairs, which are adjacent or close]."""
    words = _words(lay.text, address, size)
    if words is None:
        raise SystemExit(f"{address:08X}+0x{size:X}: outside the code sections")
    out: list[Ref] = []
    hi: dict[int, int] = {}
    group: dict[int, int] = {}
    for i, w in enumerate(words):
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:  # lis rD, imm
            hi[rd] = (imm << 16) & 0xFFFFFFFF
            group[rd] = 4 * i
            continue
        if op not in DFORM:
            continue
        mnem = MNEMONIC[op]
        if ra in (2, 13):
            base = lay.r13 if ra == 13 else lay.r2
            if base is not None:
                d = _sext16(imm)
                out.append(Ref(4 * i, (base + d) & 0xFFFFFFFF, f"r{ra}",
                               f"{mnem} r{ra}{'-' if d < 0 else '+'}0x{abs(d):X}", -1, ACCESS_SIZE.get(op, 0)))
        elif ra != 0 and ra in hi:
            target = (hi[ra] + _sext16(imm)) & 0xFFFFFFFF
            out.append(Ref(4 * i, target, "abs", f"lis+{mnem} {target:08X}", group[ra], ACCESS_SIZE.get(op, 0)))
            if op in UPDATE_FORMS:
                hi[ra] = target  # the base register now holds the effective address
        if op in LOADS_TO_GPR:
            hi.pop(rd, None)
            if op == 46:
                for r in range(rd, 32):
                    hi.pop(r, None)
    return out


# ---------------------------------------------------------------------------------------------------------------
# Function ranges (used_by)
# ---------------------------------------------------------------------------------------------------------------

class Functions:
    """Address and size, in each version, of a used_by function: a row of the functions table, or a reference
    address known to the symbol sources (size and pairing as tools/decomp_functions.py establishes them)."""

    def __init__(self, game_cfg: dict):
        import decomp_functions as df  # noqa: E402
        self.cfg = game_cfg
        self.df = df
        self.table = {f.name: f for f in df.read_table(game_cfg)}
        self.ref = game_cfg.get("decomp", {}).get("reference") or game_cfg.get("symbols", {}).get("reference")
        self._known: dict[str, object] = {}

    def known(self, version: str):
        if version not in self._known:
            self._known[version] = self.df.known_entries(self.cfg, version)
        return self._known[version]

    def range(self, name: str, version: str) -> tuple[int, int] | str:
        """(address, size) of `name` in `version`, or the reason it is not established."""
        f = self.table.get(name)
        if f is not None:
            if f.module != "dol":
                return f"{name} is in module {f.module}: REL globals are not handled by this table yet"
            a = f.addresses.get(version)
            return (a, f.size) if a is not None else f"{name} has no {version} address in the functions table"
        try:
            ref_addr = int(name, 16)
        except ValueError:
            return f"{name} is neither a row of the functions table nor a {self.ref} address"
        k = self.known(version)
        if version == self.ref:
            addr = ref_addr
        else:
            addr, how = dc.paired_address(ref_addr, k.trans, k.gaps_by_ref)
            if addr is None or dc.is_gated(how):
                return f"{self.ref} {ref_addr:08X} has no established {version} address ({how})"
        sizes = {s for s in k.sizes.get(("dol", addr), {}).values() if s}
        if len(sizes) != 1:
            return f"{version} {addr:08X}: size not established by the symbol sources ({sorted(sizes) or 'none'})"
        return addr, sizes.pop()


# ---------------------------------------------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------------------------------------------

@dataclass
class Result:
    """Outcome of the checks of one version."""
    version: str
    addresses: dict[str, int]                 # id -> address (established rows with an identifier)
    absent: set[str]                          # ids without an address in this version
    errors: list[str]
    notes: list[str]


def _pairs(fns: Functions, version: str, code: int) -> tuple[int, str] | None:
    """(reference-version address, how) of the function starting at `code` in `version`, by the transposition."""
    k = fns.known(version)
    if version == fns.ref:
        return (code, "reference") if ("dol", code) in k.sizes else None
    for ref_addr, t in k.trans.items():
        if t.target == code:
            want, how = dc.paired_address(ref_addr, k.trans, k.gaps_by_ref)
            if want == code and not dc.is_gated(how):
                return ref_addr, how
    for ref_addr, g in k.gaps_by_ref.items():
        if g.target == code and g.level in dc.ESTABLISHED_LEVELS:
            return ref_addr, f"gap-{g.level}"
    return None


def _check_contents(row: Row, version: str, lays: dict[str, Layout], rows: list[Row], fns: Functions,
                    addr: dict[str, int]) -> str | None:
    """Rule 3 for `version` against the reference version (None: identical or explained word by word)."""
    ref = fns.ref
    if version == ref or row.kind == "callback" or lays[version].sections[row.section][2] is None:
        return None  # a callback is checked by the transposition (its code may differ by relocations only)
    a, b = row.addresses[ref], row.addresses[version]
    if a is None:
        return f"no {ref} address to compare the contents with"
    x, y = lays[ref].read(a, row.size), lays[version].read(b, row.size)
    if x is None or y is None:
        return "contents not readable"
    if row.kind == "const":
        if x != y:
            diff = next(i for i in range(row.size) if x[i] != y[i])
            return f"contents differ from {ref} at +0x{diff:X} (kind const: same bytes required)"
        return None
    if row.size % 4 or a % 4 or b % 4:
        if x != y:
            return f"contents differ from {ref} (unaligned row: compared byte by byte)"
        return None
    for off in range(0, row.size, 4):
        wa, wb = struct.unpack_from(">I", x, off)[0], struct.unpack_from(">I", y, off)[0]
        if wa == wb:
            continue
        if not (lays[ref].in_image(wa) and lays[version].in_image(wb)):
            return f"word +0x{off:X} differs from {ref} ({wa:08X} / {wb:08X}) and is not a pointer in both versions"
        # A pointer to another row of the table: the same row in both versions.
        same_row = [r for r in rows if r.addresses.get(ref) == wa and r.addresses.get(version) == wb]
        if same_row:
            continue
        sa, sb = lays[ref].section_of(wa), lays[version].section_of(wb)
        if sa != sb:
            return f"word +0x{off:X}: pointers into different sections ({sa} / {sb})"
        if lays[ref].sections[sa][0] in [lo for lo, _ in lays[ref].text]:
            paired = _pairs(fns, version, wb)
            if paired is None or paired[0] != wa:
                return (f"word +0x{off:X}: code pointer {wb:08X} does not pair with {ref} {wa:08X} through the "
                        f"transposition")
            continue
        if row.kind == "vtable" and off > 0:
            pass  # a data pointer in a vtable slot other than the type descriptor: masked like any data pointer
    return None


def check_version(game_cfg: dict, version: str, dol_path: Path, ref_dol_path: Path | None = None,
                  rows: list[Row] | None = None, fns: Functions | None = None,
                  compare_reference: bool = True) -> Result:
    """Rules 1 to 4 of the header for `version`. `ref_dol_path`: the reference version's DOL (contents and
    instruction positions are compared with it); None when `version` is the reference. `compare_reference` False
    (build without the reference disc, check_build): the comparisons with the reference (rule 3 and the instruction
    positions of rule 4) are skipped, the other rules still apply."""
    rows = read_table(game_cfg) if rows is None else rows
    fns = fns or Functions(game_cfg)
    ref = fns.ref
    lays = {version: layout(game_cfg, dol_path)}
    compare = version != ref and compare_reference
    if compare:
        if ref_dol_path is None:
            raise SystemExit(f"check_version({version}): the {ref} DOL is needed to compare with the reference")
        lays[ref] = layout(game_cfg, ref_dol_path)
    lay = lays[version]
    errors: list[str] = []
    notes: list[str] = []
    addresses: dict[str, int] = {}
    absent: set[str] = set()
    placed: list[tuple[int, int, Row]] = []
    for row in rows:
        where = f"{row.id or row.name} (line {row.line})"
        a = row.addresses.get(version)
        if a is None:
            if row.id:
                absent.add(row.id)
            continue
        reasons: list[str] = []
        if row.section not in lay.sections:
            reasons.append(f"section {row.section!r} unknown (sections: {', '.join(lay.sections)})")
        else:
            lo, hi, blob = lay.sections[row.section]
            if not (lo <= a and a + row.size <= hi):
                reasons.append(f"{a:08X}+0x{row.size:X} is not inside {row.section} ({lo:08X}-{hi:08X}; the "
                               f"address is in {lay.section_of(a) or 'no section'})")
            is_text = lo in [t for t, _ in lay.text]
            if row.kind == "callback" and not is_text:
                reasons.append(f"kind callback in {row.section}, not a code section")
            if row.kind != "callback" and is_text:
                reasons.append(f"kind {row.kind} in the code section {row.section}")
            if row.kind in ("const", "vtable") and blob is None:
                reasons.append(f"kind {row.kind} in the BSS section {row.section}")
        if not reasons and row.kind == "callback":
            paired = _pairs(fns, version, a)
            if paired is None:
                reasons.append(f"{a:08X} is not an established function start of {version}")
            elif version != ref and paired[0] != row.addresses.get(ref):
                reasons.append(f"{a:08X} is the {version} address of {ref} {paired[0]:08X}, not of "
                               f"{row.addresses.get(ref) or 0:08X}")
        if not reasons and (compare or version == ref):
            why = _check_contents(row, version, lays, rows, fns, addresses)
            if why:
                reasons.append(why)
        if not reasons:
            for user in row.used_by:
                here = fns.range(user, version)
                if isinstance(here, str):
                    reasons.append(f"used_by {user}: {here}")
                    continue
                refs = [r for r in references(lay, *here) if a <= r.address < a + row.size]
                if not refs:
                    reasons.append(f"used_by {user}: its {version} code ({here[0]:08X}+0x{here[1]:X}) references no "
                                   f"address of {a:08X}+0x{row.size:X}")
                    continue
                if compare:
                    there = fns.range(user, ref)
                    ra = row.addresses.get(ref)
                    if isinstance(there, str) or ra is None:
                        reasons.append(f"used_by {user}: no {ref} counterpart to compare instruction positions")
                        continue
                    ref_refs = [r for r in references(lays[ref], *there) if ra <= r.address < ra + row.size]
                    mine = [(r.offset, r.address - a) for r in refs]
                    theirs = [(r.offset, r.address - ra) for r in ref_refs]
                    if mine != theirs:
                        reasons.append(f"used_by {user}: the references differ from {ref} (instruction offset, "
                                       f"offset in the global: {version} {mine}, {ref} {theirs})")
                        continue
                    notes.append(f"{where}: {user} {version} {', '.join(r.text for r in refs)} = {ref} "
                                 f"{', '.join(r.text for r in ref_refs)} (same instruction offsets)")
        if reasons:
            errors.append(f"{where} {version} {a:08X}: " + "; ".join(reasons))
            continue
        placed.append((a, a + row.size, row))
        if row.id:
            addresses[row.id] = a
    placed.sort(key=lambda p: p[0])
    for (a0, a1, r0), (b0, _, r1) in zip(placed, placed[1:]):
        if b0 < a1:
            errors.append(f"{r0.id or r0.name} and {r1.id or r1.name} overlap in {version} ({a0:08X}-{a1:08X}, "
                          f"{b0:08X})")
    return Result(version, addresses, absent, errors, notes)


def check_build(game_cfg: dict, version: str, disc_root: Path, enabled: list, sources: list[Path]) -> Result:
    """Checks of one version for tools/build.py: rules 1-4, plus every used_by function enabled in this build
    (rows of the functions table, `enabled`) and every identifier named by a built source (`global_id::<id>` in
    `sources`) must have an address in this version. Refuses the build (SystemExit) otherwise."""
    rows = read_table(game_cfg)
    fns = Functions(game_cfg)
    dol = disc_root / game_cfg["code"]["dol"]
    ref_dol = None
    compare = True
    if version != fns.ref:
        ref_root = _disc_root(game_cfg, fns.ref, required=False)
        ref_dol = ref_root / game_cfg["code"]["dol"] if ref_root else None
        if ref_dol is None or not ref_dol.exists():
            # Fallback (a contributor with only this version's disc): the version is checked on its own; the
            # comparison with the reference (contents, instruction positions) is left to a build that has both discs.
            compare = False
            print(f"== WARNING: {fns.ref} disc not extracted: the globals of {version} are checked without the "
                  f"comparison with {fns.ref} (contents and instruction positions); a build with both discs makes "
                  f"it (tools/extract.py {game_cfg['_dir'].name} --version {fns.ref})")
    res = check_version(game_cfg, version, dol, ref_dol, rows, fns, compare_reference=compare)
    names = {f.name for f in enabled}
    for row in rows:
        if row.addresses.get(version) is None:
            users = sorted(set(row.used_by) & names)
            if users:
                res.errors.append(f"{row.id or row.name} has no {version} address, but {', '.join(users)} (enabled) "
                                  f"uses it")
    used = source_identifiers(sources)
    for ident, files in sorted(used.items()):
        if ident in res.absent:
            res.errors.append(f"global_id::{ident} (in {', '.join(files)}) has no {version} address")
        elif ident not in res.addresses and ident not in {r.id for r in rows}:
            res.errors.append(f"global_id::{ident} (in {', '.join(files)}) is not a row of the globals table")
    if res.errors:
        raise SystemExit(f"Globals table refused for {version}:\n  " + "\n  ".join(res.errors))
    return res


SOURCE_ID = re.compile(r"\bglobal_id::([A-Za-z_][A-Za-z0-9_]*)")


def source_identifiers(sources: list[Path]) -> dict[str, list[str]]:
    """{identifier: [file names]} of the `global_id::<id>` written in `sources`."""
    out: dict[str, list[str]] = {}
    for src in sources:
        for m in SOURCE_ID.finditer(src.read_text(encoding="utf-8")):
            names = out.setdefault(m.group(1), [])
            if src.name not in names:
                names.append(src.name)
    return out


def _disc_root(game_cfg: dict, version: str, required: bool = True) -> Path | None:
    """Folder of the extracted disc of `version` (its manifest). Missing: SystemExit, or None if not `required`."""
    import json
    manifest = tc.GENERATED / game_cfg["_dir"].name / version / "manifest.json"
    if not manifest.exists():
        if not required:
            return None
        raise SystemExit(f"{manifest} missing: extract the {version} disc first (tools/extract.py); the globals of "
                         f"every version are compared with the reference version")
    return Path(json.loads(manifest.read_text(encoding="utf-8"))["disc_root"])


# ---------------------------------------------------------------------------------------------------------------
# Generation (build folder only)
# ---------------------------------------------------------------------------------------------------------------

def header_source(rows: list[Row]) -> str:
    """decomp_globals.h: the identifiers only (no address), the same for every version."""
    lines = ["// Identifiers of the game globals (tools/decomp_globals.py, from the globals table): generated by",
             "// tools/build.py into the build folder. Names only; the addresses of the version being built are in",
             "// decomp_registry.cpp (gc::set_global_table).",
             "#pragma once", "#include <cstdint>", "",
             "enum class global_id : std::uint32_t {"]
    for i, row in enumerate(identifiers(rows)):
        lines.append(f"    {row.id} = {i}, // {row.name}, {row.section}, 0x{row.size:X} bytes")
    lines += ["};", ""]
    return "\n".join(lines)


def table_source(version: str, rows: list[Row], res: Result) -> list[str]:
    """Lines of decomp_registry.cpp: the address array of `version`, indexed by identifier (0 = absent)."""
    ids = identifiers(rows)
    values = [f"0x{res.addresses[r.id]:08X}u" if r.id in res.addresses else "0u" for r in ids]
    out = [f"// Addresses of the game globals in {version} (tools/decomp_globals.py), indexed by global_id."]
    out.append("const std::uint32_t k_globals[] = {" + ", ".join(values + ["0u"]) + "}; // final 0: never empty")
    out.append(f"constexpr std::size_t k_global_count = {len(ids)};")
    return out


# ---------------------------------------------------------------------------------------------------------------
# propose
# ---------------------------------------------------------------------------------------------------------------

def propose(game_cfg: dict, function: str) -> list[str]:
    """Candidate rows for the globals that `function`'s original code references, paired between the reference
    version and the others by instruction position."""
    fns = Functions(game_cfg)
    rows = read_table(game_cfg)
    columns = version_columns(game_cfg)
    versions = [fns.ref] + [v for v in columns if v != fns.ref]
    lays = {v: layout(game_cfg, _disc_root(game_cfg, v) / game_cfg["code"]["dol"]) for v in versions}
    ranges = {v: fns.range(function, v) for v in versions}
    for v, r in ranges.items():
        if isinstance(r, str):
            raise SystemExit(f"propose {function}: {r}")
    refs = {v: references(lays[v], *ranges[v]) for v in versions}
    out = [f"# {function}: " + ", ".join(f"{v} {ranges[v][0]:08X}+0x{ranges[v][1]:X}" for v in versions)]
    by_offset = {v: {r.offset: r for r in refs[v]} for v in versions}
    ref_words = _words(lays[fns.ref].text, *ranges[fns.ref])
    # One candidate per object: the absolute references made from the same lis (fields of one object reached through
    # one base register), or each small-data address.
    groups: dict[tuple, list[Ref]] = {}
    for r in refs[fns.ref]:
        groups.setdefault(("abs", r.group) if r.how == "abs" else (r.how, r.address), []).append(r)
    placed: set[int] = set()
    for members in groups.values():
        start = min(r.address for r in members)
        end = max(r.address + max(r.width, 1) for r in members)
        cells, texts, notes = [], [], []
        for v in versions:
            paired = [by_offset[v].get(r.offset) for r in members]
            if any(x is None or x.how != members[0].how for x in paired):
                cells.append("-")
                notes.append(f"no {v} counterpart at the same instruction offsets")
                continue
            vstart = min(x.address for x in paired)
            if [x.address - vstart for x in paired] != [r.address - start for r in members]:
                cells.append("-")
                notes.append(f"{v}: the offsets inside the object differ")
                continue
            # Same instructions with the displacement masked: the pairing is positional.
            same = all(((_words(lays[v].text, ranges[v][0] + r.offset, 4) or [0])[0] ^ ref_words[r.offset // 4])
                       & 0xFFFF0000 == 0 for r in members)
            if not same:
                notes.append(f"{v}: the instructions differ beyond the displacement")
            cells.append(f"{vstart:08X}")
            texts.append(f"{', '.join(x.text for x in paired)} ({v})")
        evidence = (f"{function} +" + ", +".join(f"0x{r.offset:X}" for r in members) + ": " + " / ".join(texts)
                    + "; positional pairing, same instructions with the displacement masked [hypothesis]"
                    + "".join(f"; {n}" for n in notes))
        if lays[fns.ref].section_of(start) is None:
            out.append(f"# not an address of the DOL (a constant built by lis, written as a literal): {evidence}")
            continue
        existing = [x for x in rows if x.addresses.get(fns.ref) is not None
                    and x.addresses[fns.ref] < end and start < x.addresses[fns.ref] + x.size]
        if existing:
            out.append(f"# already in the table: {existing[0].id or existing[0].name} (line {existing[0].line}); "
                       f"{evidence}")
            continue
        if start in placed:
            out.append(f"# same address as above: {evidence}")
            continue
        placed.add(start)
        sec = lays[fns.ref].section_of(start) or "?"
        kind = "callback" if sec in (".text", ".init") else ("const" if sec in (".rodata", ".sdata2") else "data")
        size = end - start if any(r.width for r in members) else 0
        out.append("	".join(["todo_name", "?", *cells, f"{size:X}" if size else "?", sec, kind, function,
                              "hypothesis", evidence]))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="Checks the globals table and proposes rows for a function.")
    parser.add_argument("game")
    parser.add_argument("action", choices=["check", "propose"])
    parser.add_argument("functions", nargs="*", help="propose: names of the functions table or reference addresses")
    parser.add_argument("--version", action="append", help="check: version ID (repeatable; default: all)")
    parser.add_argument("-v", "--verbose", action="store_true", help="check: print the instruction comparisons")
    args = parser.parse_args()
    game_cfg = tc.load_game(args.game)
    if args.action == "propose":
        if not args.functions:
            parser.error("propose: give at least one function")
        for f in args.functions:
            print("\n".join(propose(game_cfg, f)))
        return 0
    rows = read_table(game_cfg)
    fns = Functions(game_cfg)
    versions = args.version or [v["game_id"] for v in game_cfg["versions"]]
    failed = False
    ref_dol = _disc_root(game_cfg, fns.ref) / game_cfg["code"]["dol"]
    print(f"{len(rows)} rows, {len(identifiers(rows))} with an identifier")
    for v in versions:
        res = check_version(game_cfg, v, _disc_root(game_cfg, v) / game_cfg["code"]["dol"],
                            None if v == fns.ref else ref_dol, rows, fns)
        print(f"{v}: {len(res.addresses)} identifiers placed, {len(res.absent)} absent, {len(res.errors)} refused")
        for e in res.errors:
            print(f"  REFUSED {e}")
        if args.verbose:
            for n in res.notes:
                print(f"  {n}")
        failed |= bool(res.errors)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
