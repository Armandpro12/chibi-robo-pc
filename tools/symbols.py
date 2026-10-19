"""Symbol maps: conversion of decomp-toolkit (dtk) `symbols.txt` files and transposition between versions.

Usage:
  py -3 tools/symbols.py <game> convert                 # dtk symbols.txt -> <ref>.map (+ <ref>-rel.tsv)
  py -3 tools/symbols.py <game> transpose [--to ID]     # <ref>.map -> <ID>.map by function matching
  py -3 tools/symbols.py <game> all                     # both

Game-specific parameters: [symbols] section of `games/<game>/game.toml`:
  dtk_dir      dtk folder of the reference version (holds symbols.txt and one subfolder per REL)
  reference    ID of the version described by dtk (e.g. GGTE01)
  map_dir      output folder, relative to games/<game>/ (default "symbols")
  attribution  attribution text copied at the top of each map

Output formats:
  <ID>.map      "ADDRESS SIZE NAME" in hexadecimal, one function per line, "#" comments.
                This is one of the formats read by `dolrecomp --map` (src/analysis/symbol_map.c: 3 tokens
                "address size name"; lines whose first token is not hexadecimal are ignored).
                Only non-automatic names are kept (no fn_XXXXXXXX or zz_XXXXXXXX_...).
  <ID>-rel.tsv  named functions of the RELs, relative to their section: "module module_name section
                offset size name" (dolrecomp --map only accepts DOLs: "--map currently supports
                DOL input only", src/app/main.c).
A detailed report (status of each function) is written to generated/<game>/<ID>/symbols/.

Transposition method (DOL):
  1. bounds of the reference functions: symbols.txt (every function, named or not);
  2. each function is reduced to a sequence of "masked" words: the fields that depend on addresses
     are cleared (displacement of b/bl; immediate of lis; immediate of accesses relative to r2/r13
     (SDA/SDA2 small data) or to a register loaded by a lis earlier in the function;
     displacement of a bc that leaves the function);
  3. candidate starts in the target version: bl targets, words that follow a blr and, if it exists,
     the functions.tsv file from Ghidra (tools/ghidra/analyze.py);
  4. matching by identical masked sequence: "unique" if there is a single candidate; otherwise
     "neighbor" if a single candidate keeps the offset of the neighboring unique functions;
  5. propagation through calls: in two identical functions, the bl of the same rank targets the same
     function ("call", or "call-modified" if the callee's code differs); as a last resort,
     two matched but modified functions that make the same number of calls, consistent wherever they
     are known, match their calls rank by rank ("sequence", lower confidence);
  6. checks: consistency of the bl targets between matched pairs, collisions (two reference
     functions mapped to the same target address).
The RELs use the same method, module by module, on the code section (relative offsets).
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gcinfo  # noqa: E402
import toolchain as tc  # noqa: E402

SYM_RE = re.compile(r"^(\S+)\s*=\s*([^:\s]+):0x([0-9A-Fa-f]+);\s*//\s*(.*)$")
# Automatic names (no real name known): address-based placeholders. dtk writes them as fn_XXXXXXXX or
# fn_0xXXXXXXXX, and cbr_decomp also has fn_XXXXXXXX_unk__Fi / fn_XXXXXXXX__5CFileFv (placeholder plus
# the mangled signature suffix): the placeholder part embeds the address of the reference version, wrong
# for any other version, and the real base name is unknown, so they are all treated as automatic.
# Applies to the dtk names only: the verified names of supplement.tsv never go through this filter.
AUTO_RE = re.compile(r"^(fn_(0x)?[0-9A-Fa-f_]+|fn_(0x)?[0-9A-Fa-f]{8}_.*|zz_[0-9A-Fa-f]{8}.*|lbl_.*|@.*"
                     r"|func_(0x)?[0-9A-Fa-f]+|sub_(0x)?[0-9A-Fa-f]+)$")
SDA_REGS = (2, 13)
PREFIX = 8  # length (in words) of the index key


# ---------------------------------------------------------------------------------------------
# Reading the dtk symbols


@dataclass
class Sym:
    name: str
    section: str
    addr: int
    size: int
    kind: str
    attrs: dict = field(default_factory=dict)

    @property
    def auto(self) -> bool:
        return bool(AUTO_RE.match(self.name))


def read_dtk(path: Path) -> list[Sym]:
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = SYM_RE.match(line.strip())
        if not m:
            continue
        name, section, addr, rest = m.groups()
        attrs = {}
        for tok in rest.split():
            k, _, v = tok.partition(":")
            attrs[k] = v
        size = int(attrs.get("size", "0"), 16) if attrs.get("size", "0").startswith("0x") else 0
        out.append(Sym(name, section, int(addr, 16), size, attrs.get("type", ""), attrs))
    return out


def functions(syms: list[Sym]) -> list[Sym]:
    return sorted((s for s in syms if s.kind == "function" and s.size > 0), key=lambda s: s.addr)


# ---------------------------------------------------------------------------------------------
# Access to the code (DOL or REL section)


class Image:
    """Addressable code words (DOL: absolute addresses; REL: offsets in the code section)."""

    def __init__(self, ranges: list[tuple[int, bytes]]):
        self.ranges = ranges  # (start address, bytes)

    @classmethod
    def from_dol(cls, path: Path) -> "Image":
        data = path.read_bytes()
        info = gcinfo.describe_dol(data)
        ranges = [(s["addr"], data[s["offset"]:s["offset"] + s["size"]])
                  for s in info["segments"] if s["name"].startswith("text")]
        return cls(ranges)

    @classmethod
    def from_rel(cls, path: Path) -> tuple["Image", dict]:
        data = path.read_bytes()
        info = gcinfo.describe_rel(data)
        code = [s for s in info["sections"] if s["exec"] and s["size"]]
        # dtk numbers the .text section as section 1 of the module; we work with offsets inside it.
        sec = code[0]
        return cls([(0, data[sec["offset"]:sec["offset"] + sec["size"]])]), info

    def word(self, addr: int) -> int | None:
        for base, blob in self.ranges:
            if base <= addr and addr + 4 <= base + len(blob):
                o = addr - base
                return struct.unpack_from(">I", blob, o)[0]
        return None

    def words(self, addr: int, n: int) -> list[int] | None:
        for base, blob in self.ranges:
            if base <= addr and addr + 4 * n <= base + len(blob):
                o = addr - base
                return list(struct.unpack_from(f">{n}I", blob, o))
        return None

    def all_words(self):
        for base, blob in self.ranges:
            for i in range(len(blob) // 4):
                yield base + 4 * i, struct.unpack_from(">I", blob, 4 * i)[0]


def keep_masks(words: list[int], start: int, size: int) -> list[int]:
    """For each word, the mask of the bits that are kept: fields that depend on addresses are set to 0
    (see the module docstring). Generic: also used for the signatures (tools/sdksig.py)."""
    tainted: set[int] = set()
    out = []
    for i, w in enumerate(words):
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        m = 0xFFFFFFFF
        if op == 18:                                   # b, bl: displacement
            m = 0xFC000003
        elif op == 16:                                 # bc: cleared only if it leaves the function
            bd = w & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            target = start + 4 * i + bd
            if (w & 2) == 0 and not (start <= target < start + size):
                m = 0xFFFF0003
        elif op == 15:                                 # addis / lis
            if ra == 0 or ra in SDA_REGS or ra in tainted:
                m = 0xFFFF0000
            if ra == 0:
                tainted.add(rd)
        elif op == 14 or 32 <= op <= 55:               # addi, D-form loads and stores
            if ra != 0 and (ra in SDA_REGS or ra in tainted):
                m = 0xFFFF0000
        elif op in (24, 25):                           # ori, oris: source = rS field
            if rd in tainted:
                m = 0xFFFF0000
        elif op in (56, 57, 60, 61):                   # psq_l(u), psq_st(u): 12-bit displacement
            if ra != 0 and (ra in SDA_REGS or ra in tainted):
                m = 0xFFFFF000
        out.append(m)
    return out


def masked(words: list[int], start: int, size: int) -> tuple:
    """Word sequence with the address-dependent fields cleared (see the module docstring)."""
    return tuple(w & m for w, m in zip(words, keep_masks(words, start, size)))


def bl_targets(words: list[int], start: int) -> list[tuple[int, int]]:
    """(instruction rank, target) for each relative bl."""
    res = []
    for i, w in enumerate(words):
        if (w >> 26) == 18 and (w & 3) == 1:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            res.append((i, (start + 4 * i + li) & 0xFFFFFFFF))
    return res


# ---------------------------------------------------------------------------------------------
# Matching


@dataclass
class Match:
    ref: Sym
    status: str = "absent"     # unique | neighbor | call | call-modified | sequence | supplement | ambiguous | collision | absent
    target: int | None = None
    candidates: list[int] = field(default_factory=list)
    identical: bool = False


class Matcher:
    def __init__(self, ref_img: Image, ref_funcs: list[Sym], dst_img: Image, extra_starts: set[int]):
        self.ref_img = ref_img
        self.dst_img = dst_img
        self.funcs = ref_funcs
        self.by_addr = {f.addr: f for f in ref_funcs}
        self.ref_masked = {}
        for f in ref_funcs:
            w = ref_img.words(f.addr, f.size // 4)
            self.ref_masked[f.addr] = masked(w, f.addr, f.size) if w else None
        self.starts = self._candidate_starts() | {a for a in extra_starts if dst_img.word(a) is not None}
        self._dst_cache: dict[tuple[int, int], tuple | None] = {}
        self.index: dict[tuple, list[int]] = defaultdict(list)
        for s in self.starts:
            for k in range(1, PREFIX + 1):
                mk = self.dst_masked(s, 4 * k)
                if mk is None:
                    break
                self.index[(k, mk)].append(s)

    def _candidate_starts(self) -> set[int]:
        starts = set()
        prev_blr = False
        for addr, w in self.dst_img.all_words():
            if prev_blr and w != 0:
                starts.add(addr)
            prev_blr = (w == 0x4E800020)
            if (w >> 26) == 18 and (w & 3) == 1:
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                t = (addr + li) & 0xFFFFFFFF
                if self.dst_img.word(t) is not None:
                    starts.add(t)
        return starts

    def dst_masked(self, start: int, size: int) -> tuple | None:
        key = (start, size)
        if key not in self._dst_cache:
            w = self.dst_img.words(start, size // 4)
            self._dst_cache[key] = masked(w, start, size) if w else None
        return self._dst_cache[key]

    def equal_at(self, f: Sym, dst: int) -> bool:
        ref = self.ref_masked.get(f.addr)
        return ref is not None and self.dst_masked(dst, f.size) == ref

    def run(self, seeds: dict[int, int] | None = None) -> dict[int, Match]:
        """`seeds`: pairs (reference address -> target address) established elsewhere (supplement.tsv,
        verified status); they are extra starting points for the propagation through calls."""
        res: dict[int, Match] = {}
        # Steps 1-4: index, then full check.
        for f in self.funcs:
            m = Match(f)
            ref = self.ref_masked.get(f.addr)
            if ref is None:
                res[f.addr] = m
                continue
            k = min(len(ref), PREFIX)
            cands = [c for c in self.index.get((k, ref[:k]), []) if self.equal_at(f, c)]
            m.candidates = sorted(cands)
            if len(cands) == 1:
                m.status, m.target, m.identical = "unique", cands[0], True
            elif len(cands) > 1:
                m.status = "ambiguous"
            res[f.addr] = m
        # The same target address claimed by two "unique" functions: both are demoted.
        self._drop_collisions(res)
        for ref_addr, dst_addr in (seeds or {}).items():
            m = res.get(ref_addr)
            if m is not None and m.target is None:
                m.target, m.status = dst_addr, "supplement"
                m.identical = self.equal_at(m.ref, dst_addr)
        # Step 4b: neighbors (offset of the closest unique functions), repeated.
        changed = True
        while changed:
            changed = self._resolve_by_neighbors(res)
            changed |= self._propagate_calls(res)
            self._drop_collisions(res)
            if not changed:
                # Last resort: matched but modified functions with the same sequence of calls.
                changed = self._propagate_call_sequences(res)
                self._drop_collisions(res)
        return res

    def _propagate_call_sequences(self, res: dict[int, Match]) -> bool:
        """Non-identical pairs: if both functions make the same number of calls and every call
        already known agrees rank by rank, the unknown calls are matched rank by rank ("sequence"
        status, lower confidence). Since the size of the target function is unknown, at most the
        reference size + 25 % is read, cut at the start of the next matched function."""
        taken = {m.target for m in res.values() if m.target is not None}
        changed = False
        for f in self.funcs:
            m = res[f.addr]
            if m.target is None or m.identical:
                continue
            ref_w = self.ref_img.words(f.addr, f.size // 4)
            span = (f.size * 5 // 4 + 0x40) // 4
            dst_w = None
            while span > 0 and dst_w is None:
                dst_w = self.dst_img.words(m.target, span)
                span -= 1
            if not ref_w or not dst_w:
                continue
            # On the target side, cut at the first word that starts another matched reference function.
            starts_after = sorted(t.target for t in res.values()
                                  if t.target is not None and m.target < t.target < m.target + 4 * len(dst_w))
            if starts_after:
                dst_w = dst_w[:(starts_after[0] - m.target) // 4]
            ref_calls = [t for _, t in bl_targets(ref_w, f.addr)]
            dst_calls = [t for _, t in bl_targets(dst_w, m.target)]
            if len(ref_calls) != len(dst_calls) or not ref_calls:
                continue
            consistent = True
            for tr, td in zip(ref_calls, dst_calls):
                cm = res.get(tr)
                if cm is not None and cm.target is not None and cm.target != td:
                    consistent = False
                    break
            if not consistent:
                continue
            for tr, td in zip(ref_calls, dst_calls):
                cm = res.get(tr)
                if cm is None or cm.target is not None or td in taken:
                    continue
                cm.target = td
                cm.identical = self.equal_at(cm.ref, td)
                cm.status = "call" if cm.identical else "sequence"
                taken.add(td)
                changed = True
        return changed

    @staticmethod
    def _drop_collisions(res: dict[int, Match]) -> None:
        owners = defaultdict(list)
        for m in res.values():
            if m.target is not None:
                owners[m.target].append(m)
        for t, ms in owners.items():
            if len(ms) > 1:
                for m in ms:
                    m.status, m.target = "collision", None

    def _resolve_by_neighbors(self, res: dict[int, Match]) -> bool:
        anchors = [(f.addr, res[f.addr].target) for f in self.funcs
                   if res[f.addr].status in ("unique", "neighbor", "call") and res[f.addr].identical]
        if not anchors:
            return False
        import bisect
        keys = [a for a, _ in anchors]
        taken = {m.target for m in res.values() if m.target is not None}
        changed = False
        for f in self.funcs:
            m = res[f.addr]
            if m.status not in ("ambiguous", "absent", "collision"):
                continue
            i = bisect.bisect_left(keys, f.addr)
            deltas = set()
            lo_dst, hi_dst = None, None
            if i > 0:
                deltas.add(anchors[i - 1][1] - anchors[i - 1][0])
                lo_dst = anchors[i - 1][1]
            if i < len(anchors):
                deltas.add(anchors[i][1] - anchors[i][0])
                hi_dst = anchors[i][1]
            options = set()
            for d in deltas:
                c = f.addr + d
                if c in taken:
                    continue
                if lo_dst is not None and c <= lo_dst:
                    continue
                if hi_dst is not None and c >= hi_dst:
                    continue
                if self.equal_at(f, c):
                    options.add(c)
            if len(options) == 1:
                m.status, m.target, m.identical = "neighbor", options.pop(), True
                taken.add(m.target)
                changed = True
        return changed

    def _propagate_calls(self, res: dict[int, Match]) -> bool:
        taken = {m.target for m in res.values() if m.target is not None}
        changed = False
        for f in self.funcs:
            m = res[f.addr]
            if m.target is None or not m.identical:
                continue
            ref_w = self.ref_img.words(f.addr, f.size // 4)
            dst_w = self.dst_img.words(m.target, f.size // 4)
            if not ref_w or not dst_w:
                continue
            dst_calls = dict(bl_targets(dst_w, m.target))
            for i, t_ref in bl_targets(ref_w, f.addr):
                callee = self.by_addr.get(t_ref)
                t_dst = dst_calls.get(i)
                if callee is None or t_dst is None:
                    continue
                cm = res[callee.addr]
                if cm.target is not None or t_dst in taken:
                    continue
                if cm.status in ("collision",) and t_dst in cm.candidates:
                    pass
                cm.target = t_dst
                cm.identical = self.equal_at(callee, t_dst)
                cm.status = "call" if cm.identical else "call-modified"
                taken.add(t_dst)
                changed = True
        return changed

    def call_consistency(self, res: dict[int, Match]) -> tuple[int, int]:
        ok = bad = 0
        for f in self.funcs:
            m = res[f.addr]
            if m.target is None or not m.identical:
                continue
            ref_w = self.ref_img.words(f.addr, f.size // 4)
            dst_w = self.dst_img.words(m.target, f.size // 4)
            dst_calls = dict(bl_targets(dst_w, m.target))
            for i, t_ref in bl_targets(ref_w, f.addr):
                cm = res.get(t_ref)
                if cm is None or cm.target is None:
                    continue
                if dst_calls.get(i) == cm.target:
                    ok += 1
                else:
                    bad += 1
        return ok, bad


# ---------------------------------------------------------------------------------------------
# Outputs


def header(game: dict, vid: str, extra: list[str]) -> str:
    sym = game.get("symbols", {})
    lines = [f"# Symbol map {game['game']['name']} {vid} - generated by tools/symbols.py, do not edit.",
             "# Format: ADDRESS SIZE NAME (hexadecimal), read by dolrecomp --map and tools/ghidra/ApplySymbolMap.java."]
    for a in sym.get("attribution", "").splitlines():
        lines.append("# " + a if a.strip() else "#")
    lines += ["# " + e for e in extra]
    return "\n".join(lines) + "\n"


def write_map(path: Path, head: str, entries: list[tuple[int, int, str]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    seen = set()
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(head)
        for addr, size, name in sorted(entries):
            if (addr, name) in seen:
                continue
            seen.add((addr, name))
            f.write(f"{addr:08X} {size:08X} {name}\n")


# ---------------------------------------------------------------------------------------------
# Commandes


def config(game: dict) -> dict:
    sym = game.get("symbols")
    if not sym:
        sys.exit("no [symbols] section in game.toml")
    return {
        "dtk": (tc.ROOT / sym["dtk_dir"]).resolve(),
        "ref": sym["reference"],
        "out": game["_dir"] / sym.get("map_dir", "symbols"),
        "supplement": game["_dir"] / sym.get("map_dir", "symbols") / "supplement.tsv",
        # Supplement columns: one address per version, in the order of [[versions]] in game.toml.
        "supplement_columns": ["name"] + [v["game_id"] for v in game["versions"]] + ["status", "evidence"],
    }


def read_supplement(cfg: dict, vid: str) -> list[tuple[int, str, str]]:
    """(address, name, status) for version `vid`; columns: cfg["supplement_columns"] ("-" = unknown)."""
    path = cfg["supplement"]
    if not path.exists():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        row = dict(zip(cfg["supplement_columns"], line.split("	")))
        a = row.get(vid, "-")
        if a and a != "-":
            out.append((int(a, 16), row["name"], row.get("status", "")))
    return out


def verified(status: str) -> bool:
    return status.startswith("verified")


def code_paths(slug: str, vid: str, game: dict) -> tuple[Path, Path]:
    code = game.get("code", {})
    base = tc.GENERATED / slug / vid / "disc"
    return base / code.get("dol", "sys/main.dol"), base / code.get("rel_dir", "rel")


def rel_symbol_files(dtk: Path) -> dict[str, Path]:
    """module name -> symbols.txt (the RELs are in subfolders, sometimes grouped)."""
    found = {}
    for p in dtk.rglob("symbols.txt"):
        if p.parent == dtk:
            continue
        found[p.parent.name] = p
    return found


def cmd_convert(slug: str, game: dict) -> None:
    cfg = config(game)
    syms = read_dtk(cfg["dtk"] / "symbols.txt")
    funcs = functions(syms)
    named = [f for f in funcs if not f.auto]
    entries = [(f.addr, f.size, f.name) for f in named]
    sizes = {f.addr: f.size for f in funcs}
    named_addrs = {f.addr for f in named}
    extra = [(a, sizes.get(a, 0), n) for a, n, st in read_supplement(cfg, cfg["ref"])
             if verified(st) and a not in named_addrs]
    head = header(game, cfg["ref"], [f"Source: {len(named)} named functions out of {len(funcs)} (dtk symbols.txt)",
                                     f"+ {len(extra)} verified names from supplement.tsv (this project's analysis)."])
    write_map(cfg["out"] / f"{cfg['ref']}.map", head, entries + extra)
    print(f"{cfg['ref']}.map: {len(named)} named functions (out of {len(funcs)}) + {len(extra)} from the supplement")
    # REL
    rows = []
    _, rel_dir = code_paths(slug, cfg["ref"], game)
    for mod, p in sorted(rel_symbol_files(cfg["dtk"]).items()):
        rel_path = rel_dir / f"{mod}.rel"
        mid = gcinfo.describe_rel(rel_path.read_bytes())["id"] if rel_path.exists() else -1
        for f in functions(read_dtk(p)):
            if not f.auto:
                rows.append((mid, mod, 1, f.addr, f.size, f.name))
    write_rel_tsv(cfg["out"] / f"{cfg['ref']}-rel.tsv", header(game, cfg["ref"], []), rows)
    print(f"{cfg['ref']}-rel.tsv: {len(rows)} named functions in the RELs")


def write_rel_tsv(path: Path, head: str, rows: list) -> None:
    head = head.replace("# Format: ADDRESS SIZE NAME (hexadecimal), read by dolrecomp --map and "
                        "tools/ghidra/ApplySymbolMap.java.",
                        "# Format (TSV): module_id module_name section offset size name; offset relative "
                        "to the start of the section (hexadecimal).")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(head)
        for mid, mod, sec, off, size, name in sorted(rows):
            f.write(f"{mid}\t{mod}\t{sec}\t{off:X}\t{size:X}\t{name}\n")


def ghidra_starts(slug: str, vid: str) -> set[int]:
    tsv = tc.GENERATED / slug / vid / "ghidra" / "out" / "functions.tsv"
    if not tsv.exists():
        return set()
    starts = set()
    for line in tsv.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            continue
        starts.add(int(line.split("\t")[0], 16))
    return starts


def summarize(res: dict[int, Match], only_named: bool) -> dict[str, int]:
    counts: dict[str, int] = defaultdict(int)
    for m in res.values():
        if only_named and m.ref.auto:
            continue
        counts[m.status] += 1
    return counts


def fmt_counts(c: dict[str, int]) -> str:
    total = sum(c.values())
    order = ["unique", "neighbor", "call", "call-modified", "sequence", "supplement",
             "ambiguous", "collision", "absent"]
    parts = [f"{k} {c.get(k, 0)}" for k in order if c.get(k, 0)]
    found = sum(c.get(k, 0) for k in order[:6])
    return f"{found}/{total} found ({100.0 * found / max(total, 1):.1f} %): " + ", ".join(parts)


def cmd_transpose(slug: str, game: dict, to: str) -> dict:
    cfg = config(game)
    ref = cfg["ref"]
    ref_dol, ref_rel_dir = code_paths(slug, ref, game)
    dst_dol, dst_rel_dir = code_paths(slug, to, game)
    for p in (ref_dol, dst_dol):
        if not p.exists():
            sys.exit(f"missing: {p} (run tools/extract.py {slug} --version ...)")
    report_dir = tc.GENERATED / slug / to / "symbols"
    report_dir.mkdir(parents=True, exist_ok=True)
    stats = {}

    # --- DOL
    funcs = functions(read_dtk(cfg["dtk"] / "symbols.txt"))
    extra = ghidra_starts(slug, to)
    matcher = Matcher(Image.from_dol(ref_dol), funcs, Image.from_dol(dst_dol), extra)
    ref_sup = {n: a for a, n, st in read_supplement(cfg, ref) if verified(st)}
    seeds = {ref_sup[n]: a for a, n, st in read_supplement(cfg, to) if verified(st) and n in ref_sup}
    res = matcher.run(seeds)
    ok, bad = matcher.call_consistency(res)
    all_c, named_c = summarize(res, False), summarize(res, True)
    stats["dol_all"], stats["dol_named"], stats["calls"] = all_c, named_c, (ok, bad)
    print(f"DOL, all functions: {fmt_counts(all_c)}")
    print(f"DOL, named functions: {fmt_counts(named_c)}")
    print(f"call consistency between pairs: {ok} matching bl, {bad} mismatching")
    entries = []
    with (report_dir / f"{ref}-to-{to}.tsv").open("w", encoding="utf-8", newline="\n") as f:
        f.write(f"# {ref}\t{to}\tsize\tstatus\tidentical\tname\tcandidates\n")
        for fa in sorted(res):
            m = res[fa]
            f.write(f"{fa:08X}\t{(m.target or 0):08X}\t{m.ref.size:X}\t{m.status}\t{int(m.identical)}\t"
                    f"{m.ref.name}\t{','.join(f'{c:08X}' for c in m.candidates[:8])}\n")
            if m.target is not None and not m.ref.auto:
                entries.append((m.target, m.ref.size if m.identical else 0, m.ref.name))
    taken = {a for a, _, _ in entries}
    by_target = {m.target: m for m in res.values() if m.target is not None}
    extra = []
    for a, n, st in read_supplement(cfg, to):
        if not verified(st) or a in taken:
            continue
        m = by_target.get(a)
        extra.append((a, m.ref.size if (m and m.identical) else 0, n))
    head = header(game, to, [
        f"Transposed from {ref} by function matching (masked bytes, neighbors, calls).",
        f"Named functions: {fmt_counts(named_c)}.",
        f"+ {len(extra)} verified names from supplement.tsv (this project's analysis).",
        "Size 0: function found through a call but whose code differs (unknown size)."])
    write_map(cfg["out"] / f"{to}.map", head, entries + extra)
    print(f"{to}.map: {len(entries)} named functions + {len(extra)} from the supplement")

    # --- REL
    rel_rows, rel_total = [], defaultdict(int)
    for mod, p in sorted(rel_symbol_files(cfg["dtk"]).items()):
        r_ref, r_dst = ref_rel_dir / f"{mod}.rel", dst_rel_dir / f"{mod}.rel"
        if not (r_ref.exists() and r_dst.exists()):
            continue
        img_ref, _ = Image.from_rel(r_ref)
        img_dst, info_dst = Image.from_rel(r_dst)
        rfuncs = functions(read_dtk(p))
        rm = Matcher(img_ref, rfuncs, img_dst, set())
        rres = rm.run()
        for m in rres.values():
            rel_total[m.status] += 1
            if m.target is not None and not m.ref.auto:
                rel_rows.append((info_dst["id"], mod, 1, m.target, m.ref.size if m.identical else 0, m.ref.name))
    stats["rel_all"] = dict(rel_total)
    print(f"REL, all functions: {fmt_counts(rel_total)}")
    write_rel_tsv(cfg["out"] / f"{to}-rel.tsv",
                  header(game, to, [f"Transposed from {ref}; RELs, all functions: {fmt_counts(rel_total)}."]),
                  rel_rows)
    print(f"{to}-rel.tsv: {len(rel_rows)} named functions")
    print(f"detailed report: {report_dir}")
    return stats


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("cmd", choices=["convert", "transpose", "all"])
    ap.add_argument("--to", help="target version of the transposition (default: the game's default version)")
    ns = ap.parse_args()
    game = tc.load_game(ns.game)
    to = ns.to or game["game"]["default_version"]
    if ns.cmd in ("convert", "all"):
        cmd_convert(ns.game, game)
    if ns.cmd in ("transpose", "all"):
        if to == config(game)["ref"]:
            sys.exit("the target version is the reference version")
        cmd_transpose(ns.game, game, to)
    return 0


if __name__ == "__main__":
    sys.exit(main())
