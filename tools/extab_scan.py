"""CodeWarrior (MWCC) exception tables: reads `extabindex` and `extab` of a DOL (and looks for them in its REL
modules), gives the exact bounds of every function that has an entry and its cleanup actions, and compares those
bounds with the project's function boundaries. Generic; every game-specific path comes from games/<game>/game.toml.
Read-only on the game files and on the symbol maps.

Usage:
  py -3 tools/extab_scan.py <game> [--version ID[,ID]] [--root CHECKOUT] [--no-rel] [--out DIR] [--json]
                                   [--quiet]

  --version  versions to scan (default: every version of game.toml)
  --root     checkout holding the untracked inputs (external/, generated/); default: this checkout
  --no-rel   the DOL only (default: the REL modules are searched too, relocated by tools/gcimage.py)
  --out      output folder (one sub-folder <ID>/ per version, default build/extab/<ID>/): extab-<ID>.txt
             (readable report) and extab-<ID>.json
  --json     also print the JSON summary on stdout
Outputs hold addresses, sizes and names only, never game bytes; they stay local (build/ is not in Git).
Exit code: 0 when every version was scanned (also when a DOL holds no exception table: the report says so);
2 on a usage error (unknown game or version); 1 when an input is missing or is not a DOL (the message on stderr
names the file).

What is compared (every source read only):
  - the symbol map of the version (games/<game>/<[symbols] map_dir>/<ID>.map, named functions);
  - the reference version: every function of the decomp-toolkit symbols ([symbols] dtk_dir);
  - another version: every reference function placed in it by the transposition report and the gap rows
    (games/<game>/symbols/<ref>-to-<ID>.tsv, <ID>-gaps.tsv; sizes: the gap size, or the reference size when the
    code is identical);
  - Ghidra's function list (generated/<game>/<ID>/ghidra/out/functions.tsv), when present.
For each table entry: same bounds, end differs (same start, other size), start differs (the source has a function
that contains the start), source splits it (a source function starts inside it), or absent from the source.
Functions of the covered code range that have no entry are listed with whether their prologue opens a stack frame.
Units: with the reference dtk splits.txt, the units that hold functions with an entry are listed (in another
version the units come through the transposition). [hypothesis] these are the units built with C++ exceptions on
(`-Cpp_exceptions on`); not verified: the splits define only 15 units, and almost every entry lies outside them
(the report counts them).

Formats. Location: the `_eti_init_info` table, records {extabindex start, extabindex end, code start, code size}
ended by a 0 word, found by its shape (the records point to a 12-byte-aligned index range and to code); fallback:
the longest run of index-shaped triples (the method of tools/symbols.py exception_index). extabindex entry =
{function address, function size, extab address} (12 bytes). extab entry, as decoded by
cwextab (https://github.com/CelestialAmber/cwextab, "MIT OR Apache-2.0" as declared in lib/Cargo.toml; the flags
layout is in lib/src/extab.rs, the entry and action decoding in lib/src/lib.rs and actions.rs). The action names,
their numbering and their sizes below follow cwextab; no code is copied. Entry: u16 flags, u16 field, PC ranges
{u32 start offset, u16 length in words, u16 action offset} ended by a 0 word, then the actions: u8 type (bit 7 = end of a chain), u8 parameter, then a type-dependent body
(sizes: branch 2, destroy-local 6, destroy-local-cond 10, destroy-local-pointer 6, destroy-local-array 10,
destroy-base / destroy-member 10, destroy-member-cond 14, destroy-member-array 18, delete-pointer 6,
delete-pointer-cond 10, catch-block 10, active-catch-block 2, terminate 0, specification 10 + 4 per type,
catch-block-32 14), with the destructor address at a fixed offset of the body for the destroy and delete actions.
Flags (cwextab): bit 1 ELF vector, bit 3 large frame, bit 4 frame pointer, bit 5 saved CR, bits 6-10 number of
saved FPRs, bits 11-15 number of saved GPRs (counted from the low bit as in cwextab). The tool checks the saved
register counts against each function's prologue (stmw / stw / stfd relative to r1), and the destructor addresses
against the function starts.
Limits: the function size field is used as it is (low bits checked); REL modules are only searched for a table.
Only 7 of the 17 action types of the table (type 0 is the end marker) occur in this game's tables (destroy-local,
destroy-local-cond, branch, specification, active-catch-block, terminate, catch-block-32): the sizes and offsets of
the other 10 come from cwextab and are untested on game data. Of the flags, only bits 6-15 (saved FPR and GPR
counts) are cross-checked, against the prologues; the other bits are decoded as cwextab names them.
"""

from __future__ import annotations

import argparse
import bisect
import json
import struct
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import decomp_common as dc  # noqa: E402
import gcimage  # noqa: E402
import scan_inputs as si  # noqa: E402
import toolchain as tc  # noqa: E402

ACTIONS = {  # type: (name, body size, offset of the destructor address in the body or None)
    0: ("end-of-list", 0, None), 1: ("branch", 2, None), 2: ("destroy-local", 6, 2),
    3: ("destroy-local-cond", 10, 6), 4: ("destroy-local-pointer", 6, 2), 5: ("destroy-local-array", 10, 6),
    6: ("destroy-base", 10, 6), 7: ("destroy-member", 10, 6), 8: ("destroy-member-cond", 14, 10),
    9: ("destroy-member-array", 18, 14), 10: ("delete-pointer", 6, 2), 11: ("delete-pointer-cond", 10, 6),
    12: ("catch-block", 10, None), 13: ("active-catch-block", 2, None), 14: ("terminate", 0, None),
    15: ("specification", 10, None), 16: ("catch-block-32", 14, None),
}
STWU_R1 = 0x94210000  # stwu r1,-X(r1): opens a stack frame


# ------------------------------------------------------------------------------------------- tables
def find_eti(prog: gcimage.Program, module: int) -> list[dict]:
    """`_eti_init_info` candidates of one module: list of {at, records: [(idx_start, idx_end, code, size)]}."""
    found = []
    segs = [s for s in prog.segments if s.module == module and not s.bss]
    for s in segs:
        for o in range(0, len(s.data) - 19, 4):
            a = s.addr + o
            recs = []
            x = a
            while True:
                e0, e1, c0, cs = (prog.word(x + k) for k in (0, 4, 8, 12))
                if None in (e0, e1, c0, cs) or not e0:
                    break
                if (e1 <= e0 or (e1 - e0) % 12 or prog.seg(e0) is None or prog.seg(e0) is not prog.seg(e1 - 4)
                        or prog.seg(e0).code or not cs or not prog.is_code(c0) or not prog.is_code(c0 + cs - 4)):
                    recs = []
                    break
                recs.append((e0, e1, c0, cs))
                x += 16
            if recs and prog.word(x) == 0 and all(_index_ok(prog, r) for r in recs):
                found.append({"at": a, "records": recs})
    return found


def _index_ok(prog: gcimage.Program, rec: tuple) -> bool:
    e0, e1, c0, cs = rec
    for p in range(e0, e1, 12):
        f, z, x = prog.word(p), prog.word(p + 4), prog.word(p + 8)
        if not (c0 <= f < c0 + cs and z and prog.seg(x) is not None and not prog.seg(x).code):
            return False
    return True


def longest_run(prog: gcimage.Program, module: int) -> tuple[int, int] | None:
    """Fallback: the longest run (at least 8) of {code address, size, data address} triples in increasing,
    non-overlapping order (tools/symbols.py exception_index). Returns the (start, end) of the index or None."""
    best = (0, 0, 0)
    for s in prog.segments:
        if s.module != module or s.code or s.bss:
            continue
        n = len(s.data) // 4
        ws = struct.unpack_from(f">{n}I", s.data, 0)
        for phase in range(3):
            start, count, prev_end = None, 0, 0
            for i in range(phase, n - 2, 3):
                f, z, x = ws[i], ws[i + 1], ws[i + 2]
                ok = (f % 4 == 0 and z % 4 == 0 and 0 < z < 0x100000 and f >= prev_end and prog.is_code(f)
                      and prog.is_code(f + z - 4) and prog.seg(x) is not None and not prog.seg(x).code)
                if ok:
                    if start is None:
                        start, count = s.addr + 4 * i, 0
                    count += 1
                    prev_end = f + z
                    if count > best[2]:
                        best = (start, start + 12 * count, count)
                else:
                    start, count, prev_end = None, 0, 0
    return (best[0], best[1]) if best[2] >= 8 else None


def decode(prog: gcimage.Program, addr: int, span: int) -> dict:
    """One extab entry (`span` bytes up to the next entry), decoded as cwextab does."""
    raw = prog.bytes_at(addr, span) or b""
    out = {"flags": None, "pc_actions": [], "actions": [], "error": None, "dtors": []}
    if len(raw) < 8:
        out["error"] = f"entry shorter than 8 bytes ({len(raw)})"
        return out
    flags, field = struct.unpack_from(">HH", raw, 0)
    out["flags"], out["field"] = flags, field
    out["saved_gprs"] = (flags >> 11) & 31
    out["saved_fprs"] = (flags >> 6) & 31
    out["saved_cr"] = bool((flags >> 5) & 1)
    out["frame_pointer"] = bool((flags >> 4) & 1)
    out["large_frame"] = bool((flags >> 3) & 1)
    out["elf_vector"] = bool((flags >> 1) & 1)
    o = 4
    while o + 4 <= len(raw) and struct.unpack_from(">I", raw, o)[0] != 0:
        if o + 8 > len(raw):
            out["error"] = "PC range list runs past the entry"
            return out
        start, words, act = struct.unpack_from(">IHH", raw, o)
        out["pc_actions"].append((start, start + 4 * words, act))
        o += 8
    o += 4
    while o < len(raw):
        at = o
        if out["actions"] and out["actions"][-1][2] and len(raw) - o < 4 and len(raw) % 4 == 0:
            # After the end of the last chain, only the padding up to the next 4-byte boundary is left (its
            # bytes are not always 0: uninitialized padding, as cwextab notes for action bodies).
            out["padding"] = raw[o:].hex()
            break
        t, param = raw[o], raw[o + 1] if o + 1 < len(raw) else 0
        kind = ACTIONS.get(t & 0x7F)
        if kind is None:
            out["error"] = f"unknown action type {t & 0x7F} at +0x{at:X}"
            break
        name, size, dt = kind
        if t & 0x7F == 15 and o + 4 <= len(raw):
            size += 4 * struct.unpack_from(">H", raw, o + 2)[0]
        body = o + 2
        if body + size > len(raw):
            out["error"] = f"action {name} at +0x{at:X} runs past the entry"
            break
        if dt is not None:
            out["dtors"].append(struct.unpack_from(">I", raw, body + dt)[0])
        out["actions"].append((at, name, bool(t & 0x80)))
        o = body + size
    starts = {a for a, _n, _e in out["actions"]}
    out["pc_action_targets_ok"] = all(act in starts for _s, _e, act in out["pc_actions"])
    return out


# ------------------------------------------------------------------------------------------- prologue
def prologue(prog: gcimage.Program, start: int, size: int) -> dict:
    """Saved registers seen in the first instructions of a function (frame opened, lowest GPR / FPR stored
    relative to r1 among r14-r31 / f14-f31, or by a save helper called with `bl` whose first instruction stores
    relative to r11, as the ABI's _savefpr_N / _savegpr_N ladders do)."""
    gpr, fpr, frame = 32, 32, False
    for k in range(min(size // 4, 48)):
        w = prog.word(start + 4 * k)
        if w is None:
            break
        op, rs, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        if w & 0xFFFF0000 == STWU_R1:
            frame = True
        elif ra == 1 and op == 47:                           # stmw rS, d(r1)
            gpr = min(gpr, rs)
        elif ra == 1 and op == 36 and 14 <= rs <= 31:        # stw rS, d(r1)
            gpr = min(gpr, rs)
        elif ra == 1 and op == 54 and 14 <= rs <= 31:        # stfd fS, d(r1)
            fpr = min(fpr, rs)
        elif ra == 1 and op == 60 and 14 <= rs <= 31:        # psq_st fS, d(r1), W, I (opcode 60; 61 is psq_stu)
            fpr = min(fpr, rs)
        elif op == 18 and w & 1:                             # bl: a register save helper of the ABI?
            t = prog.word(gcimage.branch_target(w, start + 4 * k) or 0)
            if t is not None and (t >> 16) & 31 == 11:         # its first instruction stores relative to r11
                if t >> 26 == 54:                                # stfd fN, d(r11): _savefpr_N
                    fpr = min(fpr, (t >> 21) & 31)
                elif t >> 26 == 36:                              # stw rN, d(r11): _savegpr_N
                    gpr = min(gpr, (t >> 21) & 31)
        elif w == 0x4E800020:                                # blr
            break
    return {"frame": frame, "gprs": 32 - gpr, "fprs": 32 - fpr}


# ------------------------------------------------------------------------------------------- sources
def sources(slug: str, game: dict, vid: str, root: Path,
            prog: gcimage.Program) -> dict[str, dict[int, tuple[int, str]]]:
    out: dict[str, dict[int, tuple[int, str]]] = {}
    out["map"] = {a: (s, n) for a, s, n in si.read_map(si.map_path(game, vid))}
    ref = si.dtk_reference(game)
    if ref == vid:
        out["dtk"] = {a: v for a, v in si.dtk_functions(game, root, prog).items() if prog.seg(a)
                      and prog.seg(a).module == 0}
    else:
        trans = dc.read_transposition(dc.transposition_path(game, tc.ROOT, vid))
        gaps = {g.ref: g for g in dc.read_gaps(dc.gaps_path(game, vid))}
        placed: dict[int, tuple[int, str]] = {}
        for r, t in trans.items():
            a, how = dc.paired_address(r, trans, gaps)
            if not a:
                continue
            g = gaps.get(r)
            size = g.size if g is not None and g.target == a and g.size else (t.ref_size if t.identical else 0)
            placed[a] = (size, t.name)
        if placed:
            out["transposed"] = placed
    g = si.ghidra_functions(slug, vid, root)
    if g:
        out["ghidra"] = {a: (s, "") for a, s in g.items()}
    return out


def compare(entries: list[dict], src: dict[int, tuple[int, str]]) -> tuple[Counter, list]:
    order = sorted(src)
    counts, diffs = Counter(), []
    for e in entries:
        s, z = e["start"], e["size"]
        if s in src:
            ss = src[s][0]
            if ss == z:
                counts["same"] += 1
            elif ss == 0:
                counts["same start, size unknown in the source"] += 1
            else:
                counts["end differs"] += 1
                diffs.append({"start": s, "size": z, "kind": "end differs", "source_size": ss, "name": src[s][1]})
            inner = [a for a in order[bisect.bisect_right(order, s):bisect.bisect_left(order, s + z)]]
            if inner:
                counts["source splits it"] += 1
                diffs.append({"start": s, "size": z, "kind": "source splits it",
                              "inner": [f"{a:08X}" for a in inner[:8]]})
            continue
        i = bisect.bisect_right(order, s) - 1
        if i >= 0 and src[order[i]][0] and s < order[i] + src[order[i]][0]:
            counts["start differs"] += 1
            diffs.append({"start": s, "size": z, "kind": "start differs", "source_start": order[i],
                          "source_size": src[order[i]][0], "name": src[order[i]][1]})
        else:
            counts["absent from the source"] += 1
    return counts, diffs


# ------------------------------------------------------------------------------------------- scan
def scan(prog: gcimage.Program, module: int = 0) -> dict:
    eti = find_eti(prog, module)
    method = "_eti_init_info"
    if eti:
        recs = eti[0]["records"]
    else:
        run = longest_run(prog, module)
        if not run:
            return {"found": False, "eti_candidates": 0}
        method = "longest run"
        recs = [(run[0], run[1], None, None)]
    idx = []
    for e0, e1, _c0, _cs in recs:
        for p in range(e0, e1, 12):
            idx.append((p, prog.word(p), prog.word(p + 4), prog.word(p + 8)))
    ext_addrs = sorted({x for _p, _f, _z, x in idx})
    ends = ext_addrs[1:] + [min(r[0] for r in recs)]
    span = {x: y - x for x, y in zip(ext_addrs, ends)}
    entries = []
    for p, f, z, x in idx:
        d = decode(prog, x, span[x])
        entries.append({"index": p, "start": f, "size": z & ~3, "size_low_bits": z & 3, "extab": x,
                        "extab_size": span[x], **d})
    return {"found": True, "method": method, "eti_candidates": len(eti),
            "eti_at": eti[0]["at"] if eti else None, "records": recs, "entries": entries,
            "extab_range": (ext_addrs[0], ends[-1]) if ext_addrs else None}


def run(slug: str, game: dict, vid: str, root: Path, rels: bool, out: Path) -> dict:
    prog = si.program(slug, game, vid, root, rels)
    res = scan(prog, 0)
    w = prog.where
    srcs = sources(slug, game, vid, root, prog)
    names = {}
    for key in ("map", "dtk", "transposed"):
        for a, (_s, n) in srcs.get(key, {}).items():
            if n and not si.is_auto(n):
                names.setdefault(a, n)
    report: dict = {"version": vid, "dol": {}, "rel": {}}
    if res["found"]:
        ents = res["entries"]
        cleanup = [e for e in ents if e["pc_actions"]]
        regs = Counter()
        for e in ents:
            pro = prologue(prog, e["start"], e["size"])
            e["prologue"] = pro
            regs["gprs agree" if pro["gprs"] == e["saved_gprs"] else "gprs differ"] += 1
            regs["fprs agree" if pro["fprs"] == e["saved_fprs"] else "fprs differ"] += 1
            regs["frame" if pro["frame"] else "no frame"] += 1
        all_starts = set()
        for s in srcs.values():
            all_starts |= set(s)
        dt = Counter()
        for e in cleanup:
            for d in e["dtors"]:
                dt["destructor at a known function start" if d in all_starts else "destructor elsewhere"] += 1
                n = names.get(d, "")
                if n:
                    dt["named " + ("__dt__" if n.startswith("__dt__") else "other")] += 1
        cmp_ = {}
        diffs = {}
        for key, src in srcs.items():
            c, dl = compare(ents, src)
            cmp_[key] = dict(c)
            diffs[key] = dl
        # Functions of the covered range without an entry.
        lo = min(r[2] for r in res["records"]) if res["records"][0][2] is not None else min(e["start"] for e in ents)
        hi = (max(r[2] + r[3] for r in res["records"]) if res["records"][0][2] is not None
              else max(e["start"] + e["size"] for e in ents))
        have = {e["start"] for e in ents}
        best = srcs.get("dtk") or srcs.get("transposed") or srcs.get("ghidra") or {}
        missing = []
        for a in sorted(best):
            if lo <= a < hi and a not in have:
                size = best[a][0] or 4 * 48
                missing.append((a, best[a][0], prologue(prog, a, size)["frame"], best[a][1]))
        # Units.
        units = units_of(slug, game, vid, root, [e["start"] for e in ents], best)
        dol = {
            "method": res["method"], "eti_init_info": w(res["eti_at"]) if res["eti_at"] else None,
            "eti_candidates": res["eti_candidates"],
            "records": [{"index": f"{w(a)}-{w(b)}", "code": f"{w(c)}+0x{s:X}" if c else None}
                        for a, b, c, s in res["records"]],
            "extab": f"{w(res['extab_range'][0])}-{w(res['extab_range'][1])}" if res["extab_range"] else None,
            "entries": len(ents), "entries_8_bytes": sum(1 for e in ents if e["extab_size"] == 8),
            "with_cleanup_actions": len(cleanup),
            "decode_errors": sum(1 for e in ents if e["error"]),
            "pc_action_targets_bad": sum(1 for e in ents if not e["pc_action_targets_ok"]),
            "size_low_bits_set": sum(1 for e in ents if e["size_low_bits"]),
            "action_types": dict(Counter(n for e in ents for _a, n, _x in e["actions"] if n != "end-of-list")),
            "destructors": dict(dt),
            "prologue_check": dict(regs),
            "boundaries": cmp_,
            "covered_range": f"{w(lo)}-{w(hi)}",
            "functions_in_range_without_entry": len(missing),
            "without_entry_frame": sum(1 for m in missing if m[2]),
            "without_entry_no_frame": sum(1 for m in missing if not m[2]),
            "units_with_entries": len(units["with"]) if units else None,
            "units_defined": units["units_defined"] if units else None,
            "entries_outside_defined_units": units["unplaced"] if units else None,
        }
        report["dol"] = dol
        report["functions"] = [{
            "start": w(e["start"]), "size": f"0x{e['size']:X}", "name": names.get(e["start"], ""),
            "extab": w(e["extab"]), "extab_size": e["extab_size"], "flags": f"0x{e['flags']:04X}",
            "saved_gprs": e["saved_gprs"], "saved_fprs": e["saved_fprs"], "prologue": e["prologue"],
            "pc_actions": [[f"+0x{s:X}", f"+0x{t:X}", f"+0x{a:X}"] for s, t, a in e["pc_actions"]],
            "actions": [n for _a, n, _x in e["actions"] if n != "end-of-list"],
            "destructors": [w(d) + (" " + names[d] if d in names else "") for d in e["dtors"]],
            "error": e["error"]} for e in ents]
        report["differences"] = {k: [{**d, "start": w(d["start"]), "size": f"0x{d['size']:X}",
                                      **({"source_start": w(d["source_start"])} if "source_start" in d else {})}
                                     for d in v] for k, v in diffs.items()}
        report["without_entry"] = [{"start": w(a), "size": f"0x{s:X}", "frame": fr, "name": n}
                                   for a, s, fr, n in missing]
        report["units"] = units
    # REL modules: look for a table.
    for mid, m in sorted(prog.modules.items()):
        if not mid:
            continue
        r = scan(prog, mid)
        report["rel"][m.name] = ({"found": True, "entries": len(r["entries"]), "method": r["method"]} if r["found"]
                                 else {"found": False})
    out.mkdir(parents=True, exist_ok=True)
    (out / f"extab-{vid}.json").write_text(json.dumps(report, indent=1, default=str), encoding="utf-8")
    (out / f"extab-{vid}.txt").write_text(text(report, game["game"]["name"]), encoding="utf-8")
    return report


def units_of(slug: str, game: dict, vid: str, root: Path, starts: list[int], best: dict) -> dict | None:
    """Units (reference dtk splits.txt) holding functions with an entry; `unplaced` counts the entries outside
    every unit the splits define (a young dtk configuration defines only some units)."""
    units = si.dtk_units(game, root)
    if not units:
        return None
    ref = si.dtk_reference(game)
    to_ref: dict[int, int] = {}
    if vid != ref:
        trans = dc.read_transposition(dc.transposition_path(game, tc.ROOT, vid))
        gaps = {g.ref: g for g in dc.read_gaps(dc.gaps_path(game, vid))}
        for r in trans:
            a, _how = dc.paired_address(r, trans, gaps)
            if a:
                to_ref[a] = r
    ustarts = [u[0] for u in units]

    def unit(a: int) -> str | None:
        r = a if vid == ref else to_ref.get(a)
        if r is None:
            return None
        i = bisect.bisect_right(ustarts, r) - 1
        return units[i][2] if i >= 0 and r < units[i][1] else None

    with_ = Counter(u for u in (unit(a) for a in starts) if u)
    unplaced = sum(1 for a in starts if unit(a) is None)
    return {"units_defined": len(units), "with": dict(sorted(with_.items())), "unplaced": unplaced}


def text(r: dict, game_name: str) -> str:
    out = [f"# Exception tables, {game_name} {r['version']}", ""]
    for k, v in r.get("dol", {}).items():
        out.append(f"{k}: {v}")
    out.append("REL modules: " + ", ".join(f"{k} {'table' if v['found'] else 'none'}" for k, v in r["rel"].items()))
    if r.get("units"):
        out += ["", "## Units with exception tables (built with C++ exceptions on)", ""]
        out += [f"{u}  {n} function(s)" for u, n in r["units"]["with"].items()]
        if r["units"]["unplaced"]:
            out.append(f"(functions not placed in a unit: {r['units']['unplaced']})")
    out += ["", "## Functions with cleanup actions", ""]
    for f in r.get("functions", []):
        if f["pc_actions"]:
            out.append(f"{f['start']} size {f['size']} {f['name']}  ranges {len(f['pc_actions'])}  actions "
                       f"{Counter(f['actions'])}  destructors {f['destructors'][:6]}")
    for key, diffs in r.get("differences", {}).items():
        out += ["", f"## Differences with {key}", ""]
        for d in diffs:
            out.append(json.dumps(d))
    out += ["", "## Functions of the covered range without an entry", ""]
    for m in r.get("without_entry", []):
        out.append(f"{m['start']} size {m['size']} frame {m['frame']} {m['name']}")
    out += ["", "## All entries", ""]
    for f in r.get("functions", []):
        out.append(f"{f['start']} size {f['size']} extab {f['extab']} ({f['extab_size']} bytes) flags {f['flags']}"
                   f" gprs {f['saved_gprs']}/{f['prologue']['gprs']} fprs {f['saved_fprs']}/{f['prologue']['fprs']}"
                   f" {f['name']}" + (f" ERROR {f['error']}" if f["error"] else ""))
    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("game")
    ap.add_argument("--version")
    ap.add_argument("--root", type=Path, default=tc.ROOT)
    ap.add_argument("--no-rel", action="store_true")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    try:
        game = tc.load_game(a.game)
    except FileNotFoundError:
        print(f"unknown game: {a.game}", file=sys.stderr)
        return 2
    for vid in si.version_ids(game, a.version):
        out = (a.out / vid) if a.out else si.default_out("extab", vid)
        r = run(a.game, game, vid, a.root.resolve(), not a.no_rel, out)
        if not a.quiet:
            d = r["dol"]
            if not d:
                print(f"{vid}: no exception table found in the DOL")
            else:
                print(f"{vid}: {d['entries']} entries ({d['method']} at {d['eti_init_info']}), index {d['records']},"
                      f" extab {d['extab']}, {d['with_cleanup_actions']} with cleanup actions, decode errors "
                      f"{d['decode_errors']}")
                print(f"  boundaries: {json.dumps(d['boundaries'])}")
                print(f"  prologue check: {d['prologue_check']}; destructors: {d['destructors']}")
                print(f"  covered range {d['covered_range']}: {d['functions_in_range_without_entry']} functions "
                      f"without entry ({d['without_entry_frame']} with a frame, {d['without_entry_no_frame']} "
                      f"without); units with entries: {d['units_with_entries']} of the {d['units_defined']} units "
                      f"defined "
                      f"({d['entries_outside_defined_units']} entries outside them)")
            print(f"  REL: {sum(1 for v in r['rel'].values() if v['found'])}/{len(r['rel'])} modules with a table")
            print(f"  report: {out / f'extab-{vid}.txt'}")
        if a.json:
            print(json.dumps({"version": vid, "dol": r["dol"], "rel": r["rel"]}, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
