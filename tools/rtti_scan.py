"""CodeWarrior (MWCC) RTTI and vtable scanner: classes, inheritance, vtables and virtual functions of a DOL and its
REL modules. Generic; every game-specific path comes from games/<game>/game.toml. Read-only on the game files.

Usage:
  py -3 tools/rtti_scan.py <game> [--version ID[,ID]] [--root CHECKOUT] [--no-rel] [--out DIR] [--json]
                                  [--quiet]

  --version  versions to scan (default: every version of game.toml)
  --root     checkout holding the untracked inputs (external/, generated/); default: this checkout
  --no-rel   the DOL only (default: the DOL and every REL of [code] rel_dir, relocated by tools/gcimage.py)
  --out      output folder (one sub-folder <ID>/ per version, default build/rtti/<ID>/): rtti-<ID>.txt
             (readable report) and rtti-<ID>.json
  --json     also print the JSON summary on stdout
Outputs hold addresses and names only (class names are the game's own type names, as in a symbol map), never
game bytes; they stay local (build/ is not in Git).
Exit code: 0 when every version was scanned; 2 on a usage error (unknown game or version); 1 when an input is
missing or is not a DOL (a version whose disc is not extracted, a file that cannot be a DOL: the message on stderr
names the file).

Layout read (MWCC with -RTTI on). Sources: measurements on the two versions of the first game ported with these tools
(every vtable found is referenced by code; local tests), cross-checked against the names of a decomp-toolkit
configuration, against Ghidra's function list and its disassembly of destructors; no Metrowerks
document was available, so the field meanings below are [hypothesis] unless stated:
  - vtable: word 0 = address of the class's RTTI object, word 1 = `this` adjustment (0 for a primary vtable,
    negative for a secondary one), then one word per virtual function (0 = pure virtual, [hypothesis] from
    abstract interfaces whose slots are 0 and are filled in the derived classes);
  - RTTI object (type_info): word 0 = address of the class name (plain, unmangled C string), word 1 = 0 (no base)
    or the address of the base list;
  - base list: pairs (RTTI object of a base, offset), ended by a 0 word. Observed flattened (every ancestor, not
    only the direct parent): the tool checks it (each base's own list is contained in the derived class's list)
    and derives the direct parents as the bases that are not an ancestor of another base.
End of a vtable: the slots run while the word is a code pointer or 0, and stop at the next object (another vtable
or RTTI object, a base list, an address formed by the code); trailing 0 words are reported apart, since they are
pure virtual slots or padding (cannot tell without a derived class).

Proposed names ([hypothesis] until cross-checked, never written into the symbol maps by this tool):
  - vtable `__vt__<len><Class>` and RTTI object `__RTTI__<len><Class>`: the CodeWarrior mangling of the dtk names
    (`__vt__7CWidget` for a class CWidget); the RTTI spelling is [hypothesis];
  - a virtual function: its existing name when the map or the dtk symbols have one (checked against the class of
    its mangled name), otherwise `<Owner>::vfunc_<slot>`, the owner being the topmost class of the hierarchy that
    has the same function at the same slot (the class that defines it). A function found in unrelated hierarchies
    (shared stub, identical code folded) gets no owner.
Limits: virtual inheritance and templates are not seen in the measured game binaries (0 secondary vtables, 0 '<'
in names): their decoding is untested; a vtable whose every slot is 0 is kept only if the code references it.
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gcimage  # noqa: E402
import scan_inputs as si  # noqa: E402
import toolchain as tc  # noqa: E402

IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_][A-Za-z0-9_]*)*(<[A-Za-z0-9_:<>, *&]*>)?$")
MAX_BASES = 64
MAX_SLOTS = 1024
OFFSET_MIN = -0x10000
HYP = "[hypothesis]"


def sda_bases(prog: gcimage.Program) -> dict[int, int]:
    """r13/r2 of the DOL from its boot code (tools/dol2elf.py find_sda_bases), {} when not found."""
    try:
        import dol2elf
        dol = prog.modules[0].path
        bases = dol2elf.find_sda_bases(dol2elf.DolImage(dol.read_bytes()))
    except Exception as e:  # noqa: BLE001 - the bases only refine the reference counts: warn, do not stop
        print(f"warning: SDA bases not found ({type(e).__name__}: {e}); accesses relative to r13/r2 are not "
              f"counted as references, so the end of some vtables may be less precise", file=sys.stderr)
        return {}
    if not bases:
        print("warning: SDA bases not found in the boot code; accesses relative to r13/r2 are not counted as "
              "references", file=sys.stderr)
    return bases


def s32(v: int) -> int:
    return v - (1 << 32) if v & 0x80000000 else v


class Scan:
    """One scan of a loaded program. Results: rtti, base_lists, vtables, classes, functions (see to_json)."""

    def __init__(self, prog: gcimage.Program, sda: dict[int, int] | None = None):
        self.p = prog
        self.ref_sites: dict[int, list[int]] = {}
        self.refs = gcimage.code_references(prog, sda or {}, sites=self.ref_sites)
        self.rtti: dict[int, tuple[str, int]] = {}         # RTTI object -> (name, base list address or 0)
        self.base_lists: dict[int, list[tuple[int, int]]] = {}
        self.base_errors: list[int] = []
        self.vtables: dict[int, dict] = {}
        self.slotless: list[int] = []
        self._find_rtti()
        self._read_base_lists()
        self._find_vtables()
        self._reachable()
        self._classes()
        self._functions()

    # --------------------------------------------------------------- objects
    def _ptr(self, addr: int) -> int | None:
        """The word at addr when it may be a pointer (any DOL word; only a relocated REL word)."""
        if not self.p.is_pointer_site(addr):
            return None
        return self.p.word(addr)

    def _find_rtti(self) -> None:
        for s in self.p.data_segments():
            for o in range(0, len(s.data) - 7, 4):
                a = s.addr + o
                w0 = self._ptr(a)
                if w0 is None or not self.p.is_data(w0):
                    continue
                name = self.p.cstr(w0)
                if not name or not IDENT_RE.match(name):
                    continue
                w1 = self.p.word(a + 4)
                if w1:
                    if self._ptr(a + 4) is None or not self.p.is_data(w1):
                        continue
                self.rtti[a] = (name, w1 or 0)

    def _read_base_lists(self) -> None:
        for r, (_n, bl) in self.rtti.items():
            if not bl or bl in self.base_lists:
                continue
            out, ok = [], False
            for k in range(MAX_BASES):
                w = self.p.word(bl + 8 * k)
                if w is None:
                    break
                if w == 0:
                    ok = True
                    break
                b = self._ptr(bl + 8 * k)
                if b is None or b not in self.rtti:
                    break
                out.append((b, s32(self.p.word(bl + 8 * k + 4) or 0)))
            if ok and out:
                self.base_lists[bl] = out
            else:
                self.base_errors.append(bl)

    def _is_header(self, a: int) -> bool:
        r = self._ptr(a)
        if r is None or r not in self.rtti:
            return False
        off = self.p.word(a + 4)
        return off is not None and OFFSET_MIN < s32(off) <= 0

    def _find_vtables(self) -> None:
        in_lists: set[int] = set()
        for bl, entries in self.base_lists.items():
            in_lists.update(range(bl, bl + 8 * len(entries) + 4, 4))
        heads = set()
        for s in self.p.data_segments():
            for o in range(0, len(s.data) - 7, 4):
                a = s.addr + o
                if a not in in_lists and self._is_header(a):
                    heads.add(a)
        stops = heads | set(self.rtti) | in_lists | set(self.refs)
        for a in sorted(heads):
            slots: list[int] = []
            x = a + 8
            end = "limit"
            while len(slots) < MAX_SLOTS:
                if x in stops:
                    end = ("vtable" if x in heads else "rtti" if x in self.rtti else "base-list" if x in in_lists
                           else "code-ref")
                    break
                w = self.p.word(x)
                if w is None:
                    end = "segment-end"
                    break
                if w == 0:
                    slots.append(0)
                elif self._ptr(x) is not None and self.p.is_code(w):
                    slots.append(w)
                else:
                    end = "data-word"
                    break
                x += 4
            trailing = 0
            while slots and slots[-1] == 0:
                slots.pop()
                trailing += 1
            referenced = self.refs.get(a, 0)
            if not slots and not (trailing and referenced):
                # An RTTI pointer and an offset with no slot after them: not a vtable (a class with a vtable
                # has at least one virtual function; an all-pure one would show 0 words, kept when referenced).
                if referenced:
                    self.slotless.append(a)
                continue
            r = self._ptr(a)
            self.vtables[a] = {"rtti": r, "class": self.rtti[r][0], "offset": s32(self.p.word(a + 4)),
                               "slots": slots, "trailing_zeros": trailing, "code_refs": referenced, "end": end}

    def _reachable(self) -> None:
        seen: set[int] = set()
        stack = [v["rtti"] for v in self.vtables.values()]
        while stack:
            r = stack.pop()
            if r in seen:
                continue
            seen.add(r)
            stack.extend(b for b, _o in self.base_lists.get(self.rtti[r][1], []))
        self.reachable = seen

    # --------------------------------------------------------------- classes
    def _classes(self) -> None:
        cls: dict[str, dict] = {}
        for r in sorted(self.reachable):
            name, bl = self.rtti[r]
            c = cls.setdefault(name, {"rtti": [], "bases": None, "base_offsets": [], "inconsistent_bases": False,
                                      "vtables": [], "primary": None})
            c["rtti"].append(r)
            bases = [self.rtti[b][0] for b, _o in self.base_lists.get(bl, [])]
            if c["bases"] is None:
                c["bases"] = bases
                c["base_offsets"] = [o for _b, o in self.base_lists.get(bl, [])]
            elif c["bases"] != bases:
                c["inconsistent_bases"] = True
        for a, v in sorted(self.vtables.items()):
            c = cls[v["class"]]
            c["vtables"].append(a)
            if v["offset"] == 0 and c["primary"] is None:
                c["primary"] = a
        flat_ok = flat_bad = 0
        for name, c in cls.items():
            bset = set(c["bases"])
            for b in c["bases"]:
                if b in cls and set(cls[b]["bases"]) <= bset:
                    flat_ok += 1
                else:
                    flat_bad += 1
            c["parents"] = [b for b in c["bases"]
                            if not any(b in cls.get(o, {}).get("bases", []) for o in c["bases"] if o != b)]
            c["depth"] = len(c["bases"])
        self.flatten_check = (flat_ok, flat_bad)
        children = defaultdict(list)
        for name, c in cls.items():
            for par in c["parents"]:
                children[par].append(name)
        for name, c in cls.items():
            c["children"] = sorted(children.get(name, []))
        self.classes = cls

    def _vt_of(self, name: str) -> dict | None:
        c = self.classes.get(name)
        return self.vtables[c["primary"]] if c and c["primary"] is not None else None

    def _functions(self) -> None:
        funcs: dict[int, dict] = {}
        prefix_bad = 0
        for a, v in sorted(self.vtables.items()):
            name = v["class"]
            c = self.classes[name]
            chain = [(name, v)] + [(b, self._vt_of(b)) for b in c["bases"] if self._vt_of(b) is not None]
            for _b, bv in chain[1:]:
                if len(bv["slots"]) > len(v["slots"]) + v["trailing_zeros"]:
                    prefix_bad += 1
            for k, f in enumerate(v["slots"]):
                if not f:
                    continue
                owners = [x for x, xv in chain if k < len(xv["slots"]) and xv["slots"][k] == f]
                owner = min(owners, key=lambda x: (self.classes[x]["depth"], x))
                e = funcs.setdefault(f, {"uses": [], "owners": set(), "slots": set()})
                e["uses"].append((name, k))
                e["owners"].add(owner)
                e["slots"].add(k)
        for f, e in funcs.items():
            owners = sorted(e["owners"])
            # Several owners are fine when they are related (one is an ancestor of the other): keep the topmost.
            tops = [o for o in owners if not any(o != q and q in self.classes[o]["bases"] for q in owners)]
            e["owner"] = tops[0] if len(tops) == 1 else None
            e["shared"] = len(tops) > 1
        self.functions = funcs
        self.prefix_violations = prefix_bad

    # --------------------------------------------------------------- names
    def propose(self, names: dict[int, str]) -> None:
        """Proposed names, and the cross-check of the existing names (`names`: address -> name, any source)."""
        self.check = Counter()
        self.mismatches: list[tuple[int, str, str]] = []
        for f, e in self.functions.items():
            known = names.get(f)
            slot = min(e["slots"])
            if known and not si.is_auto(known):
                e["name"], e["name_source"] = known, "existing"
                q = si.mangled_class(known)
                if q is None:
                    dtor_like = "destructor" in known.lower() or known.endswith("__dt")
                    if dtor_like and slot == 0:
                        self.check["unmangled destructor name at slot 0"] += 1
                    elif dtor_like:
                        self.check["unmangled destructor name at another slot"] += 1
                        self.mismatches.append((f, known, e["owner"] or "?"))
                    else:
                        self.check["existing name without class"] += 1
                elif e["owner"] is None:
                    self.check["existing name, shared function"] += 1
                elif q == e["owner"]:
                    self.check["class agrees"] += 1
                elif q in self.classes and e["owner"] in self.classes[q]["bases"]:
                    self.check["class is a descendant of the owner"] += 1
                    self.mismatches.append((f, known, e["owner"]))
                else:
                    self.check["class disagrees"] += 1
                    self.mismatches.append((f, known, e["owner"]))
            elif e["owner"] and getattr(self, "dtor_of", {}).get(f) == e["owner"] and "::" not in e["owner"]:
                e["name"], e["name_source"] = f"__dt__{len(e['owner'])}{e['owner']}Fv", HYP + " destructor"
            elif e["owner"]:
                e["name"], e["name_source"] = f"{e['owner']}::vfunc_{slot}", HYP
            else:
                e["name"], e["name_source"] = None, "shared"

    def vtable_users(self, starts: set[int]) -> None:
        """For each vtable, the functions whose code forms its address (the function holding each site: the
        nearest known function start at or before it). A constructor or destructor stores its class's vtable
        into the object; the destructor is itself virtual, so a slot that forms its own vtable's address is
        the destructor [hypothesis, supported by the counts of the cross-check, the existing destructor names and Ghidra]."""
        import bisect
        order = sorted(starts)

        def owner_of(site: int) -> int | None:
            i = bisect.bisect_right(order, site) - 1
            return order[i] if i >= 0 else None

        self.users: dict[int, set[int]] = {}
        for a in self.vtables:
            self.users[a] = {f for f in (owner_of(x) for x in self.ref_sites.get(a, [])) if f is not None}
        stats = Counter()
        self.dtor_of: dict[int, str] = {}
        for a, v in self.vtables.items():
            users = self.users[a]
            in_vt = [k for k, f in enumerate(v["slots"]) if f and f in users]
            stats["vtables"] += 1
            if not in_vt:
                stats["no slot forms its own vtable"] += 1
            elif in_vt == [0]:
                stats["only slot 0 forms its own vtable"] += 1
            else:
                stats["slots forming their own vtable: " + ",".join(map(str, in_vt))] += 1
            if 0 in in_vt and v["offset"] == 0:
                self.dtor_of[v["slots"][0]] = v["class"]
            stats["functions forming the vtable outside it (constructors?)"] += len(users - set(v["slots"]))
        self.user_stats = stats

    def dtor_slots(self, names: dict[int, str]) -> Counter:
        """Slot index of the virtual functions whose existing name is a destructor (__dt__)."""
        c = Counter()
        for f, e in self.functions.items():
            n = names.get(f, "")
            if n.startswith("__dt__"):
                for k in e["slots"]:
                    c[k] += 1
        return c

    # --------------------------------------------------------------- output
    def summary(self) -> dict:
        prim = sum(1 for v in self.vtables.values() if v["offset"] == 0)
        mods = Counter(self.p.module_of(a) for a in self.vtables)
        return {
            "rtti_candidates": len(self.rtti),
            "rtti_reachable": len(self.reachable),
            "class_names": len(self.classes),
            "names_with_several_rtti_objects": sum(1 for c in self.classes.values() if len(c["rtti"]) > 1),
            "classes_without_vtable": sum(1 for c in self.classes.values() if not c["vtables"]),
            "vtables": len(self.vtables),
            "vtables_primary": prim,
            "vtables_secondary": len(self.vtables) - prim,
            "vtables_referenced_by_code": sum(1 for v in self.vtables.values() if v["code_refs"]),
            "vtables_per_module": dict(sorted(mods.items())),
            "slots": sum(len(v["slots"]) for v in self.vtables.values()),
            "pure_virtual_slots": sum(v["slots"].count(0) for v in self.vtables.values()),
            "trailing_zero_slots": sum(v["trailing_zeros"] for v in self.vtables.values()),
            "vtables_with_trailing_zeros": sum(1 for v in self.vtables.values() if v["trailing_zeros"]),
            "virtual_functions": len(self.functions),
            "virtual_functions_with_owner": sum(1 for e in self.functions.values() if e["owner"]),
            "virtual_functions_shared": sum(1 for e in self.functions.values() if e["shared"]),
            "max_depth": max((c["depth"] for c in self.classes.values()), default=0),
            "base_lists": len(self.base_lists),
            "base_list_errors": sum(1 for r in self.reachable if self.rtti[r][1] in self.base_errors),
            "headers_without_slots": [self.p.where(a) + " " + self.rtti[self._ptr(a)][0] for a in self.slotless],
            "nonzero_base_offsets": sum(1 for bl in self.base_lists.values() for _b, o in bl if o),   # every pair read
            "flattened_lists_ok": self.flatten_check[0],
            "flattened_lists_bad": self.flatten_check[1],
            "vtable_prefix_violations": self.prefix_violations,
            "inconsistent_base_lists": sum(1 for c in self.classes.values() if c["inconsistent_bases"]),
            "template_names": sum(1 for n in self.classes if "<" in n),
        }

    def to_json(self) -> dict:
        w = self.p.where
        classes = {}
        for n, c in sorted(self.classes.items()):
            classes[n] = {"rtti": [w(r) for r in c["rtti"]], "bases": c["bases"], "parents": c["parents"],
                          "children": c["children"], "depth": c["depth"],
                          "vtables": [w(a) for a in c["vtables"]],
                          "proposed_vtable_name": f"__vt__{len(n)}{n}" if "::" not in n else None,
                          "proposed_rtti_name": f"__RTTI__{len(n)}{n}" if "::" not in n else None,
                          "status": HYP}
        vts = {}
        for a, v in sorted(self.vtables.items()):
            vts[w(a)] = {"class": v["class"], "offset": v["offset"], "code_refs": v["code_refs"], "end": v["end"],
                         "trailing_zeros": v["trailing_zeros"],
                         "slots": [None if f == 0 else w(f) for f in v["slots"]]}
        fns = {}
        for f, e in sorted(self.functions.items()):
            fns[w(f)] = {"owner": e["owner"], "slots": sorted(e["slots"]), "uses": len(e["uses"]),
                         "name": e.get("name"), "name_source": e.get("name_source"),
                         "classes": sorted({c for c, _k in e["uses"]})[:16]}
        return {"summary": self.summary(), "classes": classes, "vtables": vts, "functions": fns}

    def text(self, title: str) -> str:
        w = self.p.where
        out = [f"# {title}", ""]
        for k, v in self.summary().items():
            out.append(f"{k}: {v}")
        if getattr(self, "check", None):
            out.append("name cross-check: " + ", ".join(f"{k} {v}" for k, v in sorted(self.check.items())))
        out += ["", "## Class hierarchy (indented under the direct parent; * = no vtable of its own)", ""]

        def tree(n: str, depth: int, seen: set) -> None:
            c = self.classes[n]
            vt = w(c["primary"]) if c["primary"] is not None else "*"
            nslots = len(self.vtables[c["primary"]]["slots"]) if c["primary"] is not None else 0
            out.append(f"{'  ' * depth}{n}  vtable {vt}  slots {nslots}")
            if n in seen:
                return
            seen.add(n)
            for ch in c["children"]:
                if self.classes[ch]["parents"] and self.classes[ch]["parents"][0] == n:
                    tree(ch, depth + 1, seen)

        seen: set = set()
        for n in sorted(x for x, c in self.classes.items() if not c["parents"]):
            tree(n, 0, seen)
        out += ["", "## Vtables (slot: function, proposed name; 0 = pure virtual)", ""]
        for a, v in sorted(self.vtables.items()):
            out.append(f"{w(a)}  {v['class']}  offset {v['offset']}  slots {len(v['slots'])}"
                       f"  trailing zeros {v['trailing_zeros']}  code refs {v['code_refs']}  ends at {v['end']}")
            for k, f in enumerate(v["slots"]):
                if not f:
                    out.append(f"    {k:3d}  0")
                    continue
                e = self.functions[f]
                tag = "" if e.get("name_source") == "existing" else f"  {e.get('name_source') or ''}"
                out.append(f"    {k:3d}  {w(f)}  {e.get('name') or '(shared by unrelated classes)'}{tag}")
        if getattr(self, "mismatches", None):
            out += ["", "## Existing names whose class differs from the computed owner", ""]
            for f, n, o in sorted(self.mismatches):
                out.append(f"{w(f)}  {n}  owner {o}")
        return "\n".join(out) + "\n"


def names_for(slug: str, game: dict, vid: str, root: Path, prog: gcimage.Program) -> tuple[dict[int, str], dict]:
    """Existing names of the version: its symbol map (both versions) and, for the reference version, the dtk
    functions (DOL and RELs). Returns (address -> name, info about the sources)."""
    names: dict[int, str] = {}
    info = {}
    m = si.read_map(si.map_path(game, vid))
    for a, _s, n in m:
        names[a] = n
    info["map_names"] = len(m)
    dtk = si.dtk_functions(game, root, prog) if si.dtk_reference(game) == vid else {}
    for a, (_s, n) in dtk.items():
        if not si.is_auto(n):
            names.setdefault(a, n)
    info["dtk_functions"] = len(dtk)
    return names, {**info, "_dtk": dtk}


def run(slug: str, game: dict, vid: str, root: Path, rels: bool, out: Path) -> tuple[Scan, dict]:
    prog = si.program(slug, game, vid, root, rels)
    sc = Scan(prog, sda_bases(prog))
    names, info = names_for(slug, game, vid, root, prog)
    dtk = info.pop("_dtk")
    ghidra = si.ghidra_functions(slug, vid, root)
    sc.vtable_users(set(dtk) | set(names) | set(ghidra) | set(sc.functions))
    sc.propose(names)
    targets = list(sc.functions)
    dol_targets = [f for f in targets if prog.seg(f).module == 0]
    xc = {
        "ghidra_function_starts": f"{sum(1 for f in dol_targets if f in ghidra)}/{len(dol_targets)} DOL targets"
                                  if ghidra else "no Ghidra functions.tsv",
        "dtk_function_starts": (f"{sum(1 for f in targets if f in dtk)}/{len(targets)}" if dtk
                                else "not the dtk version"),
        "named_before": sum(1 for f in targets if f in names and not si.is_auto(names[f])),
        "destructor_slots": dict(sorted(sc.dtor_slots(names).items())),
        "name_check": dict(sc.check),
        "own_vtable_formed_by": dict(sc.user_stats),
    }
    objs = si.dtk_objects(game, root) if si.dtk_reference(game) == vid else {}
    vt_names = {a: n for a, (_s, n) in objs.items() if n.startswith("__vt__")}
    if vt_names:
        agree = sum(1 for a, n in vt_names.items() if a in sc.vtables and n == f"__vt__{len(sc.vtables[a]['class'])}"
                    f"{sc.vtables[a]['class']}")
        xc["dtk_vtable_names"] = f"{agree}/{len(vt_names)} named vtables found with the same class"
    xc.update(info)
    out.mkdir(parents=True, exist_ok=True)
    data = sc.to_json()
    data["cross_check"] = xc
    data["version"] = vid
    data["modules"] = {m.name: {"relocations": m.relocations, "unresolved": m.unresolved}
                       for m in prog.modules.values()}
    (out / f"rtti-{vid}.json").write_text(json.dumps(data, indent=1), encoding="utf-8")
    txt = sc.text(f"RTTI and vtables, {game['game']['name']} {vid}")
    txt = txt.replace("\n\n## Class", "\ncross_check: " + json.dumps(xc) + "\n\n## Class", 1)
    (out / f"rtti-{vid}.txt").write_text(txt, encoding="utf-8")
    return sc, data


def compare_versions(results: dict[str, Scan]) -> list[str]:
    """Class-by-class comparison of the versions: names, vtable lengths, owners."""
    ids = sorted(results)
    lines = []
    for i, a in enumerate(ids):
        for b in ids[i + 1:]:
            ca, cb = results[a].classes, results[b].classes
            only_a, only_b = sorted(set(ca) - set(cb)), sorted(set(cb) - set(ca))
            diff = []
            for n in sorted(set(ca) & set(cb)):
                va, vb = results[a]._vt_of(n), results[b]._vt_of(n)
                la = len(va["slots"]) if va else -1
                lb = len(vb["slots"]) if vb else -1
                if la != lb:
                    diff.append(f"{n} {la}/{lb}")
            lines.append(f"{a} vs {b}: classes only in {a}: {len(only_a)} {only_a[:10]}; only in {b}: {len(only_b)}"
                         f" {only_b[:10]}; different vtable length: {len(diff)} {diff[:20]}")
    return lines


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
    results: dict[str, Scan] = {}
    for vid in si.version_ids(game, a.version):
        out = (a.out / vid) if a.out else si.default_out("rtti", vid)
        sc, data = run(a.game, game, vid, a.root.resolve(), not a.no_rel, out)
        results[vid] = sc
        if not a.quiet:
            s = data["summary"]
            print(f"{vid}: {s['vtables']} vtables ({s['vtables_secondary']} secondary, "
                  f"{s['vtables_referenced_by_code']} referenced by code), {s['rtti_reachable']} RTTI objects "
                  f"reached ({s['rtti_candidates']} candidates), {s['class_names']} classes, {s['slots']} slots, "
                  f"{s['virtual_functions']} virtual functions ({s['virtual_functions_with_owner']} with an owner)")
            print(f"  per module: {s['vtables_per_module']}")
            print(f"  cross-check: {json.dumps(data['cross_check'])}")
            print(f"  report: {out / f'rtti-{vid}.txt'}")
        if a.json:
            print(json.dumps({"version": vid, "summary": data["summary"], "cross_check": data["cross_check"]}))
    if len(results) > 1 and not a.quiet:
        for line in compare_versions(results):
            print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
