"""Static call graph of a DOL and its REL modules, for the planning views of tools/decomp_progress.py.

What it answers, and what it refuses to answer. Two ideas come from REA (MIT; only the ideas, this is our own code):
shortest call paths carry a flag saying whether the search was exhaustive, and an unknown is never reported as an
absence. Applied here:
  - a direct call or tail branch is an edge, read from the code once the REL relocations are applied (an unlinked REL
    has displacement 0 in its calls, which would read as a jump to itself);
  - an indirect call (bctrl, bclrl) or an indirect jump (bctr: a jump table or a tail call through a pointer, the two
    are not told apart here) has unknown targets. It is counted on its function, never dropped;
  - a function can be entered through such an instruction only if its address exists somewhere: in a data word, in a
    lis + addi/ori pair of the code, in a REL relocation (applied here, so the same scan sees it), or as a module
    entry (prolog, epilog, unresolved). `Graph.taken` lists those references. A shorter way in through an indirect call
    is possible exactly for those functions, which is what makes a route "incomplete frontier".
    Not seen: an address computed in another way (added offsets, or a lis and an addi in different functions).
    extab and extabindex hold the address of every function with an exception table: they are not references.
  - `Graph.pointer_sites` turns those references into weaker steps: the code that loads a function's address, and the
    code that loads the nearest address before a data word holding it (a table of pointers, such as a virtual table;
    a guess, so such a step is only ever part of a route marked incomplete). The indirect call that uses the pointer
    stays unknown.

Function bounds come from the caller. Inside a function: basic blocks with the rules of
tools/decomp_functions.py code_blocks (every call ends a block) up to a bctr, which ends the walk here on purpose, with
their edges. A block behind a jump table has no
known predecessor: such a function is marked (`Cfg.jumps`).

Block coverage does not exist for most functions: the coverage probe records function entries and the return address
of each call. `covered_blocks` gives what can be concluded from that, a lower bound: the entry, the blocks of the
observed calls, their dominators, and the single successors of those blocks.

Read-only. Generic: nothing here names a game; game parameters come from the caller.
"""

from __future__ import annotations

import bisect
import glob
import struct
import sys
from collections import defaultdict, deque
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import decomp_functions as dfn  # noqa: E402
import gcinfo  # noqa: E402

# Sections of the DOL (names of [decomp.dol_sections], as written by the linker) that are not references to a function.
NOT_REFERENCES = ("extab", "extabindex")

R_ADDR32, R_ADDR24, R_ADDR16, R_ADDR16_LO, R_ADDR16_HI, R_ADDR16_HA, R_ADDR14 = 1, 2, 3, 4, 5, 6, 7
R_REL24, R_REL14 = 10, 11
R_DOLPHIN_NOP, R_DOLPHIN_SECTION, R_DOLPHIN_END = 201, 202, 203

TABLE_SPAN = 0x800          # bytes: how far a data word can be from the address loaded by the code that uses its table


class GraphError(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# Code and data of a version


@dataclass
class Image:
    code: list[tuple[int, bytes]]            # executable ranges (guest address, relocated bytes)
    data: list[tuple[int, bytes, str]]       # data ranges (guest address, relocated bytes, name)
    module_entries: dict[int, str]           # guest address -> "<module> prolog" and so on
    modules: dict[str, int]                  # REL name -> guest address of its first code section
    warnings: list[str] = field(default_factory=list)


def iter_relocations(data: bytes):
    """Every relocation of a REL file: (module imported, section patched, offset in it, type, target section, addend).
    Format: import table at header offset 0x28 (offset, size), 8 bytes per module; then entries of 8 bytes (u16 offset
    from the previous entry, u8 type, u8 section, u32 addend) ended by R_DOLPHIN_END. Same reading as
    tools/gcinfo.py describe_rel."""
    imp_off, imp_size = struct.unpack_from(">II", data, 0x28)
    for i in range(imp_size // 8):
        module, pos = struct.unpack_from(">II", data, imp_off + 8 * i)
        where, section = 0, None
        while pos + 8 <= len(data):
            delta, kind, sec, addend = struct.unpack_from(">HBBI", data, pos)
            pos += 8
            where += delta
            if kind == R_DOLPHIN_END:
                break
            if kind == R_DOLPHIN_SECTION:
                section, where = sec, 0
            elif kind != R_DOLPHIN_NOP:
                yield module, section, where, kind, sec, addend


def _patch(blob: bytearray, offset: int, kind: int, s: int, p: int) -> bool:
    """Writes the relocated value (ELF PowerPC relocation types, as the SDK's OSLink); False for an unknown type."""
    word = lambda: struct.unpack_from(">I", blob, offset)[0]  # noqa: E731
    if kind == R_ADDR32:
        struct.pack_into(">I", blob, offset, s & 0xFFFFFFFF)
    elif kind == R_ADDR24:
        struct.pack_into(">I", blob, offset, (word() & ~0x03FFFFFC & 0xFFFFFFFF) | (s & 0x03FFFFFC))
    elif kind in (R_ADDR16, R_ADDR16_LO):
        struct.pack_into(">H", blob, offset, s & 0xFFFF)
    elif kind == R_ADDR16_HI:
        struct.pack_into(">H", blob, offset, (s >> 16) & 0xFFFF)
    elif kind == R_ADDR16_HA:
        struct.pack_into(">H", blob, offset, ((s + 0x8000) >> 16) & 0xFFFF)
    elif kind == R_ADDR14:
        struct.pack_into(">I", blob, offset, (word() & 0xFFFF0003) | (s & 0xFFFC))
    elif kind == R_REL24:
        struct.pack_into(">I", blob, offset, (word() & 0xFC000003) | ((s - p) & 0x03FFFFFC))
    elif kind == R_REL14:
        struct.pack_into(">I", blob, offset, (word() & 0xFFFF0003) | ((s - p) & 0xFFFC))
    else:
        return False
    return True


def link_rels(layout: list[dict], paths: list[Path]) -> tuple[list, list, dict, dict, list[str]]:
    """Applies the relocations of every module at the addresses of `layout` (tools/build.py rel_layout), in memory.
    Returns (code ranges, data ranges, module entries, first code address by module, warnings)."""
    warnings: list[str] = []
    blobs: dict[tuple[int, int], tuple[int, bytearray, bool]] = {}
    addr_of: dict[tuple[int, int], int] = {}
    for m in layout:
        for i, addr, _size, ex, blob in m["sections"]:
            addr_of[(m["id"], i)] = addr
            if blob:
                blobs[(m["id"], i)] = (addr, bytearray(blob), ex)
    entries: dict[int, str] = {}
    first_code: dict[str, int] = {}
    for m, path in zip(layout, paths):
        data = path.read_bytes()
        unknown = 0
        for module, section, where, kind, sec, addend in iter_relocations(data):
            place = blobs.get((m["id"], section))
            if place is None:
                continue                       # a section with no bytes (BSS): nothing to patch
            if module == 0:
                s = addend
            elif (module, sec) in addr_of:
                s = addr_of[(module, sec)] + addend
            else:
                unknown += 1
                continue
            if not _patch(place[1], where, kind, s, place[0] + where):
                unknown += 1
        if unknown:
            warnings.append(f"{m['name']}: {unknown} relocations not applied (unknown type or target module)")
        info = gcinfo.describe_rel(data)
        for label in ("prolog", "epilog", "unresolved"):
            sec, off = info[label]
            if sec and (m["id"], sec) in addr_of:
                entries[addr_of[(m["id"], sec)] + off] = f"{m['name']} {label}"
        code = [a for (mid, _i), (a, _b, ex) in blobs.items() if mid == m["id"] and ex]
        if code:
            first_code[m["name"]] = min(code)
    code_ranges = sorted((a, bytes(b)) for a, b, ex in blobs.values() if ex)
    names = {m["id"]: m["name"] for m in layout}
    data_ranges = sorted((a, bytes(b), f"{names[mid]}.{i}")
                         for (mid, i), (a, b, ex) in blobs.items() if not ex)
    return code_ranges, data_ranges, entries, first_code, warnings


def load_image(game_cfg: dict, disc_root: Path) -> Image:
    """Code and data of one version from its extracted disc (generated/<game>/<ID>/disc), RELs linked."""
    code_cfg = game_cfg.get("code", {})
    path = disc_root / code_cfg["dol"]
    if not path.exists():
        raise GraphError(f"{path} missing (tools/extract.py, or --root)")
    blob = path.read_bytes()
    info = gcinfo.describe_dol(blob)
    names = game_cfg.get("decomp", {}).get("dol_sections", {})
    code, data = [], []
    for s in info["segments"]:
        part = blob[s["offset"]:s["offset"] + s["size"]]
        if s["name"].startswith("text"):
            code.append((s["addr"], part))
        elif names.get(s["name"], s["name"]) not in NOT_REFERENCES:
            data.append((s["addr"], part, names.get(s["name"], s["name"])))
    rel_dir = disc_root / code_cfg["rel_dir"] if code_cfg.get("rel_dir") else None
    paths = sorted(rel_dir.rglob("*.rel")) if rel_dir and rel_dir.exists() else []
    warnings: list[str] = []
    entries: dict[int, str] = {}
    modules: dict[str, int] = {}
    if paths:
        rel_code, rel_data, entries, modules, warnings = link_rels(dfn._layout(game_cfg, disc_root), paths)
        code += rel_code
        data += rel_data
    return Image(sorted(code), sorted(data), entries, modules, warnings)


# ---------------------------------------------------------------------------------------------
# Graph


@dataclass
class Site:
    at: int                 # address of the instruction
    kind: str               # bl, bcl (calls), b, bc (tail branches), fall (runs off its end into the next
                            # function), bctrl, bclrl (indirect calls), bctr (indirect jump); pointer, table: the
                            # steps of Graph.pointer_sites, never in Node.sites
    target: int | None      # address of the callee, None when indirect
    callee: int | None      # start of the function holding the target, None when indirect or outside every function
    via: int | None = None  # table step: the data word that holds the function's address


@dataclass(frozen=True)
class Ref:
    """A place where the address of a function exists."""
    kind: str               # code (lis + addi/ori), data (a data word), entry (entry of a module)
    at: int                 # the instruction or the data word; 0 for an entry
    owner: int | None       # code: the function holding the instruction
    label: str = ""         # data: the section; entry: its name

    def __str__(self) -> str:
        return f"{self.kind} {self.at:08X} {self.label}".strip() if self.at else f"{self.kind} {self.label}"


@dataclass
class Node:
    addr: int
    size: int
    name: str
    tag: object = None      # whatever the caller attaches (the inventory row)
    sites: list[Site] = field(default_factory=list)

    @property
    def indirect(self) -> list[Site]:
        return [s for s in self.sites if s.target is None]

    @property
    def unknown_targets(self) -> list[Site]:
        return [s for s in self.sites if s.target is not None and s.callee is None]


class Graph:
    def __init__(self, image: Image, nodes: list[Node]):
        self.image = image
        self.nodes = {n.addr: n for n in sorted(nodes, key=lambda n: n.addr)}
        self.starts = sorted(self.nodes)
        self.callers: dict[int, list[tuple[int, Site]]] = defaultdict(list)   # callee -> (caller, site)
        self.taken: dict[int, list[Ref]] = defaultdict(list)                  # function -> where its address is
        self.loads: list[tuple[int, int, int]] = []                           # data address built in code, where, owner
        for a, label in image.module_entries.items():
            if a in self.nodes:
                self.taken[a].append(Ref("entry", 0, None, label))
        for n in self.nodes.values():
            self._scan_function(n)
        self._scan_data()
        self.loads.sort()
        self._load_values = [v for v, _a, _o in self.loads]

    def owner(self, addr: int) -> int | None:
        """Start of the function that holds `addr`."""
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0:
            n = self.nodes[self.starts[i]]
            if addr < n.addr + n.size:
                return n.addr
        return None

    def _site(self, n: Node, at: int, word: int) -> Site:
        op, lk, xo = word >> 26, word & 1, (word >> 1) & 0x3FF
        if op in (16, 18):
            kind = ("bl" if lk else "b") if op == 18 else ("bcl" if lk else "bc")
            t = dfn._branch_target(word, at)
            return Site(at, kind, t, self.owner(t))
        return Site(at, {16: "bclrl", 528: "bctrl" if lk else "bctr"}[xo], None, None)

    def _scan_function(self, n: Node) -> None:
        base, blob = dfn._code_of(self.image.code, n.addr)
        if blob is None or n.addr + n.size > base + len(blob):
            return
        for at in dfn.call_sites(self.image.code, n.addr, n.size):
            s = self._site(n, at, dfn._word(blob, at - base))
            n.sites.append(s)
        last = dfn._word(blob, n.addr + n.size - 4 - base)
        if last and last >> 26 not in (16, 18, 19):
            # A function whose last instruction is not a branch runs into the next one: an edge like any other. (A last
            # call is a call that does not return, as a failed assertion: nothing after it.)
            t = n.addr + n.size
            n.sites.append(Site(n.addr + n.size - 4, "fall", t, self.owner(t)))
        for s in n.sites:
            if s.callee is not None:
                self.callers[s.callee].append((n.addr, s))
        # Addresses built in the code: lis rX then addi or ori from rX, in code order (no control flow: a lis in one
        # branch and its addi after the join are still paired).
        high: dict[int, int] = {}
        for i in range(n.size // 4):
            w = dfn._word(blob, n.addr - base + 4 * i)
            op, rt, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:
                high[rt] = imm
            elif op == 14 and ra and ra in high:
                self._code_ref(n, n.addr + 4 * i, ((high[ra] << 16) + (imm - 0x10000 if imm & 0x8000 else imm)))
            elif op == 24 and rt in high:             # ori: rS is the field at bit 21, rA the destination
                self._code_ref(n, n.addr + 4 * i, (high[rt] << 16) | imm)

    def _code_ref(self, n: Node, at: int, value: int) -> None:
        value &= 0xFFFFFFFF
        if value in self.nodes:
            self.taken[value].append(Ref("code", at, n.addr))
        elif any(base <= value < base + len(blob) for base, blob, _name in self.image.data):
            self.loads.append((value, at, n.addr))

    def _scan_data(self) -> None:
        for base, blob, name in self.image.data:
            for i in range(len(blob) // 4):
                w = struct.unpack_from(">I", blob, 4 * i)[0]
                if w in self.nodes:
                    self.taken[w].append(Ref("data", base + 4 * i, None, name))

    def table_loads(self, word: int) -> list[tuple[int, int, int]]:
        """Code that loads the nearest data address at or before `word`, in the same data range and at most TABLE_SPAN
        away: (value, instruction, function) of each load. A guess at the code that uses the table holding `word`."""
        i = bisect.bisect_right(self._load_values, word) - 1
        if i < 0 or word - self._load_values[i] > TABLE_SPAN:
            return []
        value = self._load_values[i]
        if not any(base <= value and word < base + len(blob) for base, blob, _name in self.image.data):
            return []
        lo = bisect.bisect_left(self._load_values, value)
        return self.loads[lo:i + 1]

    def pointer_sites(self) -> dict[int, list[Site]]:
        """Function -> steps through an address reference to other functions: `pointer` where the function loads the
        address itself, `table` where it loads a table that holds it."""
        out: dict[int, list[Site]] = defaultdict(list)
        for a, refs in self.taken.items():
            for r in refs:
                if r.kind == "code":
                    out[r.owner].append(Site(r.at, "pointer", a, a))
                elif r.kind == "data":
                    for _value, at, owner in self.table_loads(r.at):
                        out[owner].append(Site(at, "table", a, a, r.at))
        return out


# ---------------------------------------------------------------------------------------------
# Basic blocks


@dataclass
class Cfg:
    starts: list[int]                  # block starts, sorted
    succ: dict[int, list[int]]
    pred: dict[int, list[int]]
    ends: dict[int, int]               # block start -> address of its last instruction
    jumps: bool                        # holds a bctr: some blocks may have predecessors that are not known here

    def block_of(self, at: int) -> int:
        return self.starts[bisect.bisect_right(self.starts, at) - 1]


def control_flow(code: list[tuple[int, bytes]], address: int, size: int, no_return: set[int]) -> Cfg | None:
    """Basic blocks of [address, address + size) with their edges. Rules of decomp_functions.code_blocks, tables not followed; in
    addition a call to a function of `no_return` has no successor. A bctr ends its path: its targets are not followed."""
    base, blob = dfn._code_of(code, address)
    if blob is None:
        return None
    n = size // 4
    words = [dfn._word(blob, address - base + 4 * i) for i in range(n)]
    leaders = {0}
    edges: list[list[int]] = []
    jumps = False
    for i, w in enumerate(words):
        op, lk, xo = w >> 26, w & 1, (w >> 1) & 0x3FF
        nxt = [i + 1] if i + 1 < n else []
        if op in (16, 18):
            t = dfn._branch_target(w, address + 4 * i)
            target = (t - address) // 4 if address <= t < address + size and (t - address) % 4 == 0 else None
            always = op == 18 or ((w >> 21) & 0x14) == 0x14
            if lk:
                e = [] if (always and t in no_return) else nxt
            else:
                e = ([target] if target is not None else []) + ([] if always else nxt)
            edges.append(e)
            if target is not None:
                leaders.add(target)
            if i + 1 < n:
                leaders.add(i + 1)
        elif op == 19 and xo in (16, 50, 528):
            always = xo == 50 or ((w >> 21) & 0x14) == 0x14
            edges.append(nxt if (lk or not always) else [])
            jumps = jumps or (xo == 528 and not lk)
            if i + 1 < n:
                leaders.add(i + 1)
        else:
            edges.append(nxt)
    order = sorted(leaders)
    to_addr = lambda i: address + 4 * i  # noqa: E731
    succ: dict[int, list[int]] = {}
    pred: dict[int, list[int]] = defaultdict(list)
    ends: dict[int, int] = {}
    for k, start in enumerate(order):
        stop = order[k + 1] if k + 1 < len(order) else n
        ends[to_addr(start)] = to_addr(stop - 1)
        out = [j for j in edges[stop - 1]]
        succ[to_addr(start)] = [to_addr(j) for j in out]
        for j in out:
            pred[to_addr(j)].append(to_addr(start))
    return Cfg([to_addr(i) for i in order], succ, pred, ends, jumps)


def dominators(cfg: Cfg, entry: int) -> dict[int, set[int]]:
    """Dominators of the blocks reachable from `entry` (iterative data flow)."""
    reach = {entry}
    todo = [entry]
    while todo:
        for s in cfg.succ[todo.pop()]:
            if s not in reach:
                reach.add(s)
                todo.append(s)
    dom = {b: set(reach) for b in reach}
    dom[entry] = {entry}
    changed = True
    while changed:
        changed = False
        for b in cfg.starts:
            if b == entry or b not in reach:
                continue
            ps = [p for p in cfg.pred[b] if p in reach]
            new = set.intersection(*(dom[p] for p in ps)) | {b} if ps else {b}
            if new != dom[b]:
                dom[b] = new
                changed = True
    return dom


def covered_blocks(cfg: Cfg, entry: int, seen_at: set[int]) -> set[int]:
    """Blocks known to have run in a function entered at least once: its entry, the blocks of `seen_at` (instructions
    known to have run), their dominators, then the single successor of each of those. A lower bound. The dominators
    are skipped in a function with a bctr: a block has predecessors that are not known there, so a dominator computed
    without them proves nothing."""
    known = {entry} | {cfg.block_of(a) for a in seen_at}
    if not cfg.jumps:
        dom = dominators(cfg, entry)
        for b in list(known):
            known |= dom.get(b, set())
    todo = list(known)
    while todo:
        b = todo.pop()
        s = cfg.succ[b]
        if len(s) == 1 and s[0] not in known:
            known.add(s[0])
            todo.append(s[0])
    return known


COND = {(12, 0): "blt", (12, 1): "bgt", (12, 2): "beq", (12, 3): "bso",
        (4, 0): "bge", (4, 1): "ble", (4, 2): "bne", (4, 3): "bns"}


def branch_name(word: int) -> str:
    """Short name of the conditional branch `word`; "bc" for the forms that use CTR or another BO."""
    bo, bi = (word >> 21) & 0x1F, (word >> 16) & 0x1F
    name = COND.get((bo & 0x1C, bi & 3))
    if name is None:
        return "bc"
    return name if bi < 4 else f"{name} cr{bi // 4}"


def find_gate(code: list[tuple[int, bytes]], cfg: Cfg, target_block: int, known: set[int]) -> dict:
    """Closest place where a run that entered the function leaves what is known to have run on the way to
    `target_block`: walking back over the predecessors, the first block of `known`. status "gate": `at` is its last
    instruction, `to` the block it must go to, `between` the blocks after that one that have not been seen to run.
    status "contradiction": the target block itself is known to have run. status "unknown": no known block leads there
    (it sits behind a jump table, or after an instruction whose targets are not followed)."""
    if target_block in known:
        return {"status": "contradiction"}
    seen = {target_block}
    queue = deque([(target_block, 0)])
    while queue:
        b, depth = queue.popleft()
        for p in sorted(cfg.pred[b]):
            if p in seen:
                continue
            seen.add(p)
            if p in known:
                at = cfg.ends[p]
                base, blob = dfn._code_of(code, at)
                word = dfn._word(blob, at - base)
                return {"status": "gate", "at": at, "to": b, "between": depth,
                        "branch": branch_name(word) if word >> 26 == 16 else "other"}
            queue.append((p, depth + 1))
    return {"status": "unknown"}


# ---------------------------------------------------------------------------------------------
# Coverage probe


@dataclass
class Coverage:
    """Union of the runs of one version. `reached`: function entries seen; `lr`: return addresses of those calls;
    `hooks`: addresses the probe was compiled on, so that a function outside them is not measured (it may still appear
    in `reached`, when the dispatcher offered it)."""
    runs: list[str] = field(default_factory=list)
    reached: dict[int, int] = field(default_factory=dict)         # address -> calls
    lr: dict[int, set[int]] = field(default_factory=dict)         # address -> return addresses
    lr_full: set[int] = field(default_factory=set)                # the probe capped the list of this address
    hooks: set[int] = field(default_factory=set)
    warnings: list[str] = field(default_factory=list)


# The probe's file has no end marker: a cut cannot be read from its last line. It can be read from its structure instead.
# Each "A" line opens a record and declares what follows it: `sites` "C" lines and min(`states`, COV_TUPLES) "S" lines. The
# records come out in the order of the probe's table, so the last one sits near the end of that table.
COV_HEADER = "# cov probe v2"
COV_FIELDS = {"A": 14, "S": 18, "C": 3}
COV_TUPLES = 256           # TUPLE_CAP of the probe
COV_SLOTS = 1 << 15        # SLOTS of the probe
COV_HASH = 2654435761      # the probe's first slot for an address: ((address >> 2) * COV_HASH) & (COV_SLOTS - 1)
COV_LAST_SLOT_ODDS = 1e-6  # below this chance of seeing such a gap at the end of the table in a whole file: it was cut

# Share of the function entries of a run that must be in the probe list of its version. Measured on 16 real runs, 8 by
# version: 85.2 to 91.4 % in its own list, 0.9 to 1.1 % in the other version's list. The thresholds are a hypothesis:
# those runs are spent mostly in the DOL, and a run spent mostly in the RELs may lower the share (a loud refusal).
PROBE_SHARE_REFUSED = 50.0
PROBE_SHARE_WARNED = 80.0


def read_hooks(path: Path) -> set[int]:
    """Addresses the probe was compiled on. Empty is refused: everything would read as not measured."""
    try:
        hooks = {int(x, 16) for x in path.read_text(encoding="utf-8", errors="replace").split()}
    except ValueError as e:
        raise GraphError(f"{path}: not a list of probe addresses ({e})") from None
    if not hooks:
        raise GraphError(f"{path}: empty list of probe addresses: nothing could be measured")
    return hooks


def probe_share(entries: list[int], hooks: set[int]) -> float:
    """Percentage of `entries` that are in `hooks`."""
    return 100.0 * sum(1 for a in entries if a in hooks) / len(entries)


def read_cov(path: Path, cov: Coverage, name: str) -> list[int]:
    """Adds one run (cov.tsv of the coverage probe: "A addr calls ... states states_full sites sites_full", "S addr
    registers", "C addr site") and returns the function entries it saw. A line with 0 calls is no entry (the probe writes
    none). A file that is not whole is refused, see above. What this cannot see: a cut exactly between two records that
    loses less than about 1 % of the table, and whole records removed from the middle of the file."""
    text = path.read_text(encoding="utf-8", errors="replace")
    first = text.split("\n", 1)[0]
    if not first.startswith(COV_HEADER):
        raise GraphError(f"{path}: not a file of the coverage probe: its first line is not \"{COV_HEADER} ...\"")
    if "table full" in first:
        cov.warnings.append(f"{name}: the probe's table was full, some addresses are missing from this run")
    if not text.endswith("\n"):
        raise GraphError(f"{path}: cut, it ends in the middle of a line")
    entries: list[int] = []
    records = zero = 0
    last = -1                                          # address of the record being read
    label, calls, states_left, sites_left = "", 0, 0, 0

    def close() -> None:
        if states_left or sites_left:
            raise GraphError(f"{path}: cut or damaged, function {label} misses {states_left} S and {sites_left} C lines "
                             "of the ones it declares (negative: too many)")

    for no, line in enumerate(text.splitlines(), 1):
        p = line.split()
        if not p or p[0] == "#":
            continue
        try:
            if COV_FIELDS[p[0]] != len(p):
                raise ValueError
            if p[0] == "A":
                close()
                last, label = int(p[1], 16), p[1]
                calls = int(p[2])
                states_left, sites_left = min(int(p[10]), COV_TUPLES), int(p[12])
                records += 1
                if calls:
                    cov.reached[last] = cov.reached.get(last, 0) + calls
                    entries.append(last)
                    if p[13] == "1":
                        cov.lr_full.add(last)
                else:
                    zero += 1
            elif p[1] != label:
                raise ValueError
            elif p[0] == "S":
                states_left -= 1
            else:
                sites_left -= 1
                if calls:
                    cov.lr.setdefault(last, set()).add(int(p[2], 16))
        except (KeyError, ValueError):
            raise GraphError(f"{path}:{no}: cut or damaged, unexpected line: {line[:60]}") from None
    close()
    if not entries:
        raise GraphError(f"{path}: no function entry in this coverage file")
    if zero:
        cov.warnings.append(f"{name}: {zero} function entries with 0 calls ignored, the probe writes none")
    gap = COV_SLOTS - 1 - (((last >> 2) * COV_HASH) & (COV_SLOTS - 1))
    if (1 - gap / COV_SLOTS) ** records < COV_LAST_SLOT_ODDS:
        raise GraphError(f"{path}: cut, its {records} records stop {gap} slots before the end of the probe's table")
    cov.runs.append(name)
    return entries


def expand_runs(patterns: list[str], root: Path) -> list[Path]:
    """Files named by the `patterns` (glob, relative to `root`), each pattern giving at least one: a pattern that gives
    nothing is an error, it would silently turn every function of that run into a function never reached."""
    out: list[Path] = []
    for p in patterns:
        hits = sorted(glob.glob(str(root / p)))
        if not hits:
            raise GraphError(f"coverage: nothing matches {p} under {root}")
        out += [Path(h) for h in hits]
    return out


def seen_instructions(graph: Graph, cov: Coverage, code: list[tuple[int, bytes]]) -> dict[int, set[int]]:
    """Function start -> call instructions known to have run in it: lr - 4 of every recorded call, when that word is a
    call. In a tail-called function lr is the return address of an earlier call: that call ran as well."""
    out: dict[int, set[int]] = defaultdict(set)
    for addrs in cov.lr.values():
        for lr in addrs:
            at = lr - 4
            base, blob = dfn._code_of(code, at)
            if blob is None or at < base:
                continue
            w = dfn._word(blob, at - base)
            op, lk, xo = w >> 26, w & 1, (w >> 1) & 0x3FF
            if lk and (op in (16, 18) or (op == 19 and xo in (16, 528))):
                o = graph.owner(at)
                if o is not None:
                    out[o].add(at)
    return out


# ---------------------------------------------------------------------------------------------
# Searches


def shortest_from(graph: Graph, sources: list[int], skip: set[int] | None = None,
                  extra: dict[int, list[Site]] | None = None) -> dict[int, tuple[int, Site | None, int | None]]:
    """Breadth-first search over the direct edges from all `sources` (distance 0): {function: (distance, the site of the
    edge used, the caller)}. Functions and sites are visited in address order: the result does not depend on anything
    else. `skip`: functions not entered. `extra`: more steps by function (Graph.pointer_sites)."""
    skip = skip or set()
    extra = extra or {}
    out: dict[int, tuple[int, Site | None, int | None]] = {s: (0, None, None) for s in sorted(sources)}
    queue = deque(sorted(sources))
    while queue:
        a = queue.popleft()
        d = out[a][0]
        for s in sorted(graph.nodes[a].sites + extra.get(a, []), key=lambda x: x.at):
            c = s.callee
            if c is not None and c not in out and c not in skip:
                out[c] = (d + 1, s, a)
                queue.append(c)
    return out


def callers_within(graph: Graph, target: int, depth: int) -> dict[int, int]:
    """Functions that reach `target` in fewer than `depth` direct edges: {function: edges}, `target` itself at 0. The
    only functions through which a shorter way in could exist."""
    out = {target: 0}
    queue = deque([target])
    while queue:
        a = queue.popleft()
        d = out[a]
        if d + 1 >= depth:
            continue
        for c, _s in graph.callers.get(a, ()):
            if c not in out:
                out[c] = d + 1
                queue.append(c)
    return out


def all_callers(graph: Graph, target: int) -> set[int]:
    """Every function with a direct path to `target`, `target` included."""
    seen = {target}
    todo = [target]
    while todo:
        for c, _s in graph.callers.get(todo.pop(), ()):
            if c not in seen:
                seen.add(c)
                todo.append(c)
    return seen
