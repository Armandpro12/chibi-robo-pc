"""What the rows of the table of decompiled functions rest on (generic; parameters in [decomp] of game.toml).

Both checks come from REA (MIT; only the idea, this is our own code): the states of its function matching
(docs/native-function-matching.md) and a proof that applies to the digest of what it was about
(docs/reconstruction-obligation-ledgers.md).

Read by tools/decomp_functions.py check --parity and tools/decomp_progress.py --parity / --proof; tools/decomp_oracle.py
report --promote writes the digests. This file is not in ORACLE_HASHED_TOOLS: it decides nothing an oracle run measures,
so changing a report here keeps the oracle results valid.

Version parity. Each row's code in the reference version is compared with its code in every other version, after
hiding the fields that depend on addresses. The mask of tools/symbols.py keep_masks hides too much for this: a register
stays marked once a `lis` wrote it, so a field offset or an array stride on it is hidden long after the address was
complete. compared_words shows those again, from a flow analysis of the function (a register is the high half of an
address if one path says so), and shows nothing else: every word keep_masks tells apart stays apart. keep_masks stays as
it is because the signatures of tools/sdksig.py and the transpositions of tools/symbols.py rest on it.
  same     the compared words are equal in every version
  differs  a compared word differs in a version, or a constant read through r2 does (DOL rows: its value, not its
           address, tools/decomp_globals.py references). Only r2: the address is exact there, whereas that scan can take
           a register rewritten since its `lis` for a `lis` base (two false constants found by hand, 2026-11-02)
  unknown  the comparison could not be made (no address in a version, disc not extracted, range outside the code)
Never read `same` as "the same behavior", and read the share of `differs` as a floor: see PARITY_IGNORES. REL rows are
compared in the REL files, relocations not applied (the words they fill hold zeroes), without the constants.
  A `differs` asks for a version condition in the C++ (a comment `version-differs: <name>` in the row's source unit: the
file of its export and the headers it includes, which is how this tool finds it) or a reason in the table below; without
one it is a warning, and so is an `unknown` on an enabled row. check --parity --strict refuses them on enabled rows,
except an `unknown` that only says a disc is not extracted (notes/decisions.md, 2026-11-02).
  [decomp] version_parity (optional, relative to games/<game>/) names a tracked table, with no game data: one line per
row, columns `name`, `batch` and `reason` ("-" or empty = none). A row that is not in it has no known batch.

Proof digests. A row is `verified` because a full oracle report promoted it (tools/decomp_oracle.py report --promote);
nothing in the table says which code that report was about. So the promotion also records, in a local file that Git does
not track ([decomp] proof_digests, relative to the checkout that holds the tests, {slug} replaced), one line per
function and version:
  unit    SHA-256 of the row (name, module, address in that version, size) and of the function's source unit: the file
          that holds its GC_DECOMP_EXPORT and the headers it includes from the decompiled folders, transitively
  common  SHA-1 of what every function shares: oracle_hash_inputs of tools/decomp_functions.py without the units and
          without the rows of the table, plus the checkout of the recompiler, [decomp.no_return] and the globals table.
          Not oracle_sources_hash: that one holds every unit and every row, so one changed line would touch all
  sources oracle_sources_hash of the promoted results and the labels: where the line comes from, never compared
The file only grows: a line is about one state of the code, so going back to that code finds its proof again.
State of a `verified` row in a version:
  fresh         a line holds its current unit and common digests
  stale-unit    no line holds its current unit digest, but it has lines: its code or its row changed
  stale-shared  a line holds its unit digest, none with the current common digest: a shared input changed
  unknown       nothing to compare: no local file, a function never recorded, an unreadable line, no source unit
A missing file reads as unknown, never as fresh (a fresh clone has none). A digest never changes the table: the status
stays promoted by the full report only. See PROOF_LIMITS for what `fresh` does not say. decomp_progress.py --proof
--require-fresh refuses a `stale-unit`; `stale-shared` and `unknown` stay reports (they block the close of a batch or a
release, not a merge: notes/decisions.md, 2026-11-02).

No code of the game is written anywhere: only states, counts, names, addresses and hashes.
"""

from __future__ import annotations

import hashlib
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import decomp_functions as df  # noqa: E402
import symbols as sy  # noqa: E402
import toolchain as tc  # noqa: E402

SAME, DIFFERS, UNKNOWN = "same", "differs", "unknown"
FRESH, STALE_UNIT, STALE_SHARED = "fresh", "stale-unit", "stale-shared"
NO_BATCH = "(no batch)"
EXPORT = re.compile(r"^[ \t]*GC_DECOMP_EXPORT\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,", re.M)
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)
VERSION_MARK = re.compile(r"//.*\bversion-differs:\s*([A-Za-z_][A-Za-z0-9_]*)")
SHA1 = re.compile(r"^[0-9a-f]{40}$")
SHA256 = re.compile(r"^[0-9a-f]{64}$")
DIGEST_COLUMNS = ["name", "version", "unit", "common", "sources", "labels"]
NO_DISC = "disc not extracted"
VOLATILE = sum(1 << r for r in (0, *range(3, 13)))  # registers a call may change (PowerPC EABI)
PARITY_IGNORES = [
    "callees and taken addresses: a call, a string or a table can differ behind the same words",
    "writable data: what a function reads from .data, .sdata or .bss",
    "indexed accesses (lwzx and the like): the offset is in a register",
    ".rodata loaded through `lis`, and any address or integer constant built from `lis`: its halves are hidden",
    "a field offset added to the low half of an address: it is hidden with it",
    "a register that holds a high half on one path only: it stays hidden after the join",
]
PROOF_LIMITS = [
    "fresh does not cover callees in other units, the cases of the oracle, or a proof revoked since",
    "the digest file is an aid, not evidence: the gate is the oracle report",
]


# Words compared between the versions


def successors(word: int, i: int, n: int, start: int, size: int) -> list[int]:
    """Words that can run after word `i` of a function of `n` words, same edges as tools/decomp_functions.py code_blocks:
    a branch out of the function, blr, bctr and rfi end the path; a call comes back. Not its proven jump tables: the
    cases of a bctr are the code no other path reaches (high_half_registers)."""
    op = word >> 26
    next_word = [i + 1] if i + 1 < n else []
    if op in (16, 18):
        sign = 0x8000 if op == 16 else 0x02000000
        off = word & (2 * sign - 4)
        off -= 2 * sign if off & sign else 0
        target = off if word & 2 else start + 4 * i + off
        inside = [(target - start) // 4] if start <= target < start + size else []
        if word & 1:
            return next_word
        return inside if op == 18 or (word >> 21) & 0x14 == 0x14 else inside + next_word
    if op == 19 and (word >> 1) & 0x3FF in (16, 50, 528):
        ends = (word >> 1) & 0x3FF == 50 or (not word & 1 and (word >> 21) & 0x14 == 0x14)
        return [] if ends else next_word
    return next_word


def high_halves_after(word: int, bases: int) -> int:
    """`bases` (bit n: rn can hold the high half of an address) after `word`. An instruction not modelled by
    tools/decomp_functions.py _gpr_written writes nothing here: its register stays hidden, as with keep_masks."""
    op, rd, ra, xo = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 1) & 0x3FF
    if op == 15:  # lis, addis: still a high half; addi and ori complete it, through _gpr_written
        return bases | 1 << rd if ra == 0 or bases >> ra & 1 else bases & ~(1 << rd)
    if op == 31 and xo == 444 and rd == (word >> 11) & 31:  # mr rA, rS
        return bases | 1 << ra if bases >> rd & 1 else bases & ~(1 << ra)
    if word & 1 and (op in (16, 18) or (op == 19 and xo in (16, 528))):
        return bases & ~VOLATILE
    for register in df._gpr_written(word) or ():
        bases &= ~(1 << register)
    return bases


def high_half_registers(words: list[int], start: int, size: int) -> list[int]:
    """Before each word, the registers that can hold the high half of an address, on any path: forward flow to a fixed
    point, a join keeps what one path says, so a join never shows a low half. A linear scan would, each time a register is
    rewritten on one branch only. Code no path reaches is a case of a jump table: it starts from what a bctr knows."""
    n = len(words)
    before: list[int | None] = [None] * n

    def run(todo: list[int]) -> None:
        while todo:
            i = todo.pop()
            out = high_halves_after(words[i], before[i])
            for j in successors(words[i], i, n, start, size):
                merged = out | (before[j] or 0)
                if merged != before[j]:
                    before[j] = merged
                    todo.append(j)

    before[0] = 0
    run([0])
    while None in before:
        i = before.index(None)
        before[i] = 0
        for word, bases in zip(words, before):
            if bases is not None and word >> 26 == 19 and (word >> 1) & 0x3FF == 528 and not word & 1:
                before[i] |= bases
        run([i])
    return before


def immediate_base(word: int) -> int | None:
    """Register an immediate of `word` is relative to, None if it has none."""
    op, rs, ra = word >> 26, (word >> 21) & 31, (word >> 16) & 31
    if op in (24, 25):  # ori, oris: the source is the rS field
        return rs
    return (ra or None) if op in (14, 15, 56, 57, 60, 61) or 32 <= op <= 55 else None


def compared_words(words: list[int], start: int, size: int) -> list[int]:
    """`words` with the mask of keep_masks, except on an immediate whose base register holds no high half there: that one
    is a field offset or a stride, which the mask hides only because a `lis` wrote the register earlier. Only shows
    bits: two functions that keep_masks tells apart stay apart."""
    out = []
    for word, mask, bases in zip(words, sy.keep_masks(words, start, size), high_half_registers(words, start, size)):
        base = immediate_base(word)
        shown = base is not None and base not in sy.SDA_REGS and not bases >> base & 1
        out.append(word if shown else word & mask)
    return out


def parity_words(image, address: int, size: int) -> list[int] | None:
    """Compared words of the function at `address` of a tools/symbols.py Image, None outside the code."""
    words = image.words(address, size // 4)
    return compared_words(words, address, size) if words else None


# Version parity


@dataclass
class Row:
    function: df.Function
    state: str
    detail: str = ""  # differs: which words; unknown: why
    batch: str = NO_BATCH
    reason: str = ""  # the stated reason or the version condition of a row


class VersionCode:
    """Code of one version, one image per module of the table, read once."""

    def __init__(self, game_cfg: dict, disc: Path | None):
        self.code = game_cfg.get("code", {})
        self.disc = disc
        self.images: dict[str, object] = {}
        self.game_cfg = game_cfg
        self.sections = None

    def image(self, module: str):
        """Addressable code of `module` (addresses of the table's rows), or the reason there is none."""
        if module not in self.images:
            self.images[module] = self._load(module)
        return self.images[module]

    def _load(self, module: str):
        if self.disc is None or not self.disc.is_dir():
            return NO_DISC
        if module == "dol":
            path = self.disc / self.code["dol"]
            return sy.Image.from_dol(path) if path.is_file() else f"{path.name} missing"
        rel_dir = self.disc / self.code["rel_dir"] if self.code.get("rel_dir") else None
        found = sorted(rel_dir.rglob(f"{module}.rel")) if rel_dir and rel_dir.is_dir() else []
        return sy.Image.from_rel(found[0])[0] if found else f"no module {module}"


    def layout(self):
        """Sections of the DOL (tools/decomp_globals.py layout)."""
        if self.sections is None:
            import decomp_globals as dg  # noqa: E402
            self.sections = dg.layout(self.game_cfg, self.disc / self.code["dol"])
        return self.sections


def constants(code: VersionCode, address: int, size: int) -> dict[int, bytes | None]:
    """{instruction offset: value} of what the code at `address` loads through r2."""
    import decomp_globals as dg  # noqa: E402
    lay = code.layout()
    return {r.offset: lay.read(r.address, r.width) for r in dg.references(lay, address, size)
            if r.how == "r2" and r.width}


def masked_code(code: VersionCode, f: df.Function, version: str) -> list[int] | str:
    """Compared words of `f` in `version`, or the reason there are none."""
    address = f.addresses.get(version)
    if address is None:
        return f"no {version} address"
    image = code.image(f.module)
    if isinstance(image, str):
        return f"{version}: {image}"
    words = parity_words(image, address, f.size)
    return words if words else f"{version} {address:08X}+0x{f.size:X} is outside the code"


def compare_row(f: df.Function, reference: str, codes: dict[str, VersionCode]) -> tuple[str, str]:
    """State of a row and its detail: a `differs` in any version wins over an `unknown` in another one."""
    base = masked_code(codes[reference], f, reference)
    if isinstance(base, str):
        return UNKNOWN, base
    unknown = ""
    for version, code in codes.items():
        if version == reference:
            continue
        other = masked_code(code, f, version)
        if isinstance(other, str):
            unknown = unknown or other
            continue
        changed = [4 * i for i, (a, b) in enumerate(zip(base, other)) if a != b]
        if changed:
            return DIFFERS, f"{version}: {len(changed)} of {len(base)} compared words, first at +0x{changed[0]:X}"
        if f.module == "dol":
            ours = constants(codes[reference], f.addresses[reference], f.size)
            theirs = constants(code, f.addresses[version], f.size)
            moved = sorted(o for o in ours.keys() | theirs.keys() if ours.get(o) != theirs.get(o))
            if moved:
                return DIFFERS, f"{version}: same compared words, {len(moved)} constants differ, first at +0x{moved[0]:X}"
    return (UNKNOWN, unknown) if unknown else (SAME, "")


def read_parity_table(game_cfg: dict) -> dict[str, tuple[str, str]]:
    """{name: (batch, reason)} of [decomp] version_parity, empty if the key is missing."""
    rel = game_cfg.get("decomp", {}).get("version_parity")
    if not rel:
        return {}
    path = game_cfg["_dir"] / rel
    if not path.is_file():
        raise SystemExit(f"game.toml [decomp] version_parity: {path} not found")
    header = None
    out: dict[str, tuple[str, str]] = {}
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        cells = [c.strip() for c in raw.split("\t")]
        if header is None:
            header = cells
            if not {"name", "batch", "reason"} <= set(header):
                raise SystemExit(f"{path}:{n}: columns name, batch and reason expected (found {header})")
            continue
        if len(cells) != len(header):
            raise SystemExit(f"{path}:{n}: {len(cells)} cells, {len(header)} columns")
        row = dict(zip(header, cells))
        if row["name"] in out:
            raise SystemExit(f"{path}:{n}: {row['name']} listed twice")
        out[row["name"]] = (row["batch"] if row["batch"] not in df.ABSENT else NO_BATCH,
                            row["reason"] if row["reason"] not in df.ABSENT else "")
    return out


def version_conditions(game_cfg: dict, names: set[str], src: Path | None) -> set[str]:
    """`names` that a comment `version-differs: <name>` of their own source unit marks as written for both versions: a
    comment elsewhere says nothing of them."""
    src = src or decomp_sources(game_cfg)
    units, roots = source_units(game_cfg, src), source_roots(game_cfg, src)
    marks: dict[Path, set[str]] = {}
    out: set[str] = set()
    for name in names:
        for path in (p for unit in units.get(name, ()) for p in unit_files(unit, roots)):
            if path not in marks:
                marks[path] = set(VERSION_MARK.findall(path.read_text(encoding="utf-8", errors="replace")))
            if name in marks[path]:
                out.add(name)
    return out


def masked_twins(game_cfg: dict, functions: list[df.Function], codes: dict[str, VersionCode]) -> int | None:
    """Rows whose compared words are those of another function of the symbol maps, in a version: a wrong address on that
    function reads `same`. None when no version could be read."""
    map_dir = game_cfg["_dir"] / game_cfg.get("symbols", {}).get("map_dir", "symbols")
    keys = {(f.module, f.size) for f in functions}
    exposed, read = set(), False
    for version, code in codes.items():
        entries = {**df._map_entries(map_dir / f"{version}.map"), **df._map_entries(map_dir / f"{version}-rel.tsv")}
        holders: dict[tuple, set[tuple[str, int]]] = {}
        for (module, address), size in entries.items():
            image = code.image(module) if (module, size) in keys else None
            words = parity_words(image, address, size) if image is not None and not isinstance(image, str) else None
            if words:
                read = True
                holders.setdefault((module, size, tuple(words)), set()).add((module, address))
        for f in functions:
            address = f.addresses.get(version)
            image = code.image(f.module)
            words = parity_words(image, address, f.size) if address is not None and not isinstance(image, str) else None
            if words and holders.get((f.module, f.size, tuple(words)), set()) - {(f.module, address)}:
                exposed.add(f.name)
    return len(exposed) if read else None


def tally(rows: list[Row]) -> dict:
    states = Counter(r.state for r in rows)
    return {"rows": len(rows), SAME: states[SAME], DIFFERS: states[DIFFERS], UNKNOWN: states[UNKNOWN]}


def parity_report(game_cfg: dict, discs: dict[str, Path | None], src: Path | None = None) -> dict:
    """Comparison of every row, totals (all rows, verified rows, enabled rows, per batch), warnings and what --strict
    refuses. `discs`: extracted disc of every version, None if it is not there. `src`: decompiled sources to read instead
    of the configured ones."""
    sym, d = game_cfg.get("symbols", {}), game_cfg.get("decomp", {})
    reference = d.get("reference") or sym.get("reference")
    codes = {v: VersionCode(game_cfg, discs.get(v)) for v in discs}
    functions = df.read_table(game_cfg)
    table = read_parity_table(game_cfg)
    marked = version_conditions(game_cfg, {f.name for f in functions}, src)
    rows, warnings, refused, not_extracted = [], [], [], 0
    for f in functions:
        state, detail = compare_row(f, reference, codes)
        batch, reason = table.get(f.name, (NO_BATCH, ""))
        if f.name in marked:
            reason = reason or "version condition in the source unit"
        rows.append(Row(f, state, detail, batch, reason))
        if state == DIFFERS and not reason:
            warnings.append(f"{f.name}: code differs ({detail}) with no version condition and no stated reason")
            if f.enabled:
                refused.append(warnings[-1])
        elif state == UNKNOWN and f.enabled:
            warnings.append(f"{f.name}: enabled, code not compared ({detail})")
            if detail.endswith(NO_DISC):
                not_extracted += 1
            else:
                refused.append(warnings[-1])
        elif state == SAME and reason:
            warnings.append(f"{f.name}: a reason is stated, but the masked code is the same")
    names = {f.name for f in functions}
    warnings += [f"{n}: in the parity table, not in the table of functions" for n in table if n not in names]
    batches = sorted({r.batch for r in rows})
    verified = [r for r in rows if r.function.kind == "done"]
    return {"reference": reference, "versions": sorted(codes), "total": tally(rows), "verified": tally(verified),
            "enabled": tally([r for r in rows if r.function.enabled]), "ignores": PARITY_IGNORES,
            "masked_twins": masked_twins(game_cfg, functions, codes),
            "batches": {b: tally([r for r in rows if r.batch == b]) for b in batches},
            "verified_batches": {b: tally([r for r in verified if r.batch == b]) for b in batches},
            "not_same": [{"name": r.function.name, "status": r.function.status, "state": r.state, "detail": r.detail,
                          "batch": r.batch, "reason": r.reason} for r in rows if r.state != SAME],
            "warnings": warnings, "refused": refused, "not_extracted": not_extracted}


def parity_share(counts: dict) -> str:
    return f"{100 * counts[DIFFERS] / counts['rows']:.2f} %" if counts["rows"] else "-"


def parity_lines(report: dict) -> list[str]:
    others = [v for v in report["versions"] if v != report["reference"]]
    t, v, e = report["total"], report["verified"], report["enabled"]
    twins = report["masked_twins"]
    out = [f"Code of {report['reference']} against {', '.join(others)}: address fields masked, field offsets and strides "
           "kept, constants read through r2. The share of differs is a floor. same ignores:"]
    out += [f"  - {line}" for line in report["ignores"]]
    out.append("  - a wrong address on a function with the same compared words: "
               + (f"{twins} of {t['rows']} rows have such a twin" if twins is not None else "not counted, no disc read"))
    out.append(f"  Enabled rows: {parity_share(e)} differs, a selection effect (batches were chosen for matching code), "
               "not a share of the game")
    out.append(f"  {t['rows']} rows: {t[SAME]} same, {t[DIFFERS]} differs, {t[UNKNOWN]} unknown; "
               f"verified {v['rows']}: {v[SAME]} same, {v[DIFFERS]} differs, {v[UNKNOWN]} unknown")
    out.append(f"  {'batch':16s} {'rows':>5s} {'same':>5s} {'differs':>8s} {'unknown':>8s} {'differs %':>10s}")
    for batch, counts in report["batches"].items():
        out.append(f"  {batch:16s} {counts['rows']:>5d} {counts[SAME]:>5d} {counts[DIFFERS]:>8d} {counts[UNKNOWN]:>8d} "
                   f"{parity_share(counts):>10s}")
    for r in report["not_same"]:
        out.append(f"  {r['state']:8s} {r['name']} ({r['status']}): {r['detail']}"
                   + (f"; {r['reason']}" if r["reason"] else ""))
    out += [f"  WARNING: {w}" for w in report["warnings"]]
    return out


# Proof digests


def decomp_sources(game_cfg: dict) -> Path:
    return game_cfg["_dir"] / "decomp" / "src"


def normalized(path: Path) -> bytes:
    """Same reading as oracle_sources_hash: a checkout with CRLF gives the same digest."""
    return path.read_bytes().replace(b"\r\n", b"\n")


def source_roots(game_cfg: dict, src: Path | None = None) -> list[Path]:
    tests = game_cfg["_dir"] / "decomp" / "oracle_tests"
    return [src or decomp_sources(game_cfg), *([tests] if tests.is_dir() else [])]


def source_units(game_cfg: dict, src: Path | None = None) -> dict[str, list[Path]]:
    """{name: files holding its GC_DECOMP_EXPORT}. A commented-out export does not count."""
    out: dict[str, list[Path]] = {}
    for root in source_roots(game_cfg, src):
        if root.is_dir():
            for path in df.files_below(root):
                for name in EXPORT.findall(path.read_text(encoding="utf-8", errors="replace")):
                    out.setdefault(name, []).append(path)
    return out


def unit_files(unit: Path, roots: list[Path]) -> list[Path]:
    """`unit` and the headers it includes from `roots`, transitively, resolved. An include found in none of them is a
    file the common digest holds (runtime/core/decomp) or one the build generates from the globals table."""
    seen: set[Path] = set()
    todo = [unit.resolve()]
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        for name in INCLUDE.findall(path.read_text(encoding="utf-8", errors="replace")):
            for base in (path.parent, *roots):
                candidate = base / name
                if candidate.is_file():
                    todo.append(candidate.resolve())
                    break
    return sorted(seen, key=lambda p: p.as_posix())


def unit_digest(f: df.Function, version: str, files: list[Path], roots: list[Path]) -> str:
    """Digest of the row of `f` in `version` and of its source unit. Files are named relative to their root, so that a
    worktree and the main checkout give the same digest."""
    resolved = [r.resolve() for r in roots]
    h = hashlib.sha256(f"{f.name}\t{f.module}\t{f.addresses.get(version) or 0:08X}\t{f.size:X}\n".encode())
    for path in files:
        root = next((r for r in resolved if path.is_relative_to(r)), path.parent)
        h.update(path.relative_to(root).as_posix().encode() + b"\0" + normalized(path) + b"\0")
    return h.hexdigest()


def common_hash(game_cfg: dict, units: dict[str, list[Path]], src: Path | None = None,
                external: Path | None = None) -> str:
    """Digest of what every function shares (module docstring). A file of the decompiled sources in no unit is shared
    too: a helper with no export of its own can reach any function. `external`: folder of the recompiler's checkout."""
    src = src or decomp_sources(game_cfg)
    decomp = game_cfg["_dir"] / "decomp"
    own = (decomp / "src", decomp / "oracle_tests")
    h = hashlib.sha1()
    for name, path in df.oracle_hash_inputs(game_cfg):
        if not any(path.is_relative_to(root) for root in own):
            h.update(name.encode() + b"\0" + normalized(path) + b"\0")
    roots = source_roots(game_cfg, src)
    in_units = {p for paths in units.values() for unit in paths for p in unit_files(unit, roots)}
    if src.is_dir():
        for path in df.files_below(src):
            if path.resolve() not in in_units:
                h.update(path.relative_to(src).as_posix().encode() + b"\0" + normalized(path) + b"\0")
    df.hash_shared_inputs(h, game_cfg, tc.git_head(external) if external else df.dolrecomp_state())
    return h.hexdigest()


def shown_path(path: Path, *bases: Path | None) -> str:
    """`path` for a report: relative to the first of `bases` that holds it, else its name. No report carries a local path."""
    resolved = path.resolve()
    for base in bases:
        if base and resolved.is_relative_to(base.resolve()):
            return resolved.relative_to(base.resolve()).as_posix()
    return path.name


def proof_path(game_cfg: dict, root: Path) -> Path | None:
    """Local digest file ([decomp] proof_digests under the checkout `root`), None if the key is missing."""
    rel = game_cfg.get("decomp", {}).get("proof_digests")
    return root / rel.replace("{slug}", game_cfg["_dir"].name) if rel else None


@dataclass
class Proofs:
    path: Path | None
    exists: bool = False
    damaged: int = 0
    records: dict[tuple[str, str], set[tuple[str, str]]] = field(default_factory=dict)  # (name, version) -> digests


def read_proofs(path: Path | None) -> Proofs:
    """Digests of the local file. A line that cannot be read counts as damaged and proves nothing."""
    proofs = Proofs(path)
    if path is None or not path.is_file():
        return proofs
    proofs.exists = True
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not raw.strip() or raw.startswith("#"):
            continue
        cells = raw.split("\t")
        if len(cells) != len(DIGEST_COLUMNS) or not (SHA256.match(cells[2]) and SHA1.match(cells[3])):
            proofs.damaged += 1
            continue
        proofs.records.setdefault((cells[0], cells[1]), set()).add((cells[2], cells[3]))
    return proofs


def pending_proofs(game_cfg: dict, path: Path, names: list[str], versions: list[str], sources: str,
                   labels: list[str]) -> tuple[list[str], list[str]]:
    """Lines of the promoted `names` not in the local file yet, and the names with no source unit. Writes nothing: the
    promotion computes them before it touches the table, so that a failure here leaves the table as it was. Neither
    digest reads a status, so writing the table next does not change them."""
    roots = source_roots(game_cfg)
    units = source_units(game_cfg)
    common = common_hash(game_cfg, units)
    existing = read_proofs(path).records
    lines, missing = [], []
    for f in df.read_table(game_cfg):
        if f.name not in names:
            continue
        found = units.get(f.name, [])
        if len(found) != 1:
            missing.append(f.name)
            continue
        files = unit_files(found[0], roots)
        for version in versions:
            digest = unit_digest(f, version, files, roots)
            if (digest, common) not in existing.get((f.name, version), ()):
                lines.append("\t".join([f.name, version, digest, common, sources, ",".join(labels)]))
    return lines, missing


def append_proofs(path: Path, lines: list[str]) -> int:
    """Adds the lines of pending_proofs to the local file and returns how many."""
    if lines:
        path.parent.mkdir(parents=True, exist_ok=True)
        head = "" if path.exists() else "# " + "\t".join(DIGEST_COLUMNS) + "\n"
        tail = path.read_bytes()[-1:] if path.exists() else b"\n"
        with path.open("a", encoding="utf-8", newline="\n") as out:
            out.write(("" if tail == b"\n" else "\n") + head + "\n".join(lines) + "\n")
    return len(lines)


def proof_state(f: df.Function, version: str, proofs: Proofs, units: dict[str, list[Path]], roots: list[Path],
                common: str) -> tuple[str, str]:
    """State of a verified row in `version`, and the file that holds its export (or why it has none)."""
    found = units.get(f.name, [])
    if len(found) != 1:
        return UNKNOWN, f"{len(found)} source units hold its export"
    shown = shown_path(found[0], *roots)
    records = proofs.records.get((f.name, version))
    if not proofs.exists or not records:
        return UNKNOWN, shown
    digest = unit_digest(f, version, unit_files(found[0], roots), roots)
    if (digest, common) in records:
        return FRESH, shown
    return (STALE_SHARED if any(unit == digest for unit, _ in records) else STALE_UNIT), shown


def proof_report(game_cfg: dict, path: Path | None, versions: list[str], src: Path | None = None,
                 external: Path | None = None, root: Path | None = None) -> dict:
    """State of every verified row, per version. Reads only: staleness never edits the table. `root`: the checkout that
    holds the digest file, for the path shown."""
    proofs = read_proofs(path)
    roots = source_roots(game_cfg, src)
    units = source_units(game_cfg, src)
    common = common_hash(game_cfg, units, src, external)
    verified = [f for f in df.read_table(game_cfg) if f.kind == "done"]
    out = {"file": shown_path(path, root) if path else None, "file_present": proofs.exists,
           "damaged_lines": proofs.damaged, "common": common, "limits": PROOF_LIMITS, "versions": {}, "rows": []}
    for version in versions:
        states = Counter()
        for f in verified:
            state, unit = proof_state(f, version, proofs, units, roots, common)
            states[state] += 1
            if state != FRESH:
                out["rows"].append({"name": f.name, "version": version, "state": state, "unit": unit})
        out["versions"][version] = {"verified": len(verified), FRESH: states[FRESH],
                                    "stale": states[STALE_UNIT] + states[STALE_SHARED],
                                    STALE_UNIT: states[STALE_UNIT], STALE_SHARED: states[STALE_SHARED],
                                    UNKNOWN: states[UNKNOWN]}
    return out


def proof_lines(report: dict, want_list: bool = False) -> list[str]:
    where = report["file"] or "no [decomp] proof_digests"
    out = [f"Proof of the verified rows, against {where}"
           + ("" if report["file_present"] else " (no such local file: unknown, never fresh)")]
    out += [f"  {line}" for line in report["limits"]]
    if report["damaged_lines"]:
        out.append(f"  {report['damaged_lines']} unreadable lines: they prove nothing")
    for version, c in report["versions"].items():
        out.append(f"  {version}: {c['verified']} verified: {c[FRESH]} fresh, {c['stale']} stale ({c[STALE_UNIT]} unit "
                   f"or row changed, {c[STALE_SHARED]} shared input changed), {c[UNKNOWN]} unknown")
    # A shared input touches every row: the names say nothing more unless they are asked for.
    for r in report["rows"]:
        if want_list or r["state"] == STALE_UNIT:
            out.append(f"  {r['state']:12s} {r['version']} {r['name']} ({r['unit']})")
    return out
