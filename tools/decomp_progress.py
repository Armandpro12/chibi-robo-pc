"""Decompilation progress: share of functions and code bytes decompiled, per version and per module.

Usage:
  py -3 tools/decomp_progress.py <game> [--version ID[,ID]] [--module NAME[,NAME]] [--list] [--json]
                                        [--root CHECKOUT] [--require-complete]

  --list              also print every function left to decompile, and every automatic name found in the
                      decompiled code (file:line)
  --json              machine-readable output (same content)
  --root              checkout holding the untracked inputs (external/, generated/); default: this checkout.
                      game.toml and the decompiled code are always read from this checkout.
  --table, --decomp-dir  read another table / code folder instead of the configured ones (tests)
  --require-complete  exit code 1 unless every version is at 100 % (functions and bytes, DOL and every REL),
                      every function has an address in every version, and no automatic name is left in the
                      decompiled code (the stage's final criterion)

Exit codes: 0 report printed; 1 --require-complete not met; 2 usage or configuration error.

Game-specific parameters: [decomp] section of `games/<game>/game.toml` (all paths are relative):
  reference      version whose functions define the work (default: [symbols] reference)
  dir            folder of the decompiled code, relative to games/<game>/ (scanned for automatic names)
  functions      table of the decompiled functions, relative to games/<game>/ (missing = 0 % decompiled)
  columns        { <version ID> = "<column name>" }: column of the table holding each version's address
  done_statuses  status values that count a row as decompiled (case-insensitive prefix, "[]" ignored)
  transposition  transposition report of tools/symbols.py for a non-reference version, relative to the
                 root ({slug}, {ref}, {id} replaced)
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
  Byte figures always use the reference version's function sizes.

Table of the decompiled functions (format still open; this reader is tolerant):
  - one row per function, columns separated by tabs (or by spaces when a row has no tab);
  - a header row (optionally starting with "#") names the columns; recognized names: "name" (or function,
    symbol), "status" (or state), "size", "module" (or rel), and for each version the column given by
    [decomp] columns, or the version ID itself ("GGXX01", "ggxx01_addr", "ggxx01_address"), or the region
    ("us_addr", "pal_addr", "jp_addr", with or without "_addr"/"_address");
  - without a header: name, then the version addresses in the order of [decomp] columns, then size, status;
  - addresses in hexadecimal (with or without 0x); "-" or empty = no address in that version. A REL function is
    "<module>:<offset>" (offset inside the module's code section, as in <ID>-rel.tsv), or an offset with a
    "module" column. Other lines starting with "#" are comments.
  Rows that match no function, an excluded function, a different address than the transposition, or another
  size, are reported as warnings.

Automatic names: placeholders of dtk, Ghidra and m2c (fn_8001234, FUN_..., DAT_..., uVar1, local_10, param_1,
var_r31, sp1C, unk1C...). Counted (1) in the decompiled code folder and the names of the table, and (2) among
the functions left to decompile, from the symbol maps (<ID>.map, <ID>-rel.tsv) and the dtk names.
Read-only: the script writes nothing.
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
import toolchain as tc  # noqa: E402

CODE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp"}
# Automatic names produced by the usual tools (kind, regular expression). Generic: no game in here.
AUTO_PATTERNS = [
    ("address placeholder", r"\b(?:fn|func|sub|lbl|zz|jumptable|jtbl)_(?:0x)?[0-9A-Fa-f]{6,8}\w*"),
    ("Ghidra label", r"\b(?:thunk_)?(?:FUN|DAT|LAB|PTR|SUB|UNK|BYTE|WORD|DWORD|QWORD|FLOAT|DOUBLE|switchD|caseD"
                     r"|code_r0x)_[0-9A-Fa-f]{4,}\w*"),
    ("Ghidra variable", r"\b[a-z]{1,5}Var\d+\b"),
    ("Ghidra local/param", r"\blocal_(?:res)?[0-9A-Fa-f]+\b|\bparam_\d+\b|\b(?:unaff|extraout)_\w+"
                           r"|\b[a-z]{1,5}Stack_[0-9A-Fa-f]+\b|\bin_(?:r\d+|f\d+|cr\d+|lr|ctr|xer|stack_\w+)\b"),
    ("raw field", r"\bfield\d*_0x[0-9A-Fa-f]+\b|\b_?unk_?(?:0x)?[0-9A-Fa-f]+\b"),
    ("m2c temporary", r"\b(?:var|temp|phi|saved_reg)_(?:r|f|cr|lr|ctr)\d*(?:_\d+)?\b|\bsp[0-9A-F]{1,4}\b"
                      r"|\barg\d+\b|\bM2C_\w+"),
]
REGION_ALIASES = {"ntsc-u": "us", "ntsc-j": "jp", "pal": "pal", "ntsc-k": "kr"}
NO_ADDRESS = {"", "-", "0", "none", "n/a", "absent"}


@dataclass
class Fn:
    module: str          # "dol" or the REL module name
    ref: int             # reference address (DOL) or offset in the code section (REL)
    size: int
    name: str            # best known name (symbol maps first, then dtk)
    category: str
    addr: dict = field(default_factory=dict)     # version -> address/offset or None
    how: dict = field(default_factory=dict)      # version -> mapping status
    done: dict = field(default_factory=dict)     # version -> status text of the table row

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


def is_auto(name: str, extra: list[re.Pattern]) -> bool:
    if not name or sy.AUTO_RE.match(name):
        return True
    return any(p.search(name) for _, p in compiled_patterns(extra))


_PATTERN_CACHE: dict = {}


def compiled_patterns(extra: list[re.Pattern]) -> list[tuple[str, re.Pattern]]:
    key = tuple(p.pattern for p in extra)
    if key not in _PATTERN_CACHE:
        _PATTERN_CACHE[key] = [(k, re.compile(r)) for k, r in AUTO_PATTERNS] + [("game.toml pattern", p)
                                                                               for p in extra]
    return _PATTERN_CACHE[key]


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
        "done": [s.lower() for s in dc.get("done_statuses", ["verified", "hypothesis", "decompiled", "done"])],
        "transposition": dc.get("transposition", "generated/{slug}/{id}/symbols/{ref}-to-{id}.tsv"),
        "excluded": set(dc.get("excluded", [])),
        "ranges": ranges,
        "overrides": {parse_int(o["addr"]): (o.get("name"), o["category"]) for o in dc.get("overrides", [])},
        "rel_excluded": set(dc.get("rel_excluded_names", [])),
        "extra_patterns": [re.compile(p) for p in dc.get("auto_name_patterns", [])],
        "disc": lambda vid: root / "generated" / slug / vid / "disc" / code.get("rel_dir", "rel"),
    }


# ---------------------------------------------------------------------------------------------
# Inventory, names, per-version addresses


def read_map(path: Path) -> dict[int, str]:
    out = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        p = line.split()
        if len(p) == 3 and not line.startswith("#"):
            try:
                out[int(p[0], 16)] = p[2]
            except ValueError:
                pass
    return out


def read_rel_names(path: Path) -> dict[tuple[str, int], str]:
    out = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        p = line.split("\t")
        if len(p) >= 6:
            out[(p[1], int(p[3], 16))] = p[5]
    return out


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
    rel = cfg["transposition"].format(slug=slug, ref=ref, id=vid)
    path = cfg["root"] / rel
    dol = {f.ref: f for f in fns if f.module == "dol"}
    if path.exists():
        src["dol"] = rel
        for line in path.read_text(encoding="utf-8").splitlines():
            if line.startswith("#") or not line.strip():
                continue
            p = line.split("\t")
            f = dol.get(int(p[0], 16))
            if f is None:
                continue
            tgt = int(p[1], 16)
            f.addr[vid] = tgt or None
            f.how[vid] = p[3]
    else:
        src["dol"] = None
        warnings.append(f"{vid}: no DOL addresses ({rel} missing): run `py -3 tools/symbols.py {slug} transpose "
                        f"--to {vid}` (or pass --root)")
    for f in dol.values():
        f.how.setdefault(vid, "unknown")
        f.addr.setdefault(vid, None)
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
    src["rel"] = "matcher" if not missing else f"matcher; no REL files for {', '.join(missing)}"
    if missing:
        warnings.append(f"{vid}: REL files missing for {len(missing)} modules under {dst_dir} or {ref_dir} "
                        "(tools/extract.py, or --root)")
    return src


# ---------------------------------------------------------------------------------------------
# Table of the decompiled functions


def column_aliases(cfg: dict) -> dict[str, str]:
    """lower-case header name -> canonical key ("name", "status", "size", "module", "v:<ID>")."""
    al = {"name": "name", "function": "name", "symbol": "name", "status": "status", "state": "status",
          "size": "size", "module": "module", "rel": "module"}
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
        cells = [c.strip() for c in (line.split("\t") if "\t" in line else line.split())]
        head = [c.lstrip("#").strip().lower() for c in cells]
        if cols is None and sum(h in aliases for h in head) >= 2:
            cols = [aliases.get(h) for h in head]
            continue
        if line.lstrip().startswith("#"):
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
    seen = set()
    loc = f"{cfg['table'].name}"
    for row in rows:
        n = row["line"]
        status = row.get("status", "").strip("[]` ").lower()
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
                if int(row["size"], 16) != f.size:
                    warnings.append(f"{loc}:{n}: size {row['size']} differs from the inventory ({f.size:X})")
            except ValueError:
                warnings.append(f"{loc}:{n}: unreadable size {row['size']}")
        if not any(status.startswith(s) for s in cfg["done"]):
            if status:
                warnings.append(f"{loc}:{n}: status '{status}' not in [decomp] done_statuses: not counted")
            continue
        for vid, _ in cfg["versions"]:
            a = addrs.get(vid)
            if a is None:
                continue
            want = f.addr.get(vid)
            if want is not None and (f.module, want) != a:
                warnings.append(f"{loc}:{n}: {vid} address {a[1]:X} differs from the transposition ({want:X}): "
                                "not counted")
                continue
            if want is None:
                warnings.append(f"{loc}:{n}: {vid} address {a[1]:X} known only from the table (no transposed "
                                "address): to verify")
                f.addr[vid], f.how[vid] = a[1], "table"
            f.done[vid] = status
    return stats


# ---------------------------------------------------------------------------------------------
# Automatic names in the decompiled code


def scan_code(cfg: dict, table_names: list[tuple[int, str]]) -> dict:
    d = cfg["decomp_dir"]
    pats = compiled_patterns(cfg["extra_patterns"])
    hits = []
    if d.exists():
        for p in sorted(d.rglob("*")):
            if p.suffix.lower() not in CODE_EXTENSIONS or not p.is_file():
                continue
            for n, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                for kind, pat in pats:
                    for m in pat.finditer(line):
                        hits.append({"file": rel_path(p), "line": n, "name": m.group(0),
                                     "kind": kind})
    table = rel_path(cfg["table"])
    for n, name in table_names:
        if is_auto(name, cfg["extra_patterns"]):
            hits.append({"file": table, "line": n, "name": name or "(empty)", "kind": "table name"})
    return {"path": rel_path(d), "present": d.exists(), "count": len(hits),
            "by_kind": dict(Counter(h["kind"] for h in hits)), "hits": hits}


# ---------------------------------------------------------------------------------------------
# Report


def module_stats(fns: list[Fn], vid: str, extra: list[re.Pattern]) -> dict:
    s = {"functions": 0, "functions_done": 0, "functions_verified": 0, "bytes": 0, "bytes_done": 0,
         "no_address": 0, "no_address_bytes": 0, "auto_names_left": 0, "address_status": Counter()}
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
            s["functions_verified"] += f.done[vid].startswith("verified")
        elif is_auto(f.name, extra):
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
            t = v["total"]
            complete = complete and t["functions_done"] == t["functions"] and t["no_address"] == 0
            if want_list:
                v["remaining"] = [{"module": f.module, "address": f.label(vid), "reference": f"{f.ref:08X}"
                                   if f.module == "dol" else f"{f.module}:{f.ref:X}", "size": f.size,
                                   "category": f.category, "address_status": f.how.get(vid), "name": f.name}
                                  for f in sel if vid not in f.done]
            out["versions"][vid] = v
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
        print(f"  {'module':9s} {'functions':>22s} {'bytes':>27s} {'no address':>11s} {'auto names left':>16s}")
        rows = list(v["modules"].items()) + [("total", v["total"])]
        for m, s in rows:
            fn = f"{s['functions_done']}/{s['functions']} {fmt_pct(s['percent_functions'])}"
            by = f"{s['bytes_done']}/{s['bytes']} {fmt_pct(s['percent_bytes'])}"
            print(f"  {m:9s} {fn:>22s} {by:>27s} {s['no_address']:>11d} {s['auto_names_left']:>16d}")
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
    print(f"\nComplete (100 % everywhere, every address known, no automatic name): {'yes' if out['complete'] else 'no'}")


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
    ap.add_argument("--require-complete", action="store_true", help="exit 1 unless everything is decompiled")
    ns = ap.parse_args()
    root = (ns.root or tc.ROOT).resolve()
    try:
        cfg = load_config(ns.game, root)
    except (ConfigError, KeyError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    if ns.table:
        cfg["table"] = ns.table.resolve()
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
    if ns.json:
        print(json.dumps(out, indent=1))
    else:
        print_text(out, ns.list)
    if ns.require_complete and not out["complete"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
