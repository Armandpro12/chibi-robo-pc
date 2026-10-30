"""Cost by path of decompiled functions (generic; no game here): graph of an original's conditional branches with the
guest cycles DolRecomp charges along each edge.

Why: on the cycle clock a decompiled function is charged the guest cycles its recompiled original would have spent
(runtime/core/decomp/gc_abi.h, "Guest time"). When that cost depends on the original's path, a mean is exact only on
average; runtime/core/decomp/gc_path.h charges instead the cycles of the path the decompiled code marks. This tool
makes the graph that walk follows, from two sources and never by hand:

  * the original's instructions (its words in the disc image): the conditional branches and where each path goes;
  * the code DolRecomp generated for it (the recompiler folder of the build): the charge of each cost group, the
    `ctx->downcount -= N;` that follows the `label_<address>:` of the group's head. Cycles are charged only there
    (DolRecomp src/backend/emitter.c, emit_group_charge), so the cycles of a path are the sum of the charges of the
    instructions it runs.

Graph: an entry edge, then one node per conditional branch reached (bc, bclr), each with a "taken" and a "not taken"
edge: the cycles charged until the next conditional branch, and that branch's index or RETURN (blr, a taken bclr, a
tail branch out of the function). Unconditional branches inside the function are followed; a call (bl, bctrl) goes
on to the next instruction (the callee is charged where it runs: the cost is exclusive, as the oracle measures it).
Refused (ValueError): an indirect jump (bcctr without link), a conditional call, rfi, a straight-line loop, a path
that runs off the end of the function, a function whose loops DolRecomp outlined (`loop_<address>`, not read here),
an instruction absent from the generated code.

Call sites (issue #68): the graph also lists the original's reachable call sites (bl, bcl, bcctrl, and the branches
out of the function: tail calls), each with its target (0 for bcctrl), the cycles charged after it until the next
conditional branch or the return (`after`, 0 for a tail call) and that branch (`next`). The gateway charges, before
a guest call of the decompiled code, the walk's cycles minus `after`: the original's cycles up to the call instruction
(runtime/core/decomp/gc_path.h). A call site whose continuation cannot be followed (it runs off the function: a call
that does not return at its end) has no entry, and a call there has no known prefix.

Usage (diagnostic): py -3 tools/decomp_path_cost.py <game> --version <ID> [--function NAME ...]
prints, for each enabled function of the game's table, its constant cost or its graph (path-dependent).
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

RETURN = 0xFFFFFFFF  # gc_path.h path_return

LABEL = re.compile(r"^label_([0-9A-F]{8}):")
CHARGE = re.compile(r"ctx->downcount -= (\d+);")
COMMENT = re.compile(r"^\s*// ([0-9A-F]{8}): ")
OUTLINED_LOOP = re.compile(r"^static void loop_([0-9A-F]{8})\(")
CHUNK_BASE = re.compile(r"_([0-9A-F]{8})\.c$")


@dataclass
class Edge:
    cycles: int
    next: int  # node index or RETURN


@dataclass
class Node:
    address: int
    taken: Edge
    not_taken: Edge


@dataclass
class Call:
    address: int
    target: int  # 0: indirect (bcctrl)
    after: int   # cycles charged after the call instruction until `next`
    next: int    # node index or RETURN


@dataclass
class Graph:
    address: int
    entry: Edge
    nodes: list[Node] = field(default_factory=list)
    calls: list[Call] = field(default_factory=list)
    call_errors: list[str] = field(default_factory=list)  # call sites without entry (continuation not followed)

    def has_cycle(self) -> bool:
        state = [0] * len(self.nodes)  # 0 unseen, 1 on the stack, 2 done

        def visit(i: int) -> bool:
            state[i] = 1
            for e in (self.nodes[i].taken, self.nodes[i].not_taken):
                if e.next != RETURN and (state[e.next] == 1 or (state[e.next] == 0 and visit(e.next))):
                    return True
            state[i] = 2
            return False

        return any(state[i] == 0 and visit(i) for i in range(len(self.nodes)))

    def totals(self) -> set[int]:
        """Distinct costs of the paths from the entry to a return (acyclic graph only)."""
        memo: dict[int, set[int]] = {}

        def from_node(i: int) -> set[int]:
            if i == RETURN:
                return {0}
            if i not in memo:
                memo[i] = {e.cycles + t for e in (self.nodes[i].taken, self.nodes[i].not_taken)
                           for t in from_node(e.next)}
            return memo[i]

        return {self.entry.cycles + t for t in from_node(self.entry.next)}

    def constant(self) -> int | None:
        """The cost of every path when they all cost the same (no loop, one total), else None (path-dependent)."""
        if self.has_cycle():
            return None
        t = self.totals()
        return next(iter(t)) if len(t) == 1 else None

    def walk(self, outcomes: list[bool]) -> int | None:
        """Cycles of the path given by the outcomes of its conditional branches, None if they do not end on a return
        (the walk of gc_path.h, for tests)."""
        cycles, node = self.entry.cycles, self.entry.next
        for taken in outcomes:
            if node == RETURN:
                return None
            e = self.nodes[node].taken if taken else self.nodes[node].not_taken
            cycles += e.cycles
            node = e.next
        return cycles if node == RETURN else None

    def prefix(self, outcomes: list[bool], call: int) -> int | None:
        """Cycles charged before the guest call at the instruction `call`, the outcomes of the conditional branches
        before it being `outcomes` (gc_abi.cpp call_prefix, for tests); None if no call site of the walk's edge."""
        cycles, node = self.entry.cycles, self.entry.next
        for taken in outcomes:
            if node == RETURN:
                return None
            e = self.nodes[node].taken if taken else self.nodes[node].not_taken
            cycles += e.cycles
            node = e.next
        for c in self.calls:
            if c.address == call and c.next == node:
                return cycles - c.after
        return None


def _signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def _target(word: int, at: int) -> int:
    if word >> 26 == 18:
        off = _signed(word & 0x03FFFFFC, 26)
    else:
        off = _signed(word & 0xFFFC, 16)
    return (off & 0xFFFFFFFF) if (word & 2) else (at + off) & 0xFFFFFFFF


def build_graph(words: list[int], address: int, charges: dict[int, int]) -> Graph:
    """Graph of the function whose instructions are `words` (starting at `address`), with the cycle charges of its
    cost-group heads (`charges`: {instruction address: N}). ValueError if the code is not supported (module doc)."""
    n = len(words)
    end = address + 4 * n

    def inside(t: int) -> bool:
        return address <= t < end and (t - address) % 4 == 0

    # Kind of each instruction: ("go", index) straight or unconditional branch; ("ret",) end of the path;
    # ("cond", taken target index or None for a return or a tail branch).
    kinds: list[tuple] = []
    for i, w in enumerate(words):
        at = address + 4 * i
        op, lk = w >> 26, w & 1
        always = ((w >> 21) & 0x14) == 0x14
        if op == 18:
            t = _target(w, at)
            if lk:
                kinds.append(("go", i + 1))
            elif inside(t):
                kinds.append(("go", (t - address) // 4))
            else:
                kinds.append(("ret",))  # tail branch to another function
        elif op == 16:
            t = _target(w, at)
            if lk:
                if not always:
                    raise ValueError(f"{at:08X}: conditional call (bcl) not supported")
                kinds.append(("go", i + 1))
            elif always:
                kinds.append(("go", (t - address) // 4) if inside(t) else ("ret",))
            else:
                kinds.append(("cond", (t - address) // 4 if inside(t) else None))
        elif op == 19 and ((w >> 1) & 0x3FF) == 16:  # bclr
            if lk:
                raise ValueError(f"{at:08X}: bclrl not supported")
            kinds.append(("ret",) if always else ("cond", None))
        elif op == 19 and ((w >> 1) & 0x3FF) == 528:  # bcctr
            if not lk:
                raise ValueError(f"{at:08X}: indirect jump (bcctr) not supported")
            if not always:
                raise ValueError(f"{at:08X}: conditional call (bcctrl) not supported")
            kinds.append(("go", i + 1))
        elif op == 19 and ((w >> 1) & 0x3FF) == 50:
            raise ValueError(f"{at:08X}: rfi not supported")
        else:
            kinds.append(("go", i + 1))

    graph = Graph(address, Edge(0, RETURN))
    index: dict[int, int] = {}
    todo: list[int] = []
    reached: set[int] = set()  # instructions on some path from the entry

    def node_of(i: int) -> int:
        if i not in index:
            index[i] = len(graph.nodes)
            graph.nodes.append(Node(address + 4 * i, Edge(0, RETURN), Edge(0, RETURN)))
            todo.append(i)
        return index[i]

    def follow(i: int) -> Edge:
        cycles, seen = 0, set()
        while True:
            if not 0 <= i < n:
                raise ValueError(f"{address:08X}: a path runs off the end of the function")
            if i in seen:
                raise ValueError(f"{address + 4 * i:08X}: loop without a conditional branch")
            seen.add(i)
            reached.add(i)
            cycles += charges.get(address + 4 * i, 0)
            k = kinds[i]
            if k[0] == "go":
                i = k[1]
            elif k[0] == "ret":
                return Edge(cycles, RETURN)
            else:
                return Edge(cycles, node_of(i))

    graph.entry = follow(0)
    while todo:
        i = todo.pop()
        node = graph.nodes[index[i]]
        target = kinds[i][1]
        node.taken = follow(target) if target is not None else Edge(0, RETURN)
        node.not_taken = follow(i + 1)
    # Call sites of the reachable code (module doc). Every instruction after a reachable call is reachable, and so is
    # the conditional branch its continuation reaches: following it adds no node.
    nodes_before = len(graph.nodes)
    for i in sorted(reached):
        w = words[i]
        at = address + 4 * i
        op, lk = w >> 26, w & 1
        always = ((w >> 21) & 0x14) == 0x14
        xo = (w >> 1) & 0x3FF
        if op == 18 or op == 16:
            t = _target(w, at)
            if not lk and inside(t):
                continue  # a branch inside the function
            if not lk:
                # A branch out of the function (tail call), conditional or not: the path ends there.
                graph.calls.append(Call(at, t, 0, RETURN))
                continue
            target = t
        elif op == 19 and xo == 528 and lk:
            target = 0
        else:
            continue
        try:
            e = follow(i + 1)
        except ValueError as exc:
            graph.call_errors.append(f"{at:08X}: {exc}")
            continue
        graph.calls.append(Call(at, target, e.cycles, e.next))
    if len(graph.nodes) != nodes_before:
        raise ValueError(f"{address:08X}: a call's continuation reached an unknown conditional branch")
    return graph


class GeneratedCode:
    """Cost-group charges of the code DolRecomp generated in a recompiler folder (tools/build.py recomp_dir):
    `charges(address, size)` -> {head address: N} for [address, address + size)."""

    def __init__(self, recomp_dir: Path):
        self.files = sorted(recomp_dir.rglob("*.c"))
        if not self.files:
            raise ValueError(f"{recomp_dir}: no generated code")
        self._parsed: dict[Path, tuple[dict[int, int], set[int], set[int]]] = {}

    def _parse(self, path: Path) -> tuple[dict[int, int], set[int], set[int]]:
        if path not in self._parsed:
            charges: dict[int, int] = {}
            seen: set[int] = set()
            outlined: set[int] = set()
            cur = None
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                m = LABEL.match(line)
                if m:
                    cur = int(m.group(1), 16)
                    continue
                m = OUTLINED_LOOP.match(line)
                if m:
                    outlined.add(int(m.group(1), 16))
                m = COMMENT.match(line)
                if m:
                    seen.add(int(m.group(1), 16))
                    cur = None  # the head's charge comes before its first instruction
                    continue
                if cur is not None:
                    c = CHARGE.search(line)
                    if c:
                        if charges.get(cur, int(c.group(1))) != int(c.group(1)):
                            raise ValueError(f"{path.name}: two charges at {cur:08X}")
                        charges[cur] = int(c.group(1))
                        cur = None
            self._parsed[path] = (charges, seen, outlined)
        return self._parsed[path]

    def _candidates(self, address: int) -> list[Path]:
        based = []
        for f in self.files:
            m = CHUNK_BASE.search(f.name)
            if m and int(m.group(1), 16) <= address:
                based.append((int(m.group(1), 16), f))
        based.sort(reverse=True)
        return [f for _, f in based[:2]] + [f for f in self.files if f not in {g for _, g in based[:2]}]

    def charges(self, address: int, size: int) -> dict[int, int]:
        wanted = set(range(address, address + size, 4))
        for f in self._candidates(address):
            charges, seen, outlined = self._parse(f)
            if address not in seen:
                continue
            missing = wanted - seen
            if missing:
                raise ValueError(f"{address:08X}: instructions {min(missing):08X}... not in {f.name}")
            loops = sorted(a for a in outlined if a in wanted)
            if loops:
                raise ValueError(f"{address:08X}: DolRecomp outlined a loop at {loops[0]:08X} (loop_ functions are not "
                                 f"read by this tool)")
            return {a: c for a, c in charges.items() if a in wanted}
        raise ValueError(f"{address:08X}: not found in the generated code")


def graphs_for(recomp_dir: Path, words: dict[int, list[int]],
               errors: dict[int, str] | None = None) -> dict[int, Graph]:
    """{guest address: Graph} of the functions `words` ({address: instruction words}) of one build. A function this
    tool cannot read (ValueError: module doc) has no graph; its reason goes into `errors` if given, else it raises."""
    code = GeneratedCode(recomp_dir)
    out: dict[int, Graph] = {}
    for a, w in words.items():
        try:
            out[a] = build_graph(w, a, code.charges(a, 4 * len(w)))
        except ValueError as e:
            if errors is None:
                raise
            errors[a] = str(e)
    return out


def registry_lines(graphs: dict[int, Graph], wanted: list[int]) -> tuple[list[str], dict[int, str]]:
    """C++ lines of the path graphs of `wanted` (path-dependent functions) for the registration table, and {address:
    name of the graph} (registration::path)."""
    lines: list[str] = []
    names: dict[int, str] = {}

    def edge(e: Edge) -> str:
        nxt = "::gc::abi::path_return" if e.next == RETURN else f"{e.next}u"
        return f"{{{e.cycles}u, {nxt}}}"

    for a in wanted:
        g = graphs[a]
        name = f"k_path_{a:08X}"
        nodes = calls = "nullptr"  # no zero-length array in C++
        if g.nodes:
            lines.append(f"const ::gc::abi::path_node {name}_nodes[] = {{")
            for nd in g.nodes:
                lines.append(f"    {{0x{nd.address:08X}u, {edge(nd.taken)}, {edge(nd.not_taken)}}},")
            lines.append("};")
            nodes = f"{name}_nodes"
        if g.calls:
            lines.append(f"const ::gc::abi::path_call {name}_calls[] = {{")
            for c in g.calls:
                nxt = "::gc::abi::path_return" if c.next == RETURN else f"{c.next}u"
                lines.append(f"    {{0x{c.address:08X}u, 0x{c.target:08X}u, {c.after}u, {nxt}}},")
            lines.append("};")
            calls = f"{name}_calls"
        lines.append(f"const ::gc::abi::path_graph {name} = {{{edge(g.entry)}, {len(g.nodes)}u, {nodes}, "
                     f"{len(g.calls)}u, {calls}}};")
        names[a] = name
    return lines, names


def describe(name: str, g: Graph) -> str:
    calls = "".join(f"\n  call {c.address:08X} -> {c.target:08X} after {c.after}->{'R' if c.next == RETURN else c.next}"
                    for c in g.calls) + "".join(f"\n  call without prefix {e}" for e in g.call_errors)
    c = g.constant()
    if c is not None:
        return f"{name} {g.address:08X} constant {c}" + calls
    out = [f"{name} {g.address:08X} path-dependent{' (loop)' if g.has_cycle() else ''}"
           + ("" if g.has_cycle() else f" {min(g.totals())}..{max(g.totals())}")
           + f" entry {g.entry.cycles}->{'R' if g.entry.next == RETURN else g.entry.next}"]
    for i, nd in enumerate(g.nodes):
        def e(x: Edge) -> str:
            return f"{x.cycles}->{'R' if x.next == RETURN else x.next}"
        out.append(f"  {i}: {nd.address:08X} taken {e(nd.taken)} not {e(nd.not_taken)}")
    return "\n".join(out) + calls


def main() -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import decomp_functions  # noqa: E402
    import toolchain as tc  # noqa: E402
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("game")
    p.add_argument("--version", required=True)
    p.add_argument("--function", action="append", default=[], help="only these names (repeatable)")
    p.add_argument("--recomp-dir", help="recompiler folder (default: generated/<game>/<version>/recomp)")
    args = p.parse_args()
    import json
    game_cfg = tc.load_game(args.game)
    manifest = tc.GENERATED / args.game / args.version / "manifest.json"
    if not manifest.exists():
        raise SystemExit(f"{manifest} missing: extract the disc first (tools/extract.py)")
    disc_root = Path(json.loads(manifest.read_text(encoding="utf-8"))["disc_root"])
    words: dict[int, list[int]] = {}
    entries = decomp_functions.resolve(game_cfg, args.version, disc_root,
                                       decomp_functions._layout(game_cfg, disc_root), words=words)
    recomp = Path(args.recomp_dir) if args.recomp_dir else tc.GENERATED / args.game / args.version / "recomp"
    errors: dict[int, str] = {}
    graphs = graphs_for(recomp, words, errors)
    for a, f in entries:
        if args.function and f.name not in args.function:
            continue
        print(describe(f.name, graphs[a]) if a in graphs else f"{f.name} {a:08X} no graph: {errors[a]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
