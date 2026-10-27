"""Rules shared by the decompilation tools (generic; parameters in games/<game>/game.toml, [decomp]).

Read by tools/decomp_functions.py (and through it tools/build.py) and tools/decomp_progress.py, so that the three
tools read the table of decompiled functions the same way:

  * statuses: `normalize_status` and `status_kind`. A status is compared whole, case-insensitive, surrounding
    "[]", backquotes and spaces removed, never by prefix ("verified-failed" is not "verified"). The three lists come
    from [decomp] done_statuses (default ["verified"]), replaced_statuses (default ["unverified"]) and
    listed_statuses (default ["disabled"]). decomp_functions refuses any other status; decomp_progress warns and
    does not count it.
  * gap addresses: `read_gaps` reads `<ID>-gaps.tsv` written by `tools/symbols.py <game> gaps` next to the symbol
    maps ([symbols] map_dir). Only the levels in ESTABLISHED_LEVELS give an established address. The levels in
    ORACLE_LEVELS give an address that may be enabled (decomp_functions) or counted (decomp_progress) only on a table
    row whose oracle column for that version ([decomp.oracle_columns]) is "yes", meaning: the decompilation oracle
    has passed on that version. Any other level never gives an address.
  * pairing (`read_transposition`, `paired_address`): the address of a reference function in another version is
    the one that version's transposition report (`<ref>-to-<ID>.tsv` of `tools/symbols.py transpose`, next to the
    symbol maps, in Git) or its gap file gives to THAT reference address. decomp_functions refuses a table row whose
    address in a version is another one (even the start of a function of the same size); decomp_progress does not
    count it.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

# Levels of tools/symbols.py gaps (see the header of an <ID>-gaps.tsv file).
ESTABLISHED_LEVELS = ("strong", "graph")
ORACLE_LEVELS = ("position", "sized")
ORACLE_YES = ("yes",)
ORACLE_NO = ("", "-", "no")

DEFAULT_STATUSES = {"done": ["verified"], "replaced": ["unverified"], "listed": ["disabled"]}


def normalize_status(text: str) -> str:
    return (text or "").strip("[]` \t").lower()


def status_sets(decomp_cfg: dict) -> dict[str, set[str]]:
    """{"done", "replaced", "listed"} -> statuses, from [decomp] done_statuses, replaced_statuses, listed_statuses."""
    return {kind: {normalize_status(s) for s in decomp_cfg.get(f"{kind}_statuses", default)}
            for kind, default in DEFAULT_STATUSES.items()}


def status_kind(text: str, sets: dict[str, set[str]]) -> str | None:
    """"done", "replaced", "listed", or None for an unknown status."""
    s = normalize_status(text)
    for kind in ("done", "replaced", "listed"):
        if s in sets[kind]:
            return kind
    return None


def all_statuses(sets: dict[str, set[str]]) -> list[str]:
    return sorted(sets["done"] | sets["replaced"] | sets["listed"])


def oracle_columns(decomp_cfg: dict) -> dict[str, str]:
    """{version ID: column name} from [decomp.oracle_columns] (optional)."""
    return dict(decomp_cfg.get("oracle_columns", {}))


def parse_oracle(text: str) -> bool:
    """True for "yes"; False for "", "-", "no"; ValueError otherwise (case-insensitive)."""
    t = (text or "").strip().lower()
    if t in ORACLE_YES:
        return True
    if t in ORACLE_NO:
        return False
    raise ValueError(text)


@dataclass(frozen=True)
class Gap:
    ref: int          # reference address
    target: int       # address in the version (0 = none)
    ref_size: int
    size: int         # size in the version (0 = none)
    level: str        # strong, graph, position, sized, weak, not-linked, unresolved


def gaps_path(game_cfg: dict, version: str) -> Path:
    sym = game_cfg.get("symbols", {})
    return game_cfg["_dir"] / sym.get("map_dir", "symbols") / f"{version}-gaps.tsv"


def read_gaps(path: Path) -> list[Gap]:
    """Rows of an <ID>-gaps.tsv file ([] if the file does not exist). Columns: ref, target, ref_size, target_size,
    status, ... (hexadecimal)."""
    if not path.exists():
        return []
    out = []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        p = line.split("\t")
        try:
            out.append(Gap(int(p[0], 16), int(p[1], 16), int(p[2], 16), int(p[3], 16), p[4].strip()))
        except (IndexError, ValueError):
            raise SystemExit(f"{path}:{n}: unreadable gap row (expected ref, target, ref_size, target_size, status)")
    return out


DEFAULT_TRANSPOSITION = "games/{slug}/symbols/{ref}-to-{id}.tsv"


@dataclass(frozen=True)
class Transposed:
    ref: int                   # reference address
    target: int                # address in the version (0 = not found)
    ref_size: int
    status: str                # unique, neighbor, ... absent (tools/symbols.py Matcher)
    identical: bool            # same code (relocations masked): the version's size is ref_size
    name: str
    candidates: tuple[int, ...]


def transposition_path(game_cfg: dict, root: Path, version: str) -> Path:
    """Transposition report of `version` ([decomp] transposition, relative to the repository root `root`)."""
    d = game_cfg.get("decomp", {})
    ref = d.get("reference") or game_cfg.get("symbols", {}).get("reference")
    rel = d.get("transposition", DEFAULT_TRANSPOSITION)
    return root / rel.format(slug=game_cfg["_dir"].name, ref=ref, id=version)


def read_transposition(path: Path) -> dict[int, Transposed]:
    """Rows of a transposition report by reference address ({} if the file does not exist). Columns: ref, target,
    ref_size, status, identical, name, candidates (hexadecimal)."""
    if not path.exists():
        return {}
    out: dict[int, Transposed] = {}
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        p = line.split("\t")
        try:
            cands = tuple(int(x, 16) for x in p[6].split(",")) if len(p) > 6 and p[6].strip() else ()
            out[int(p[0], 16)] = Transposed(int(p[0], 16), int(p[1], 16), int(p[2], 16), p[3].strip(),
                                            p[4].strip() == "1", p[5].strip() if len(p) > 5 else "", cands)
        except (IndexError, ValueError):
            raise SystemExit(f"{path}:{n}: unreadable transposition row (expected ref, target, size, status, "
                             f"identical, name, candidates)")
    return out


def paired_address(ref: int, trans: dict[int, Transposed], gaps_by_ref: dict[int, Gap]) -> tuple[int | None, str]:
    """(address of the reference function `ref` in the version, how) from that version's transposition report and
    gap rows (by reference address). how: the transposition status; "gap-strong" / "gap-graph" (established); or
    "gap-position" / "gap-sized" (`is_gated`: the address is usable only on a row marked by the oracle of that
    version). The address is None when neither file gives one (how = the transposition status, or "unknown")."""
    g = gaps_by_ref.get(ref)
    if g is not None and not g.target:
        g = None
    if g is not None and g.level in ORACLE_LEVELS:
        return g.target, f"gap-{g.level}"
    t = trans.get(ref)
    if t is not None and t.target:
        return t.target, t.status
    if g is not None and g.level in ESTABLISHED_LEVELS:
        return g.target, f"gap-{g.level}"
    return None, t.status if t is not None else "unknown"


def is_gated(how: str) -> bool:
    return how.startswith("gap-") and how[4:] in ORACLE_LEVELS
