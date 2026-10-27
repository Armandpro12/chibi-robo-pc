"""Symbol maps: conversion of decomp-toolkit (dtk) `symbols.txt` files and transposition between versions.

Usage:
  py -3 tools/symbols.py <game> convert                 # dtk symbols.txt -> <ref>.map (+ <ref>-rel.tsv)
  py -3 tools/symbols.py <game> transpose [--to ID]     # <ref>.map -> <ID>.map by function matching
  py -3 tools/symbols.py <game> all                     # both
  py -3 tools/symbols.py <game> gaps [--to ID]          # place what transpose did not find -> <ID>-gaps.tsv

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
  <ref>-to-<ID>.tsv  transposition report: every function of the reference DOL (dtk symbols.txt, named or
                not) with its reference size, its <ID> address (00000000 = not found), status, identical flag
                and candidates. Written next to the maps and kept in Git: tools/decomp_functions.py (run by
                tools/build.py) checks the addresses of the decompiled functions against it, so that a build
                needs neither the dtk symbols nor the Ghidra entries the matcher reads in generated/.

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
# Gap filling (command "gaps"): the functions the transposition left without a target


GAP_TODO = ("absent", "ambiguous", "collision")
NL = "\n"
GAP_STRONG_SIM = 0.80
# Longest run of target-only code (functions of the target version with no counterpart in the reference) that a
# layout may leave between two placed functions, or between a placed function and an anchor.
GAP_MAX_TARGET_ONLY = 0x400


def data_words(path: Path) -> dict[int, int]:
    """Count of each 32-bit word of the non-code segments of a DOL (references to functions from data)."""
    data = path.read_bytes()
    counts: dict[int, int] = defaultdict(int)
    for s in gcinfo.describe_dol(data)["segments"]:
        if s["name"].startswith("text"):
            continue
        blob = data[s["offset"]:s["offset"] + s["size"]]
        for (w,) in struct.iter_unpack(">I", blob[:len(blob) // 4 * 4]):
            counts[w] += 1
    return counts


def exception_index(path: Path) -> dict[int, int]:
    """Function start -> size, from the exception index of a DOL built by CodeWarrior (MWCC): the table
    `extabindex` is an array of {function address, function size, extab address} triples, in address order.
    The table is located as the longest run of such triples in any segment (function and function end in a
    text segment, sizes multiple of 4, non-overlapping and increasing). Only functions with exception data
    are listed (a few hundred in a game). Empty if the DOL has no such table."""
    data = path.read_bytes()
    segs = gcinfo.describe_dol(data)["segments"]
    text = [(s["addr"], s["addr"] + s["size"]) for s in segs if s["name"].startswith("text")]

    def in_text(a: int) -> bool:
        return any(lo <= a < hi for lo, hi in text)

    best: dict[int, int] = {}
    for s in segs:
        n = s["size"] // 4
        ws = struct.unpack_from(f">{n}I", data, s["offset"])
        for phase in range(3):
            run: dict[int, int] = {}
            prev_end = 0
            for i in range(phase, n - 2, 3):
                f, z, x = ws[i], ws[i + 1], ws[i + 2]
                if (f % 4 == 0 and z % 4 == 0 and 0 < z < 0x100000 and x and x % 4 == 0 and f >= prev_end
                        and in_text(f) and in_text(f + z - 4)):
                    run[f] = z
                    prev_end = f + z
                    continue
                if len(run) > len(best):
                    best = run
                run, prev_end = {}, 0
            if len(run) > len(best):
                best = run
    return best if len(best) >= 8 else {}


def similarity(a: tuple | None, b: tuple | None) -> float:
    if not a or not b:
        return 0.0
    import difflib
    return difflib.SequenceMatcher(None, a, b, autojunk=False).ratio()


def boundary_kind(w: int | None) -> str:
    if w is None:
        return "?"
    if w == 0x4E800020:
        return "blr"
    if (w >> 26) == 18 and (w & 3) == 0:
        return "b"
    if w == 0x4C000064:
        return "rfi"
    return "other"


@dataclass
class Gap:
    ref: Sym
    status: str = "unresolved"   # strong | graph | position | weak | sized | not-linked | unresolved
    target: int | None = None
    size: int = 0                # size in the target version
    sim: float = -1.0
    checks: dict = field(default_factory=dict)


class GapFiller:
    """Places the functions of the reference version that the transposition did not find ("absent",
    "ambiguous", "collision") by their position between two found functions ("anchors"):
      1. anchors = every found function; their targets must increase with the reference address;
      2. between two consecutive anchors, the unplaced reference functions keep their order (the linker keeps the
         order of the objects); they are laid out on candidate starts of the target version (Ghidra function
         entries, words after a blr, bl targets) by dynamic programming that maximizes the sum of
         size x similarity of the masked code (difflib ratio); a placed range may never contain a "hard" entry
         of the target strictly inside it (Ghidra function entry or exception-index entry, unless the reference
         function holds an entry of the same kind at the same offset: a split that exists in both versions);
         if no layout exists, the layout may leave target-only code (at most GAP_MAX_TARGET_ONLY bytes,
         starting at a hard entry) unassigned, then leave out a reference function ("not-linked") when the
         target bytes cannot hold it;
      3. once every function is placed, the placed functions and the unassigned target-only code cover the
         target code, and each placed function is checked on it: same set of callers, same set of callees,
         same number of references from data words, same kind of instruction before its start and at its end
         (a call from target-only code has no owner);
      4. status: "strong" (similarity >= GAP_STRONG_SIM and every check passes), "graph" (every check passes,
         lower similarity), "position" (callers, data references and boundaries agree, callees differ: code
         changed between versions), "weak" (any other failure). Functions already found but whose code differs
         (size unknown in the map) get their target size from the partition ("sized"), cut at the first hard
         entry inside it."""

    def __init__(self, m: Matcher, res: dict[int, Match], dst_entries: set[int],
                 dst_eti: set[int] | None = None, ref_hard: set[int] | None = None):
        self.m, self.res = m, res
        self.funcs = m.funcs
        self.by_addr = m.by_addr
        self.dst_entries = dst_entries
        self.dst_eti = set(dst_eti or ())
        heur = set(m.starts)
        self.dst_starts = sorted(heur | dst_entries | self.dst_eti
                                 | {r.target for r in res.values() if r.target is not None})
        self.hard = set(dst_entries) | self.dst_eti
        self.hard_sorted = sorted(self.hard)
        ref_sorted = sorted(ref_hard or ())
        # Offsets of the hard entries strictly inside each reference function (splits present in both versions).
        self.mirror: dict[int, set[int]] = {}
        import bisect
        for f in self.funcs:
            k = bisect.bisect_right(ref_sorted, f.addr)
            while k < len(ref_sorted) and ref_sorted[k] < f.addr + f.size:
                self.mirror.setdefault(f.addr, set()).add(ref_sorted[k] - f.addr)
                k += 1
        self.target_only: list[tuple[int, int]] = []   # (start, end) left unassigned by a layout
        self._sim: dict[tuple[int, int, int], float] = {}

    def cut(self, f: Sym, s: int, e: int) -> int:
        """End of the range [s, e) of `f` in the target, cut at the first hard entry strictly inside it."""
        import bisect
        k = bisect.bisect_right(self.hard_sorted, s)
        ok = self.mirror.get(f.addr, ())
        while k < len(self.hard_sorted) and self.hard_sorted[k] < e:
            if self.hard_sorted[k] - s not in ok:
                return self.hard_sorted[k]
            k += 1
        return e

    def _anchors(self) -> list[tuple[int, int]]:
        import bisect
        cand = [(f.addr, self.res[f.addr].target) for f in self.funcs
                if self.res[f.addr].target is not None and self.res[f.addr].status not in GAP_TODO]
        # Longest increasing subsequence of the targets (an out-of-order pair is not an anchor).
        tails, tail_idx, prev = [], [], [-1] * len(cand)
        for i, (_, t) in enumerate(cand):
            j = bisect.bisect_left(tails, t)
            if j == len(tails):
                tails.append(t)
                tail_idx.append(i)
            else:
                tails[j], tail_idx[j] = t, i
            prev[i] = tail_idx[j - 1] if j > 0 else -1
        out, k = [], tail_idx[-1] if tail_idx else -1
        while k >= 0:
            out.append(cand[k])
            k = prev[k]
        return out[::-1]

    def score(self, f: Sym, s: int, e: int) -> float:
        if not (f.size // 2 <= e - s <= 2 * f.size + 64):
            return -1.0
        if self.cut(f, s, e) != e:
            return -1.0
        key = (f.addr, s, e)
        if key not in self._sim:
            self._sim[key] = similarity(self.m.ref_masked.get(f.addr), self.m.dst_masked(s, e - s))
        return self._sim[key]

    def _layout(self, inside: list[Sym], cands: list[int], first: int | None, end: int, allow_skip: bool,
                target_skip: bool = False):
        """Best layout of `inside` on `cands` (target starts) ending at `end`; None if impossible.
        allow_skip: a reference function may be left out; target_skip: target-only code (starting at a hard
        entry, at most GAP_MAX_TARGET_ONLY bytes) may be left unassigned after a placed function or after `first`.
        Returns [(start, end, similarity) | None (left out)].
        Memoized depth-first search over the states (j = inside[j] starts at cands[k]), run with an explicit
        stack (no recursion: an interval may hold hundreds of functions)."""
        n, neg, nc = len(inside), float("-inf"), len(cands)
        # memo[(j, k)] = (score, choice); choice = (e, sim, jn, k3) (k3 = None: last function) or None.
        memo: dict[tuple[int, int], tuple[float, tuple | None]] = {}

        def after(k2: int, e: int):
            """Indices of the possible next starts once a function ends at `e` (= cands[k2], or `end`)."""
            if k2 is not None:
                yield k2
            if target_skip and e in self.hard:
                for k3 in range((k2 + 1) if k2 is not None else 0, nc):
                    if cands[k3] <= e:
                        continue
                    if cands[k3] - e > GAP_MAX_TARGET_ONLY:
                        break
                    yield k3

        def moves(j: int, k: int):
            """(sim, e, k3, jn) in the order of preference of the search (k3 None = last function)."""
            f, s = inside[j], cands[k]
            if j == n - 1:
                sc = self.score(f, s, end)
                if sc >= 0:
                    yield sc, end, None, None
                if target_skip:
                    for k2 in range(k + 1, nc):
                        e = cands[k2]
                        if e >= end or e - s > 2 * f.size + 64:
                            break
                        if e in self.hard and end - e <= GAP_MAX_TARGET_ONLY:
                            sc = self.score(f, s, e)
                            if sc >= 0:
                                yield sc, e, None, None
                return
            for k2 in range(k + 1, nc):
                e = cands[k2]
                if e - s > 2 * f.size + 64:
                    break
                sc = self.score(f, s, e)
                if sc < 0:
                    continue
                for k3 in after(k2, e):
                    for jn in ([j + 1, j + 2] if allow_skip and j + 2 < n else [j + 1]):
                        yield sc, e, k3, jn

        def solve(j0: int, k0: int):
            stack = [(j0, k0)]
            while stack:
                j, k = stack[-1]
                if (j, k) in memo:
                    stack.pop()
                    continue
                missing = [(jn, k3) for _, _, k3, jn in moves(j, k) if k3 is not None and (jn, k3) not in memo]
                if missing:
                    stack.extend(missing)
                    continue
                f = inside[j]
                best: tuple[float, tuple | None] = (neg, None)
                for sc, e, k3, jn in moves(j, k):
                    if k3 is None:
                        tot = sc * f.size
                    else:
                        sub = memo[(jn, k3)]
                        if sub[1] is None:
                            continue
                        tot = sc * f.size + sub[0] - sum(inside[x].size for x in range(j + 1, jn))
                    if tot > best[0]:
                        best = (tot, (e, sc, jn, k3))
                memo[(j, k)] = best
                stack.pop()
            return memo[(j0, k0)]

        def unroll(j: int, k: int) -> list:
            out = []
            while True:
                e, sc, jn, k3 = memo[(j, k)][1]
                out.append((cands[k], e, sc))
                if k3 is None:
                    return out
                out += [None] * (jn - j - 1)
                j, k = jn, k3

        if first is not None:
            starts = [cands.index(first)]
            if target_skip:
                starts += [k for k in after(None, first)]
        else:
            starts = range(nc)
        sol: tuple[float, tuple | None] = (neg, None)
        for j0 in ([0, 1] if allow_skip and n > 1 else [0]):
            for k in starts:
                r = solve(j0, k)
                if r[1] is not None and r[0] - sum(inside[x].size for x in range(j0)) > sol[0]:
                    sol = (r[0] - sum(inside[x].size for x in range(j0)), (j0, k))
        if sol[1] is None:
            return None
        j0, k = sol[1]
        return [None] * j0 + unroll(j0, k)

    def run(self) -> dict[int, Gap]:
        import bisect
        anchors = self._anchors()
        ref_addrs = [f.addr for f in self.funcs]
        out: dict[int, Gap] = {}
        for i in range(len(anchors) - 1):
            (a, pa), (b, pb) = anchors[i], anchors[i + 1]
            lo = bisect.bisect_right(ref_addrs, a)
            inside = []
            for f in self.funcs[lo:]:
                if f.addr >= b:
                    break
                inside.append(f)
            if not inside:
                continue
            fa = self.by_addr[a]
            first = pa + fa.size if self.res[a].identical else None
            lo_i = bisect.bisect_right(self.dst_starts, pa)
            hi_i = bisect.bisect_left(self.dst_starts, pb)
            cands = self.dst_starts[lo_i:hi_i]
            if first is not None and first not in cands:
                cands = sorted(set(cands) | {first})
            if first is not None and first >= pb:
                for f in inside:      # no target byte between the two anchors
                    out[f.addr] = Gap(f, "not-linked")
                continue
            # Contiguous layouts first; target-only code and left-out functions only when nothing else fits.
            lay = (self._layout(inside, cands, first, pb, False) or self._layout(inside, cands, first, pb, False, True)
                   or self._layout(inside, cands, first, pb, True) or self._layout(inside, cands, first, pb, True, True))
            if lay is not None:
                pos = first if first is not None else None
                for x in lay:
                    if x is None:
                        continue
                    if pos is not None and x[0] > pos:
                        self.target_only.append((pos, x[0]))
                    pos = x[1]
                if pos is not None and pos < pb:
                    self.target_only.append((pos, pb))
            for j, f in enumerate(inside):
                g = Gap(f)
                if lay is None:
                    pass
                elif lay[j] is None:
                    g.status = "not-linked"
                else:
                    g.target, end, g.sim = lay[j]
                    g.size = end - g.target
                out[f.addr] = g
        return out

    def uncovered(self) -> list[tuple[int, int]]:
        """Ranges of the target code (between the first and the last placed function) that no placed function
        covers and that are not only zero words (needs check() first)."""
        out = []
        rows = sorted(self.dst_size.items())
        for (s1, z1), (s2, _) in zip(rows, rows[1:]):
            e = s1 + (z1 or 0)
            if z1 and e < s2 and any(self.m.dst_img.word(x) for x in range(e, s2, 4)):
                out.append((e, s2))
        return out

    def check(self, gaps: dict[int, Gap], ref_dol: Path, dst_dol: Path) -> None:
        """Step 3: checks on the complete partition (see the class docstring)."""
        import bisect
        place = {f.addr: self.res[f.addr].target for f in self.funcs
                 if self.res[f.addr].target is not None and self.res[f.addr].status not in GAP_TODO}
        place.update({a: g.target for a, g in gaps.items() if g.target is not None})
        inv = {t: a for a, t in place.items()}
        dst_sorted = sorted(inv)
        ref_sorted = [f.addr for f in self.funcs]
        # Sizes of the found but modified functions, from the partition, cut at the first hard entry inside.
        for f in self.funcs:
            r = self.res[f.addr]
            if f.addr in gaps or r.target is None or r.identical or r.status in GAP_TODO:
                continue
            k = bisect.bisect_right(dst_sorted, r.target)
            if k < len(dst_sorted):
                e = self.cut(f, r.target, dst_sorted[k])
                if e < dst_sorted[k]:
                    self.target_only.append((e, dst_sorted[k]))
                gaps[f.addr] = Gap(f, "sized", r.target, e - r.target,
                                   similarity(self.m.ref_masked.get(f.addr),
                                              self.m.dst_masked(r.target, e - r.target)))
        # Target size of every placed function (None: unknown, the last function before the end of the code).
        dst_size: dict[int, int] = {}
        for f in self.funcs:
            r = self.res[f.addr]
            if r.target is not None and r.status not in GAP_TODO and r.identical and f.addr not in gaps:
                dst_size[r.target] = f.size
        dst_size.update({g.target: g.size for g in gaps.values() if g.target is not None})
        self.dst_size = dst_size

        def owner_dst(x: int):
            # Owner of a target address: the placed function whose range holds it (none in target-only code).
            k = bisect.bisect_right(dst_sorted, x) - 1
            if k < 0:
                return None
            t = dst_sorted[k]
            z = dst_size.get(t)
            return inv[t] if z is None or x < t + z else None

        def owner_ref(x: int):
            k = bisect.bisect_right(ref_sorted, x) - 1
            f = self.funcs[k] if k >= 0 else None
            return f.addr if f and f.addr <= x < f.addr + f.size else None

        def graph(img: Image, owner):
            callers, callees = defaultdict(set), defaultdict(set)
            for addr, w in img.all_words():
                if (w >> 26) == 18 and (w & 3) == 1:
                    li = w & 0x03FFFFFC
                    if li & 0x02000000:
                        li -= 0x04000000
                    t = (addr + li) & 0xFFFFFFFF
                    src = owner(addr)
                    if src is not None:
                        callers[t].add(src)
                        callees[src].add(t)
            return callers, callees

        r_callers, r_callees = graph(self.m.ref_img, owner_ref)
        d_callers, d_callees = graph(self.m.dst_img, owner_dst)
        r_data, d_data = data_words(ref_dol), data_words(dst_dol)
        for a, g in gaps.items():
            if g.target is None:
                continue
            f = g.ref
            callers_ok = r_callers.get(a, set()) == d_callers.get(g.target, set())
            mapped = {inv.get(t, -t) for t in d_callees.get(a, set())}
            callees_ok = r_callees.get(a, set()) == mapped
            dref = (r_data.get(a, 0), d_data.get(g.target, 0))
            bounds_ok = (boundary_kind(self.m.ref_img.word(a - 4)) == boundary_kind(self.m.dst_img.word(g.target - 4))
                         and boundary_kind(self.m.ref_img.word(a + f.size - 4))
                         == boundary_kind(self.m.dst_img.word(g.target + g.size - 4)))
            g.checks = {"callers": (len(r_callers.get(a, ())), len(d_callers.get(g.target, ())), callers_ok),
                        "callees": (len(r_callees.get(a, ())), len(mapped), callees_ok),
                        "data": (dref[0], dref[1], dref[0] == dref[1]),
                        "bounds": bounds_ok, "entry": g.target in self.dst_entries}
            if g.status == "sized":
                continue
            if callers_ok and callees_ok and dref[0] == dref[1] and bounds_ok:
                g.status = "strong" if g.sim >= GAP_STRONG_SIM else "graph"
            elif callers_ok and dref[0] == dref[1] and bounds_ok:
                g.status = "position"
            else:
                g.status = "weak"


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


def report_header(game: dict, ref: str, to: str, count: int) -> str:
    """Comment lines at the top of the transposition report (the column line comes last)."""
    sym = game.get("symbols", {})
    lines = [f"# Transposition report {game['game']['name']} {ref} -> {to} - generated by tools/symbols.py transpose, "
             "do not edit.",
             f"# Every function of the {ref} DOL ({count}, dtk symbols.txt) with its {ref} size and its {to} address",
             f"#   (00000000 = not found; the {to} size is the {ref} size only when identical = 1); candidates = at",
             "#   most 8 starts where the matcher found code equal to the reference (relocations masked).",
             "# Read by tools/decomp_functions.py (activation lock of the decompiled functions, run by tools/build.py)",
             "#   and tools/decomp_progress.py."]
    for a in sym.get("attribution", "").splitlines():
        lines.append("# " + a if a.strip() else "#")
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
    report = cfg["out"] / f"{ref}-to-{to}.tsv"
    report.parent.mkdir(parents=True, exist_ok=True)
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
    with report.open("w", encoding="utf-8", newline="\n") as f:
        f.write(report_header(game, ref, to, len(res)))
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
    print(f"transposition report: {report}")
    return stats


def dtk_revision(dtk: Path, label: str) -> str:
    """Git commit of the repository that holds the dtk folder (the generated files name the symbols they used)."""
    import subprocess
    try:
        out = subprocess.run(["git", "-C", str(dtk), "log", "-1", "--format=%H (%cs)"],
                             capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return "commit unknown (git not available)"
    commit = out.stdout.strip() if out.returncode == 0 else ""
    return f"{label}, commit {commit}" if commit else f"{label}, commit unknown"


def cmd_gaps(slug: str, game: dict, to: str) -> None:
    """Places the functions that the transposition did not find (GapFiller) and writes <to>-gaps.tsv next to the maps.
    Read-only on the transposition: the maps and the transposition report are not rewritten."""
    cfg = config(game)
    ref = cfg["ref"]
    ref_dol, _ = code_paths(slug, ref, game)
    dst_dol, _ = code_paths(slug, to, game)
    for p in (ref_dol, dst_dol):
        if not p.exists():
            sys.exit(f"missing: {p} (run tools/extract.py {slug} --version ...)")
    sym_txt = cfg["dtk"] / "symbols.txt"
    if not sym_txt.exists():
        sys.exit(f"missing: {sym_txt} (dtk symbols of {ref}: set dtk_dir in the [symbols] section of game.toml, "
                 f"or fetch the decomp project it points to)")
    funcs = functions(read_dtk(sym_txt))
    entries = ghidra_starts(slug, to)
    if not entries:
        sys.exit(f"missing: Ghidra functions.tsv of {to} (run tools/ghidra/analyze.py)")
    ref_entries = ghidra_starts(slug, ref)
    dst_eti, ref_eti = exception_index(dst_dol), exception_index(ref_dol)
    matcher = Matcher(Image.from_dol(ref_dol), funcs, Image.from_dol(dst_dol), entries)
    ref_sup = {n: a for a, n, st in read_supplement(cfg, ref) if verified(st)}
    seeds = {ref_sup[n]: a for a, n, st in read_supplement(cfg, to) if verified(st) and n in ref_sup}
    res = matcher.run(seeds)
    filler = GapFiller(matcher, res, entries, set(dst_eti), ref_entries | set(ref_eti))
    gaps = filler.run()
    filler.check(gaps, ref_dol, dst_dol)
    counts: dict[str, int] = defaultdict(int)
    for g in gaps.values():
        counts[(g.status, res[g.ref.addr].status if g.status != "sized" else "found")] += 1
    todo = [g for g in gaps.values() if g.status != "sized"]
    placed = [g for g in todo if g.target is not None]
    lines = [f"Functions without a target after the transposition: {len(todo)}; placed: {len(placed)}."]
    for st in ("strong", "graph", "position", "weak", "not-linked", "unresolved", "sized"):
        n = sum(v for (s, _), v in counts.items() if s == st)
        if n:
            lines.append(f"  {st}: {n}")
    lines.append(f"Exception index: {len(ref_eti)} entries in {ref}, {len(dst_eti)} in {to}.")
    tonly = sorted(set(filler.target_only))
    lines.append(f"Target-only code left unassigned by the layouts ({len(tonly)}): "
                 + (", ".join(f"{s:08X}+{e - s:X}" for s, e in tonly) or "none") + ".")
    holes = filler.uncovered()
    lines.append(f"Target code covered by no placed function, zero padding excluded ({len(holes)}): "
                 + (", ".join(f"{s:08X}+{e - s:X}" for s, e in holes) or "none") + ".")
    print(NL.join(lines))
    sym = game.get("symbols", {})
    head = [f"# Gap filling {game['game']['name']} {ref} -> {to} - generated by tools/symbols.py gaps, do not edit.",
            "# Functions that the transposition (<ID>.map) did not find, placed by position between found functions",
            "# and checked on the complete partition of the target code; plus the target size of the found but",
            "# modified functions (size 0 in <ID>.map). Not read by dolrecomp; method: GapFiller in tools/symbols.py.",
            f"# Format (TSV, hexadecimal addresses and sizes): {ref} {to} {ref}_size {to}_size status similarity",
            "#   callers(ref/target) callees(ref/target) data_refs(ref/target) bounds ghidra_entry transposition name",
            "# status: strong = similarity >= 0.80 and same callers, callees, data references and boundaries;",
            "#   graph = same checks, lower similarity; position = callers, data references and boundaries agree,",
            "#   callees differ (code changed); weak = other failure; not-linked = no room in the target between its",
            "#   neighbors; unresolved = no layout; sized = found by the transposition, target size from the partition.",
            "# A placed range never holds a Ghidra function entry or an exception-index entry of the target strictly",
            "#   inside it (except a split present at the same offset in the reference); such code is target-only.",
            "# '!' marks a check that differs. Names: as in the reference map (fn_<address> = automatic name).",
            "# Activation: no row of this file may be enabled by default in a function list (e.g. functions.tsv of",
            "#   the decompilation). The position and sized rows in particular are enabled per version, and only",
            "#   after the decompilation oracle has passed on that version.",
            f"# Reference symbols: {dtk_revision(cfg['dtk'], sym.get('dtk_dir', ''))}.",
            "# This file only contains names, addresses and sizes, no game data."]
    for a in sym.get("attribution", "").splitlines():
        if a.strip() and not a.strip().startswith("This file only contains"):
            head.append(f"# {a.strip()}")
    head += [f"# {x}" for x in lines]
    out = cfg["out"] / f"{to}-gaps.tsv"
    with out.open("w", encoding="utf-8", newline=NL) as f:
        f.write(NL.join(head) + NL)
        for a in sorted(gaps):
            g = gaps[a]
            c = g.checks

            def pair(k):
                if k not in c:
                    return "-"
                x, y, ok = c[k]
                return f"{x}/{y}" + ("" if ok else "!")
            bounds = ("ok" if c.get("bounds") else "!") if c else "-"
            row = [f"{a:08X}", f"{(g.target or 0):08X}", f"{g.ref.size:X}", f"{g.size:X}", g.status, f"{g.sim:.3f}",
                   pair("callers"), pair("callees"), pair("data"), bounds, str(int(bool(c.get("entry")))),
                   res[a].status, g.ref.name]
            f.write("\t".join(row) + NL)
    print(f"{out}: {len(gaps)} rows")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("cmd", choices=["convert", "transpose", "all", "gaps"])
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
    if ns.cmd == "gaps":
        if to == config(game)["ref"]:
            sys.exit("the target version is the reference version")
        cmd_gaps(ns.game, game, to)
    return 0


if __name__ == "__main__":
    sys.exit(main())
