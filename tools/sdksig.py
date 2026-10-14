"""Address-independent function signatures (Nintendo SDK, Metrowerks runtime...). Generic.

Usage:
  py -3 tools/sdksig.py make <main.dol> <symbols.map> [--filter REGEX] [-o sigs.tsv]
  py -3 tools/sdksig.py find <main.dol> <sigs.tsv> [--starts functions.tsv]
  py -3 tools/sdksig.py catalog <sigs.tsv> [--include REGEX] [--hooks A,B] [--merge CATALOG] -o runtime/core/hle/sdk_signatures.tsv
    (--merge: extends an existing catalog without changing its lines; the new names go at the end)

A signature = (size, CRC-32 of the sequence of "masked" words, pattern of the first words).
The masking is the same as in tools/symbols.py (keep_masks): b/bl displacements, lis immediates,
accesses relative to r2/r13 (small data) or to a register loaded by lis, and bc instructions that leave
the function are ignored. Two copies of the same SDK function linked into two different games
therefore have the same signature as long as the compiler and the SDK version are the same.
The displayed pattern replaces with "?" every nibble that has at least one masked bit.

A signature only holds 8 pattern words and a checksum: no game code is copied.
"""

from __future__ import annotations

import argparse
import re
import sys
import zlib
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import symbols as sy  # noqa: E402
import toolchain  # noqa: E402,F401  (console encoding)

PATTERN_WORDS = 8
KEY_WORDS = 4


def pattern(words: list[int], masks: list[int]) -> str:
    out = []
    for w, m in zip(words, masks):
        s = ""
        for shift in range(28, -4, -4):
            nib_m = (m >> shift) & 0xF
            s += f"{(w >> shift) & 0xF:X}" if nib_m == 0xF else "?"
        out.append(s)
    return " ".join(out)


def signature(img: sy.Image, addr: int, size: int) -> tuple[int, str] | None:
    words = img.words(addr, size // 4)
    if not words:
        return None
    masks = sy.keep_masks(words, addr, size)
    data = b"".join((w & m).to_bytes(4, "big") for w, m in zip(words, masks))
    return zlib.crc32(data), pattern(words[:PATTERN_WORDS], masks[:PATTERN_WORDS])


def read_map(path: Path) -> list[tuple[int, int, str]]:
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        a, s, n = line.split()[:3]
        out.append((int(a, 16), int(s, 16), n))
    return out


def cmd_make(ns) -> int:
    img = sy.Image.from_dol(Path(ns.dol))
    rx = re.compile(ns.filter) if ns.filter else None
    lines = ["# name\tsize\tcrc32\tpattern (first 8 words, ? = masked nibble)"]
    for addr, size, name in read_map(Path(ns.map)):
        if size == 0 or (rx and not rx.search(name)):
            continue
        sig = signature(img, addr, size)
        if sig:
            lines.append(f"{name}\t{size:X}\t{sig[0]:08X}\t{sig[1]}")
    text = "\n".join(lines) + "\n"
    if ns.output:
        Path(ns.output).write_text(text, encoding="utf-8")
        print(f"{len(lines) - 1} signatures -> {ns.output}")
    else:
        sys.stdout.write(text)
    return 0


def parse_pattern(text: str) -> list[tuple[int, int]]:
    """"7C0802A6 9001????" -> [(value, mask)], nibble by nibble."""
    out = []
    for tok in text.split():
        v = m = 0
        for ch in tok:
            v <<= 4
            m <<= 4
            if ch != "?":
                v |= int(ch, 16)
                m |= 0xF
        out.append((v, m))
    return out


def read_sigs(path: Path) -> list[tuple[str, int, int, list[tuple[int, int]]]]:
    """Reads a signature file (`make` format) or a runtime catalog (`catalog` format)."""
    sigs = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        f = line.split("\t")
        if len(f) >= 5 and re.fullmatch(r"[0-9A-Fa-f]{8}", f[3]) and re.fullmatch(r"[0-9A-Fa-f]{8}", f[4]):
            pat = [(int(f[3], 16), int(f[4], 16))]  # catalog: one key word (value, mask)
        else:
            pat = parse_pattern(f[3])
        sigs.append((f[0], int(f[1], 16), int(f[2], 16), pat))
    return sigs


def find_in_image(img: sy.Image, sigs, extra_starts=()) -> dict[str, list[int]]:
    """Addresses where each signature is found (name -> sorted list). Candidate starts: bl targets,
    words following a blr, plus `extra_starts`. Same algorithm as the runtime scan (runtime/core/hle)."""
    starts = sy.Matcher(img, [], img, set())._candidate_starts()
    starts.update(extra_starts)
    index = defaultdict(lambda: defaultdict(list))
    for sig in sigs:
        v, m = sig[3][0]
        index[m][v].append(sig)
    found = defaultdict(list)
    for s in sorted(starts):
        w0 = img.word(s)
        if w0 is None:
            continue
        for m, table in index.items():
            for name, size, crc, pat in table.get(w0 & m, ()):
                head = img.words(s, len(pat))
                if not head or any((w & pm) != pv for w, (pv, pm) in zip(head, pat)):
                    continue
                sig = signature(img, s, size)
                if sig and sig[0] == crc:
                    found[name].append(s)
    return found


def cmd_catalog(ns) -> int:
    """Catalog embedded in the runtime: name, size, masked CRC32, masked first word (value, mask),
    flags. No address. The key is the first instruction word with its address-dependent fields masked:
    when the instruction has no such field, it is the complete instruction word."""
    rx = re.compile(ns.include) if ns.include else None
    hooks = set(ns.hooks.split(",")) if ns.hooks else set()
    lines = ["# Catalog of signatures of SDK and Metrowerks library functions, embedded in the runtime.",
             "# This file is dedicated to the public domain under CC0 1.0 (see /LICENSES/CC0-1.0.txt).",
             "# Produced by: py -3 tools/sdksig.py catalog. Holds no address. Per function:",
             "# size, CRC-32 of the masked words (tools/symbols.py:keep_masks), and the first masked instruction",
             "# word as a lookup key (the complete instruction word when it has no address-dependent field).",
             "# flags: hook = replacement provided by runtime/core/hle (entry point compiled with a hook).",
             "# Names: cbr_decomp decompilation (https://github.com/eavpsp/cbr_decomp, decomp_dtk_main, CC0 1.0)",
             "# and this project's analysis (recorded in the project's private working notes). Signatures are",
             "# identical from one game version to another.",
             "# Internal (static) SDK functions: \"name@file\" (e.g. AlarmHandler@dvdlow) to tell same-named",
             "# functions apart.",
             "# Several names can share a signature (identical code after masking); the cases are listed in the",
             "# project's private working notes.",
             "# name\tsize\tcrc32\tkey\tmask\tflags"]
    seen = set()
    kept = added = 0
    if ns.merge:
        # Merge: the lines of the existing catalog are kept as they are (order, flags); only the
        # signatures of missing names are appended. The order matters: the runtime names an address
        # recognized by several signatures after the first one in the catalog.
        for line in Path(ns.merge).read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            f = line.split("\t")
            if len(f) != 6:
                sys.exit(f"unexpected catalog line in {ns.merge}: {line!r}")
            seen.add(f[0])
            lines.append(line)
            kept += 1
    for name, size, crc, pat in read_sigs(Path(ns.sigs)):
        if name in seen or (rx and not rx.search(name)):
            continue
        seen.add(name)
        v, m = pat[0]
        lines.append(f"{name}\t{size:X}\t{crc:08X}\t{v:08X}\t{m:08X}\t{'hook' if name in hooks else '-'}")
        added += 1
    text = "\n".join(lines) + "\n"
    Path(ns.output).write_text(text, encoding="utf-8", newline="\n")
    print(f"{sum(1 for l in lines if not l.startswith('#'))} signatures -> {ns.output}"
          + (f" ({kept} kept from {ns.merge}, {added} added)" if ns.merge else ""))
    return 0


def cmd_find(ns) -> int:
    img = sy.Image.from_dol(Path(ns.dol))
    sigs = read_sigs(Path(ns.sigs))
    # Candidate starts: bl targets, words following a blr, and Ghidra functions if provided.
    starts = sy.Matcher(img, [], img, set())._candidate_starts()
    if ns.starts:
        for line in Path(ns.starts).read_text(encoding="utf-8").splitlines():
            if not line.startswith("#"):
                starts.add(int(line.split("	")[0], 16))
    # Index on the first word (mask, value) to avoid recomputing everything.
    index = defaultdict(list)
    for sig in sigs:
        v, m = sig[3][0]
        index[m].append(sig)
    by_key = {m: defaultdict(list) for m in index}
    for m, lst in index.items():
        for sig in lst:
            by_key[m][sig[3][0][0]].append(sig)
    found = defaultdict(list)
    for s in sorted(starts):
        w0 = img.word(s)
        if w0 is None:
            continue
        for m, table in by_key.items():
            for name, size, crc, pat in table.get(w0 & m, ()):
                head = img.words(s, len(pat))
                if not head or any((w & pm) != pv for w, (pv, pm) in zip(head, pat)):
                    continue
                sig = signature(img, s, size)
                if sig and sig[0] == crc:
                    found[name].append(s)
    for name, size, _, _ in sigs:
        hits = found.get(name, [])
        status = "unique" if len(hits) == 1 else ("absent" if not hits else "multiple")
        print(f"{name}	{status}	{','.join(f'{h:08X}' for h in hits[:6])}")
    n_unique = sum(1 for n, _, _, _ in sigs if len(found.get(n, [])) == 1)
    print(f"# {n_unique}/{len(sigs)} signatures found exactly once", file=sys.stderr)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    mk = sub.add_parser("make")
    mk.add_argument("dol")
    mk.add_argument("map")
    mk.add_argument("--filter")
    mk.add_argument("-o", "--output")
    fd = sub.add_parser("find")
    fd.add_argument("dol")
    fd.add_argument("sigs")
    fd.add_argument("--starts")
    ct = sub.add_parser("catalog", help="runtime catalog (runtime/core/hle/sdk_signatures.tsv)")
    ct.add_argument("sigs", help="file produced by `make`")
    ct.add_argument("--include", help="REGEX of the names to keep (generic functions only)")
    ct.add_argument("--hooks", help="comma-separated names to flag as \"hook\"")
    ct.add_argument("--merge", help="existing catalog to extend: its lines are kept as they are, "
                                    "only the missing names are appended")
    ct.add_argument("-o", "--output", required=True)
    ns = ap.parse_args()
    return {"make": cmd_make, "find": cmd_find, "catalog": cmd_catalog}[ns.cmd](ns)


if __name__ == "__main__":
    sys.exit(main())
