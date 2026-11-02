"""Required case kinds and value grids of the decompilation oracle (tools/decomp_oracle.py cases, report).

One idea comes from REA (MIT; only the idea, this is our own code): the kinds of case a function must show are a list
derived from what the function is, not a matter of who thought of a value.

The oracle's promotion rule asks for block coverage. A block can run on friendly values only: anomaly A19 (a copy that
reads while it writes) was found by overlap cases added for another reason, not by a list. This module turns the list
into something a tool checks. From the machine code of the original (never from the decompiled source: a shape must
not depend on the code under test) it derives the SHAPE of a leaf function, then the kinds of case that shape requires:

  null            a pointer argument tested before any access through it: the argument is 0
  sentinel        a pointer argument compared with a constant before any access: the argument is that constant
  bound           a scalar argument compared with N: N-1, N, N+1
  field_bound     a value loaded through a pointer argument and compared with N: N-1, N, N+1 in memory
  relation        a scalar argument compared with such a value: the argument is the value -1, +0, +1
  float           a floating-point argument: +0, -0, denormal, +inf, -inf, QNaN, SNaN
  float_ps        the same, in the second half of the register, for an argument a paired-single operation reads
  float_field     the same seven values, in memory, for a float loaded through a pointer argument
  overlap_before  two pointer arguments, one written: the source starts before the destination and overlaps it
  overlap_after   the same, the source starts after the destination

Grid (`cases --grid`): for each case of the function's hand-written file (the TEMPLATE,
games/<game>/decomp/oracle_cases, whose values are invented: it gives the nested pointers a generated state could not
guess) each value of each required kind replaces what the template sets, one at a time. A function with no hand-written
case gets a bare template (zeroed area, pointers apart). Beyond the required kinds it adds: overlap_same (both pointers
equal), nan_pair (two NaN arguments, in both orders: which one wins) and the `_d` values, which only a double-precision
register can hold (a denormal, a QNaN and an SNaN no single-precision load gives): gc_float.h says it does not follow
those, so they are expected to differ and a caller that cannot produce them makes the state impossible, not an error.

The values are invented or come from the immediate operands of the compare instructions: never tables or strings of
the game. The output goes under build/ and is never tracked: tracked cases would change the hash of the synthetic
cases (tools/decomp_oracle.py synthetic_hash) and turn a difference on an impossible state into a refused promotion,
which is a change of what `verified` means. Hence a grid run has its own label (`run --grid DIR`), its results never
promote, and `report --grid-label L` reads them apart. A difference is sorted by what it touches, not only by its case
label: a line of grid_triage.tsv names the registers (and their bits) allowed to differ, and anything else on that label
is a real difference. Given to `report`, a grid can only refuse: an open difference refuses its function and makes the
exit code non-zero, a sorted one refuses nothing. A grid is not required: without --grid-label none of this runs.

Limits. The shape is a walk of the paths of the original with a small abstract value per register: an address built
another way, or a compare hidden in arithmetic (a subfic + cntlzw sequence), is not seen, so a shape is a lower bound of
what is required. A psq_l is taken for two floats in memory (quantization registers at their float default [hypothesis]:
a function that sets a GQR to an integer format loads something else). An instruction the walk does not know, or a bctr,
gives no shape (unknown): the function is listed, nothing is guessed. A case cannot set a game global, and it runs only
at the function's first call: the scenario must call the function. A decompiled access outside guest RAM stops the run
(gc_abi.cpp memory_fault): the generator only writes a pointer it can prove valid, 0 where it is tested first, or a
template's own value; it never puts a bound or a pattern in a field the walk saw used as an address.

Generic: no game here, everything comes from the functions table, the code and the template files.
"""
from __future__ import annotations

import json
import re
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

ARG_GPRS = range(3, 11)
ARG_FPRS = range(1, 9)
FULL = 0xFFFFFFFF
MAX_AREA = 0x400
WALK_LIMIT = 200_000
WIDEN_AFTER = 32  # visits of one address after which a loop's changing values are forgotten
DEFAULT_OUT = "build/oracle-grid"

# Register images (double-precision bits) and memory images (single-precision bits) of the float kinds. Invented:
# the IEEE classes themselves, with a payload that a single can hold (bit 29 of the double fraction) for the NaNs.
FLOAT_REGISTER = [("pzero", "0000000000000000"), ("nzero", "8000000000000000"), ("denormal", "36A0000000000000"),
                  ("ndenormal", "B6A0000000000000"), ("pinf", "7FF0000000000000"), ("ninf", "FFF0000000000000"),
                  ("qnan", "7FF8000020000000"), ("snan", "7FF0000020000000")]
# Only a double-precision register holds these: reported apart (gc_float.h, limits).
FLOAT_REGISTER_DOUBLE = [("denormal_d", "0000000000000001"), ("qnan_d", "7FF8000000000001"),
                         ("snan_d", "7FF0000000000001")]
FLOAT_MEMORY = [("pzero", 0x00000000), ("nzero", 0x80000000), ("denormal", 0x00000001), ("ndenormal", 0x80000001),
                ("pinf", 0x7F800000), ("ninf", 0xFF800000), ("qnan", 0x7FC00000), ("snan", 0x7F800001)]
FLOAT_CLASSES = ("pzero", "nzero", "denormal", "pinf", "ninf", "qnan", "snan")
NAN_FIRST, NAN_SECOND = "7FF8000040000000", "7FF0000060000000"  # two NaNs with different payloads, for nan_pair


def _signed(value: int, bits: int) -> int:
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def rotate_mask(mb: int, me: int) -> int:
    """The mask of rlwinm: bits mb..me, bit 0 being the most significant, wrapping when mb > me."""
    first, last = 0xFFFFFFFF >> mb, (0xFFFFFFFF << (31 - me)) & 0xFFFFFFFF
    return (first & last) if mb <= me else (first | last)


# --- shape of an original ---------------------------------------------------------------------------------------------

class UnknownInstruction(Exception):
    """An instruction the walk does not model: the function gets no shape."""


@dataclass
class Shape:
    """What the walk of an original found. Offsets are from the argument's value at entry."""
    leaf: bool = True
    unknown: str | None = None
    # {argument: (offset, size, store)}
    accesses: dict[int, set[tuple[int, int, bool]]] = field(default_factory=lambda: defaultdict(set))
    indexed: set[int] = field(default_factory=set)      # pointers also reached with a computed offset
    tested: set[int] = field(default_factory=set)       # tested against 0 before any access on that path
    late_test: set[int] = field(default_factory=set)    # tested only after an access through it
    # (N, mask) an argument, or a field (argument, offset, size), is compared with
    constants: dict[int, set[tuple[int, int]]] = field(default_factory=lambda: defaultdict(set))
    field_constants: dict[tuple[int, int, int], set[tuple[int, int]]] = field(default_factory=lambda: defaultdict(set))
    chased: set[tuple[int, int]] = field(default_factory=set)        # (arg, off): a field used as an address
    unguarded: set[tuple[int, int]] = field(default_factory=set)     # the same, used without a test against 0 first
    # (scalar argument, pointer argument, offset, size, mask): a scalar compared with a loaded value
    relations: set[tuple[int, int, int, int, int]] = field(default_factory=set)
    float_args: set[int] = field(default_factory=set)
    paired_args: set[int] = field(default_factory=set)  # float arguments read by a paired-single operation
    float_fields: set[tuple[int, int, int]] = field(default_factory=set)  # (arg, off, size)
    gpr_args: set[int] = field(default_factory=set)     # argument registers read before any write
    late_constants: set[int] = field(default_factory=set)  # arguments compared after an access through them


Value = tuple | None
# Abstract value of a register: ("a", n, delta, mask) argument n + delta, then masked; ("f", n, off, size, mask,
# tested) loaded through argument n, `tested` once compared with 0; ("d", n) computed from argument n (an index);
# ("i", n) argument n + an index; ("x", n, off) loaded through a field; ("c", v) a constant; None unknown.


LOADS = {32: 4, 33: 4, 34: 1, 35: 1, 40: 2, 41: 2, 42: 2, 43: 2}
STORES = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2}
FLOAT_LOADS = {48: 4, 49: 4, 50: 8, 51: 8}
FLOAT_STORES = {52: 4, 53: 4, 54: 8, 55: 8}
X_LOADS = {23: 4, 55: 4, 87: 1, 119: 1, 279: 2, 311: 2, 343: 2, 375: 2, 20: 4, 534: 4, 790: 2}
X_STORES = {151: 4, 183: 4, 215: 1, 247: 1, 407: 2, 439: 2, 150: 4, 662: 4, 918: 2}
X_FLOAT_LOADS = {535: 4, 567: 4, 599: 8, 631: 8}
X_FLOAT_STORES = {663: 4, 695: 4, 727: 8, 759: 8}
X_RD_AB = {266, 10, 138, 40, 8, 136, 235, 75, 11, 491, 459}
X_RD_A = {104, 202, 234, 200, 232}
X_RA_SB = {28, 60, 444, 412, 316, 124, 476, 284, 24, 536, 792}
X_RA_S = {824, 26, 954, 922}
X_RD_ONLY = {19, 339, 371, 83}
X_NONE = {467, 144, 146, 598, 854, 86, 54, 982, 278, 246, 1014}
FLOAT_A_FORM = {18: "ab", 20: "ab", 21: "ab", 22: "b", 24: "b", 25: "ac", 26: "b", 23: "abc", 28: "abc", 29: "abc",
                30: "abc", 31: "abc"}
FLOAT_X_FORM = {0: "ab", 32: "ab", 12: "b", 14: "b", 15: "b", 40: "b", 72: "b", 136: "b", 264: "b"}
# Primary opcode 4: the paired-single operations (A-form by the low 5 bits of the extended opcode, then X-form).
PAIRED_A_FORM = {**FLOAT_A_FORM, 10: "abc", 11: "abc", 12: "ac", 13: "ac", 14: "abc", 15: "abc"}
PAIRED_X_FORM = {0: "ab", 32: "ab", 64: "ab", 96: "ab", 40: "b", 72: "b", 136: "b", 264: "b", 528: "ab", 560: "ab",
                 592: "ab", 624: "ab"}
PSQ_LOADS, PSQ_STORES = (56, 57), (60, 61)


class Walker:
    """One path-sensitive walk of a function: a worklist of (pc, registers, written FPRs, arguments accessed)."""

    def __init__(self, words: list[int], address: int):
        self.words = words
        self.address = address
        self.end = address + 4 * len(words)
        self.shape = Shape()

    def run(self) -> Shape:
        start = tuple(("a", n, 0, FULL) if n in ARG_GPRS else None for n in range(32))
        todo = [(self.address, start, frozenset(), frozenset(), frozenset())]
        seen = set()
        steps = 0
        visits: Counter = Counter()
        while todo:
            state = todo.pop()
            visits[state[0]] += 1
            if visits[state[0]] > WIDEN_AFTER:
                state = (state[0], tuple(widened(v) for v in state[1])) + state[2:]
            if state in seen:
                continue
            seen.add(state)
            steps += 1
            if steps > WALK_LIMIT:
                self.shape.unknown = "too many paths"
                return self.shape
            try:
                todo += self.step(*state)
            except UnknownInstruction as e:
                self.shape.unknown = str(e)
                return self.shape
        return self.shape

    def read_gpr(self, regs, written, n):
        """An argument register read: remembered as an argument if nothing wrote it on this path."""
        if n in ARG_GPRS and n not in written:
            self.shape.gpr_args.add(n)
        return regs[n]

    def access(self, base: Value, off: int, size: int, store: bool, fp: bool,
               accessed: frozenset) -> tuple[Value, frozenset]:
        """An access through `base` + off: (value a load gives, arguments accessed so far)."""
        if not base:
            return None, accessed
        kind = base[0]
        if kind == "a" and base[3] == FULL:
            n, delta = base[1], base[2]
            self.shape.accesses[n].add((delta + off, size, store))
            if fp and not store:
                self.shape.float_fields.add((n, delta + off, size))
            return ("f", n, delta + off, size, FULL, False), accessed | {n}
        if kind == "i":
            self.shape.indexed.add(base[1])
            return None, accessed | {base[1]}
        if kind == "f":
            self.shape.chased.add((base[1], base[2]))
            if not base[5]:
                self.shape.unguarded.add((base[1], base[2]))
            return ("x", base[1], base[2]), accessed
        if kind == "x":
            return ("x", base[1], base[2]), accessed
        return None, accessed

    def compare_constant(self, value: Value, imm: int, accessed: frozenset) -> None:
        if not value:
            return
        if value[0] == "a":
            n, delta, mask = value[1], value[2], value[3]
            if imm == 0 and delta == 0 and mask == FULL:
                (self.shape.late_test if n in accessed else self.shape.tested).add(n)
            self.shape.constants[n].add((((imm - delta) & FULL), mask))
            if n in accessed:
                self.shape.late_constants.add(n)
        elif value[0] == "f":
            self.shape.field_constants[(value[1], value[2], value[3])].add((imm & FULL, value[4]))

    def compare_registers(self, a: Value, b: Value, accessed: frozenset) -> None:
        if not a or not b:
            return
        if a[0] == "c" or b[0] == "c":
            constant, other = (a, b) if a[0] == "c" else (b, a)
            self.compare_constant(other, constant[1], accessed)
        elif a[0] == "a" and b[0] == "f":
            self.shape.relations.add((a[1], b[1], b[2], b[3], b[4]))
        elif a[0] == "f" and b[0] == "a":
            self.shape.relations.add((b[1], a[1], a[2], a[3], a[4]))

    def step(self, pc, regs, fp_written, gpr_written, accessed):
        if not self.address <= pc < self.end:
            return []
        w = self.words[(pc - self.address) // 4]
        prim, rd, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        simm, uimm = _signed(w & 0xFFFF, 16), w & 0xFFFF
        xo, rc = (w >> 1) & 1023, w & 1
        next_pc = pc + 4
        new = list(regs)
        written = set(gpr_written)

        def define(n, value):
            new[n] = value
            written.add(n)

        def read(n):
            return self.read_gpr(regs, gpr_written, n)

        def read_fpr(n):
            if n in ARG_FPRS and n not in fp_written:
                self.shape.float_args.add(n)

        def read_paired(n):
            read_fpr(n)
            if n in ARG_FPRS and n not in fp_written:
                self.shape.paired_args.add(n)

        def state(at, acc=None, fps=None):
            return (at, tuple(new), fps if fps is not None else fp_written, frozenset(written),
                    acc if acc is not None else accessed)

        if prim == 18:
            target = (pc + _signed(w & 0x3FFFFFC, 26)) & FULL if not (w & 2) else w & 0x3FFFFFC
            if w & 1 or not self.address <= target < self.end:
                self.shape.leaf = False
                return []
            return [state(target)]
        if prim == 16:
            target = (pc + _signed(w & 0xFFFC, 16)) & FULL if not (w & 2) else w & 0xFFFC
            if w & 1 or not self.address <= target < self.end:
                self.shape.leaf = False
                return []
            always = ((w >> 21) & 0x14) == 0x14
            return [state(target)] if always else [state(target), state(next_pc)]
        if prim == 19:
            if xo == 16:
                if w & 1:
                    self.shape.leaf = False
                    return []
                always = ((w >> 21) & 0x14) == 0x14
                return [] if always else [state(next_pc)]
            if xo == 528:
                if w & 1:
                    self.shape.leaf = False
                    return []
                raise UnknownInstruction(f"bcctr at {pc:08X}")
            if xo in (0, 33, 129, 193, 225, 257, 289, 417, 449, 150):
                return [state(next_pc)]
            raise UnknownInstruction(f"op 19/{xo} at {pc:08X}")
        if prim == 3:  # twi
            return [state(next_pc)]
        if prim in (7, 8):
            value = read(ra)
            define(rd, ("d", value[1]) if value and value[0] in ("a", "d") else None)
        elif prim in (10, 11):
            imm = uimm if prim == 10 else simm
            value = read(ra)
            self.compare_constant(value, imm, accessed)
            if value and value[0] == "f" and imm == 0:
                define(ra, value[:5] + (True,))
                written.discard(ra)
        elif prim in (12, 13, 14, 15):
            add = (simm << 16) if prim == 15 else simm
            if prim in (14, 15) and ra == 0:
                define(rd, ("c", add & FULL))
            else:
                value = read(ra)
                if value and value[0] == "a" and value[3] == FULL:
                    define(rd, ("a", value[1], value[2] + add, FULL))
                elif value and value[0] == "c":
                    define(rd, ("c", (value[1] + add) & FULL))
                elif value and value[0] in ("d", "i"):
                    define(rd, value)
                else:
                    define(rd, None)
                if prim == 13:
                    self.compare_constant(new[rd], 0, accessed)
        elif prim == 20:
            read(rd), read(ra)
            define(ra, None)
        elif prim == 21:
            value = read(rd)
            sh, mb, me = (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
            if value and value[0] in ("a", "f") and sh == 0:
                mask = rotate_mask(mb, me)
                define(ra, masked(value, mask))
            elif value and value[0] in ("a", "d"):
                define(ra, ("d", value[1]))
            else:
                define(ra, None)
        elif prim == 23:
            value = read(rd)
            read(rb)
            define(ra, ("d", value[1]) if value and value[0] in ("a", "d") else None)
        elif prim in (24, 25, 26, 27, 28, 29):
            value = read(rd)
            imm = uimm << 16 if prim in (25, 27, 29) else uimm
            if prim in (28, 29) and value and value[0] in ("a", "f"):
                define(ra, masked(value, imm))
            elif prim in (24, 25) and imm == 0:
                define(ra, value)
            elif prim == 24 and value and value[0] == "c":
                define(ra, ("c", value[1] | imm))
            else:
                define(ra, None)
        elif prim in LOADS or prim in STORES:
            size = LOADS.get(prim) or STORES[prim]
            store = prim in STORES
            base = read(ra) if ra else None
            if store:
                read(rd)
            value, acc = self.access(base, simm, size, store, False, accessed)
            if prim & 1 and ra:  # update forms
                define(ra, ("a", base[1], base[2] + simm, FULL) if base and base[0] == "a" else None)
            if not store:
                define(rd, value)
            return [state(next_pc, acc)]
        elif prim in (46, 47):
            if prim == 46:
                for n in range(rd, 32):
                    define(n, None)
        elif prim in FLOAT_LOADS or prim in FLOAT_STORES:
            size = FLOAT_LOADS.get(prim) or FLOAT_STORES[prim]
            store = prim in FLOAT_STORES
            base = read(ra) if ra else None
            if store:
                read_fpr(rd)
            _, acc = self.access(base, simm, size, store, True, accessed)
            if prim & 1 and ra:
                define(ra, ("a", base[1], base[2] + simm, FULL) if base and base[0] == "a" else None)
            return [state(next_pc, acc, fp_written if store else fp_written | {rd})]
        elif prim in PSQ_LOADS or prim in PSQ_STORES:
            store = prim in PSQ_STORES
            size = 4 if w & 0x8000 else 8  # W: one float, or a pair
            off = _signed(w & 0xFFF, 12)
            base = read(ra) if ra else None
            if store:
                read_paired(rd)
            _, acc = self.access(base, off, size, store, False, accessed)
            if not store and base and base[0] == "a" and base[3] == FULL:
                for i in range(0, size, 4):
                    self.shape.float_fields.add((base[1], base[2] + off + i, 4))
            if prim & 1 and ra:
                define(ra, ("a", base[1], base[2] + off, FULL) if base and base[0] == "a" else None)
            return [state(next_pc, acc, fp_written if store else fp_written | {rd})]
        elif prim == 4:
            xo5 = (w >> 1) & 31
            frc = (w >> 6) & 31
            if xo == 1014:  # dcbz_l
                return [state(next_pc)]
            if xo5 in PAIRED_A_FORM:
                uses, defines = PAIRED_A_FORM[xo5], True
            elif xo in PAIRED_X_FORM:
                uses, defines = PAIRED_X_FORM[xo], xo not in (0, 32, 64, 96)
            else:
                raise UnknownInstruction(f"paired-single op {xo} at {pc:08X}")
            for letter, reg in (("a", ra), ("b", rb), ("c", frc)):
                if letter in uses:
                    read_paired(reg)
            return [state(next_pc, None, fp_written | {rd} if defines else fp_written)]
        elif prim in (59, 63):
            xo5 = (w >> 1) & 31
            frc = (w >> 6) & 31
            if xo5 in FLOAT_A_FORM and (prim == 59 or xo5 >= 18):
                uses = FLOAT_A_FORM[xo5]
                defines = True
            elif prim == 63 and xo in FLOAT_X_FORM:
                uses = FLOAT_X_FORM[xo]
                defines = xo not in (0, 32)
            elif prim == 63 and xo in (38, 70, 134, 583, 711):
                uses, defines = "", xo == 583
            else:
                raise UnknownInstruction(f"floating-point op {prim}/{xo} at {pc:08X}")
            for letter, reg in (("a", ra), ("b", rb), ("c", frc)):
                if letter in uses:
                    read_fpr(reg)
            return [state(next_pc, None, fp_written | {rd} if defines else fp_written)]
        elif prim == 31:
            if xo in (0, 32):
                self.compare_registers(read(ra), read(rb), accessed)
            elif xo in X_LOADS or xo in X_STORES or xo in X_FLOAT_LOADS or xo in X_FLOAT_STORES:
                store = xo in X_STORES or xo in X_FLOAT_STORES
                fp = xo in X_FLOAT_LOADS or xo in X_FLOAT_STORES
                size = {**X_LOADS, **X_STORES, **X_FLOAT_LOADS, **X_FLOAT_STORES}[xo]
                a, b = read(ra) if ra else None, read(rb)
                base = None
                for one in (a, b):
                    if one and one[0] == "a" and one[3] == FULL and not base:
                        base = ("i", one[1])
                    elif one and one[0] == "i":
                        base = one
                value, acc = self.access(base, 0, size, store, fp, accessed)
                if store and not fp:
                    read(rd)
                if store and fp:
                    read_fpr(rd)
                if not store and not fp:
                    define(rd, value)
                return [state(next_pc, acc, fp_written | {rd} if fp and not store else fp_written)]
            elif xo in X_RD_AB:
                a, b = read(ra), read(rb)
                if xo in (266, 10):  # add: a pointer and an index, or a value and a constant
                    def pointer(p, q):
                        if p and p[0] == "a" and p[3] == FULL:
                            if q and q[0] == "c":
                                return ("a", p[1], p[2] + _signed(q[1], 32), FULL)
                            if q and (q[0] in ("d", "a", "f")):
                                return ("i", p[1])
                        return None
                    define(rd, pointer(a, b) or pointer(b, a) or (("i", a[1]) if a and a[0] == "i" else None))
                else:
                    source = a if a and a[0] in ("a", "d") else (b if b and b[0] in ("a", "d") else None)
                    define(rd, ("d", source[1]) if source else None)
            elif xo in X_RD_A:
                a = read(ra)
                define(rd, ("d", a[1]) if a and a[0] in ("a", "d") else None)
            elif xo in X_RA_SB:
                s, b = read(rd), read(rb)
                if xo == 444 and rd == rb:  # mr
                    tested = bool(rc and s and s[0] == "f")
                    define(ra, s[:5] + (True,) if tested else s)
                    if rc:
                        self.compare_constant(s, 0, accessed)
                else:
                    source = s if s and s[0] in ("a", "d") else (b if b and b[0] in ("a", "d") else None)
                    define(ra, ("d", source[1]) if source else None)
            elif xo in X_RA_S:
                s = read(rd)
                if xo in (954, 922):  # extsb, extsh: the compare still bounds the same value
                    define(ra, s)
                else:
                    define(ra, ("d", s[1]) if s and s[0] in ("a", "d") else None)
            elif xo in X_RD_ONLY:
                define(rd, None)
            elif xo in X_NONE:
                pass
            else:
                raise UnknownInstruction(f"op 31/{xo} at {pc:08X}")
        else:
            raise UnknownInstruction(f"op {prim} at {pc:08X}")
        return [state(next_pc)]


def masked(value: tuple, mask: int) -> tuple:
    """An argument or a loaded value after an AND with `mask`."""
    return value[:3] + (value[3] & mask,) if value[0] == "a" else value[:4] + (value[4] & mask, value[5])


def widened(value: Value) -> Value:
    """The value a loop may be stepping: constants and offsets are forgotten, an argument plus an offset is an index."""
    if value and value[0] == "c":
        return None
    if value and value[0] == "a" and value[2]:
        return ("i", value[1])
    return value


def shape_of(words: list[int], address: int) -> Shape:
    return Walker(words, address).run()


# --- required kinds ---------------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class Required:
    """One requirement: `kind`, its target and, for a value kind, the value a case must show."""
    kind: str
    target: str
    value: str = ""

    def __str__(self) -> str:
        return f"{self.kind}:{self.target}" + (f"={self.value}" if self.value else "")


def pointers(shape: Shape) -> set[int]:
    return set(shape.accesses) | shape.indexed


def ranges_of(shape: Shape, n: int) -> tuple[int, int, bool]:
    """(first offset, end offset, stores through it) of the direct accesses of pointer n."""
    accesses = shape.accesses.get(n, set())
    return (min(o for o, _, _ in accesses), max(o + s for o, s, _ in accesses), any(st for _, _, st in accesses))


def overlap_pairs(shape: Shape) -> list[tuple[int, int]]:
    """(destination, source) of each pair of pointer arguments with direct accesses, one of them written through.
    A pointer reached with a computed offset has no range: left out."""
    direct = sorted(n for n in shape.accesses if n not in shape.indexed)
    out = []
    for i, a in enumerate(direct):
        for b in direct[i + 1:]:
            a_stores, b_stores = ranges_of(shape, a)[2], ranges_of(shape, b)[2]
            if a_stores:
                out.append((a, b))
            elif b_stores:
                out.append((b, a))
    return out


def bound_values(n: int) -> list[int]:
    return [(n - 1) & FULL, n & FULL, (n + 1) & FULL]


def field_values(c: int, mask: int, size: int) -> list[int]:
    """N-1, N, N+1 as a field of `size` bytes holds them, under the mask of the compare."""
    limit = (1 << (8 * size)) - 1
    return sorted({v & mask & limit for v in bound_values(c)})


def scalar_bounds(shape: Shape, n: int) -> list[tuple[int, int]]:
    return sorted(shape.constants.get(n, ()))


def sentinels(shape: Shape, n: int) -> list[int]:
    """Constants a pointer argument is compared with before any access through it (never 0, never masked)."""
    if n in shape.late_constants:
        return []
    return sorted(c for c, mask in shape.constants.get(n, ()) if c and mask == FULL)


def required_kinds(shape: Shape) -> list[Required]:
    if not shape.leaf or shape.unknown:
        return []
    out: list[Required] = []
    ptrs = pointers(shape)
    for n in sorted(shape.tested & ptrs):
        out.append(Required("null", f"r{n}"))
    for n in sorted(ptrs):
        for c in sentinels(shape, n):
            out.append(Required("sentinel", f"r{n}", f"{c:08X}"))
    for n in sorted(shape.constants):
        if n in ptrs:
            continue
        for c, mask in scalar_bounds(shape, n):
            for v in bound_values(c):
                out.append(Required("bound", f"r{n}&{mask:08X}", f"{v & mask:08X}"))
    for (n, off, size), constants in sorted(shape.field_constants.items()):
        if (n, off) in shape.chased:
            continue
        for c, mask in sorted(constants):
            for v in field_values(c, mask, size):
                out.append(Required("field_bound", f"r{n}+{off:X}/{size}&{mask:08X}", f"{v:08X}"))
    for s, n, off, size, mask in sorted(shape.relations):
        if s in ptrs or (n, off) in shape.chased:
            continue
        for d in (-1, 0, 1):
            out.append(Required("relation", f"r{s}~r{n}+{off:X}/{size}&{mask:08X}", f"{d:+d}"))
    for n in sorted(shape.float_args):
        for c in FLOAT_CLASSES:
            out.append(Required("float", f"f{n}", c))
    for n in sorted(shape.paired_args):
        for c in FLOAT_CLASSES:
            out.append(Required("float_ps", f"p{n}", c))
    for n, off, size in sorted(shape.float_fields):
        for c in FLOAT_CLASSES:
            out.append(Required("float_field", f"r{n}+{off:X}/{size}", c))
    for dst, src in overlap_pairs(shape):
        out.append(Required("overlap_before", f"r{dst}<-r{src}"))
        out.append(Required("overlap_after", f"r{dst}<-r{src}"))
    return out


# --- templates --------------------------------------------------------------------------------------------------------

@dataclass
class Number:
    relative: bool
    value: int

    def text(self) -> str:
        if not self.relative:
            return f"{self.value:016X}" if self.value > FULL else f"{self.value & FULL:08X}"
        return "S" if not self.value else f"S{'+' if self.value > 0 else '-'}{abs(self.value):X}"


def parse_number(text: str) -> Number:
    if text.startswith("S"):
        return Number(True, 0 if text == "S" else int(text[2:], 16) * (-1 if text[1] == "-" else 1))
    return Number(False, int(text, 16))


@dataclass
class Template:
    label: str
    area: int = 0x100
    regs: dict[str, Number] = field(default_factory=dict)  # "r3" -> value (fN, pN: hexadecimal bits, not relative)
    mem: list[tuple[int, int, Number]] = field(default_factory=list)  # applied in order, a later line wins
    has_calls: bool = False
    reserve: str | None = None

    def copy(self) -> "Template":
        return Template(self.label, self.area, dict(self.regs), list(self.mem), self.has_calls, self.reserve)

    def base(self, n: int) -> int | None:
        """Offset from S of the pointer in rN, None if the template does not set it relative to S."""
        value = self.regs.get(f"r{n}")
        return value.value if value and value.relative else None

    def bytes_at(self) -> dict[int, tuple]:
        """{offset: byte, or (bit shift, Number) of a relative value}, after every mem line."""
        out: dict[int, tuple] = {}
        for off, size, number in self.mem:
            for i in range(size):
                out[off + i] = (number.text(), size - 1 - i)
        return out

    def word_at(self, off: int, size: int) -> int:
        """The value at `off`: 0 for an unset byte; a relative value counts as 0 (only constants are bounded)."""
        value = 0
        last: dict[int, int] = {}
        for at, sz, number in self.mem:
            for i in range(sz):
                last[at + i] = 0 if number.relative else (number.value >> (8 * (sz - 1 - i))) & 0xFF
        for i in range(size):
            value = (value << 8) | last.get(off + i, 0)
        return value

    def set_memory(self, off: int, size: int, value: int) -> bool:
        if off < 0 or off + size > MAX_AREA:
            return False
        self.mem.append((off, size, Number(False, value)))
        self.area = max(self.area, min(MAX_AREA, (off + size + 3) & ~3))
        return True

    def key(self) -> tuple:
        last: dict[int, tuple] = {}
        for off, size, number in self.mem:
            for i in range(size):
                last[off + i] = (number.relative, number.value, size, i) if number.relative else \
                    (0, (number.value >> (8 * (size - 1 - i))) & 0xFF)
        last = {k: v for k, v in last.items() if v != (0, 0)}
        return (tuple(sorted((k, v.text()) for k, v in self.regs.items())), tuple(sorted(last.items())), self.reserve)


LINE = re.compile(r"^(r|f|p)(\d+)\s*=\s*(\S+)$")


def parse_templates(text: str, function: str) -> list[Template]:
    """The cases of `function` in a hand-written file (grammar: gc_oracle.cpp, "Synthetic cases")."""
    out: list[Template] = []
    current: Template | None = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        words = line.split()
        if words[0] == "case":
            current = Template(words[2]) if len(words) >= 3 and words[1] == function else None
        elif current is None:
            continue
        elif words[0] == "area":
            current.area = int(words[1], 16)
        elif words[0] == "mem":
            current.mem.append((parse_number(words[1]).value, int(words[2], 0), parse_number(words[3])))
        elif words[0] == "call":
            current.has_calls = True
        elif words[0] == "reserve":
            current.reserve = words[1]
        elif words[0] == "end":
            out.append(current)
            current = None
        else:
            m = LINE.match(line)
            if m:
                current.regs[m[1] + m[2]] = parse_number(m[3])
    return out


def bare_template(shape: Shape) -> Template:
    """A zeroed area with each pointer apart from the others, each scalar 1."""
    t = Template("bare")
    cursor = 0x10
    for n in sorted(shape.gpr_args):
        if n in pointers(shape):
            accesses = shape.accesses.get(n, set())
            low = min((o for o, _, _ in accesses), default=0)
            high = max((o + s for o, s, _ in accesses), default=0x10)
            t.regs[f"r{n}"] = Number(True, cursor - low)
            cursor += (high - low + 0x13) & ~0xF
        else:
            t.regs[f"r{n}"] = Number(False, 1)
    t.area = min(MAX_AREA, max(0x100, (cursor + 0xFF) & ~0xFF))
    return t


# --- the grid ---------------------------------------------------------------------------------------------------------

def pattern_byte(offset: int) -> int:
    """Invented bytes, different from their neighbours, whose words read as ordinary floats."""
    return 0x41 + (offset * 3) % 61


def addresses_hold(shape: Shape, t: Template, skip: int | None = None) -> bool:
    """True when the template gives every field used as an address without a test (skip: the argument the case nulls)
    a pointer of its own: a case that moves the function to another path must not reach an address nothing set (the
    decompiled version would stop the run)."""
    relative = {}
    for off, _, number in t.mem:
        relative[off] = number.relative
    for n, off in shape.unguarded:
        base = t.base(n)
        if n != skip and base is not None and not relative.get(base + off, False):
            return False
    return True


def protected_offsets(shape: Shape, n: int) -> set[int]:
    """Offsets from pointer n an overlap pattern must keep: fields used as addresses or compared (their value chooses
    the path, and an address must stay valid)."""
    out = set()
    for arg, off in shape.chased:
        if arg == n:
            out.update(range(off, off + 4))
    for (arg, off, size) in shape.field_constants:
        if arg == n:
            out.update(range(off, off + size))
    for s, arg, off, size, _ in shape.relations:
        if arg == n:
            out.update(range(off, off + size))
    return out


def overlap_shifts(shape: Shape, dst: int, src: int) -> list[int]:
    """Values of (source pointer - destination pointer) to try: each pair of accessed offsets on the same address, and
    the source's start a byte or two either side of the destination's (A19 was a partial overlap)."""
    d0, d1, _ = ranges_of(shape, dst)
    s0, s1, _ = ranges_of(shape, src)
    shifts = {od - os for od, _, _ in shape.accesses[dst] for os, _, _ in shape.accesses[src]}
    shifts |= {d0 - s0 + delta for delta in (-2, -1, 1, 2)}
    return sorted(s for s in shifts if s + s0 < d1 and d0 < s + s1)


def overlap_case(shape: Shape, t: Template, dst: int, src: int, shift: int) -> Template | None:
    """The template with the source moved to destination + shift, the union of both ranges filled with a pattern,
    except what must keep its value; the source's own fields follow it. None when it cannot be placed."""
    d_base, s_base = t.base(dst), t.base(src)
    if d_base is None:
        return None
    d0, d1, _ = ranges_of(shape, dst)
    s0, s1, _ = ranges_of(shape, src)
    new_src = d_base + shift
    low = min(d_base + d0, new_src + s0)
    high = max(d_base + d1, new_src + s1)
    if low < 0 or high > MAX_AREA:
        return None
    dst_keep = {d_base + o for o in protected_offsets(shape, dst)}
    src_keep = {new_src + o for o in protected_offsets(shape, src)}
    for arg, off in shape.chased:  # an address in the other object's range: nothing proves it stays valid
        if arg == dst and new_src + s0 < d_base + off + 4 and d_base + off < new_src + s1:
            return None
        if arg == src and d_base + d0 < new_src + off + 4 and new_src + off < d_base + d1:
            return None
    case = t.copy()
    case.regs[f"r{src}"] = Number(True, new_src)
    set_bytes = set(t.bytes_at())
    for at in range(low, high):
        if at not in set_bytes and at not in dst_keep and at not in src_keep:
            case.mem.append((at, 1, Number(False, pattern_byte(at))))
    if s_base is not None:  # the source's fields the template set move with it
        for off, size, number in t.mem:
            if s_base + s0 <= off and off + size <= s_base + s1:
                moved = off - s_base + new_src
                if moved < 0 or moved + size > MAX_AREA:
                    return None
                case.mem.append((moved, size, number))
    case.area = max(case.area, min(MAX_AREA, (high + 3) & ~3))
    return case


def grid_cases(shape: Shape, templates: list[Template]) -> tuple[list[tuple[str, Template]], Counter]:
    """([(label, starting state)], kinds generated) of one function: every value of every kind on every template,
    the states a template already is left out, nothing twice."""
    out: list[tuple[str, Template]] = []
    counts: Counter = Counter()
    seen = {t.key() for t in templates}
    usable = [t for t in templates if not t.has_calls] or ([bare_template(shape)] if not templates else [])
    if not usable:
        return [], counts
    ptrs = pointers(shape)

    def add(kind: str, detail: str, t: Template, case: Template | None) -> None:
        if case is None:
            return
        key = case.key()
        if key in seen:
            return
        seen.add(key)
        counts[kind] += 1
        out.append((f"grid.{kind}.{detail}.{t.label}", case))

    def with_reg(t: Template, reg: str, value: int) -> Template:
        case = t.copy()
        case.regs[reg] = Number(False, value & (FULL if reg[0] == "r" else (1 << 64) - 1))
        return case

    for t in usable:
        for n in sorted(shape.tested & ptrs):
            add("null", f"r{n}", t, with_reg(t, f"r{n}", 0))
        if not addresses_hold(shape, t):
            counts["template_unsafe"] += 1
            continue
        for n in sorted(ptrs):
            for c in sentinels(shape, n):
                add("sentinel", f"r{n}_{c:08X}", t, with_reg(t, f"r{n}", c))
        for n in sorted(shape.constants):
            if n in ptrs:
                continue
            for c, mask in scalar_bounds(shape, n):
                base = t.regs[f"r{n}"].value if f"r{n}" in t.regs and not t.regs[f"r{n}"].relative else 0
                for v in bound_values(c):
                    value = (base & ~mask) | (v & mask)
                    add("bound", f"r{n}_{v:08X}", t, with_reg(t, f"r{n}", value))
        for (n, off, size), constants in sorted(shape.field_constants.items()):
            base = t.base(n)
            if (n, off) in shape.chased or base is None:
                continue
            for c, mask in sorted(constants):
                for v in bound_values(c):
                    case = t.copy()
                    limit = (1 << (8 * size)) - 1
                    word = (t.word_at(base + off, size) & ~mask) | (v & mask)
                    if case.set_memory(base + off, size, word & limit):
                        add("field_bound", f"r{n}_{off:X}_{v & limit:X}", t, case)
        for s, n, off, size, mask in sorted(shape.relations):
            base = t.base(n)
            if s in ptrs or (n, off) in shape.chased or base is None or base + off < 0 or base + off + size > MAX_AREA:
                continue
            value = t.word_at(base + off, size) & mask
            for d in (-1, 0, 1):
                add("relation", f"r{s}_{d:+d}_r{n}_{off:X}", t, with_reg(t, f"r{s}", (value + d) & FULL))
        for n in sorted(shape.float_args):
            for name, bits in FLOAT_REGISTER + FLOAT_REGISTER_DOUBLE:
                add("float", f"f{n}_{name}", t, with_reg(t, f"f{n}", int(bits, 16)))
        for n in sorted(shape.paired_args):
            for name, bits in FLOAT_REGISTER + FLOAT_REGISTER_DOUBLE:
                add("float_ps", f"p{n}_{name}", t, with_reg(t, f"p{n}", int(bits, 16)))
        float_args = sorted(shape.float_args)
        for i, a in enumerate(float_args):
            for b in float_args[i + 1:]:
                for first, second, tag in ((a, b, "qs"), (b, a, "sq")):
                    case = with_reg(t, f"f{first}", int(NAN_FIRST, 16))
                    case.regs[f"f{second}"] = Number(False, int(NAN_SECOND, 16))
                    add("nan_pair", f"f{first}_f{second}_{tag}", t, case)
        for n, off, size in sorted(shape.float_fields):
            base = t.base(n)
            if base is None:
                continue
            for name, bits in FLOAT_MEMORY if size == 4 else FLOAT_REGISTER:
                case = t.copy()
                word = bits if size == 4 else int(bits, 16)
                ok = case.set_memory(base + off, size, word) if size == 4 else \
                    case.set_memory(base + off, 4, word >> 32) and case.set_memory(base + off + 4, 4, word & FULL)
                if ok:
                    add("float_field", f"r{n}_{off:X}_{name}", t, case)
        for dst, src in overlap_pairs(shape):
            if t.base(src) is None and f"r{src}" in t.regs:
                continue  # a template that nulls the source shows nothing about an overlap
            d0, _, _ = ranges_of(shape, dst)
            s0, _, _ = ranges_of(shape, src)
            for shift in overlap_shifts(shape, dst, src):
                start = shift + s0 - d0
                name = "overlap_same" if not shift else ("overlap_before" if start < 0 else
                                                         ("overlap_after" if start > 0 else "overlap_start"))
                add(name, f"r{dst}_r{src}_{'m' if shift < 0 else 'p'}{abs(shift):X}", t,
                    overlap_case(shape, t, dst, src, shift))
    return out, counts


def case_text(function: str, label: str, case: Template) -> list[str]:
    lines = [f"case {function} {label}"]
    if case.area != 0x100:
        lines.append(f"area {case.area:X}")
    for reg in sorted(case.regs, key=lambda r: (r[0], int(r[1:]))):
        number = case.regs[reg]
        text = number.text() if reg[0] == "r" or number.relative else f"{number.value:016X}"
        lines.append(f"{reg} = {text}")
    if case.reserve:
        lines.append(f"reserve {case.reserve}")
    for off, size, number in case.mem:
        lines.append(f"mem S+{off:X} {size} {number.text()}")
    lines.append("end")
    return lines


# --- what the runs covered --------------------------------------------------------------------------------------------

def float_class(bits: int) -> str | None:
    """Class of a double-precision register image, None for an ordinary number."""
    sign, exponent, fraction = bits >> 63, (bits >> 52) & 0x7FF, bits & ((1 << 52) - 1)
    if exponent == 0x7FF:
        if fraction == 0:
            return "ninf" if sign else "pinf"
        return "qnan" if fraction >> 51 else "snan"
    if exponent == 0 and fraction == 0:
        return "nzero" if sign else "pzero"
    if exponent == 0 or 874 <= exponent <= 896:
        return "denormal"
    return None


def single_class(bits: int) -> str | None:
    sign, exponent, fraction = bits >> 31, (bits >> 23) & 0xFF, bits & 0x7FFFFF
    if exponent == 0xFF:
        if fraction == 0:
            return "ninf" if sign else "pinf"
        return "qnan" if fraction >> 22 else "snan"
    if exponent == 0:
        if fraction == 0:
            return "nzero" if sign else "pzero"
        return "denormal"
    return None


def case_values(record: dict) -> tuple[list[int], list[int], list[int], dict[int, tuple[int, int]]]:
    """(gpr, fpr, ps1, {address: (size, value)}) of a stored case: entry registers, and the bytes the decompiled
    version read, which is what the oracle kept of the memory."""
    gpr = [int(x, 16) for x in record["entry"]["gpr"]]
    fpr = [int(x, 16) for x in record["entry"]["fpr"]]
    ps1 = [int(x, 16) for x in record["entry"].get("ps1", ())]
    reads = {}
    for address, size, value in record.get("reads", ()):
        reads[int(address, 16)] = (int(size), int(value, 16))
    return gpr, fpr, ps1, reads


def read_at(reads: dict[int, tuple[int, int]], address: int, size: int) -> int | None:
    """The `size` bytes at `address` from the recorded reads, None if they were not all read (reads are recorded by
    access, at least as wide as the field the decompiled code loaded)."""
    if address in reads and reads[address][0] == size:
        return reads[address][1] & ((1 << (8 * size)) - 1)
    for at, (width, value) in reads.items():
        if at <= address and address + size <= at + width:
            return (value >> (8 * (at + width - address - size))) & ((1 << (8 * size)) - 1)
    return None


def satisfied(shape: Shape, record: dict) -> set[Required]:
    """The requirements one stored case meets, from its values (a hand-written case or a real call counts as much as
    a generated one)."""
    gpr, fpr, ps1, reads = case_values(record)
    out: set[Required] = set()
    ptrs = pointers(shape)
    for n in sorted(shape.tested & ptrs):
        if gpr[n] == 0:
            out.add(Required("null", f"r{n}"))
    for n in sorted(ptrs):
        for c in sentinels(shape, n):
            if gpr[n] == c:
                out.add(Required("sentinel", f"r{n}", f"{c:08X}"))
    for n in sorted(shape.constants):
        if n in ptrs:
            continue
        for c, mask in scalar_bounds(shape, n):
            for v in bound_values(c):
                if gpr[n] & mask == v & mask:
                    out.add(Required("bound", f"r{n}&{mask:08X}", f"{v & mask:08X}"))
    for (n, off, size), constants in shape.field_constants.items():
        if (n, off) in shape.chased:
            continue
        value = read_at(reads, (gpr[n] + off) & FULL, size)
        if value is None:
            continue
        for c, mask in constants:
            for v in field_values(c, mask, size):
                if value & mask == v:
                    out.add(Required("field_bound", f"r{n}+{off:X}/{size}&{mask:08X}", f"{v:08X}"))
    for s, n, off, size, mask in shape.relations:
        if s in ptrs or (n, off) in shape.chased:
            continue
        value = read_at(reads, (gpr[n] + off) & FULL, size)
        if value is None:
            continue
        d = _signed((gpr[s] - (value & mask)) & FULL, 32)
        if d in (-1, 0, 1):
            out.add(Required("relation", f"r{s}~r{n}+{off:X}/{size}&{mask:08X}", f"{d:+d}"))
    for n in shape.float_args:
        c = float_class(fpr[n])
        if c:
            out.add(Required("float", f"f{n}", c))
    for n in shape.paired_args:
        c = float_class(ps1[n]) if ps1 else None
        if c:
            out.add(Required("float_ps", f"p{n}", c))
    for n, off, size in shape.float_fields:
        value = read_at(reads, (gpr[n] + off) & FULL, size)
        if value is not None:
            c = single_class(value) if size == 4 else float_class(value)
            if c:
                out.add(Required("float_field", f"r{n}+{off:X}/{size}", c))
    for dst, src in overlap_pairs(shape):
        if not gpr[dst] or not gpr[src]:
            continue
        d0, d1, _ = ranges_of(shape, dst)
        s0, s1, _ = ranges_of(shape, src)
        a0, a1, b0, b1 = gpr[dst] + d0, gpr[dst] + d1, gpr[src] + s0, gpr[src] + s1
        if b0 < a1 and a0 < b1 and b0 != a0:
            out.add(Required("overlap_before" if b0 < a0 else "overlap_after", f"r{dst}<-r{src}"))
    return out


# --- commands ---------------------------------------------------------------------------------------------------------

def shapes_of(game_cfg: dict, version: str, status: str | None) -> dict[str, tuple[int, Shape]]:
    """{function: (address, shape)} of the enabled rows of the table in `status` (None: every enabled row)."""
    import decomp_functions as df
    import toolchain as tc
    manifest = tc.GENERATED / game_cfg["_dir"].name / version / "manifest.json"
    if not manifest.exists():
        raise SystemExit(f"{manifest} missing: extract the disc first (tools/extract.py)")
    disc_root = Path(json.loads(manifest.read_text(encoding="utf-8"))["disc_root"])
    if not disc_root.exists():  # a worktree: the manifest names the main checkout's folder
        raise SystemExit(f"{disc_root} missing")
    words: dict[int, list[int]] = {}
    entries = df.resolve(game_cfg, version, disc_root, df._layout(game_cfg, disc_root), words=words)
    return {f.name: (a, shape_of(words[a], a)) for a, f in entries if status is None or f.status == status}


def templates_of(game_cfg: dict, name: str) -> list[Template]:
    folder = game_cfg["_dir"] / "decomp" / "oracle_cases"
    out: list[Template] = []
    for path in sorted(folder.glob("*.txt"), key=lambda p: p.name):
        text = path.read_text(encoding="utf-8")
        if f"case {name} " in text:
            out += parse_templates(text, name)
    return out


def cases_command(args, oracle) -> int:
    """`cases --grid`: writes <out>/<ID>/grid_<function>.txt for every version, deterministically."""
    import toolchain as tc
    if not args.grid:
        raise SystemExit("cases: --grid is the only mode (generated value grids)")
    game = oracle.game_name(args.game)
    game_cfg = tc.load_game(game)
    out = Path(args.out or DEFAULT_OUT)
    out = out if out.is_absolute() else tc.ROOT / out
    build_root = (tc.ROOT / "build").resolve()
    if build_root not in out.resolve().parents:
        raise SystemExit(f"--out {out}: must be a folder under build/ (generated cases are never tracked)")
    wanted = [x for x in (args.functions or "").split(",") if x]
    totals: dict[str, Counter] = {}
    per_version: dict[str, dict[str, str]] = {}
    for version in oracle.versions_of(game_cfg, args.versions):
        shapes = shapes_of(game_cfg, version, args.status)
        unknown = [n for n in wanted if n not in shapes]
        if unknown:
            raise SystemExit(f"--functions: not rows of status {args.status}: {', '.join(unknown)}")
        folder = out / version
        if not args.list:
            folder.mkdir(parents=True, exist_ok=True)
            for old in folder.glob("grid_*.txt"):
                old.unlink()
        totals[version] = Counter()
        per_version[version] = {}
        applied = kindless = no_shape = 0
        for name in sorted(shapes):
            if wanted and name not in wanted:
                continue
            _, shape = shapes[name]
            required = required_kinds(shape)
            if shape.unknown or not shape.leaf:
                no_shape += 1
                print(f"{version} {name}: no shape ({shape.unknown or 'not a leaf'})")
                continue
            if not required:
                kindless += 1
                continue
            cases, counts = grid_cases(shape, templates_of(game_cfg, name))
            applied += 1
            totals[version].update(counts)
            text = [f"# Generated by tools/decomp_oracle.py cases --grid: invented values on the hand-written cases of "
                    f"{name}. Never tracked."]
            for label, case in cases:
                text += case_text(name, label, case)
            body = "\n".join(text) + "\n"
            per_version[version][name] = body
            if not args.list:
                (folder / f"grid_{name}.txt").write_text(body, encoding="utf-8", newline="\n")
            line = ", ".join(f"{k} {v}" for k, v in sorted(counts.items()))
            if args.list or args.verbose:
                print(f"{version} {name}: {len(cases)} cases ({line}); required: {len(required)}")
        print(f"== {version}: {applied} functions with a required kind, {kindless} without, {no_shape} without a shape, "
              f"{sum(totals[version].values())} cases: "
              f"{', '.join(f'{k} {v}' for k, v in sorted(totals[version].items()))}")
        if not args.list:
            print(f"== {version}: grid hash {oracle.synthetic_hash(game_cfg, folder)} (the `synthetic` of a run on it)")
    versions = list(per_version)
    if len(versions) > 1:
        differ = sorted(n for n in set().union(*per_version.values())
                        if len({per_version[v].get(n) for v in versions}) > 1)
        print(f"== {len(differ)} functions with different grids between {', '.join(versions)}"
              + (f": {', '.join(differ)}" if differ else ""))
    return 0


def records(oracle, bench: Path, game: str, label: str, version: str, *kinds: str):
    """The records of these kinds in the oracle files of (label, version). An unreadable line is skipped here: `report`
    counts it as a problem of its own, and this module only prints warnings."""
    wanted = [f'"kind":"{k}"' for k in kinds]
    for path in sorted(oracle.oracle_dir(bench, game, label, version).glob("oracle-*.jsonl")):
        with path.open(encoding="utf-8", errors="replace") as f:
            for line in f:
                if any(w in line for w in wanted):
                    try:
                        yield json.loads(line)
                    except json.JSONDecodeError:
                        continue


def seen_requirements(oracle, bench: Path, game: str, labels: list[str], version: str,
                      shapes: dict[str, tuple[int, Shape]]) -> dict[str, set[Required]]:
    """{function: requirements met} by the stored cases of `labels` in `version`."""
    out: dict[str, set[Required]] = defaultdict(set)
    for label in labels:
        for rec in records(oracle, bench, game, label, version, "case"):
            name = rec.get("function")
            if name in shapes:
                out[name] |= satisfied(shapes[name][1], rec)
    return out


def report_kinds(oracle, game_cfg: dict, bench: Path, game: str, labels: list[str], versions: list[str]) -> int:
    """Warnings of `report`: a required kind no stored case shows. Never a problem: nothing here refuses a promotion
    (refusing would change what `verified` means). Returns the number of functions with a missing kind."""
    print("Required case kinds (shape of the original; warning only, never refuses a promotion):")
    flagged: set[str] = set()
    for version in versions:
        try:
            shapes = shapes_of(game_cfg, version, None)
        except SystemExit as e:
            print(f"  {version}: not checked: {e}")
            continue
        seen = seen_requirements(oracle, bench, game, labels, version, shapes)
        checked = 0
        total = 0
        for name in sorted(shapes):
            shape = shapes[name][1]
            if shape.unknown or not shape.leaf:
                print(f"  {version} {name}: no shape ({shape.unknown or 'not a leaf'}): kinds not checked")
                continue
            required = set(required_kinds(shape))
            if not required:
                continue
            checked += 1
            total += len(required)
            missing = sorted(str(r) for r in required - seen.get(name, set()))
            if missing:
                flagged.add(name)
                shown = ", ".join(missing[:8]) + (f", ... ({len(missing)} in all)" if len(missing) > 8 else "")
                print(f"  KIND MISSING {version} {name}: {shown}")
        print(f"  {version}: {checked} functions with a required kind, {total} requirements")
    print(f"  {len(flagged)} functions with a missing kind in at least one version"
          + (" (tools/decomp_oracle.py cases --grid, run --grid, then report --grid-label)" if flagged else ""))
    return len(flagged)


def label_kind(label: str) -> str:
    if not label:
        return "real calls"
    return label.split(".")[1] if label.startswith("grid.") else "hand"


# One register in the differences of a mismatch record, as describe() of runtime/core/decomp/gc_oracle_compare.h writes
# it. Anything else (a byte, a call, cr2-cr4, no-return, reservation, unjournaled) does not fit, so it is never allowed.
REGISTER_DIFFERENCE = re.compile(r"(?P<name>[rf]\d+(?:\.ps1)?)(?: bits)?: original (?P<original>[0-9A-F]+), "
                                 r"decompiled (?P<decompiled>[0-9A-F]+)")
ALLOWED_REGISTER = re.compile(r"(?P<name>[rf]\d+(?:\.ps1)?)(?::(?P<mask>[0-9A-Fa-f]+))?(?P<nan>:nan)?")
ALL_BITS = 0xFFFFFFFFFFFFFFFF


@dataclass(frozen=True)
class TriageRow:
    """A line of grid_triage.tsv. It sorts a difference only if the label matches and everything the record shows is
    allowed: `registers` is {register: bits that may differ}, `nan` the registers that must hold a NaN in both versions
    (a difference in the low bits of a number is an error, and a NaN turned into an infinity is too), and nothing
    written is ever allowed."""
    pattern: re.Pattern
    verdict: str
    registers: dict[str, int]
    nan: frozenset[str]
    reason: str

    def refused(self, differences: list[str]) -> list[str]:
        """The items of a record's differences this row does not allow. A record that lists nothing is refused too:
        nothing would show what is being sorted."""
        out = []
        for text in differences:
            m = REGISTER_DIFFERENCE.fullmatch(text)
            if m is None or m["name"] not in self.registers:
                out.append(text)
                continue
            values = int(m["original"], 16), int(m["decompiled"], 16)
            if (values[0] ^ values[1]) & ~self.registers[m["name"]] & ALL_BITS or \
                    (m["name"] in self.nan and not all(float_class(v) in ("qnan", "snan") for v in values)):
                out.append(text)
        return out if differences else ["no difference listed in the record"]


def sort_difference(rows: list[TriageRow], rec: dict) -> tuple[TriageRow | None, list[str]]:
    """(the row that sorts this mismatch record, None if it stays open; what a row of its label refused). A real call
    has no label, so it is never sorted."""
    label = rec.get("synthetic") or ""
    refused: list[str] = []
    for row in rows if label.startswith("grid.") else ():
        if row.pattern.search(label):
            refused = row.refused(rec.get("differences") or [])
            if not refused:
                return row, []
    return None, refused


@dataclass
class GridVerdict:
    """What report_grid hands to `report`. `open_differences`: {(version, function): differences no triage line sorts}.
    `unread`: what stops the grid from saying there is none (a label with no results, a run stopped before its end)."""
    open_differences: Counter
    unread: list[str]


def run_grid_hash(oracle, bench: Path, game: str, label: str, version: str) -> str:
    """The hash of the cases a stored grid run says it ran (`synthetic` of run-<ID>.json, written by `run --grid`)."""
    path = oracle.oracle_dir(bench, game, label, version) / f"run-{version}.json"
    if not path.exists():
        return f"unknown (no {path.name})"
    return json.loads(path.read_text(encoding="utf-8")).get("synthetic") or "none (run without cases)"


def report_grid(oracle, bench: Path, game: str, labels: list[str], versions: list[str], triage: dict,
                sources: str | None = None) -> GridVerdict:
    """`report --grid-label`: what the grid runs show, apart from the promotion rule. A difference a triage line allows
    prints SORTED under its class; any other prints DIFFERENCE, with what its line did not allow. The caller refuses
    the functions of `open_differences`, and only those; a sorted difference refuses nothing.

    Two counters answer "what was not compared", and they count different things: `cases` are the cases a run loaded
    (summary synthetic_not_compared), `real calls` the calls the scenario made (summary not_compared). One case can
    cost all the calls of a function: a store of the original outside RAM makes it not comparable (gc_oracle.cpp
    compare_call), and each later call and case of that function then runs the original alone. NOT COMPARED gives both
    and the reason, function by function; NOT RUN gives the cases that never ran (a case runs at the first call of its
    function). `sources`: current oracle sources hash; a grid run built from others is reported, and what it shows
    counts all the same: an open difference is not excused by its age."""
    print(f"Grid runs {', '.join(labels)} (they never promote; an open difference refuses its function):")
    open_differences: Counter = Counter()
    unread: list[str] = []
    for version in versions:
        compared: Counter = Counter()
        different: dict[tuple[str, str], list[dict]] = defaultdict(list)
        stats = Counter()
        partial: list[dict] = []
        for label in labels:
            if not any(oracle.oracle_dir(bench, game, label, version).glob("oracle-*.jsonl")):
                print(f"  {version}: no oracle file for {label}")
                unread.append(f"{version} {label}: no oracle file")
                continue
            print(f"  {version} {label}: grid hash {run_grid_hash(oracle, bench, game, label, version)}")
            for rec in records(oracle, bench, game, label, version, "header", "end", "summary", "case", "mismatch"):
                tag = rec.get("synthetic") or ""
                if rec["kind"] == "header":
                    stats["runs"] += 1
                    if sources and rec.get("sources") != sources:
                        print(f"  WARNING {version} {label}: built from sources {rec.get('sources')}, not the current "
                              f"{sources}: what follows is about another build")
                elif rec["kind"] == "end":
                    stats["ended"] += 1
                elif rec["kind"] == "summary":
                    stats["divergent"] += int(rec.get("fmadds_dolphin_divergent", 0))
                    stats["cases_not_compared"] += int(rec.get("synthetic_not_compared", 0))
                    stats["calls"] += int(rec.get("calls", 0))
                    stats["calls_not_compared"] += int(rec.get("not_compared", 0))
                    stats["calls_different"] += int(rec.get("mismatches", 0))
                    ran = int(rec.get("synthetic_compared", 0)) + int(rec.get("synthetic_not_compared", 0))
                    rec["never_ran"] = max(int(rec.get("synthetic_cases", 0)) - ran, 0)
                    stats["cases_never_ran"] += rec["never_ran"]
                    if rec["never_ran"] or int(rec.get("not_compared", 0)) or int(rec.get("synthetic_not_compared", 0)):
                        partial.append(rec)
                else:
                    if tag.startswith("grid."):
                        compared[(rec["function"], label_kind(tag))] += 1
                    if rec["kind"] == "mismatch":
                        different[(rec["function"], label_kind(tag))].append(rec)
        counts: Counter = Counter()
        for (name, kind), found in sorted(different.items()):
            outcome = [sort_difference(triage.get(name, []), rec) for rec in found]
            sorted_rows = [row for row, _ in outcome if row]
            still_open = [(rec, why) for rec, (row, why) in zip(found, outcome) if not row]
            for row in sorted_rows:
                counts[row.verdict] += 1
            counts["open"] += len(still_open)
            open_differences[(version, name)] += len(still_open)
            of = f"{{}} of {compared[(name, kind)]} cases" if compared[(name, kind)] else "{}"
            if sorted_rows:
                row = sorted_rows[0]
                print(f"  SORTED {version} {name} {kind}: {of.format(len(sorted_rows))}, class {row.verdict}, only "
                      f"{', '.join(row.registers)} within its allowed bits{', on a NaN' if row.nan else ''}, nothing "
                      f"written")
            if still_open:
                example, why = still_open[0]
                shown = why or example.get("differences", [])
                print(f"  DIFFERENCE {version} {name} {kind}: {of.format(len(still_open))}, e.g. "
                      f"{example.get('synthetic') or 'a real call'}: {'; '.join(shown[:3])}"
                      + (" (the label is on a triage line, which does not allow this)" if why else ""))
        for rec in partial:
            if int(rec.get("not_compared", 0)) or int(rec.get("synthetic_not_compared", 0)):
                print(f"  NOT COMPARED {version} {rec['function']}: {rec.get('not_compared', 0)} of "
                      f"{rec.get('calls', 0)} real calls, {rec.get('synthetic_not_compared', 0)} of "
                      f"{rec.get('synthetic_cases', 0)} cases: "
                      f"{rec.get('reason') or 'no reason recorded'}")
            if rec["never_ran"]:
                print(f"  NOT RUN {version} {rec['function']}: {rec['never_ran']} of {rec.get('synthetic_cases', 0)} "
                      f"cases (the scenarios of this run did not call the function)")
        cut_short = stats["runs"] - stats["ended"]
        if cut_short:
            print(f"  WARNING {version}: {cut_short} of {stats['runs']} runs have no end line (stopped early): cases "
                  f"and calls after the stop appear nowhere below")
            unread.append(f"{version}: {cut_short} of {stats['runs']} runs stopped early")
        print(f"  {version}: {sum(compared.values())} generated cases compared, {sum(len(r) for r in different.values())} "
              f"differences ({', '.join(f'{k} {v}' for k, v in sorted(counts.items()) if v) or 'none'}), "
              f"{stats['cases_not_compared']} cases not compared, {stats['cases_never_ran']} cases never run; "
              f"real calls: {stats['calls']}, {stats['calls_not_compared']} not compared, {stats['calls_different']} "
              f"with a difference; {stats['divergent']} compared calls with an fmadds Dolphin would flush "
              f"(fmadds_dolphin_divergent, real and synthetic)")
    open_differences = +open_differences  # no line for a function with nothing open
    if open_differences:
        names = sorted({name for _, name in open_differences})
        print(f"  {len(names)} functions with an open difference, refused: {', '.join(names)}")
    return GridVerdict(open_differences, unread)


def read_triage(path: Path) -> dict[str, list[TriageRow]]:
    """grid_triage.tsv. A line must say what may differ: one that does not (the older four columns, a register list
    the grammar does not read, writes other than none) stops the report instead of sorting more than it says."""
    out: dict[str, list[TriageRow]] = defaultdict(list)
    if not path.exists():
        return out
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw.strip() or raw.startswith("#"):
            continue
        fields = raw.split("\t", 5)
        if len(fields) != 6 or not all(f.strip() for f in fields):
            raise SystemExit(f"{path}:{number}: expected <function> <label regular expression> <class> <registers> "
                             f"<writes> <reason>, tab-separated")
        name, pattern, verdict, allowed, writes, reason = fields
        if writes != "none":
            raise SystemExit(f"{path}:{number}: writes {writes!r}: only none is read (a line never allows a byte "
                             f"written)")
        registers: dict[str, int] = {}
        nan: set[str] = set()
        for item in allowed.split(","):
            m = ALLOWED_REGISTER.fullmatch(item)
            if m is None or (m["nan"] and not m["name"].startswith("f")):
                raise SystemExit(f"{path}:{number}: {item!r} is not <register>[:<hexadecimal mask>][:nan], :nan on a "
                                 f"floating-point register")
            registers[m["name"]] = int(m["mask"], 16) if m["mask"] else ALL_BITS
            if m["nan"]:
                nan.add(m["name"])
        out[name].append(TriageRow(re.compile(pattern), verdict, registers, frozenset(nan), reason))
    return out
