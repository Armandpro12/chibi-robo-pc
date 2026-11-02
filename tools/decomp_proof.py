"""What the rows of the table of decompiled functions rest on (generic; parameters in [decomp] of game.toml).

Read by tools/decomp_functions.py check --parity and tools/decomp_progress.py --parity / --proof; tools/decomp_oracle.py
report --promote writes the digests. This file is not in ORACLE_HASHED_TOOLS: it decides nothing an oracle run measures,
so changing a report here keeps the oracle results valid.

Version parity. Each row's code in the reference version is compared with its code in every other version, after
masking the fields that depend on addresses (tools/sdksig.py masked_words, the mask of tools/symbols.py keep_masks):
  same     the masked words are equal in every version
  differs  a masked word differs in a version, or a constant read through r2 does (DOL rows: its value, not its
           address, tools/decomp_globals.py references). Only r2: the address is exact there, whereas that scan can take
           a register rewritten since its `lis` for a `lis` base (two false constants found by hand, 2026-11-02)
  unknown  the comparison could not be made (no address in a version, disc not extracted, range outside the code)
Never read `same` as "the same behavior": the masks still hide the targets of calls, the addresses a function only
takes (a callee, a string or a table can differ), what it reads from writable data and the .rodata values it loads
through `lis`. Two functions of a version can also share their masked code, so a wrong address on such a twin reads
`same`. REL rows are compared in the REL files, relocations not applied (the words they fill hold zeroes), without the
constants.
  A `differs` asks for a version condition in the C++ (a comment `version-differs: <name>` in the row's source unit,
which is how this tool finds it) or a reason in the table below; without one it is a warning, and so is an `unknown` on an
enabled row. Warnings refuse nothing yet.
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
stays promoted by the full report only. The cases of the oracle are not in the digests.

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
        import symbols as sy  # noqa: E402  (generic tool of the repository)
        if self.disc is None or not self.disc.is_dir():
            return "disc not extracted"
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
    """Masked words of `f` in `version`, or the reason there are none."""
    import sdksig  # noqa: E402  (generic tool of the repository)
    address = f.addresses.get(version)
    if address is None:
        return f"no {version} address"
    image = code.image(f.module)
    if isinstance(image, str):
        return f"{version}: {image}"
    words = sdksig.masked_words(image, address, f.size)
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
            return DIFFERS, f"{version}: {len(changed)} of {len(base)} masked words, first at +0x{changed[0]:X}"
        if f.module == "dol":
            ours = constants(codes[reference], f.addresses[reference], f.size)
            theirs = constants(code, f.addresses[version], f.size)
            moved = sorted(o for o in ours.keys() | theirs.keys() if ours.get(o) != theirs.get(o))
            if moved:
                return DIFFERS, f"{version}: same masked words, {len(moved)} constants differ, first at +0x{moved[0]:X}"
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


def version_conditions(src: Path) -> set[str]:
    """Names that a comment `version-differs: <name>` of the decompiled code marks as written for both versions."""
    out: set[str] = set()
    if src.is_dir():
        for path in df.files_below(src):
            out.update(VERSION_MARK.findall(path.read_text(encoding="utf-8", errors="replace")))
    return out


def tally(rows: list[Row]) -> dict:
    states = Counter(r.state for r in rows)
    return {"rows": len(rows), SAME: states[SAME], DIFFERS: states[DIFFERS], UNKNOWN: states[UNKNOWN]}


def parity_report(game_cfg: dict, discs: dict[str, Path | None], src: Path | None = None) -> dict:
    """Comparison of every row, totals (all rows, verified rows, per batch) and warnings. `discs`: extracted disc of
    every version, None if it is not there. `src`: decompiled sources to read instead of the configured ones."""
    sym, d = game_cfg.get("symbols", {}), game_cfg.get("decomp", {})
    reference = d.get("reference") or sym.get("reference")
    codes = {v: VersionCode(game_cfg, discs.get(v)) for v in discs}
    functions = df.read_table(game_cfg)
    table = read_parity_table(game_cfg)
    marked = version_conditions(src or decomp_sources(game_cfg))
    rows, warnings = [], []
    for f in functions:
        state, detail = compare_row(f, reference, codes)
        batch, reason = table.get(f.name, (NO_BATCH, ""))
        if f.name in marked:
            reason = reason or "version condition in the source unit"
        rows.append(Row(f, state, detail, batch, reason))
        if state == DIFFERS and not reason:
            warnings.append(f"{f.name}: code differs ({detail}) with no version condition and no stated reason")
        elif state == UNKNOWN and f.enabled:
            warnings.append(f"{f.name}: enabled, code not compared ({detail})")
        elif state == SAME and reason:
            warnings.append(f"{f.name}: a reason is stated, but the masked code is the same")
    names = {f.name for f in functions}
    warnings += [f"{n}: in the parity table, not in the table of functions" for n in table if n not in names]
    batches = sorted({r.batch for r in rows})
    verified = [r for r in rows if r.function.kind == "done"]
    return {"reference": reference, "versions": sorted(codes), "total": tally(rows), "verified": tally(verified),
            "batches": {b: tally([r for r in rows if r.batch == b]) for b in batches},
            "verified_batches": {b: tally([r for r in verified if r.batch == b]) for b in batches},
            "not_same": [{"name": r.function.name, "status": r.function.status, "state": r.state, "detail": r.detail,
                          "batch": r.batch, "reason": r.reason} for r in rows if r.state != SAME],
            "warnings": warnings}


def parity_share(counts: dict) -> str:
    return f"{100 * counts[DIFFERS] / counts['rows']:.2f} %" if counts["rows"] else "-"


def parity_lines(report: dict) -> list[str]:
    others = [v for v in report["versions"] if v != report["reference"]]
    out = [f"Code of {report['reference']} against {', '.join(others)}: masked words (tools/sdksig.py) and constants read "
           "through r2; same says nothing of calls, taken addresses or writable data"]
    t, v = report["total"], report["verified"]
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


def record_proofs(game_cfg: dict, path: Path, names: list[str], versions: list[str], sources: str,
                  labels: list[str]) -> tuple[int, list[str]]:
    """Adds the digests of the promoted `names` to the local file. Returns (lines added, names with no source unit)."""
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
    if lines:
        path.parent.mkdir(parents=True, exist_ok=True)
        head = "" if path.exists() else "# " + "\t".join(DIGEST_COLUMNS) + "\n"
        tail = path.read_bytes()[-1:] if path.exists() else b"\n"
        with path.open("a", encoding="utf-8", newline="\n") as out:
            out.write(("" if tail == b"\n" else "\n") + head + "\n".join(lines) + "\n")
    return len(lines), missing


def proof_state(f: df.Function, version: str, proofs: Proofs, units: dict[str, list[Path]], roots: list[Path],
                common: str) -> tuple[str, str]:
    """State of a verified row in `version`, and the file that holds its export (or why it has none)."""
    found = units.get(f.name, [])
    if len(found) != 1:
        return UNKNOWN, f"{len(found)} source units hold its export"
    shown = found[0].as_posix()
    records = proofs.records.get((f.name, version))
    if not proofs.exists or not records:
        return UNKNOWN, shown
    digest = unit_digest(f, version, unit_files(found[0], roots), roots)
    if (digest, common) in records:
        return FRESH, shown
    return (STALE_SHARED if any(unit == digest for unit, _ in records) else STALE_UNIT), shown


def proof_report(game_cfg: dict, path: Path | None, versions: list[str], src: Path | None = None,
                 external: Path | None = None) -> dict:
    """State of every verified row, per version. Reads only: staleness never edits the table."""
    proofs = read_proofs(path)
    roots = source_roots(game_cfg, src)
    units = source_units(game_cfg, src)
    common = common_hash(game_cfg, units, src, external)
    verified = [f for f in df.read_table(game_cfg) if f.kind == "done"]
    out = {"file": path.as_posix() if path else None, "file_present": proofs.exists, "damaged_lines": proofs.damaged,
           "common": common, "versions": {}, "rows": []}
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
