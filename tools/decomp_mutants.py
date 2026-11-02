"""Mutation testing of decompiled code (tools/decomp_oracle.py mutate): do the oracle's cases catch plausible errors?

The oracle's promotion rule is block coverage: it says every block ran, not that the cases tell a right version from a
wrong one. This tool makes small deliberate errors in the decompiled sources (mutants) and replays each one offline on
the cases the oracle captured. A case that fails kills the mutant; a mutant no case kills is a question about the cases.
The score blocks nothing; what matters is that every survivor is sorted (below).

Mutants never leave the build folder: each is a temporary copy of one source file (and its sibling headers) under
build/, compiled alone and linked with the unchanged objects of the replay. Nothing here writes to the decompiled
sources, the production build or the oracle build; --out outside build/, or build/ itself, is refused.

Operators (OPERATORS, on the functions of the table, their helpers in the same file, the constants and field types
they use): invert_branch (a condition, each operand of || and &&, == and != outside conditions), relational (< and <=,
> and >=), constant (a literal +1, -1), removed_write (a store or a local update becomes an empty statement),
swapped_writes (two consecutive stores in one block exchange places), write_before_read (the shape of a real bug of
this project, anomaly A19: a store moved above a load that follows it, or a temporary inlined so that a struct copy
reads while it writes; defined after that bug was known, so its results on that bug prove nothing), signedness (s8/u8,
s16/u16, s32/u32 in the signature, the body, or the type of a field the function uses).

Outcome of a mutant, per version:
  invalid     it does not compile (dropped);
  equivalent  same machine code as the unchanged source, proven by comparing the object's code sections (so no replay).
              Sufficient, not necessary: a mutant that behaves the same can still compile differently (a load moved past
              an address check, one more host read seen by GC_MEM_OBSERVER); it then survives and is sorted by hand;
  killed      a case shows a difference in the compared results while staying inside the bytes the case gives;
  weak        every failing case read outside the bytes the case gives, or the mutant crashed or hung: the replay has
              no value for those bytes (it reads 0), so this is not a difference proven against the original (it is
              not a survivor either; the in-game oracle settles it);
  survived    every case passes;
  unknown     only visible through cr (the replay keeps the entry cr: runtime/core/decomp/oracle_replay.cpp), to be
              settled by the in-game oracle;
  no_cases    the function has no replayable case (non-leaf cases are not replayable offline).
Replay blind spots beyond cr: path marks are plain conditions offline (no cost walk), and FPSCR, xer, ctr are compared
by neither oracle. The replay here installs the version's address table of the game globals (generated source
replay_globals.cpp, under the build folder), which tools/decomp_oracle.py replay does not.

Sorting survivors: --triage FILE (default games/<game>/decomp/oracle_mutants/triage.tsv), tab-separated
  <mutant id> <verdict> <reason>
verdict: equivalent (no input can tell it from the source: the reason says why), hypothesis (not proven either way,
the reason says what would settle it), case:<synthetic label> (the case added to kill it), gap (a real gap, no case
yet). The ids are stable while the mutated text does not change. Survivors with no line are listed as untriaged.

Prediction: a run on a batch (--functions) expects --prediction FILE, a note written before the run that holds a line
  mutants-prediction: survivors=<n> gaps=<n>
Without it the tool only warns (it never blocks); with it the output compares the figures. Real gaps count the survivors
sorted gap, case:<label> or hypothesis: those that need a case, or stay unproven for want of one.

Generic: no game here, everything comes from the functions table, the sources and the captured cases.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402

OPERATORS = ("invert_branch", "relational", "constant", "removed_write", "swapped_writes", "write_before_read",
             "signedness")
SIGNED = {"s8": "u8", "u8": "s8", "s16": "u16", "u16": "s16", "s32": "u32", "u32": "s32"}
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "catch", "else", "do"}
ASSIGNMENTS = ("<<=", ">>=", "|=", "&=", "^=", "+=", "-=", "*=", "/=", "%=", "=")
LITERAL = r"(?:0[xX][0-9a-fA-F]+|\d+\.\d*(?:[eE][+-]?\d+)?|\.\d+|\d+)[uUlLfF]*"
REPLAY_TIMEOUT_SECONDS = 20
PREDICTION = re.compile(r"^\W*mutants-prediction:\s*survivors=(\d+)\s+gaps=(\d+)\s*$", re.M)
GAP_VERDICTS = ("gap", "case", "hypothesis")


# --- parsing the sources (no compiler: comments and strings blanked, then braces, parentheses, statements) ------------

def blank(text: str) -> str:
    """`text` with comments, strings, character literals and preprocessor lines replaced by spaces (newlines kept), so
    every offset is the same in both."""
    out = list(text)
    i, n = 0, len(text)

    def fill(a: int, b: int) -> None:
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    line_start = True
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            fill(i, j)
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            fill(i, j)
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            fill(i, j + 1)
            i = j + 1
        elif c == "#" and line_start:
            j = i
            while True:
                j = text.find("\n", j)
                if j < 0:
                    j = n
                    break
                if text[j - 1] != "\\" and text[j - 2:j] != "\\\r":
                    break
                j += 1
            fill(i, j)
            i = j
        else:
            if c == "\n":
                line_start = True
            elif not c.isspace():
                line_start = False
            i += 1
    return "".join(out)


def match(code: str, i: int, opening: str, closing: str) -> int:
    """Offset of the bracket that closes the one at `i`."""
    depth = 0
    for k in range(i, len(code)):
        if code[k] == opening:
            depth += 1
        elif code[k] == closing:
            depth -= 1
            if depth == 0:
                return k
    raise ValueError(f"unmatched {opening} at offset {i}")


@dataclass
class Function:
    name: str
    start: int  # first character of the declaration
    open: int   # offset of the body's {
    close: int  # offset of the body's }


def functions_in(code: str) -> list[Function]:
    """Functions defined at namespace level of blanked source `code`."""
    out: list[Function] = []
    i, boundary, n = 0, 0, len(code)
    while i < n:
        c = code[i]
        if c == "{":
            head = code[boundary:i]
            if re.search(r"\bnamespace\b|\bextern\s*$", head):
                boundary = i + 1
                i += 1
                continue
            close = match(code, i, "{", "}")
            paren = re.search(r"([A-Za-z_]\w*)\s*\(", head)
            if paren and paren.group(1) not in KEYWORDS and "=" not in head[:paren.start()] \
                    and not re.search(r"\b(struct|class|union|enum)\b", head):
                start = boundary + len(head) - len(head.lstrip())
                out.append(Function(paren.group(1), start, i, close))
            i = close + 1
            boundary = i
            continue
        if c in ";}":
            boundary = i + 1
        i += 1
    return out


@dataclass
class Statement:
    start: int
    end: int    # past the ;
    kind: str   # write, local, decl, other
    block: int  # statements of one block share this number
    index: int  # position in its block


@dataclass
class Body:
    statements: list[Statement] = field(default_factory=list)
    conditions: list[tuple[int, int]] = field(default_factory=list)  # (start, end) of each condition


def top_level_assignment(text: str) -> tuple[int, str] | None:
    """(offset, operator) of the assignment of statement `text`, outside any bracket, with spaces around it."""
    depth = 0
    i = 0
    while i < len(text):
        c = text[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and c == " ":
            for op in ASSIGNMENTS:
                if text.startswith(f" {op} ", i):
                    return i, op
        i += 1
    return None


def classify(text: str) -> str:
    """Kind of statement `text` (blanked): a store to guest memory, an update of a local, a declaration, or other."""
    s = " ".join(text.rstrip(";").split())
    if re.match(r"(?:gc::)?store(?:_raw)?\(", s):
        return "write"
    if re.fullmatch(r"(\+\+|--)[\w.\->\[\]]+|[\w.\->\[\]]+(\+\+|--)", s):
        return "local" if re.fullmatch(r"(\+\+|--)?\w+(\+\+|--)?", s) else "write"
    found = top_level_assignment(s)
    if found:
        lhs = s[:found[0]].strip()
        plain = lhs
        while re.search(r"<[^<>]*>", plain):
            plain = re.sub(r"<[^<>]*>", "", plain)
        if found[1] == "=" and re.fullmatch(r"(?:(?:const|constexpr|static)\s+)*[\w:]+[\s*&]+\w+(\[.*\])?", plain):
            return "decl"
        return "local" if re.fullmatch(r"\w+", lhs) else "write"
    plain = s
    while re.search(r"<[^<>]*>", plain):
        plain = re.sub(r"<[^<>]*>", "", plain)
    if re.fullmatch(r"(?:(?:const|constexpr|static)\s+)*[\w:]+[\s*&]+\w+(\{.*\}|\(.*\)|\[.*\])?", plain) \
            and not re.match(r"(return|goto)\b", plain):
        return "decl"
    return "other"


def parse_body(code: str, open_brace: int, close_brace: int) -> Body:
    """Statements and conditions of the function body between the two offsets of blanked source `code`."""
    body = Body()
    counter = [0]

    def new_block() -> int:
        counter[0] += 1
        return counter[0]

    def skip(i: int) -> int:
        while i < close_brace and code[i].isspace():
            i += 1
        return i

    def keyword(i: int) -> str | None:
        m = re.compile(r"(if|for|while|switch|else|do|case|default|return|break|continue|goto)\b").match(code, i)
        return m.group(1) if m else None

    def block(i: int, end: int) -> None:
        number = new_block()
        index = 0
        while True:
            i = skip(i)
            if i >= end:
                return
            i, index = one(i, number, index)

    def one(i: int, number: int, index: int) -> tuple[int, int]:
        """Parses the statement at `i`; returns the offset after it and the next index in the block."""
        i = skip(i)
        c = code[i]
        if c == "{":
            close = match(code, i, "{", "}")
            block(i + 1, close)
            return close + 1, index + 1
        word = keyword(i)
        if word in ("if", "while", "switch", "for"):
            p = code.index("(", i)
            q = match(code, p, "(", ")")
            if word in ("if", "while"):
                body.conditions.append((p + 1, q))
            elif word == "for" and ";" in code[p:q]:  # not a range-based loop
                first = code.index(";", p)
                second = code.index(";", first + 1)
                if second > first + 1:
                    body.conditions.append((first + 1, second))
            i, _ = one(q + 1, new_block(), 0)
            return i, index + 1
        if word == "else" or word == "do":
            i, _ = one(i + len(word), new_block(), 0)
            if word == "do":
                i = skip(i)
                p = code.index("(", i)
                q = match(code, p, "(", ")")
                body.conditions.append((p + 1, q))
                i = code.index(";", q) + 1
            return i, index + 1
        if word in ("case", "default"):
            i = code.index(":", i) + 1
            return i, 0
        end = i
        depth = 0
        while code[end] != ";" or depth:
            if code[end] in "({[":
                depth += 1
            elif code[end] in ")}]":
                depth -= 1
            end += 1
        end += 1
        kind = "other" if word else classify(code[i:end])
        body.statements.append(Statement(i, end, kind, number, index))
        return end, index + 1

    block(open_brace + 1, close_brace)
    return body


def split_operands(code: str, start: int, end: int) -> list[tuple[int, int]]:
    """The operands of the top-level || and && of the condition, or the condition alone."""
    parts, depth, s = [], 0, start
    i = start
    while i < end:
        c = code[i]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0 and code[i:i + 2] in ("||", "&&"):
            parts.append((s, i))
            s = i + 2
            i += 1
        i += 1
    parts.append((s, end))
    out = []
    for a, b in parts:
        while a < b and code[a].isspace():
            a += 1
        while b > a and code[b - 1].isspace():
            b -= 1
        out.append((a, b))
    return out


# --- mutants ----------------------------------------------------------------------------------------------------------

@dataclass
class Mutant:
    id: str
    operator: str
    function: str              # the C++ function
    file: Path                 # the source file the mutant is compiled from
    line: int
    before: str
    after: str
    targets: list[str]         # table names whose cases judge it
    edits: dict[Path, list[tuple[int, int, str]]] = field(default_factory=dict)


@dataclass
class Unit:
    path: Path
    text: str
    code: str
    functions: list[Function]
    exports: dict[str, str]   # table name -> C++ function name
    headers: dict[Path, tuple[str, str]] = field(default_factory=dict)  # sibling header -> (text, blanked)


EXPORT = re.compile(r"GC_DECOMP_EXPORT\(\s*(\w+)\s*,\s*([\w:]+)\s*,")


def read_unit(path: Path) -> Unit:
    text = path.read_bytes().decode("utf-8")
    code = blank(text)
    exports = {m.group(1): m.group(2).split("::")[-1] for m in EXPORT.finditer(code)}
    if not exports:
        exports = {m.group(1): m.group(2).split("::")[-1] for m in EXPORT.finditer(text)}
    unit = Unit(path, text, code, functions_in(code), exports)
    for h in sorted(path.parent.glob("*.h")):
        t = h.read_bytes().decode("utf-8")
        unit.headers[h] = (t, blank(t))
    return unit


def callees(code: str, fn: Function, names: set[str]) -> set[str]:
    return {m.group(1) for m in re.finditer(r"\b(\w+)\s*(?:<[^;()]*>)?\s*\(", code[fn.open:fn.close])
            if m.group(1) in names and m.group(1) != fn.name}


def reach(unit: Unit, selected: set[str]) -> dict[str, set[str]]:
    """{C++ function of the file: table names of `selected` that run it}, helpers included."""
    by_name = {f.name: f for f in unit.functions}
    names = set(by_name)
    out: dict[str, set[str]] = defaultdict(set)
    for table_name, cpp in unit.exports.items():
        if table_name not in selected or cpp not in by_name:
            continue
        todo, seen = [cpp], set()
        while todo:
            n = todo.pop()
            if n in seen:
                continue
            seen.add(n)
            todo += sorted(callees(unit.code, by_name[n], names))
        for n in seen:
            out[n].add(table_name)
    return out


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def squeeze(text: str) -> str:
    return " ".join(text.split())


class Builder:
    """Collects the mutants of one function; ids are stable while the mutated text and the order of equal mutations
    do not change."""

    def __init__(self, unit: Unit, fn: Function, targets: list[str]):
        self.unit, self.fn, self.targets = unit, fn, sorted(targets)
        self.seen: Counter = Counter()
        self.out: list[Mutant] = []

    def add(self, operator: str, path: Path, text: str, edits: list[tuple[int, int, str]], before: str, after: str):
        key = (operator, before, after)
        self.seen[key] += 1
        digest = hashlib.sha1(f"{operator}\0{before}\0{after}\0{self.seen[key]}".encode()).hexdigest()[:8]
        self.out.append(Mutant(f"{self.fn.name}.{operator}.{digest}", operator, self.fn.name, self.unit.path,
                               line_of(text, edits[0][0]), squeeze(before), squeeze(after), self.targets,
                               {path: edits}))

    def body(self, operator: str, edits: list[tuple[int, int, str]], before: str, after: str):
        self.add(operator, self.unit.path, self.unit.text, edits, before, after)


def invert_branch(b: Builder, code: str, text: str, parsed: Body) -> None:
    covered: list[tuple[int, int]] = []
    for start, end in parsed.conditions:
        covered.append((start, end))
        condition = text[start:end]
        b.body("invert_branch", [(start, end, f"!({condition})")], condition, f"!({condition})")
        operands = split_operands(code, start, end)
        if len(operands) > 1:
            for a, e in operands:
                b.body("invert_branch", [(a, e, f"!({text[a:e]})")], f"{text[a:e]} [in {squeeze(condition)}]",
                       f"!({text[a:e]})")
    for m in re.finditer(r" (==|!=) ", code[b.fn.open:b.fn.close]):
        at = b.fn.open + m.start(1)
        if any(s <= at < e for s, e in covered):
            continue
        new = "!=" if m.group(1) == "==" else "=="
        context = squeeze(text[max(b.fn.open, at - 24):at + 26])
        b.body("invert_branch", [(at, at + 2, new)], f"{m.group(1)} in {context}", new)


def relational(b: Builder, code: str, text: str) -> None:
    table = {"<": "<=", "<=": "<", ">": ">=", ">=": ">"}
    for m in re.finditer(r" (<=|>=|<|>) ", code[b.fn.open:b.fn.close]):
        at = b.fn.open + m.start(1)
        end = at + len(m.group(1))
        context = squeeze(text[max(b.fn.open, at - 24):end + 24])
        b.body("relational", [(at, end, table[m.group(1)])], f"{m.group(1)} in {context}", table[m.group(1)])


def literal_one(literal: str) -> str:
    return "1.0f" if re.search(r"[.eEfF]", literal) and not literal.lower().startswith("0x") else "1"


def constant(b: Builder, code: str, text: str, parsed: Body) -> None:
    for m in re.finditer(rf"(?<![\w.]){LITERAL}(?![\w.])", code[b.fn.open:b.fn.close]):
        at = b.fn.open + m.start()
        lit = m.group()
        if re.search(r"<[^<>;()]*$", code[max(b.fn.open, at - 40):at]) and code[at - 1] != " ":
            continue  # a template argument
        context = squeeze(text[max(b.fn.open, at - 24):at + len(lit) + 24])
        for sign in "+-":
            new = f"({lit} {sign} {literal_one(lit)})"
            b.body("constant", [(at, at + len(lit), new)], f"{lit} in {context}", new)
    # Named constants of the file and its sibling headers that the function uses (its parameters and locals shadow them).
    declared = set(re.findall(r"([A-Za-z_]\w*)\s*[,)]", code[b.fn.start:b.fn.open]))
    for s in parsed.statements:
        if s.kind == "decl":
            found = re.search(r"([A-Za-z_]\w*)\s*(?:=|\{|;|\()", code[s.start:s.end])
            declared |= {found.group(1)} if found else set()
    used = set(re.findall(r"\b[A-Za-z_]\w*\b", code[b.fn.start:b.fn.close])) - declared
    bodies = [(f.start, f.close) for f in b.unit.functions]
    sources = [(b.unit.path, text, code, bodies)]
    sources += [(p, t, c, []) for p, (t, c) in b.unit.headers.items()]
    for path, t, c, skip_ranges in sources:
        for m in re.finditer(rf"\b([A-Za-z_]\w*)\s*(?<![=!<>])=\s*({LITERAL})(?=\s*[;,\n}}])", c):
            if m.group(1) not in used or any(s <= m.start() < e for s, e in skip_ranges):
                continue
            lit = m.group(2)
            for sign in "+-":
                new = f"({lit} {sign} {literal_one(lit)})"
                b.add("constant", path, t, [(m.start(2), m.end(2), new)], f"{m.group(1)} = {lit}", new)


def removed_write(b: Builder, text: str, parsed: Body) -> None:
    for s in parsed.statements:
        if s.kind in ("write", "local"):
            b.body("removed_write", [(s.start, s.end, ";")], text[s.start:s.end], ";")


def consecutive(parsed: Body) -> list[tuple[Statement, Statement]]:
    by_block: dict[int, dict[int, Statement]] = defaultdict(dict)
    for s in parsed.statements:
        by_block[s.block][s.index] = s
    pairs = []
    for statements in by_block.values():
        for index, s in sorted(statements.items()):
            if index + 1 in statements:
                pairs.append((s, statements[index + 1]))
    return pairs


def swapped_writes(b: Builder, text: str, parsed: Body) -> None:
    for x, y in consecutive(parsed):
        if x.kind == "write" and y.kind == "write":
            tx, ty = text[x.start:x.end], text[y.start:y.end]
            b.body("swapped_writes", [(x.start, x.end, ty), (y.start, y.end, tx)], f"{tx} {ty}", f"{ty} {tx}")


def write_before_read(b: Builder, code: str, text: str, parsed: Body) -> None:
    """A store moved above the load just before it (the load then sees it), or, when the store is the only use of the
    loaded temporary, the temporary inlined (`const T v = *p; q->f = v;` -> `q->f = *p;`: gc_mem.h copies field by
    field, reading while it writes). The shape of anomaly A19, defined after the fact: see the module header."""
    for x, y in consecutive(parsed):
        if x.kind != "decl" or y.kind != "write":
            continue
        m = re.search(r"([A-Za-z_]\w*)\s*=\s*", code[x.start:x.end])
        if not m:
            continue
        name = m.group(1)
        value = text[x.start + m.end():x.end - 1].strip()
        if not re.search(r"->|\*|\[", value):
            continue
        tx, ty = text[x.start:x.end], text[y.start:y.end]
        if not re.search(rf"\b{name}\b", code[y.start:y.end]):
            b.body("write_before_read", [(x.start, x.end, ty), (y.start, y.end, tx)], f"{tx} {ty}", f"{ty} {tx}")
            continue
        uses = len(re.findall(rf"\b{name}\b", code[b.fn.open:b.fn.close]))
        if uses == 2 and re.search(rf"= {name};$", ty):
            inlined = ty[:ty.rindex(name)] + value + ";"
            b.body("write_before_read", [(x.start, x.end, ";"), (y.start, y.end, inlined)], f"{tx} {ty}", inlined)


def signedness(b: Builder, code: str, text: str) -> None:
    pattern = r"\b(" + "|".join(SIGNED) + r")\b"
    for m in re.finditer(pattern, code[b.fn.start:b.fn.close]):
        at = b.fn.start + m.start()
        new = SIGNED[m.group(1)]
        context = squeeze(text[max(b.fn.start, at - 24):at + 30])
        b.body("signedness", [(at, at + len(m.group(1)), new)], f"{m.group(1)} in {context}", new)
    # The type of a field the function uses, declared in this file or a sibling header.
    used = {m.group(1) for m in re.finditer(r"(?:->|\.)\s*([A-Za-z_]\w*)", code[b.fn.open:b.fn.close])}
    bodies = [(f.start, f.close) for f in b.unit.functions]
    sources = [(b.unit.path, text, code, bodies)]
    sources += [(p, t, c, []) for p, (t, c) in b.unit.headers.items()]
    for path, t, c, skip_ranges in sources:
        for m in re.finditer(pattern + r">?\s+([A-Za-z_]\w*)\s*(?:;|:\s*\d)", c):
            if m.group(2) not in used or any(s <= m.start() < e for s, e in skip_ranges):
                continue
            new = SIGNED[m.group(1)]
            b.add("signedness", path, t, [(m.start(1), m.end(1), new)], f"field {squeeze(c[m.start():m.end()])}", new)


def mutants_of_unit(unit: Unit, selected: set[str], operators: list[str]) -> list[Mutant]:
    out: list[Mutant] = []
    runners = reach(unit, selected)
    for fn in unit.functions:
        targets = runners.get(fn.name)
        if not targets:
            continue
        b = Builder(unit, fn, sorted(targets))
        parsed = parse_body(unit.code, fn.open, fn.close)
        if "invert_branch" in operators:
            invert_branch(b, unit.code, unit.text, parsed)
        if "relational" in operators:
            relational(b, unit.code, unit.text)
        if "constant" in operators:
            constant(b, unit.code, unit.text, parsed)
        if "removed_write" in operators:
            removed_write(b, unit.text, parsed)
        if "swapped_writes" in operators:
            swapped_writes(b, unit.text, parsed)
        if "write_before_read" in operators:
            write_before_read(b, unit.code, unit.text, parsed)
        if "signedness" in operators:
            signedness(b, unit.code, unit.text)
        out += b.out
    return out


def apply_edits(text: str, edits: list[tuple[int, int, str]]) -> str:
    for start, end, new in sorted(edits, reverse=True):
        text = text[:start] + new + text[end:]
    return text


# --- building a mutant ------------------------------------------------------------------------------------------------

def code_sections(obj: Path) -> str:
    """Hash of the code and data sections of a COFF object, with their relocations (the header's time stamp, the symbol
    table and the file name stay out): equal hashes = same machine code."""
    data = obj.read_bytes()
    count = struct.unpack_from("<H", data, 2)[0]
    optional = struct.unpack_from("<H", data, 16)[0]
    table = 20 + optional
    h = hashlib.sha1()
    for i in range(count):
        at = table + 40 * i
        name = data[at:at + 8].rstrip(b"\0")
        size, pointer, relocations = struct.unpack_from("<III", data, at + 16)
        relocation_count = struct.unpack_from("<H", data, at + 32)[0]
        if not name.startswith((b".text", b".rdata", b".data", b".bss", b"/")):
            continue
        h.update(name + b"\0")
        if pointer:
            h.update(data[pointer:pointer + size])
        if relocation_count:
            h.update(data[relocations:relocations + 10 * relocation_count])
    return h.hexdigest()


@dataclass
class Build:
    clang: str
    flags: list[str]
    env: dict
    objects: dict[Path, Path]     # source -> object of the replay
    extra_objects: list[Path]     # replay_table, oracle_replay, globals...
    work: Path
    baseline_code: dict[Path, str] = field(default_factory=dict)


def compile_copy(build: Build, source: Path, headers: list[Path], folder: Path, replaced: dict[Path, str]) -> Path | None:
    """Compiles `source` from a copy in `folder`, with its sibling headers (a relative include finds the copies):
    from that folder with a relative file name, so the path never reaches the object."""
    folder.mkdir(parents=True, exist_ok=True)
    for p in [source, *headers]:
        target = folder / p.name
        if p in replaced:
            target.write_bytes(replaced[p].encode("utf-8"))
        else:
            target.write_bytes(p.read_bytes())
    obj = folder / f"{source.stem}.obj"
    r = subprocess.run([build.clang, "/nologo", "/c", *build.flags, source.name, f"/Fo{obj.name}"], cwd=folder,
                       capture_output=True, text=True, env=build.env)
    return obj if r.returncode == 0 and obj.exists() else None


def link(build: Build, replaced: dict[Path, Path], folder: Path) -> Path | None:
    exe = folder / f"replay{tc.EXE}"
    objects = [replaced.get(s, o) for s, o in build.objects.items()] + build.extra_objects
    r = subprocess.run([build.clang, "/nologo", *[str(o) for o in objects], f"/Fe{exe}"], capture_output=True, text=True,
                       env=build.env, cwd=folder)
    return exe if r.returncode == 0 and exe.exists() else None


# --- running -----------------------------------------------------------------------------------------------------------

@dataclass
class Replay:
    outcome: str
    failed: int = 0
    passed: int = 0
    killed_by: list[str] = field(default_factory=list)
    detail: str = ""


def run_replay(exe: Path, cases: Path, globals_file: Path | None) -> Replay:
    """Runs the replay executable on one case file and sorts the result."""
    env = dict(os.environ)
    if globals_file:
        env["GC_REPLAY_GLOBALS"] = str(globals_file)
    try:
        r = subprocess.run([str(exe), str(cases)], capture_output=True, text=True, timeout=REPLAY_TIMEOUT_SECONDS,
                           env=env, errors="replace")
    except subprocess.TimeoutExpired:
        return Replay("weak", detail="hang")
    summary = [x for x in r.stdout.splitlines() if x.startswith("REPLAY ")]
    if not summary:
        return Replay("weak", detail=f"crash, exit {r.returncode:#x}")
    strong, weak = [], []
    passed = 0
    first = ""
    for line in r.stdout.splitlines():
        if line.startswith("PASS "):
            passed += 1
        elif line.startswith("FAIL "):
            parts = line.split(" ", 3)
            label = parts[2].rstrip(":")
            rest = parts[3] if len(parts) > 3 else ""
            (weak if "read outside the case" in line else strong).append(label)
            first = first or rest
    if strong:
        return Replay("killed", len(strong) + len(weak), passed, strong, first[:160])
    if weak:
        return Replay("weak", len(weak), passed, weak, first[:160])
    return Replay("survived", 0, passed)


def write_globals_source(path: Path) -> None:
    path.write_text("""// Generated by tools/decomp_mutants.py (build folder only). Installs the address table of the game globals for the
// version under test, read from the file named by GC_REPLAY_GLOBALS (one hexadecimal address per line).
#include "decomp/gc_mem.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
namespace {
std::vector<std::uint32_t> s_table;
struct install {
    install() {
        const char* name = std::getenv("GC_REPLAY_GLOBALS");
        std::FILE* f = name ? std::fopen(name, "r") : nullptr;
        if (!f)
            return;
        unsigned value = 0;
        while (std::fscanf(f, "%x", &value) == 1)
            s_table.push_back(value);
        std::fclose(f);
        s_table.push_back(0u); // never empty
        gc::set_global_table(s_table.data(), s_table.size() - 1);
    }
} s_install;
} // namespace
""", encoding="utf-8")


def load_cases(oracle, bench: Path, game: str, label: str, version: str, wanted: set[str], drop: set[str],
               seen: set) -> tuple[dict, Counter]:
    """{function: [case lines]} of the oracle files of (label, version), one per (function, fingerprint, synthetic
    label) over the labels of a run (`seen`), with the fingerprint marked `:<label>` for a synthetic case (the replay
    prints it back). Cases with calls are not replayable offline; `drop` removes the synthetic cases of those labels.
    Counts in the second result."""
    out: dict[str, list[list[str]]] = defaultdict(list)
    counts: Counter = Counter()
    files = sorted(oracle.oracle_dir(bench, game, label, version).glob("oracle-*.jsonl"))
    if not files:
        raise SystemExit(f"no oracle file for {label}/{version}")
    for path in files:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if '"kind":"case"' not in line:
                continue
            rec = json.loads(line)
            name = rec.get("function")
            if name not in wanted:
                continue
            if rec.get("calls"):
                counts["with_calls"] += 1
                continue
            tag = rec.get("synthetic") or ""
            if tag in drop:
                counts["dropped"] += 1
                continue
            key = (name, rec.get("fingerprint"), tag)
            if key in seen:
                continue
            seen.add(key)
            if tag:
                rec = dict(rec, fingerprint=f"{rec.get('fingerprint', '-')}:{tag}")
            out[name].append(oracle.case_lines(rec))
            counts["synthetic" if tag else "real"] += 1
    return out, counts


def globals_addresses(game_cfg: dict, version: str) -> list[int]:
    """The address of every global identifier in `version`, in identifier order (0 = absent there)."""
    import decomp_globals as dg
    if dg.table_path(game_cfg) is None:
        return []
    return [row.addresses.get(version) or 0 for row in dg.identifiers(dg.read_table(game_cfg))]


# --- the command -------------------------------------------------------------------------------------------------------

def read_triage(path: Path) -> dict[str, tuple[str, str]]:
    out: dict[str, tuple[str, str]] = {}
    if path.is_file():
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip() or line.startswith("#"):
                continue
            cells = line.split("\t")
            if len(cells) >= 3 and cells[0] != "id":
                out[cells[0]] = (cells[1], cells[2])
    return out


def read_prediction(path: Path | None) -> tuple[int, int] | None:
    """(survivors, real gaps) of the note's prediction line, or None (no note, or no such line)."""
    if path is None or not path.is_file():
        return None
    found = PREDICTION.search(path.read_text(encoding="utf-8", errors="replace"))
    return (int(found.group(1)), int(found.group(2))) if found else None


def run(args, oracle) -> int:
    started = time.monotonic()
    game = oracle.game_name(args.game)
    game_cfg = tc.load_game(game)
    bench = Path(args.bench_root).resolve() if args.bench_root else oracle.main_checkout()
    versions = oracle.versions_of(game_cfg, args.versions)
    labels = [x for x in args.label.split(",") if x]
    out = Path(args.out).resolve() if args.out else tc.ROOT / "build" / "oracle-mutants" / labels[0]
    build_root = (tc.ROOT / "build").resolve()
    if build_root not in out.parents:  # build/ itself too: the tool would write cases/, work/, base/ straight into it
        raise SystemExit(f"--out {out}: mutants stay under {build_root}, in a folder of their own")
    operators = [x for x in (args.operators or ",".join(OPERATORS)).split(",") if x]
    unknown = [x for x in operators if x not in OPERATORS]
    if unknown:
        raise SystemExit(f"unknown operators: {', '.join(unknown)} (known: {', '.join(OPERATORS)})")
    table = oracle.df.read_table(game_cfg)
    if args.functions:
        selected = {x for x in args.functions.split(",") if x}
        missing = selected - {f.name for f in table}
        if missing:
            raise SystemExit(f"not in the functions table: {', '.join(sorted(missing))}")
    else:
        selected = {f.name for f in table if f.status == args.status}
    sources = sorted((tc.ROOT / "games" / game / "decomp" / "src").rglob("*.cpp"))
    units = [read_unit(p) for p in sources]
    mutants: list[Mutant] = []
    for unit in units:
        mutants += mutants_of_unit(unit, selected, operators)
    missing = selected - {t for m in mutants for t in m.targets}
    print(f"== Mutants: {len(mutants)} on {len(selected)} functions, operators {', '.join(operators)}")
    for name, n in sorted(Counter(m.operator for m in mutants).items()):
        print(f"   {name}: {n}")
    if missing:
        print(f"   functions with no mutant: {', '.join(sorted(missing))}")
    if args.only:
        wanted = re.compile(args.only)
        mutants = [m for m in mutants if wanted.search(m.id)]
        print(f"   --only keeps {len(mutants)}")
    if args.list:
        for m in mutants:
            print(f"{m.id}\t{m.file.name}:{m.line}\t{m.before}\t=> {m.after}")
        return 0
    prediction = read_prediction(Path(args.prediction) if args.prediction else None)
    if args.functions and prediction is None:
        print("WARNING: no prediction for this batch: --prediction FILE, a note written before the run with the line "
              "'mutants-prediction: survivors=<n> gaps=<n>' (not blocking)")

    drop ={x for x in (args.drop_synthetic or "").split(",") if x}
    if drop:
        print(f"   synthetic cases dropped: {', '.join(sorted(drop))}")
    out.mkdir(parents=True, exist_ok=True)
    case_files: dict[tuple[str, str], Path] = {}
    per_version: dict[str, dict] = {}
    for version in versions:
        folder = out / "cases" / version
        folder.mkdir(parents=True, exist_ok=True)
        merged: dict[str, list[list[str]]] = defaultdict(list)
        totals: Counter = Counter()
        seen: set = set()
        for label in labels:
            cases, counts = load_cases(oracle, bench, game, label, version, selected, drop, seen)
            totals.update(counts)
            for name, items in cases.items():
                merged[name] += items
        for name, items in merged.items():
            path = folder / f"{name}.txt"
            path.write_text("\n".join(x for case in items for x in case) + "\n", encoding="utf-8")
            case_files[(version, name)] = path
        per_version[version] = {name: len(items) for name, items in merged.items()}
        print(f"   {version}: {dict(totals)}")

    # The replay, once: table, oracle_replay and every source; a mutant replaces the object of its file.
    extra = out / "replay_globals.cpp"
    write_globals_source(extra)
    t0 = time.monotonic()
    objects, others, flags, clang, env = oracle.build_replay_objects(game, game_cfg, out / "base", [extra])
    build = Build(clang, flags, env, objects, others, out / "work")
    baseline = link(build, {}, out / "base")
    if not baseline:
        raise SystemExit("link of the unchanged replay failed")
    print(f"   baseline replay built in {time.monotonic() - t0:.1f} s")
    globals_files = {}
    for version in versions:
        path = out / f"globals-{version}.txt"
        path.write_text("".join(f"{a:08X}\n" for a in globals_addresses(game_cfg, version)), encoding="ascii")
        globals_files[version] = path

    # A function whose unchanged code fails its cases cannot judge a mutant.
    bad: dict[tuple[str, str], str] = {}
    for (version, name), path in sorted(case_files.items()):
        r = run_replay(baseline, path, globals_files[version])
        if r.outcome != "survived":
            bad[(version, name)] = f"{r.outcome}: {r.failed} failing, {r.detail}"
            print(f"   BASELINE FAIL {version} {name}: {bad[(version, name)]}")

    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)  # no error box from a mutant that crashes

    triage_path = Path(args.triage) if args.triage else tc.ROOT / "games" / game / "decomp" / "oracle_mutants" / "triage.tsv"
    triage = read_triage(triage_path)
    build.work.mkdir(parents=True, exist_ok=True)
    unit_of = {u.path: u for u in units}

    def baseline_code(unit: Unit) -> str:
        if unit.path not in build.baseline_code:
            folder = out / "base-copies" / unit.path.stem
            obj = compile_copy(build, unit.path, list(unit.headers), folder, {})
            build.baseline_code[unit.path] = code_sections(obj) if obj else ""
        return build.baseline_code[unit.path]

    for unit in units:
        if any(m.file == unit.path for m in mutants):
            baseline_code(unit)

    def judge(index_mutant: tuple[int, Mutant]) -> dict:
        index, m = index_mutant
        t = time.monotonic()
        unit = unit_of[m.file]
        replaced: dict[Path, str] = {}
        for path, edits in m.edits.items():
            text = unit.text if path == unit.path else unit.headers[path][0]
            replaced[path] = apply_edits(text, edits)
        folder = build.work / str(index)
        result = {"id": m.id, "operator": m.operator, "function": m.function, "file": m.file.name, "line": m.line,
                  "before": m.before, "after": m.after, "targets": m.targets, "versions": {}}
        obj = compile_copy(build, m.file, list(unit.headers), folder, replaced)
        if obj is None:
            result["outcome"] = "invalid"
        elif build.baseline_code.get(m.file) and code_sections(obj) == build.baseline_code[m.file]:
            result["outcome"] = "equivalent"
        else:
            exe = link(build, {m.file: obj}, folder)
            if exe is None:
                result["outcome"] = "invalid"
            else:
                for version in versions:
                    runs = []
                    for name in m.targets:
                        path = case_files.get((version, name))
                        if path is not None and (version, name) not in bad:
                            runs.append(run_replay(exe, path, globals_files[version]))
                    if not runs:
                        result["versions"][version] = {"outcome": "no_cases"}
                        continue
                    order = ("killed", "weak", "survived")
                    outcome = next(x for x in order if any(r.outcome == x for r in runs))
                    killers = [k for r in runs for k in r.killed_by]
                    tags = Counter(k.rsplit(":", 1)[1] if ":" in k else "(real)" for k in killers)
                    result["versions"][version] = {
                        "outcome": outcome, "failed": sum(r.failed for r in runs),
                        "passed": sum(r.passed for r in runs), "killed_by": dict(tags.most_common(6)),
                        "detail": next((r.detail for r in runs if r.detail), "")}
                outcomes = {v["outcome"] for v in result["versions"].values()}
                result["outcome"] = next(x for x in ("killed", "weak", "survived", "no_cases") if x in outcomes)
                if result["outcome"] == "survived" and re.search(r"\bcr\b|clobber_cr", m.before + " " + m.after):
                    result["outcome"] = "unknown"
                result["version_split"] = len(outcomes) > 1
        result["seconds"] = round(time.monotonic() - t, 3)
        if not args.keep:
            shutil.rmtree(folder, ignore_errors=True)
        return result

    t0 = time.monotonic()
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(judge, enumerate(mutants)))
    elapsed = time.monotonic() - t0

    # Report: per function, then the survivors and their sorting.
    per_function: dict[str, Counter] = defaultdict(Counter)
    for r in results:
        for name in r["targets"]:
            per_function[name][r["outcome"]] += 1
    columns = ("killed", "weak", "survived", "unknown", "no_cases", "equivalent", "invalid")
    weak_kinds: Counter = Counter()
    for r in results:
        if r["outcome"] == "weak":
            details = [v.get("detail", "") for v in r["versions"].values()]
            weak_kinds["crash" if any(d.startswith("crash") for d in details) else
                       "hang" if "hang" in details else "read outside the case"] += 1
    print(f"\n{'function':<42}" + "".join(f"{c[:8]:>9}" for c in columns))
    for name in sorted(per_function):
        print(f"{name:<42}" + "".join(f"{per_function[name][c]:>9}" for c in columns))
    totals = Counter(r["outcome"] for r in results)
    print(f"{'TOTAL (each mutant once)':<42}" + "".join(f"{totals[c]:>9}" for c in columns))
    judged = totals["killed"] + totals["weak"] + totals["survived"]
    if judged:
        print(f"score {100 * totals['killed'] / judged:.1f} % killed of {judged} judged (not blocking)")
    if weak_kinds:
        print("weak: " + ", ".join(f"{n} {what}" for what, n in sorted(weak_kinds.items())))
    split = [r for r in results if r.get("version_split")]
    if split:
        print(f"{len(split)} mutants judged differently by the versions:")
        for r in split:
            print(f"  VERSION {r['id']}: " + ", ".join(f"{v} {x['outcome']}" for v, x in r["versions"].items()))
    survivors = [r for r in results if r["outcome"] == "survived"]
    untriaged = [r for r in survivors if r["id"] not in triage]
    verdicts = Counter(triage[r["id"]][0].split(":")[0] for r in survivors if r["id"] in triage)
    print(f"survivors {len(survivors)}, sorted {len(survivors) - len(untriaged)} ({dict(sorted(verdicts.items()))}), "
          f"untriaged {len(untriaged)}")
    for r in survivors:
        verdict = triage.get(r["id"], ("UNTRIAGED", ""))
        print(f"SURVIVED {r['id']} {r['file']}:{r['line']} [{verdict[0]}] {r['before']} => {r['after']}")
    if prediction:
        gaps = sum(triage[r["id"]][0].split(":")[0] in GAP_VERDICTS for r in survivors if r["id"] in triage)
        print(f"prediction: {prediction[0]} survivors, measured {len(survivors)}; {prediction[1]} real gaps, measured "
              f"{gaps}" + (f" (+ {len(untriaged)} untriaged)" if untriaged else ""))
    if not args.only and not args.functions and not args.drop_synthetic:
        alive = {r["id"] for r in survivors}
        for stale in sorted(t for t in set(triage) - alive if t.split(".")[1] in operators):
            print(f"STALE triage line: {stale} (no longer a survivor)")
    seconds = [r["seconds"] for r in results]
    if seconds:
        print(f"time: {elapsed:.1f} s for {len(results)} mutants with {args.jobs} jobs; mean "
              f"{sum(seconds) / len(seconds):.2f} s per mutant (worker time), total {time.monotonic() - started:.1f} s")
    report = out / (args.report or "mutants.json")
    report.write_text(json.dumps({"labels": labels, "versions": versions, "operators": operators,
                                  "dropped_synthetic": sorted(drop), "cases": per_version, "functions": sorted(selected),
                                  "baseline_failures": {f"{v} {n}": t for (v, n), t in bad.items()},
                                  "results": results}, indent=1), encoding="utf-8")
    print(f"report: {report}")
    return 0
