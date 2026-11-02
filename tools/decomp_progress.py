"""Decompilation progress: share of functions and code bytes decompiled, per version and per module.

Usage:
  py -3 tools/decomp_progress.py <game> [--version ID[,ID]] [--module NAME[,NAME]] [--list] [--json]
                                        [--root CHECKOUT] [--require-complete]
                                        [--routes] [--closure] [--function NAME|ADDRESS[,...]] [--top N]
                                        [--cov ID:NAME:PATH ...] [--cov-hooks ID:PATH ...]
                                        [--parity] [--proof [--require-fresh]] [--proof-file PATH]

  --list              also print every function left to decompile, and every automatic name found in the
                      decompiled code (file:line)
  --json              machine-readable output (same content)
  --root              checkout holding the untracked inputs (external/, generated/); default: this checkout.
                      game.toml and the decompiled code are always read from this checkout.
  --table, --decomp-dir, --map-dir  read another table / code folder / symbol map folder instead of the
                      configured ones (tests)
  --require-complete  exit code 1 unless every version is at 100 % (functions and bytes, DOL and every REL),
                      every function has an address in every version, and no automatic name is left in the
                      decompiled code (the stage's final criterion). Always computed on every version and every
                      module of game.toml: --version and --module only filter what is printed.
  --routes            planning view, per version (section "Routes" below). Printed instead of the progress table;
                      with --json the view is added to the report. Needs the coverage of that version.
  --closure           planning view, per version (section "Closure" below). Same printing rule. Not
                      decomp_oracle.py report --closure, which says of each enabled function whether a callee can wait.
  --function          limit the rows of --routes to these functions (name, reference address, or address in a
                      version); the figures stay those of the whole game
  --top               length of the rankings of --routes and --closure (default 20)
  --cov, --cov-hooks  coverage runs (ID:NAME:<cov.tsv or its folder>) and probe address list of a version to read
                      instead of the configured ones; a --cov replaces all the configured runs of every version
  --parity            rows of the table whose masked code is the same, differs or is unknown between the versions,
                      totals and per batch (tools/decomp_proof.py). Printed instead of the progress table, like the
                      planning views; with --json added to the report. Always every version, and every row whatever
                      --module says: reads the extracted discs under --root, an absent disc reads as unknown
  --proof             verified rows whose proof still applies, per version: fresh, stale (unit or row changed, or a
                      shared input changed), unknown (tools/decomp_proof.py). Same printing rule. Reads the local
                      digest file under --root ([decomp] proof_digests): the checkout that holds tests/. On a fresh
                      clone there is no such file and every verified row reads as unknown, never as fresh
  --require-fresh     with --proof: exit code 1 if a verified row is stale-unit in any version (its code or its row
                      changed since its promotion). stale-shared and unknown are printed and never refuse here; they
                      block the close of a batch or a release (notes/decisions.md, 2026-11-02). Always computed on every
                      version of game.toml, like --require-complete
  --proof-file        digest file to read instead

Exit codes: 0 report printed; 1 --require-complete or --require-fresh not met; 2 usage or configuration error, or an input of --routes
or --closure missing (an absent input is never read as "nothing was reached").

Game-specific parameters: [decomp] section of `games/<game>/game.toml` (all paths are relative):
  reference      version whose functions define the work (default: [symbols] reference)
  dir            folder of the decompiled code, relative to games/<game>/ (scanned for automatic names)
  functions      table of the decompiled functions, relative to games/<game>/ (missing = 0 % decompiled)
  columns        sub-table [decomp.columns], <version ID> = "<column name>": column of the table holding each
                 version's address (written as a [decomp.columns] table, never as an inline table: one form only)
  done_statuses      statuses counted as decompiled: the function replaces its recompiled version and passed
                     the oracle (default ["verified"])
  replaced_statuses  statuses shown as "replaced, not verified" (in the build, not yet through the oracle):
                     reported in their own column, never in the percentages nor in --require-complete
                     (default ["unverified"])
  listed_statuses    statuses of rows listed but not built (default ["disabled"]): not counted, no warning
                 A status is compared whole (case-insensitive, surrounding "[]" and backquotes removed), never by
                 prefix: "verified-failed" is not "verified". Any other status: warning, not counted. Same
                 reading as tools/decomp_functions.py (shared code: tools/decomp_common.py), which refuses the
                 table instead of warning.
  transposition  transposition report of tools/symbols.py for a non-reference version, relative to the
                 repository of the tools (not --root: it is a file of the game, in Git, next to the maps; {slug},
                 {ref}, {id} replaced; default games/{slug}/symbols/{ref}-to-{id}.tsv)
  oracle_columns optional sub-table [decomp.oracle_columns], <version ID> = "<column name>": column of the
                 table that says "yes" once the oracle has passed on that version (see the gaps below)
  excluded       partition categories not to decompile (e.g. libraries replaced by native code)
  ranges         [[decomp.ranges]] category, start, end (end optional = no upper bound): DOL partition in
                 reference addresses, [start, end); a function belongs to the range holding its start
  overrides      [[decomp.overrides]] addr, name (optional, checked), category: single DOL functions
  rel_excluded_names   optional list of REL function names not to decompile (default: none)
  auto_name_patterns   optional extra regular expressions counted as automatic names

Inputs that are not in Git:
  - the function inventory: dtk `symbols.txt` of the reference version ([symbols] dtk_dir, every function with
    its bounds, the DOL and one subfolder per REL). It is the only list of every function with its size; without
    it the percentages cannot be computed and the script says so;
  - for each other version, the DOL addresses come from the transposition report written by
    `tools/symbols.py <game> transpose --to <ID>`, and the REL offsets from the same matcher run here on the two
    versions' REL files (generated/<game>/<ID>/disc/, [code] rel_dir). A function with no address in a version
    never counts as decompiled in that version.
  - then the gap file `<ID>-gaps.tsv` of `tools/symbols.py <game> gaps` (in Git, next to the symbol maps,
    [symbols] map_dir), read through tools/decomp_common.py (activation lock): a function it places at the level
    strong or graph gets that address; one at the level position or sized has NO established address (a sized
    function loses the address of the transposition: its size there is not established), and counts in that
    version only on a table row giving that same address with "yes" in the version's oracle column; any other
    level gives no address.
  Byte figures always use the reference version's function sizes.

Table of the decompiled functions (format still open; this reader is tolerant):
  - one row per function, columns separated by tabs (or by spaces when a row has no tab);
  - a header row names the columns: a line with at least two recognized names; a line starting with "#" is a
    header only when it has tabs or when every one of its words is a recognized name (so that a prose comment
    quoting two column names, "#   name ... one line per name", is not taken for the header); recognized names: "name" (or function,
    symbol), "status" (or state), "size", "module" (or rel), and for each version the column given by
    [decomp] columns, or the version ID itself ("GGXX01", "ggxx01_addr", "ggxx01_address"), or the region
    ("us_addr", "pal_addr", "jp_addr", with or without "_addr"/"_address");
  - without a header: name, then the version addresses in the order of [decomp] columns, then size, status;
  - addresses in hexadecimal (with or without 0x); "-" or empty = no address in that version. A REL function is
    "<module>:<offset>" (offset inside the module's code section, as in <ID>-rel.tsv), or an offset with a
    "module" column. Other lines starting with "#" are comments.
  - size (optional): "0x"-prefixed = hexadecimal ("0x20" = 32 bytes), digits only = decimal ("20" = 20 bytes);
    bare hexadecimal ("2F4") is refused. A row whose size differs from the inventory, or cannot be read, does
    not count in any version.
  A row counts in a version only when it gives that version's address and the address is established:
    - equal to the transposition (DOL) or matcher (REL) address, when there is one; a different one does not
      count;
    - when there is none (absent, ambiguous, collision), only if it is not already the address of another
      function in that version, and either the only matcher candidate of that function (code equal to the
      reference, relocations masked) not held by another function, or the start of a function of the same size
      in that version's symbol map (<ID>.map, <ID>-rel.tsv) with no contradicting name. Two rows giving the
      same such address: neither counts. Anything else does not count: the address goes first through
      symbols/supplement.tsv (verified) and the transposition.
      Limit of the symbol-map route: names "contradict" only when both are hand-written; while the reference
      function still has an automatic name (fn_...), a same-size function there is accepted whatever its name in
      that version's map (the size and "not taken" checks still apply).
  Every row that is not counted, fully or in one version, is reported as a warning with the reason.

Automatic names: placeholders of dtk, Ghidra and m2c (fn_80012345, fn_1_264, FUN_..., PTR_FUN_..., _DAT_...,
s_text_80003000, uRam80001234, UNRECOVERED_JUMPTABLE, uVar1, local_10, param_1, var_r31, sp1C, arg0, unk1C...):
the patterns and their limits are in AUTO_PATTERNS below. Counted (1) in the decompiled code folder, comments
and string literals left out, and in the names of the table, and (2) among the functions left to decompile,
from the symbol maps (<ID>.map, <ID>-rel.tsv) and the dtk names (also with the dtk filter of tools/symbols.py).
Read-only: the script writes nothing.

Routes (--routes): what to aim for so that a scenario reaches a function no run reached. The tool says what to aim for,
not how to get there: the real cost stays the scenario. Static call graph of tools/decomp_graph.py, in each version from
that version's own code, with the REL relocations applied.
  Each function to decompile has a reach status in each version:
    reached        the probe saw its entry in at least one run
    never-reached  the probe was compiled on its address and never saw it
    not-measured   rel-module (the probe had no hook in the RELs), no-probe, or no-address: NOT "never reached", never
                   counted with the never-reached functions; a route is still given when there is one
    An address that is not established (gap level position or sized) is used, as the probe did, and marked "?".
  Route: shortest path of direct calls and tail branches from the nearest reached function (all reached functions are
  sources). It gives the start, each hop, the call site to take in the start, the block ending at that site, and the
  gate: the closest branch known to have run from which that block can be taken. Block coverage does not exist: what is
  known to have run is a lower bound (tools/decomp_graph.py covered_blocks), so the true decision can be further along.
  Route status and flag:
    found / exhaustive             no shorter way exists on the evidence: no function that could shorten the path has
                                   its address taken or is not measured, the function itself included (limit: callers in code
                                   outside this version's inventory are not in the graph)
    found / incomplete frontier    a route, but an indirect call or a function not measured may give a shorter one
                                   (reasons listed), or the block sits behind a jump that is not followed
    pointer / incomplete frontier  no direct path, but a path with at least one step through an address reference: a
                                   function loads the address (step "pointer") or loads a table holding it (step
                                   "table", the nearest address loaded before that data word: a guess). The indirect
                                   call that uses the pointer is not known. The gate "ran" says the load is known to run
    unknown / incomplete frontier  none of those, but the function or one of its callers has its address taken or is
                                   not measured: the absence is not established
    not-found / exhaustive         no direct path, and no address reference on any of its callers: dead on static
                                   evidence (limits: an address computed in another way, tools/decomp_graph.py; callers in code
                                   outside this version's inventory are not in the graph)
  Inputs (not in Git): the coverage probe's files, [decomp] coverage_hooks and [decomp.coverage_runs] of game.toml,
  paths relative to --root, {slug} and {id} replaced. Refused (code 2), never read as "nothing reached": a run of
  another version (under half of its entries in the probe list of its version; under 80 %: a warning; both thresholds
  are a hypothesis, set on 16 runs spent mostly in the DOL), a file that is cut (limit: tools/decomp_graph.py, read_cov),
  an empty probe list. A function entry with 0 calls is not reached. --closure alone ignores a faulty configured
  coverage, which only adds a column; give --cov to have it checked.

Closure (--closure): what decompiled code still depends on. Each function of a version is decompiled, replaced (in the
build, oracle not passed), native (tools/build.py signature_scan: a superset of what the runtime replaces, see
replaceable_copies), library (excluded from the decompilation, still recompiled) or recompiled.
  State of a function from its transitive direct callees. A native function is a leaf: its guest body does not run. A
  decompiled function is followed through its original, which makes the same calls.
    closed    every callee is decompiled or native, and no indirect call or jump anywhere: proven
    unknown   no recompiled callee found, but an indirect call or jump is on the way: its targets are not known
    library   library functions are left, and no recompiled function to decompile
    open      a recompiled function to decompile is among the callees
  The ranking gives the recompiled functions with the most call sites in decompiled code: the next profitable batches.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import symbols as sy  # noqa: E402
import decomp_common as dcom  # noqa: E402
import toolchain as tc  # noqa: E402

CODE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp"}
# Automatic names produced by the usual tools (kind, regular expression, optional check of the match).
# Generic: no game in here. Rules, so that ordinary names do not match:
#  - a name starts after a character that is neither a letter nor a digit (an underscore is allowed, so
#    "_DAT_80004000" and "PTR_FUN_80001234" are found: Ghidra adds such prefixes);
#  - the number of an address placeholder is either 8 hexadecimal digits with at least one decimal digit (a
#    32-bit address: "sub_facade" is not one) or the dtk REL form <module>_<offset> ("fn_1_264");
#  - a match must end at a word boundary (no letter or digit right after the number);
#  - comments and string or character literals are not scanned (see strip_comments): a name quoted there for
#    traceability ("was fn_800B26BC") is not code.
#  - a Ghidra type prefix (uVar1, pcVar2, ppuVar3, auStack_18, puRam80001234) is pointer/array letters ("p",
#    "a", at most 4) then ONE base letter, so "hasVar2" or "envVar2" are not taken; not handled: struct pointer
#    names made from a type's capital letter (pSVar1);
#  - Ghidra's case labels "caseD_<case value>" have any number of hexadecimal digits (caseD_1, caseD_ff).
# Deliberate choices: "arg0" (m2c's argument names) and "unkA"/"unk1C" (dtk/m2c field names) count, as in the
# tools' output there is no way to tell them from a hand-written name; "envVar2", "spEED", "local_face" do not.
NB = r"(?<![A-Za-z0-9])"                          # start of a name (underscore allowed before)
END = r"(?![A-Za-z0-9])"                          # end of the number part
HEX8 = r"(?=[0-9A-Fa-f]{0,7}[0-9])[0-9A-Fa-f]{8}"  # 8 hexadecimal digits, at least one decimal digit
GHIDRA_TYPES = "bcdfhilpqsuvwxz"                   # base letters of Ghidra's type prefixes (uVar, cVar, fVar...)
GHIDRA_PREFIX = r"[pa]{0,4}[" + GHIDRA_TYPES + r"]"  # pointer/array letters, then one base letter (ppuVar, auStack)


def _m2c_stack(m: re.Match) -> bool:
    """sp<offset>: m2c writes the stack offset in upper-case hexadecimal; without a decimal digit it is only
    taken when it is a multiple of 4 (spC, spAC: yes; spEED: no)."""
    off = m.group(0)[2:]
    return any(c.isdigit() for c in off) or int(off, 16) % 4 == 0


AUTO_PATTERNS = [
    ("address placeholder", NB + r"_*(?:fn|func|sub|lbl|zz|loc|off|jumptable|jtbl)_(?:0x)?(?:" + HEX8
     + r"|\d+_(?:(?:text|data|rodata|bss|sdata2?|sbss2?|ctors|dtors)_)?[0-9A-Fa-f]+)" + END + r"\w*", None),
    ("Ghidra label", NB + r"_*(?:PTR_)*(?:thunk_)?(?:(?:FUN|DAT|LAB|PTR|SUB|UNK|EXT|OFF|BYTE|WORD|DWORD|QWORD"
     r"|FLOAT|DOUBLE|switchD|caseD)_|code_r0x)(?:0x)?(?=[0-9A-Fa-f]{0,7}[0-9])[0-9A-Fa-f]{4,8}" + END + r"\w*"
     + r"|" + NB + r"_*caseD_(?:0x)?[0-9A-Fa-f]+" + END, None),
    ("Ghidra string label", NB + r"_*(?:PTR_)*[su]_\w*?_" + HEX8 + END, None),
    ("Ghidra global", NB + r"_*" + GHIDRA_PREFIX + r"Ram" + HEX8 + END, None),   # type + space "ram"
    ("Ghidra pseudo-name", NB + r"UNRECOVERED_JUMPTABLE\w*", None),
    ("Ghidra variable", NB + GHIDRA_PREFIX + r"Var\d+" + END, None),
    ("Ghidra local/param", NB + r"local_(?:res)?(?=[0-9A-Fa-f]*[0-9])[0-9A-Fa-f]+" + END + r"|" + NB
     + r"param_\d+" + END + r"|" + NB + r"(?:unaff|extraout)_\w+|" + NB + GHIDRA_PREFIX
     + r"Stack_(?=[0-9A-Fa-f]*[0-9])[0-9A-Fa-f]+" + END + r"|" + NB
     + r"in_(?:r\d+|f\d+|cr\d+|lr|ctr|xer|stack_\w+)" + END, None),
    ("raw field", NB + r"field\d*_0x[0-9A-Fa-f]+" + END + r"|" + NB + r"_?unk_?(?:0x)?[0-9A-F]+" + END, None),
    ("m2c temporary", NB + r"(?:var|temp|phi|saved_reg)_(?:r\d+|f\d+|cr\d+|lr|ctr)(?:_\d+)?" + END + r"|" + NB
     + r"arg\d+" + END + r"|" + NB + r"M2C_\w+", None),
    ("m2c stack slot", NB + r"sp[0-9A-F]{1,4}" + END, _m2c_stack),
]
REGION_ALIASES = {"ntsc-u": "us", "ntsc-j": "jp", "pal": "pal", "ntsc-k": "kr"}
NO_ADDRESS = {"", "-", "0", "none", "n/a", "absent"}
TRANSPOSITION_CANDIDATES = 8   # tools/symbols.py writes at most 8 candidates per function in its report
REASONS_KEPT = 6                # --routes: reasons kept per route, the total is given


@dataclass
class Fn:
    module: str          # "dol" or the REL module name
    ref: int             # reference address (DOL) or offset in the code section (REL)
    size: int
    name: str            # best known name (symbol maps first, then dtk)
    category: str
    addr: dict = field(default_factory=dict)     # version -> address/offset or None
    how: dict = field(default_factory=dict)      # version -> mapping status
    done: dict = field(default_factory=dict)     # version -> status of the row (done_statuses: verified)
    replaced: dict = field(default_factory=dict)  # version -> status (replaced_statuses: replaced, not verified)
    cands: dict = field(default_factory=dict)    # version -> (starts whose code equals the reference, full list?)
    gated: dict = field(default_factory=dict)    # version -> (address, level): gap address enabled by the oracle only

    def label(self, vid: str) -> str:
        a = self.addr.get(vid)
        if a is None:
            return "-"
        return f"{a:08X}" if self.module == "dol" else f"{self.module}:{a:X}"


class ConfigError(Exception):
    pass


def pct(a: int, b: int) -> float | None:
    """Rounded down to 0.01 so that 100 % means really everything."""
    return None if b == 0 else math.floor(a * 10000 / b) / 100


def fmt_pct(p: float | None) -> str:
    return "  n/a  " if p is None else f"{p:6.2f} %"


def is_auto(name: str, extra: list[re.Pattern], dtk_name: bool = True) -> bool:
    """`dtk_name`: a name from the symbol maps or dtk, also tested with the broader dtk filter of
    tools/symbols.py (sub_<any hex>...); the names written by hand in the table only go through AUTO_PATTERNS."""
    if not name or (dtk_name and sy.AUTO_RE.match(name)):
        return True
    return any(True for _ in auto_matches(name, extra))


_PATTERN_CACHE: dict = {}


def compiled_patterns(extra: list[re.Pattern]) -> list[tuple[str, re.Pattern, object]]:
    key = tuple(p.pattern for p in extra)
    if key not in _PATTERN_CACHE:
        _PATTERN_CACHE[key] = ([(k, re.compile(r), chk) for k, r, chk in AUTO_PATTERNS]
                               + [("game.toml pattern", p, None) for p in extra])
    return _PATTERN_CACHE[key]


def auto_matches(text: str, extra: list[re.Pattern]):
    """(kind, matched name) of every automatic name in `text`; a name found by two patterns counts once (the
    first pattern of the list names its kind)."""
    spans = []
    for kind, pat, chk in compiled_patterns(extra):
        for m in pat.finditer(text):
            s, e = m.span()
            if (chk is None or chk(m)) and not any(s < e1 and s1 < e for s1, e1 in spans):
                spans.append((s, e))
                yield kind, m.group(0)


def strip_comments(text: str) -> str:
    """C/C++ text with comments and string/character literals replaced by spaces (line breaks kept, so line
    numbers do not move). A quote right after a letter or digit is a C++14 digit separator (1'000), not a
    literal. Not handled (not used in the decompiled code): raw strings R"(...)", trigraphs, a backslash that
    continues a // comment."""
    return _NOT_CODE.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


# Leftmost match wins, so a "//" inside a string stays in the string and a quote inside a comment stays in the
# comment. An unterminated literal stops at the end of its line.
_NOT_CODE = re.compile(r"//[^\n]*|/\*.*?(?:\*/|\Z)|\"(?:\\.|[^\"\\\n])*\"?|(?<![A-Za-z0-9])'(?:\\.|[^'\\\n])*'?",
                       re.S)


def rel_path(p: Path) -> str:
    try:
        return p.resolve().relative_to(tc.ROOT).as_posix()
    except ValueError:
        return p.as_posix()


def parse_int(v) -> int:
    return v if isinstance(v, int) else int(str(v), 0)


# ---------------------------------------------------------------------------------------------
# Configuration


def load_config(slug: str, root: Path) -> dict:
    try:
        game = tc.load_game(slug)
    except FileNotFoundError:
        raise ConfigError(f"unknown game: games/{slug}/game.toml not found")
    dc = game.get("decomp")
    if not dc:
        raise ConfigError(f"no [decomp] section in games/{slug}/game.toml")
    sym = game.get("symbols", {})
    ref = dc.get("reference") or sym.get("reference")
    versions = [(v["game_id"], v.get("region", "")) for v in game.get("versions", [])]
    if not ref or ref not in [v for v, _ in versions]:
        raise ConfigError("[decomp] reference must be one of the [[versions]]")
    ranges = []
    for r in dc.get("ranges", []):
        start = parse_int(r["start"])
        end = parse_int(r["end"]) if "end" in r else 1 << 32
        if end <= start:
            raise ConfigError(f"[[decomp.ranges]] {r['category']}: end <= start")
        ranges.append((start, end, r["category"]))
    ranges.sort()
    for (s0, e0, c0), (s1, _e1, c1) in zip(ranges, ranges[1:]):
        if s1 < e0:
            raise ConfigError(f"[[decomp.ranges]] overlap: {c0} {s0:08X}-{e0:08X} and {c1} at {s1:08X}")
    gdir = game["_dir"]
    code = game.get("code", {})
    return {
        "game": game, "slug": slug, "root": root, "ref": ref, "versions": versions,
        "dtk": root / sym["dtk_dir"] if sym.get("dtk_dir") else None,
        "map_dir": gdir / sym.get("map_dir", "symbols"),
        "decomp_dir": gdir / dc.get("dir", "decomp"),
        "table": gdir / dc.get("functions", "decomp/functions.tsv"),
        "columns": dc.get("columns", {}),
        "statuses": dcom.status_sets(dc),
        "oracle_columns": dcom.oracle_columns(dc),
        "excluded": set(dc.get("excluded", [])),
        "ranges": ranges,
        "overrides": {parse_int(o["addr"]): (o.get("name"), o["category"]) for o in dc.get("overrides", [])},
        "rel_excluded": set(dc.get("rel_excluded_names", [])),
        "extra_patterns": [re.compile(p) for p in dc.get("auto_name_patterns", [])],
        "disc": lambda vid: root / "generated" / slug / vid / "disc" / code.get("rel_dir", "rel"),
    }


# ---------------------------------------------------------------------------------------------
# Inventory, names, per-version addresses


def read_map_entries(path: Path) -> dict[tuple[str, int], tuple[int, str]]:
    """Symbol maps of tools/symbols.py: <ID>.map ("ADDRESS SIZE NAME") and <ID>-rel.tsv ("module_id module
    section offset size name"), hexadecimal. (module, address or offset) -> (size, name)."""
    out = {}
    if not path.exists():
        return out
    rel = path.suffix == ".tsv"
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        try:
            if rel:
                p = line.split("\t")
                if len(p) >= 6:
                    out[(p[1], int(p[3], 16))] = (int(p[4], 16), p[5])
            else:
                p = line.split()
                if len(p) == 3:
                    out[("dol", int(p[0], 16))] = (int(p[1], 16), p[2])
        except ValueError:
            pass
    return out


def read_map(path: Path) -> dict[int, str]:
    return {a: name for (_m, a), (_s, name) in read_map_entries(path).items()}


def read_rel_names(path: Path) -> dict[tuple[str, int], str]:
    return {k: name for k, (_s, name) in read_map_entries(path).items()}


def category_of(cfg: dict, addr: int) -> str:
    if addr in cfg["overrides"]:
        return cfg["overrides"][addr][1]
    for s, e, c in cfg["ranges"]:
        if s <= addr < e:
            return c
    return "unclassified"


def build_inventory(cfg: dict, warnings: list[str]) -> list[Fn] | None:
    dtk = cfg["dtk"]
    if dtk is None or not (dtk / "symbols.txt").exists():
        warnings.append(f"function inventory missing: {dtk / 'symbols.txt' if dtk else '[symbols] dtk_dir'} "
                        "(dtk symbols of the reference version, from a local clone in external/, not in Git). "
                        "Clone it, or pass --root to a checkout that has it. Percentages cannot be computed.")
        return None
    ref = cfg["ref"]
    names = read_map(cfg["map_dir"] / f"{ref}.map")
    fns = []
    for s in sy.functions(sy.read_dtk(dtk / "symbols.txt")):
        name = names.get(s.addr) or s.name
        fns.append(Fn("dol", s.addr, s.size, name, category_of(cfg, s.addr)))
    for addr, (oname, cat) in cfg["overrides"].items():
        hit = [f for f in fns if f.ref == addr]
        if not hit:
            warnings.append(f"[[decomp.overrides]] {addr:08X} ({oname}): no function starts there")
        elif oname and not is_auto(hit[0].name, []) and hit[0].name != oname:
            warnings.append(f"[[decomp.overrides]] {addr:08X}: expected {oname}, the maps say {hit[0].name}")
    unclassified = [f for f in fns if f.category == "unclassified"]
    if unclassified:
        warnings.append(f"{len(unclassified)} DOL functions outside every [[decomp.ranges]]: counted as to "
                        "decompile")
    rel_names = read_rel_names(cfg["map_dir"] / f"{ref}-rel.tsv")
    for mod, path in sorted(sy.rel_symbol_files(dtk).items()):
        for s in sy.functions(sy.read_dtk(path)):
            name = rel_names.get((mod, s.addr)) or s.name
            cat = "rel-excluded" if name in cfg["rel_excluded"] else "rel"
            fns.append(Fn(mod, s.addr, s.size, name, cat))
    for f in fns:
        f.addr[ref], f.how[ref] = f.ref, "reference"
    return fns


def map_version(cfg: dict, fns: list[Fn], vid: str, warnings: list[str]) -> dict:
    """Fills f.addr[vid] / f.how[vid]; returns the sources used."""
    ref, slug = cfg["ref"], cfg["slug"]
    src = {}
    # The report is a file of the game (in Git, next to the maps): read from the tools' tree, as map_dir.
    path = dcom.transposition_path(cfg["game"], cfg["game"]["_dir"].parent.parent, vid)
    rel = rel_path(path)
    dol = {f.ref: f for f in fns if f.module == "dol"}
    trans = dcom.read_transposition(path)
    if path.exists():
        src["dol"] = rel
    else:
        src["dol"] = None
        warnings.append(f"{vid}: no DOL addresses ({rel} missing): run `py -3 tools/symbols.py {slug} transpose "
                        f"--to {vid}`")
    # Gap addresses (tools/symbols.py gaps), activation lock of tools/decomp_common.py: strong and graph are
    # established; position and sized only through a table row marked by the oracle; other levels give nothing.
    # Same pairing as tools/decomp_functions.py (decomp_common.paired_address).
    gpath = cfg["map_dir"] / f"{vid}-gaps.tsv"
    gaps = dcom.read_gaps(gpath)
    if gaps:
        src["gaps"] = rel_path(gpath)
    gaps_by_ref = {g.ref: g for g in gaps if g.target}
    for f in dol.values():
        t = trans.get(f.ref)
        if t is not None and t.candidates:
            f.cands[vid] = (set(t.candidates), len(t.candidates) < TRANSPOSITION_CANDIDATES)
        addr, how = dcom.paired_address(f.ref, trans, gaps_by_ref)
        if dcom.is_gated(how):
            f.addr[vid], f.how[vid] = None, how
            f.gated[vid] = (addr, how[4:])
        else:
            f.addr[vid], f.how[vid] = addr, how
    # RELs: same matcher as tools/symbols.py, module by module
    mods = defaultdict(list)
    for f in fns:
        if f.module != "dol":
            mods[f.module].append(f)
    ref_dir, dst_dir = cfg["disc"](ref), cfg["disc"](vid)
    missing = []
    for mod, mfns in sorted(mods.items()):
        r_ref, r_dst = ref_dir / f"{mod}.rel", dst_dir / f"{mod}.rel"
        if not (r_ref.exists() and r_dst.exists()):
            missing.append(mod)
            for f in mfns:
                f.addr[vid], f.how[vid] = None, "unknown"
            continue
        img_ref, _ = sy.Image.from_rel(r_ref)
        img_dst, _ = sy.Image.from_rel(r_dst)
        syms = [sy.Sym(f.name, ".text", f.ref, f.size, "function") for f in mfns]
        res = sy.Matcher(img_ref, syms, img_dst, set()).run()
        for f in mfns:
            m = res.get(f.ref)
            f.addr[vid] = m.target if m else None
            f.how[vid] = m.status if m else "absent"
            if m and m.candidates:
                f.cands[vid] = (set(m.candidates), True)
    src["rel"] = "matcher" if not missing else f"matcher; no REL files for {', '.join(missing)}"
    if missing:
        warnings.append(f"{vid}: REL files missing for {len(missing)} modules under {dst_dir} or {ref_dir} "
                        "(tools/extract.py, or --root)")
    return src


# ---------------------------------------------------------------------------------------------
# Table of the decompiled functions


def column_aliases(cfg: dict) -> dict[str, str]:
    """lower-case header name -> canonical key ("name", "status", "size", "module", "v:<ID>", "o:<ID>")."""
    al = {"name": "name", "function": "name", "symbol": "name", "status": "status", "state": "status",
          "size": "size", "module": "module", "rel": "module"}
    for vid, column in cfg.get("oracle_columns", {}).items():
        al[column.lower()] = f"o:{vid}"
    for vid, region in cfg["versions"]:
        keys = {vid.lower(), f"{vid.lower()}_addr", f"{vid.lower()}_address"}
        reg = REGION_ALIASES.get(region.lower())
        if reg:
            keys |= {reg, f"{reg}_addr", f"{reg}_address"}
        if vid in cfg["columns"]:
            keys.add(cfg["columns"][vid].lower())
        for k in keys:
            al[k] = f"v:{vid}"
    return al


def parse_addr(text: str, module: str | None) -> tuple[str, int] | None:
    t = text.strip()
    if t.lower() in NO_ADDRESS:
        return None
    m = re.match(r"^([A-Za-z_][\w.]*)[:+](?:0x)?([0-9A-Fa-f]+)$", t)
    if m:
        return m.group(1).removesuffix(".rel"), int(m.group(2), 16)
    v = int(t, 16)
    if module and module.lower() not in ("dol", "main", "main.dol", "-", ""):
        return module.removesuffix(".rel"), v
    return "dol", v


def read_table(cfg: dict, warnings: list[str]) -> list[dict] | None:
    path = cfg["table"]
    if not path.exists():
        return None
    aliases = column_aliases(cfg)
    order = ["name"] + [f"v:{v}" for v in cfg["columns"]] + ["size", "status"]
    cols = None
    rows = []
    for n, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
        if not line.strip():
            continue
        tabbed = "\t" in line
        cells = [c.strip() for c in (line.split("\t") if tabbed else line.split())]
        comment = line.lstrip().startswith("#")
        if comment and cells and cells[0] == "#":
            cells = cells[1:]                     # "# name us_addr ...": the "#" alone is not a column
        head = [c.lstrip("#").strip().lower() for c in cells]
        if (cols is None and sum(h in aliases for h in head) >= 2
                and (not comment or tabbed or all(h in aliases for h in head if h))):
            cols = [aliases.get(h) for h in head]
            continue
        if comment:
            continue
        keys = cols or order
        row = {"line": n}
        for k, c in zip(keys, cells):
            if k:
                row[k] = c
        rows.append(row)
    return rows


def apply_table(cfg: dict, fns: list[Fn] | None, rows: list[dict], warnings: list[str]) -> dict:
    stats = {"rows": len(rows), "matched": 0, "names": []}
    if fns is None:
        return stats
    ref = cfg["ref"]
    index = {(f.module, f.ref): f for f in fns}
    by_version = {vid: {(f.module, f.addr[vid]): f for f in fns if f.addr.get(vid) is not None}
                  for vid, _ in cfg["versions"]}
    maps = {vid: {**read_map_entries(cfg["map_dir"] / f"{vid}.map"),
                  **read_map_entries(cfg["map_dir"] / f"{vid}-rel.tsv")} for vid, _ in cfg["versions"]}
    pending = []
    seen = set()
    loc = f"{cfg['table'].name}"
    for row in rows:
        n = row["line"]
        status = dcom.normalize_status(row.get("status", ""))
        try:
            addrs = {vid: parse_addr(row.get(f"v:{vid}", "-"), row.get("module")) for vid, _ in cfg["versions"]}
        except ValueError:
            warnings.append(f"{loc}:{n}: unreadable address")
            continue
        f = None
        if addrs.get(ref):
            f = index.get(addrs[ref])
        else:
            for vid, a in addrs.items():
                if a and by_version[vid].get(a):
                    f = by_version[vid][a]
                    break
        if f is None:
            warnings.append(f"{loc}:{n}: {row.get('name', '?')} matches no function of the inventory")
            continue
        if id(f) in seen:
            warnings.append(f"{loc}:{n}: {row.get('name', '?')} listed twice")
            continue
        seen.add(id(f))
        stats["matched"] += 1
        stats["names"].append((n, row.get("name", "")))
        if f.category in cfg["excluded"] or f.category == "rel-excluded":
            warnings.append(f"{loc}:{n}: {row.get('name', '?')} is in the excluded category {f.category}")
            continue
        if "size" in row and row["size"] not in ("", "-"):
            try:
                size = parse_size(row["size"])
            except ValueError:
                warnings.append(f"{loc}:{n}: unreadable size {row['size']} (0x-prefixed hexadecimal or decimal "
                                "digits): not counted")
                continue
            if size != f.size:
                warnings.append(f"{loc}:{n}: size {row['size']} differs from the inventory (0x{f.size:X} = "
                                f"{f.size}): not counted")
                continue
        kind = dcom.status_kind(status, cfg["statuses"])
        if kind not in ("done", "replaced"):
            if kind is None:
                warnings.append(f"{loc}:{n}: status '{status}' is not a recognized status ([decomp] done_statuses, "
                                "replaced_statuses, listed_statuses): not counted")
            continue
        for vid, _ in cfg["versions"]:
            a = addrs.get(vid)
            if a is None:
                warnings.append(f"{loc}:{n}: {row.get('name', '?')} has no {vid} address in the table: not "
                                f"counted in {vid}")
                continue
            want = f.addr.get(vid)
            if want is not None:
                if (f.module, want) != a:
                    warnings.append(f"{loc}:{n}: {vid} address {a[1]:X} differs from the transposition "
                                    f"({want:X}): not counted")
                else:
                    getattr(f, kind)[vid] = status
                continue
            gated = f.gated.get(vid)
            if gated is not None:
                # Gap address at the level position or sized: only with the oracle mark of that version.
                try:
                    marked = dcom.parse_oracle(row.get(f"o:{vid}", ""))
                except ValueError:
                    warnings.append(f"{loc}:{n}: oracle column {row.get(f'o:{vid}')!r} must be yes, no or -: not "
                                    f"counted in {vid}")
                    continue
                if (f.module, gated[0]) != a:
                    warnings.append(f"{loc}:{n}: {vid} address {a[1]:X} differs from the gap address "
                                    f"({gated[0]:X}, level {gated[1]}): not counted")
                elif not marked:
                    warnings.append(f"{loc}:{n}: {vid} address {a[1]:X} of {row.get('name', '?')} is at the gap "
                                    f"level {gated[1]}: counted only once the oracle has passed on {vid} (oracle "
                                    "column yes)")
                else:
                    pending.append((n, f, vid, a, f"gap-{gated[1]}-oracle", (kind, status)))
                continue
            why, how = check_table_address(cfg, f, vid, a, by_version[vid], maps[vid])
            if why:
                warnings.append(f"{loc}:{n}: {vid} address {a[1]:X} of {row.get('name', '?')} not counted: {why}")
            else:
                pending.append((n, f, vid, a, how, (kind, status)))
    # The same table-only address given to two functions: neither counts (as the matcher's collisions).
    claims = Counter((vid, a) for _n, _f, vid, a, _h, _s in pending)
    for n, f, vid, a, how, status in pending:
        if claims[(vid, a)] > 1:
            warnings.append(f"{loc}:{n}: {vid} address {a[1]:X} given to {claims[(vid, a)]} functions by the "
                            "table: not counted")
            continue
        f.addr[vid], f.how[vid] = a[1], how
        getattr(f, status[0])[vid] = status[1]
    return stats


def parse_size(text: str) -> int:
    """Size column: "0x"-prefixed = hexadecimal (0x20 = 32), digits only = decimal (20 = 20). Anything else, such
    as bare hexadecimal ("2F4"), is refused (ValueError) rather than guessed."""
    t = text.strip()
    if re.fullmatch(r"0[xX][0-9A-Fa-f]+", t):
        return int(t, 16)
    if re.fullmatch(r"[0-9]+", t):
        return int(t, 10)
    raise ValueError(t)


def check_table_address(cfg: dict, f: Fn, vid: str, a: tuple[str, int], taken: dict,
                        vmap: dict) -> tuple[str | None, str]:
    """A version address given by the table for a function that has none from the transposition (absent,
    ambiguous, collision...). It counts only when verified: (1) not already the address of another function in
    that version, and (2) either the only one of the function's candidates (starts where the matcher found code
    equal to the reference, relocations masked: same start, same size) that no other function holds, from a
    complete candidate list (elimination, as the matcher's neighbors), or the start of a function of the same size in
    that version's symbol map (<ID>.map / <ID>-rel.tsv) whose name does not contradict. Returns (reason it does
    not count, or None; address status)."""
    if a[0] != f.module:
        return f"module {a[0]} is not the function's module ({f.module})", ""
    other = taken.get(a)
    if other is not None and other is not f:
        return f"already the {vid} address of {other.name} ({other.label(cfg['ref'])} in {cfg['ref']})", ""
    cands, full = f.cands.get(vid, (set(), True))
    if a[1] in cands:
        free = sorted(c for c in cands if taken.get((f.module, c)) in (None, f))
        if not full:
            return (f"{len(cands)} candidates listed, the transposition report keeps only the first "
                    f"{TRANSPOSITION_CANDIDATES}: cannot tell which one is this function"), ""
        if free != [a[1]]:
            return (f"one of {len(free)} candidates not taken by another function "
                    f"({', '.join(f'{c:X}' for c in free)}): which one is this function is not established"), ""
        return None, "table-candidate"
    entry = vmap.get(a)
    if entry:
        size, name = entry
        if size != f.size:
            return f"the {vid} symbol map gives a function of size 0x{size:X} there, not 0x{f.size:X}", ""
        if name != f.name and not is_auto(name, []) and not is_auto(f.name, []):
            return f"the {vid} symbol map names the function there {name}, not {f.name}", ""
        return None, "table-map"
    return (f"not verified (neither a candidate of the matcher, code equal to the reference, nor a function of "
            f"that size in the {vid} symbol map); establish it in symbols/supplement.tsv (verified) and rerun "
            f"`tools/symbols.py {cfg['slug']} transpose --to {vid}`"), ""


# ---------------------------------------------------------------------------------------------
# Automatic names in the decompiled code


def scan_code(cfg: dict, table_names: list[tuple[int, str]]) -> dict:
    d = cfg["decomp_dir"]
    hits = []
    if d.exists():
        for p in sorted(d.rglob("*")):
            if p.suffix.lower() not in CODE_EXTENSIONS or not p.is_file():
                continue
            code = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
            for n, line in enumerate(code.splitlines(), 1):
                for kind, name in auto_matches(line, cfg["extra_patterns"]):
                    hits.append({"file": rel_path(p), "line": n, "name": name, "kind": kind})
    table = rel_path(cfg["table"])
    for n, name in table_names:
        if is_auto(name, cfg["extra_patterns"], dtk_name=False):
            hits.append({"file": table, "line": n, "name": name or "(empty)", "kind": "table name"})
    return {"path": rel_path(d), "present": d.exists(), "count": len(hits),
            "by_kind": dict(Counter(h["kind"] for h in hits)), "hits": hits}


# ---------------------------------------------------------------------------------------------
# Report


def module_stats(fns: list[Fn], vid: str, extra: list[re.Pattern]) -> dict:
    s = {"functions": 0, "functions_done": 0, "bytes": 0, "bytes_done": 0, "functions_replaced": 0,
         "bytes_replaced": 0, "no_address": 0, "no_address_bytes": 0, "auto_names_left": 0, "address_status": Counter()}
    for f in fns:
        s["functions"] += 1
        s["bytes"] += f.size
        s["address_status"][f.how.get(vid, "unknown")] += 1
        if f.addr.get(vid) is None:
            s["no_address"] += 1
            s["no_address_bytes"] += f.size
        if vid in f.done:
            s["functions_done"] += 1
            s["bytes_done"] += f.size
            continue
        if vid in f.replaced:
            s["functions_replaced"] += 1
            s["bytes_replaced"] += f.size
        if is_auto(f.name, extra):
            s["auto_names_left"] += 1
    s["percent_functions"] = pct(s["functions_done"], s["functions"])
    s["percent_bytes"] = pct(s["bytes_done"], s["bytes"])
    s["address_status"] = dict(s["address_status"])
    return s


def report(cfg: dict, fns: list[Fn] | None, versions: list[str], modules: set[str] | None, want_list: bool,
           table: dict, code: dict, warnings: list[str]) -> dict:
    out = {"game": cfg["slug"], "reference": cfg["ref"], "byte_sizes": f"functions of {cfg['ref']}",
           "inventory": {"available": fns is not None,
                         "source": (cfg["dtk"] / "symbols.txt").as_posix() if cfg["dtk"] else None},
           "table": {"path": rel_path(cfg["table"]), "present": table is not None,
                     "rows": table["rows"] if table else 0, "matched": table["matched"] if table else 0},
           "partition": {}, "versions": {}, "auto_names": {"decomp_dir": {k: v for k, v in code.items()
                                                                           if want_list or k != "hits"}},
           "warnings": warnings}
    complete = fns is not None and code["count"] == 0
    if fns is not None:
        part = defaultdict(lambda: {"functions": 0, "bytes": 0})
        for f in fns:
            key = ("dol:" if f.module == "dol" else "rel:") + f.category
            part[key]["functions"] += 1
            part[key]["bytes"] += f.size
        for k, v in part.items():
            v["excluded"] = k.split(":", 1)[1] in cfg["excluded"] or k == "rel:rel-excluded"
        out["partition"] = dict(sorted(part.items()))
        work = [f for f in fns if f.category not in cfg["excluded"] and f.category != "rel-excluded"]
        mod_names = ["dol"] + sorted({f.module for f in work if f.module != "dol"})
        for vid in versions:
            region = dict(cfg["versions"]).get(vid, "")
            v = {"region": region, "modules": {}}
            for m in mod_names:
                if modules and m not in modules:
                    continue
                v["modules"][m] = module_stats([f for f in work if f.module == m], vid, cfg["extra_patterns"])
            sel = [f for f in work if not modules or f.module in modules]
            v["total"] = module_stats(sel, vid, cfg["extra_patterns"])
            if want_list:
                v["remaining"] = [{"module": f.module, "address": f.label(vid), "reference": f"{f.ref:08X}"
                                   if f.module == "dol" else f"{f.module}:{f.ref:X}", "size": f.size,
                                   "category": f.category, "address_status": f.how.get(vid), "name": f.name}
                                  for f in sel if vid not in f.done]
            out["versions"][vid] = v
        # Completeness ignores --version and --module: every version, every module, always.
        for vid, _ in cfg["versions"]:
            complete = complete and all(vid in f.done and f.addr.get(vid) is not None for f in work)
    out["complete"] = complete
    return out


def print_text(out: dict, want_list: bool) -> None:
    print(f"Decompilation progress: {out['game']} (reference {out['reference']}; bytes = sizes of the "
          f"{out['reference']} functions)")
    t = out["table"]
    print(f"Table: {t['path']}: " + (f"{t['rows']} rows, {t['matched']} matched" if t["present"]
                                    else "missing (nothing decompiled yet)"))
    if not out["inventory"]["available"]:
        print("Function inventory missing: no percentages (see the warnings).")
    if out["partition"]:
        print("\nPartition (functions / bytes):")
        for k, v in out["partition"].items():
            print(f"  {k:22s} {v['functions']:6d} fn {v['bytes']:9d} B  "
                  f"{'excluded' if v['excluded'] else 'to decompile'}")
    for vid, v in out["versions"].items():
        print(f"\n{vid} ({v['region']}):")
        print(f"  {'module':9s} {'functions (verified)':>22s} {'bytes (verified)':>27s} "
              f"{'replaced, not verified':>23s} {'no address':>11s} {'auto names left':>16s}")
        rows = list(v["modules"].items()) + [("total", v["total"])]
        for m, s in rows:
            fn = f"{s['functions_done']}/{s['functions']} {fmt_pct(s['percent_functions'])}"
            by = f"{s['bytes_done']}/{s['bytes']} {fmt_pct(s['percent_bytes'])}"
            rp = f"{s['functions_replaced']} fn {s['bytes_replaced']} B"
            print(f"  {m:9s} {fn:>22s} {by:>27s} {rp:>23s} {s['no_address']:>11d} {s['auto_names_left']:>16d}")
        st = v["total"]["address_status"]
        if list(st) != ["reference"]:
            print("  address sources: " + ", ".join(f"{k} {n}" for k, n in sorted(st.items(), key=lambda x: -x[1])))
            print(f"  without an address in {vid}: {v['total']['no_address']} functions, "
                  f"{v['total']['no_address_bytes']} B (cannot count as decompiled)")
        if want_list:
            print(f"  remaining ({len(v['remaining'])}): module address reference size category status name")
            for r in v["remaining"]:
                print(f"    {r['module']:8s} {r['address']:>16s} {r['reference']:>16s} {r['size']:6X} "
                      f"{r['category']:12s} {r['address_status']:14s} {r['name']}")
    c = out["auto_names"]["decomp_dir"]
    print(f"\nAutomatic names in {c['path']}" + ("" if c["present"] else " (folder missing)") + f": {c['count']}"
          + (" (" + ", ".join(f"{k} {n}" for k, n in c["by_kind"].items()) + ")" if c["by_kind"] else ""))
    if want_list:
        for h in c.get("hits", []):
            print(f"  {h['file']}:{h['line']}: {h['name']} ({h['kind']})")
    if out["warnings"]:
        print(f"\nWarnings ({len(out['warnings'])}):")
        for w in out["warnings"]:
            print(f"  - {w}")
    print("\nComplete (every version and module, whatever the filters: 100 %, every address known, no automatic "
          f"name): {'yes' if out['complete'] else 'no'}")


# ---------------------------------------------------------------------------------------------
# Planning views (--routes, --closure): static graph of tools/decomp_graph.py, one per version


def disc_root(cfg: dict, vid: str) -> Path:
    return cfg["root"] / "generated" / cfg["slug"] / vid / "disc"


def guest_address(f: Fn, vid: str, image) -> tuple[int | None, bool]:
    """(guest address of `f` in `vid`, provisional): a gap address not established is given, marked provisional."""
    a, provisional = f.addr.get(vid), False
    if a is None and vid in f.gated:
        a, provisional = f.gated[vid][0], True
    if a is None:
        return None, False
    if f.module != "dol":
        base = image.modules.get(f.module)
        return (None, False) if base is None else (base + a, provisional)
    return a, provisional


def version_sizes(cfg: dict, vid: str) -> dict[int, int]:
    """Reference address -> size of the function in `vid`, where the symbol files establish it: same code as the
    reference (transposition report, identical), or the target size of the gap file."""
    if vid == cfg["ref"]:
        return {}
    out = {r: t.ref_size for r, t in dcom.read_transposition(
        dcom.transposition_path(cfg["game"], cfg["game"]["_dir"].parent.parent, vid)).items() if t.identical}
    for g in dcom.read_gaps(cfg["map_dir"] / f"{vid}-gaps.tsv"):
        if g.size:
            out[g.ref] = g.size
    return out


def version_graph(cfg: dict, fns: list[Fn], vid: str, warnings: list[str]):
    """(graph, image) of `vid`: every function of the inventory that has an address there."""
    import decomp_functions as dfn
    import decomp_graph as dg
    image = dg.load_image(cfg["game"], disc_root(cfg, vid))
    warnings += [f"{vid}: {w}" for w in image.warnings]
    placed: dict[int, Fn] = {}
    for f in fns:
        a = guest_address(f, vid, image)[0]
        if a is None:
            continue
        if a in placed:
            warnings.append(f"{vid}: {f.name} and {placed[a].name} both at {a:08X}: the first one is kept in the graph")
            continue
        placed[a] = f
    sizes = version_sizes(cfg, vid)
    starts = sorted(placed)
    nodes = []
    for i, a in enumerate(starts):
        f = placed[a]
        base, blob = dfn._code_of(image.code, a)
        if blob is None:
            warnings.append(f"{vid}: {f.name}: {a:08X} is outside the code sections, not in the graph")
            continue
        limit = min(starts[i + 1] if i + 1 < len(starts) else base + len(blob), base + len(blob))
        if vid == cfg["ref"]:
            size = f.size
        elif f.module == "dol" and f.ref in sizes:
            size = sizes[f.ref]
        elif f.size <= limit - a and dfn._ends_function(dfn._word(blob, a + f.size - 4 - base)):
            size = f.size                              # not established: the reference size still ends a function
        else:
            size = limit - a                           # not established: up to the next known function
            while size > 4 and dfn._word(blob, a + size - 4 - base) == 0:
                size -= 4
        nodes.append(dg.Node(a, min(size, limit - a), f.name, f))
    return dg.Graph(image, nodes), image


def load_coverage(cfg: dict, cov_args: list[str], hook_args: list[str]) -> dict:
    """Version -> dg.Coverage, for the versions that have runs. Configured runs: [decomp.coverage_runs]; --cov replaces
    all of them. A list of probe addresses is required: without it, not measured and never reached look the same."""
    import decomp_graph as dg
    root, slug = cfg["root"], cfg["slug"]
    dc = cfg["game"].get("decomp", {})
    runs: dict[str, list[tuple[str, Path]]] = defaultdict(list)
    if cov_args:
        for c in cov_args:
            vid, name, path = c.split(":", 2)
            p = Path(path)
            runs[vid].append((name, p / "cov.tsv" if p.is_dir() else p))
    else:
        for vid, patterns in dc.get("coverage_runs", {}).items():
            for p in dg.expand_runs([x.format(slug=slug, id=vid) for x in patterns], root):
                runs[vid].append((p.parent.name, p))
    hooks = {vid: root / dc["coverage_hooks"].format(slug=slug, id=vid) for vid, _r in cfg["versions"]
             if dc.get("coverage_hooks")}
    for h in hook_args:
        vid, path = h.split(":", 1)
        hooks[vid] = Path(path)

    def better_list(vid: str, entries: list[int]) -> str:
        """Names the version whose probe list holds these entries, when it is not `vid`."""
        for other, path in hooks.items():
            if other != vid and path.exists() and dg.probe_share(entries, dg.read_hooks(path)) >= dg.PROBE_SHARE_REFUSED:
                return f"; they fit the list of {other}"
        return ""

    out = {}
    for vid, items in runs.items():
        if vid not in hooks or not hooks[vid].exists():
            raise dg.GraphError(f"{vid}: list of the addresses the probe was compiled on missing "
                                f"({hooks.get(vid, '[decomp] coverage_hooks')}): not measured and never reached "
                                "cannot be told apart")
        cov = dg.Coverage()
        cov.hooks = dg.read_hooks(hooks[vid])
        for name, path in items:
            if not path.exists():
                raise dg.GraphError(f"{path}: coverage file missing")
            entries = dg.read_cov(path, cov, name)
            share = dg.probe_share(entries, cov.hooks)
            said = f"{share:.1f} % of the {len(entries)} function entries of {name} are in the probe list of {vid}"
            if share < dg.PROBE_SHARE_REFUSED:
                raise dg.GraphError(f"{path}: only {said}: this run or this list comes from another version"
                                    f"{better_list(vid, entries)}")
            if share < dg.PROBE_SHARE_WARNED:
                cov.warnings.append(f"only {said}: check that this run and this list go together")
        out[vid] = cov
    return out


def reach_status(f: Fn, addr: int | None, cov) -> str:
    if addr is None:
        return "not-measured:no-address"
    if addr in cov.reached:
        return "reached"
    if addr in cov.hooks:
        return "never-reached"
    return "not-measured:rel-module" if f.module != "dol" else "not-measured:no-probe"


def reached_in(f: Fn, vid: str, cov) -> bool:
    """`f` was seen in `vid`. DOL only: REL functions were not measured."""
    if f.module != "dol":
        return False
    a = f.addr.get(vid)
    if a is None and vid in f.gated:
        a = f.gated[vid][0]
    return a is not None and a in cov.reached


def path_of(found: dict, target: int) -> list[tuple[int, object]]:
    """Functions from the source to `target`, each with the site of the edge that enters it."""
    out = []
    a = target
    while a is not None:
        _d, site, parent = found[a]
        out.insert(0, (a, site))
        a = parent
    return out


def taken_reason(graph, a: int) -> str:
    refs = graph.taken[a]
    return f"{graph.nodes[a].name} ({a:08X}) has its address taken ({len(refs)}, first {refs[0]})"


def routes_view(cfg: dict, fns: list[Fn], work: list[Fn], rows_of: list[Fn], vid: str, covs: dict, top: int,
                warnings: list[str]) -> dict:
    import decomp_functions as dfn
    import decomp_graph as dg
    cov = covs[vid]
    graph, image = version_graph(cfg, fns, vid, warnings)
    no_return = dfn.no_return_addresses(cfg["game"], vid)
    code = image.code
    status_at = {a: reach_status(n.tag, a, cov) for a, n in graph.nodes.items()}
    unmeasured = {a for a, s in status_at.items() if s.startswith("not-measured")}
    sources = [a for a, s in status_at.items() if s == "reached"]
    near = dg.shortest_from(graph, sources)
    loose = dg.shortest_from(graph, sources, extra=graph.pointer_sites())
    far = dg.shortest_from(graph, sorted(unmeasured), skip=set(sources))
    seen = dg.seen_instructions(graph, cov, code)
    flows: dict[int, tuple] = {}

    def flow(a: int):
        if a not in flows:
            c = dg.control_flow(code, a, graph.nodes[a].size, no_return)
            flows[a] = (c, dg.covered_blocks(c, a, seen.get(a, set())))
        return flows[a]

    def hops(found: dict, target: int) -> list[dict]:
        return [{"function": f"{a:08X}", "name": graph.nodes[a].name, "reach": status_at[a],
                 "site": f"{s.at:08X}" if s else None, "kind": s.kind if s else None,
                 "data": f"{s.via:08X}" if s and s.via else None} for a, s in path_of(found, target)]

    def first_step(found: dict, a: int) -> dict:
        chain = path_of(found, a)
        start, site = chain[0][0], chain[1][1]
        c, known = flow(start)
        block = c.block_of(site.at)
        gate = dg.find_gate(code, c, block, known)
        for k in ("at", "to"):
            if k in gate:
                gate[k] = f"{gate[k]:08X}"
        return {"hops": len(chain) - 1, "start": f"{start:08X}", "start_name": graph.nodes[start].name,
                "site": f"{site.at:08X}", "site_kind": site.kind, "block": f"{block:08X}", "gate": gate,
                "path": hops(found, a)}

    def capped(reasons: list[str]) -> dict:
        return {"reasons": reasons[:REASONS_KEPT], "reasons_total": len(reasons)}

    def itself_unmeasured(a: int) -> list[str]:
        """A function not measured is never "exhaustive": it may have run. As a last reason only: the reach column
        already says it, the other rows do not need it."""
        return [f"{graph.nodes[a].name} ({a:08X}) is not measured"] if a in unmeasured else []

    def route(a: int) -> dict:
        if a in near:
            r = first_step(near, a)
            d = r["hops"]
            ball = dg.callers_within(graph, a, d)
            reasons = [taken_reason(graph, x) for x in sorted(ball) if graph.taken.get(x)]
            reasons += [f"{graph.nodes[x].name} ({x:08X}) is not measured" for x in sorted(ball)
                        if x in unmeasured and x != a]
            if r["gate"]["status"] == "unknown":
                reasons.append(f"block {r['block']} is behind a jump or a path that is not followed")
            reasons = reasons or itself_unmeasured(a)
            r["alternatives"] = sum(1 for x, e in dg.callers_within(graph, a, d + 1).items()
                                    if e == d and status_at[x] == "reached")
            return {"status": "found", "flag": "incomplete frontier" if reasons else "exhaustive", **r, **capped(reasons)}
        if a in loose:
            r = first_step(loose, a)
            if r["gate"]["status"] == "contradiction":
                r["gate"] = {"status": "ran"}          # a load that ran is no contradiction: nothing called the pointer
            reason = "a step goes through an address reference: the indirect call that uses it is not known"
            return {"status": "pointer", "flag": "incomplete frontier", **r, **capped([reason])}
        ancestors = dg.all_callers(graph, a)
        reasons = [taken_reason(graph, x) for x in sorted(ancestors) if graph.taken.get(x)]
        reasons += [f"{graph.nodes[x].name} ({x:08X}) is not measured" for x in sorted(ancestors)
                    if x in unmeasured and x != a]
        reasons = reasons or itself_unmeasured(a)
        if not reasons:
            return {"status": "not-found", "flag": "exhaustive", "hops": None, **capped([])}
        r = {"status": "unknown", "flag": "incomplete frontier", "hops": None, **capped(reasons)}
        if a in far and far[a][0]:
            r["from_not_measured"] = hops(far, a)
        return r

    rows = []
    wanted = {id(f) for f in rows_of}
    counts: Counter = Counter()
    by_hops: Counter = Counter()
    targets: dict[str, dict[tuple[str, str], dict]] = {"found": {}, "pointer": {}}
    for f in work:
        a, prov = guest_address(f, vid, image)
        if a is not None and a not in graph.nodes:
            a = None
        st = reach_status(f, a, cov)
        kind = st.split(":")[0]
        counts[st] += 1
        if st == "reached":
            continue
        r = route(a) if a is not None else {"status": "unknown", "flag": "incomplete frontier", "hops": None,
                                            "reasons": [f"no address in {vid}"], "reasons_total": 1}
        counts[f"{kind} {r['status']} {r['flag']}"] += 1
        if kind == "never-reached":
            by_hops[r["hops"] if r["hops"] is not None else "-"] += 1
            if r["status"] in targets:
                t = targets[r["status"]].setdefault((r["start"], r["site"]), {
                    "start": r["start"], "start_name": r["start_name"], "site": r["site"], "kind": r["site_kind"],
                    "block": r["block"], "gate": r["gate"], "functions": 0, "bytes": 0})
                t["functions"] += 1
                t["bytes"] += f.size
        if id(f) in wanted:
            rows.append({"name": f.name, "module": f.module, "address": f"{a:08X}" if a is not None else "-",
                         "reference": f"{f.ref:08X}" if f.module == "dol" else f"{f.module}:{f.ref:X}", "size": f.size,
                         "category": f.category, "reach": st, "provisional": prov, "decompiled": vid in f.done,
                         "reached_in": [v for v, c in covs.items() if v != vid and reached_in(f, v, c)], "route": r})
    return {"region": dict(cfg["versions"]).get(vid, ""), "runs": cov.runs, "hooks": len(cov.hooks),
            "probe_warnings": cov.warnings, "edges": sum(len(v) for v in graph.callers.values()),
            "functions": len(graph.nodes), "counts": dict(counts),
            "hops": {str(k): v for k, v in sorted(by_hops.items(), key=lambda x: str(x[0]))},
            "targets": {k: sorted(v.values(), key=lambda t: (-t["functions"], -t["bytes"], t["site"]))[:top]
                        for k, v in targets.items()},
            "rows": rows}


def closure_view(cfg: dict, fns: list[Fn], work: list[Fn], vid: str, covs: dict, top: int, warnings: list[str]) -> dict:
    import build
    import decomp_functions as dfn
    import decomp_graph as dg
    graph, image = version_graph(cfg, fns, vid, warnings)
    disc = disc_root(cfg, vid)
    scan = build.signature_scan(disc / cfg["game"]["code"]["dol"], dfn._layout(cfg["game"], disc))
    native = {a for addrs in scan["replaceable"].values() for a in addrs}

    def kind(a: int) -> str:
        f = graph.nodes[a].tag
        if vid in f.done:
            return "decompiled"
        if vid in f.replaced:
            return "replaced"
        if a in native:
            return "native"
        if f.category in cfg["excluded"] or f.category == "rel-excluded":
            return "library"
        return "recompiled"

    kinds = {a: kind(a) for a in graph.nodes}

    def spread(seeds: set[int], include_seeds: bool) -> set[int]:
        """Functions with a transitive direct callee in `seeds`, not through a native function (its body does not
        run). `include_seeds`: a seed counts for itself (an indirect call of its own); otherwise only for its callers."""
        out = set(seeds) if include_seeds else set()
        seen = set(seeds)
        todo = list(seeds)
        while todo:
            for c, _s in graph.callers.get(todo.pop(), ()):
                if kinds[c] == "native":
                    continue
                out.add(c)
                if c not in seen:
                    seen.add(c)
                    todo.append(c)
        return out

    recompiled = {a for a, k in kinds.items() if k == "recompiled"}
    library = {a for a, k in kinds.items() if k == "library"}
    uncertain = {a for a, n in graph.nodes.items() if kinds[a] != "native" and (n.indirect or n.unknown_targets)}
    open_set, lib_set, unk_set = spread(recompiled, False), spread(library, False), spread(uncertain, True)

    def state(a: int) -> str:
        return "open" if a in open_set else "library" if a in lib_set else "unknown" if a in unk_set else "closed"

    tally = {"all": Counter(), "decompiled": Counter(), "recompiled": Counter()}
    no_address = 0
    work_nodes = []
    for f in work:
        a = guest_address(f, vid, image)[0]
        if a is None or a not in graph.nodes:
            no_address += 1
            continue
        work_nodes.append(a)
        s = state(a)
        tally["all"][s] += 1
        tally["decompiled" if kinds[a] in ("decompiled", "replaced") else "recompiled"][s] += 1
    decompiled = {a for a, k in kinds.items() if k in ("decompiled", "replaced")}
    leaves = sum(1 for a in work_nodes if state(a) == "closed" and not graph.nodes[a].sites)
    cov = covs.get(vid)

    def ranking(wanted: set[int]) -> list[dict]:
        rows = []
        for a in wanted:
            sites = [(c, s) for c, s in graph.callers.get(a, ()) if c in decompiled]
            if not sites:
                continue
            n = graph.nodes[a]
            rows.append({"address": f"{a:08X}", "name": n.name, "size": n.size, "category": n.tag.category,
                         "module": n.tag.module, "call_sites": len(sites), "callers": len({c for c, _ in sites}),
                         "decompiled_blocked": len({c for c in dg.all_callers(graph, a) if c in decompiled}),
                         "calls": None if cov is None else cov.reached.get(a, 0)})
        return sorted(rows, key=lambda r: (-r["call_sites"], -r["callers"], r["address"]))

    next_batches = ranking(recompiled)
    libraries = ranking(library)
    ready_set = {a for a in work_nodes if kinds[a] == "recompiled" and state(a) == "closed"}

    def unlocks(a: int) -> int:
        """Recompiled callers of `a` that become ready once `a` is decompiled: `a` is the only direct callee they still
        wait for, and they have no indirect call."""
        n = 0
        for c in {c for c, _s in graph.callers.get(a, ())}:
            nc = graph.nodes[c]
            if kinds[c] != "recompiled" or c == a or nc.indirect or nc.unknown_targets:
                continue
            if all(s.callee in (a, c) or kinds.get(s.callee) in ("decompiled", "replaced", "native") for s in nc.sites):
                n += 1
        return n

    ready = sorted(ready_set, key=lambda a: (-unlocks(a), -len({c for c, _s in graph.callers.get(a, ())}), a))
    total = sum(tally["all"].values())
    return {"region": dict(cfg["versions"]).get(vid, ""),
            "functions": {k: sum(1 for x in kinds.values() if x == k)
                          for k in ("decompiled", "replaced", "native", "library", "recompiled")},
            "work": total, "no_address": no_address, "leaves_closed": leaves,
            "states": {k: dict(v) for k, v in tally.items()},
            "percent_closed": pct(tally["all"]["closed"], total),
            "percent_decompiled_closed": pct(tally["decompiled"]["closed"], sum(tally["decompiled"].values())),
            "next_batches": next_batches[:top], "next_batches_total": len(next_batches),
            "libraries": libraries[:top], "libraries_total": len(libraries),
            "ready": [{"address": f"{a:08X}", "name": graph.nodes[a].name, "size": graph.nodes[a].size,
                       "unlocks": unlocks(a), "callers": len({c for c, _ in graph.callers.get(a, ())})}
                      for a in ready[:top]],
            "ready_total": len(ready)}


def gate_text(g: dict) -> str:
    if g["status"] != "gate":
        return g["status"]
    return f"{g['branch']} {g['at']} -> {g['to']} (+{g['between']})"


def route_text(r: dict) -> str:
    x = r["route"]
    parts = []
    if x["status"] in ("found", "pointer"):
        parts.append(f"from {x['start']} {x['start_name']} site {x['site']} ({x['site_kind']}) block {x['block']} "
                     f"gate {gate_text(x['gate'])} path "
                     + " > ".join(h["function"] + (f"({h['kind']})" if h["kind"] in ("pointer", "table") else "")
                                  for h in x["path"])
                     + (f" [{x['alternatives']} start(s) at this distance]" if "alternatives" in x else ""))
    elif "from_not_measured" in x:
        parts.append("from a function not measured: " + " > ".join(h["function"] for h in x["from_not_measured"]))
    if x["reasons"]:
        parts.append(f"{x['reasons_total']} reason(s), first: {x['reasons'][0]}")
    if r["reached_in"]:
        parts.append("reached in " + ",".join(r["reached_in"]))
    return " ; ".join(parts)


def print_routes(out: dict) -> None:
    x = out.get("routes_all_versions")
    if x:
        print(f"Functions of the DOL to decompile: {x['functions']}; reached in at least one version: {x['reached_any']}; "
              f"reached in no version: {x['reached_none']} ({x['reached_none_remaining']} of them not decompiled yet); "
              f"reached in one version only: {x['reached_one']}")
    for vid, v in out["routes"].items():
        print(f"\nRoutes {vid} ({v['region']}): {v['functions']} functions in the graph, {v['edges']} direct edges; "
              f"{len(v['runs'])} runs: {', '.join(v['runs'])}; probe on {v['hooks']} addresses")
        for w in v["probe_warnings"]:
            print(f"  warning: {w}")
        c = v["counts"]
        print("  Reach of the functions to decompile:")
        for k in sorted(k for k in c if " " not in k):
            print(f"    {k:28s} {c[k]:6d}")
        print("  Routes:")
        for k in sorted(k for k in c if " " in k):
            print(f"    {k:52s} {c[k]:6d}")
        print("  Hops of the never-reached functions (- = no route): "
              + ", ".join(f"{k}: {n}" for k, n in v["hops"].items()))
        for key, title in (("found", "Call sites that open the most never-reached functions"),
                           ("pointer", "Address loads behind the most never-reached functions, the indirect call unknown")):
            print(f"  {title} (top {len(v['targets'][key])}): functions bytes site kind block gate, start")
            for t in v["targets"][key]:
                print(f"    {t['functions']:5d} {t['bytes']:7d} {t['site']} {t['kind']:7s} {t['block']} "
                      f"{gate_text(t['gate']):34s} {t['start']} {t['start_name']}")
        print(f"  Functions ({len(v['rows'])}): address size reach route flag hops name | detail")
        for r in v["rows"]:
            hops = r["route"]["hops"]
            print(f"    {r['address']:>8s} {r['size']:6X} {r['reach'] + ('?' if r['provisional'] else ''):24s} "
                  f"{r['route']['status']:9s} {r['route']['flag']:19s} {'-' if hops is None else hops:>2} "
                  f"{r['name']} | {route_text(r)}")


def print_closure(out: dict) -> None:
    for vid, v in out["closure"].items():
        print(f"\nClosure {vid} ({v['region']}): " + ", ".join(f"{k} {n}" for k, n in v["functions"].items())
              + " (whole inventory; native = superset of what the runtime replaces)")
        print(f"  Functions to decompile with an address here: {v['work']} (no address: {v['no_address']})")
        print("  State (transitive direct callees)   closed  unknown  library     open")
        for k in ("all", "decompiled", "recompiled"):
            s = v["states"][k]
            print(f"    {k:32s} {s.get('closed', 0):7d} {s.get('unknown', 0):8d} {s.get('library', 0):8d} "
                  f"{s.get('open', 0):8d}")
        print(f"  Closed, proven: {fmt_pct(v['percent_closed'])} of the functions to decompile ({v['leaves_closed']} of "
              f"them have no call at all); {fmt_pct(v['percent_decompiled_closed'])} of the decompiled ones")
        print(f"  Next batches: recompiled functions called by decompiled code ({v['next_batches_total']}, top "
              f"{len(v['next_batches'])}): sites callers blocked size address name [calls measured]")
        for r in v["next_batches"]:
            print(f"    {r['call_sites']:4d} {r['callers']:4d} {r['decompiled_blocked']:4d} {r['size']:6X} "
                  f"{r['address']} {r['name']} [{'-' if r['calls'] is None else r['calls']}]")
        print(f"  Library functions still recompiled, called by decompiled code ({v['libraries_total']}, top "
              f"{len(v['libraries'])}): sites callers blocked size address name")
        for r in v["libraries"]:
            print(f"    {r['call_sites']:4d} {r['callers']:4d} {r['decompiled_blocked']:4d} {r['size']:6X} "
                  f"{r['address']} {r['name']}")
        print(f"  Ready: recompiled, every callee decompiled or native, no indirect call ({v['ready_total']}, top "
              f"{len(v['ready'])}): unlocks callers size address name (unlocks: callers it would make ready)")
        for r in v["ready"]:
            print(f"    {r['unlocks']:4d} {r['callers']:4d} {r['size']:6X} {r['address']} {r['name']}")


def selected_functions(work: list[Fn], names: list[str]) -> list[Fn]:
    wanted = {n.lower() for n in names}
    out = []
    for f in work:
        known = [a for a in f.addr.values() if a is not None] + [g[0] for g in f.gated.values()]
        keys = {f.name.lower(), f"{f.ref:08x}"} | {f"{a:08x}" for a in known}
        if keys & wanted:
            out.append(f)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--version", help="comma-separated version IDs (default: all of game.toml)")
    ap.add_argument("--module", help="comma-separated modules: dol and/or REL names (default: all)")
    ap.add_argument("--list", action="store_true", help="list what remains and the automatic names found")
    ap.add_argument("--json", action="store_true", help="JSON output")
    ap.add_argument("--root", type=Path, help="checkout holding external/ and generated/ (default: this one)")
    ap.add_argument("--table", type=Path, help="table of the decompiled functions to read instead")
    ap.add_argument("--decomp-dir", type=Path, help="decompiled code folder to scan instead")
    ap.add_argument("--map-dir", type=Path, help="folder of the symbol maps (<ID>.map, <ID>-rel.tsv) to read instead")
    ap.add_argument("--require-complete", action="store_true", help="exit 1 unless everything is decompiled")
    ap.add_argument("--routes", action="store_true", help="routes to the functions no run reached")
    ap.add_argument("--closure", action="store_true", help="what decompiled code still depends on (not "
                    "decomp_oracle.py report --closure: whether a callee of an enabled function can wait)")
    ap.add_argument("--function", help="comma-separated: limit the rows of --routes (name or address)")
    ap.add_argument("--top", type=int, default=20, help="length of the rankings (default 20)")
    ap.add_argument("--cov", action="append", default=[], help="ID:NAME:<cov.tsv or its folder>, repeatable")
    ap.add_argument("--cov-hooks", action="append", default=[], help="ID:<list of probe addresses>, repeatable")
    ap.add_argument("--parity", action="store_true", help="masked code of the rows between the versions")
    ap.add_argument("--proof", action="store_true", help="verified rows: fresh, stale or unknown proof")
    ap.add_argument("--require-fresh", action="store_true", help="with --proof: exit 1 on a stale-unit row")
    ap.add_argument("--proof-file", type=Path, help="local digest file to read instead of [decomp] proof_digests")
    ns = ap.parse_args()
    if ns.require_fresh and not ns.proof:
        ap.error("--require-fresh goes with --proof")
    root = (ns.root or tc.ROOT).resolve()
    try:
        cfg = load_config(ns.game, root)
    except (ConfigError, KeyError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if ns.table:
        cfg["table"] = ns.table.resolve()
    if ns.map_dir:
        cfg["map_dir"] = ns.map_dir.resolve()
    if ns.decomp_dir:
        cfg["decomp_dir"] = ns.decomp_dir.resolve()
    all_ids = [v for v, _ in cfg["versions"]]
    versions = ns.version.split(",") if ns.version else all_ids
    bad = [v for v in versions if v not in all_ids]
    if bad:
        print(f"error: unknown version(s) {', '.join(bad)} (known: {', '.join(all_ids)})", file=sys.stderr)
        return 2
    modules = set(ns.module.split(",")) if ns.module else None
    warnings: list[str] = []
    fns = build_inventory(cfg, warnings)
    if fns is not None:
        known = {f.module for f in fns}
        if modules and modules - known:
            print(f"error: unknown module(s) {', '.join(sorted(modules - known))}", file=sys.stderr)
            return 2
        for vid in all_ids:
            if vid != cfg["ref"]:
                map_version(cfg, fns, vid, warnings)
    rows = read_table(cfg, warnings)
    table = apply_table(cfg, fns, rows, warnings) if rows is not None else None
    code = scan_code(cfg, table["names"] if table else [])
    out = report(cfg, fns, versions, modules, ns.list, table, code, warnings)
    if ns.routes or ns.closure:
        if fns is None:
            print("error: --routes and --closure need the function inventory (see the warnings)", file=sys.stderr)
            return 2
        import decomp_graph as dg
        work = [f for f in fns if f.category not in cfg["excluded"] and f.category != "rel-excluded"]
        rows_of = [f for f in work if not modules or f.module in modules]
        if ns.function:
            rows_of = selected_functions(rows_of, ns.function.split(","))
        try:
            try:
                covs = load_coverage(cfg, ns.cov, ns.cov_hooks)
            except dg.GraphError:
                if ns.routes or ns.cov or ns.cov_hooks:
                    raise
                covs = {}                              # --closure alone: the coverage only adds a column
            for vid in versions:
                if ns.routes and vid not in covs:
                    raise dg.GraphError(f"{vid}: no coverage run ([decomp.coverage_runs] of game.toml, or --cov): what "
                                        "was reached is not known, which is not the same as nothing")
                if ns.routes:
                    out.setdefault("routes", {})[vid] = routes_view(cfg, fns, work, rows_of, vid, covs, ns.top, warnings)
                if ns.closure:
                    out.setdefault("closure", {})[vid] = closure_view(cfg, fns, work, vid, covs, ns.top, warnings)
            if ns.routes and len(covs) > 1:
                dol = [f for f in work if f.module == "dol"]
                seen_in = {id(f): sum(reached_in(f, v, c) for v, c in covs.items()) for f in dol}
                out["routes_all_versions"] = {
                    "functions": len(dol), "reached_any": sum(1 for n in seen_in.values() if n),
                    "reached_none": sum(1 for n in seen_in.values() if not n),
                    "reached_none_remaining": sum(1 for f in dol if not seen_in[id(f)] and not f.done),
                    "reached_one": sum(1 for n in seen_in.values() if n == 1)}
        except (dg.GraphError, ValueError) as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
    if ns.parity or ns.proof:
        import decomp_proof as dp
        game = dict(cfg["game"], decomp=dict(cfg["game"]["decomp"], functions=str(cfg["table"])))
        src = cfg["decomp_dir"] / "src"
        try:
            if ns.parity:
                out["parity"] = dp.parity_report(game, {v: disc_root(cfg, v) for v in all_ids}, src)
            if ns.proof:
                digests = ns.proof_file.resolve() if ns.proof_file else dp.proof_path(game, root)
                out["proof"] = dp.proof_report(game, digests, all_ids if ns.require_fresh else versions, src,
                                               root / "external" / "DolRecomp", root)
        except SystemExit as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
    if ns.json:
        print(json.dumps(out, indent=1))
    elif ns.routes or ns.closure or ns.parity or ns.proof:
        if ns.routes:
            print_routes(out)
        if ns.closure:
            print_closure(out)
        if ns.parity or ns.proof:
            lines = dp.parity_lines(out["parity"]) if ns.parity else []
            lines += dp.proof_lines(out["proof"], ns.list) if ns.proof else []
            print("\n".join(lines))
        if warnings:
            print(f"\nWarnings ({len(warnings)}):")
            for w in warnings:
                print(f"  - {w}")
    else:
        print_text(out, ns.list)
    if ns.require_complete and not out["complete"]:
        return 1
    if ns.require_fresh:
        stale = sum(c[dp.STALE_UNIT] for c in out["proof"]["versions"].values())
        rest = sum(c[dp.STALE_SHARED] + c[dp.UNKNOWN] for c in out["proof"]["versions"].values())
        print(f"--require-fresh: {stale} stale-unit (refused); {rest} stale-shared or unknown (reported only)",
              file=sys.stderr)
        return 1 if stale else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
