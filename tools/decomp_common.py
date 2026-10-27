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
