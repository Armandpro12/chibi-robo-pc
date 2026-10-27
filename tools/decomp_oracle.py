"""Decompilation oracle (stage 2, D1.3): runs scenarios on an oracle executable, collects the per-function report of
every version, proposes a status, promotes on request, and replays the captured cases offline.

The oracle executable (tools/build.py --oracle --tag T) runs, on every call of a decompiled function, the recompiled
original and then the decompiled version from the same state, compares them (runtime/core/decomp/gc_oracle.cpp:
result registers, registers the EABI preserves, bytes written; FPSCR not compared) and keeps the original's results,
so the game behaves as the recompiled one. With GCRT_DECOMP_ORACLE_OUT it writes oracle-<ID>-<pid>-<time>.jsonl:
distinct starting states ("case": entry registers, the bytes the decompiled version read, the original's registers
and writes), differences ("mismatch") and one "summary" per function. These files hold game memory: they stay local
(tests/<game>/oracle/<label>/<ID>/ of the main checkout), never in Git.

Commands (run from any checkout; scenarios through tools/ab.py of the main checkout, which holds the references):
  py -3 tools/decomp_oracle.py run --exe "build/.../chibi-robo-{version}-Release-oracle/bin/chibi-robo.exe"
        --label L [--versions GGTE01,GGTP01] [--scenarios title-boot,new-game,...]
  py -3 tools/decomp_oracle.py report --label L[,L2...] [--min-cases 10] [--promote]
  py -3 tools/decomp_oracle.py replay --label L [--versions ...] [--negative DIR] [--extra-source FILE]
  py -3 tools/decomp_oracle.py replay --cases FILE.jsonl [...]      (replay of any case file)

Promotion rule (report): a row of the game's functions table is proposed `verified` only if, in EVERY version, the
oracle compared it at least once, never saw a difference, found it to be a leaf in every run that called it, and saw
at least --min-cases distinct starting states (default 10; a distinct state = argument registers + bytes the
decompiled version read), or fewer when they are saturated: at least --saturation comparisons (default 1000), all on
those few states, i.e. every state the scenarios reach. A function never reached stays `unverified`, whatever its
other versions show. --promote writes
`verified` (and "yes" in the oracle column of each version that has one, [decomp.oracle_columns] of game.toml:
the PAL activation lock of the levels position and sized) only for rows that pass in all versions; it never touches
a disabled row, and refuses results of a negative-test executable (tools/build.py --oracle-negative).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import decomp_common as dc  # noqa: E402
import decomp_functions as df  # noqa: E402
import toolchain as tc  # noqa: E402

DEFAULT_SCENARIOS = "title-boot,new-game,load-game,overwrite-save,intro-no-card"
DEFAULT_MIN_CASES = 10
DEFAULT_SATURATION = 1000
ORACLE_LINE = re.compile(r"^ORACLE (\S+)\s+(\S+) ([0-9A-F]{8}) calls=(\d+) compared=(\d+) mismatches=(\d+) "
                         r"cases=(\d+) bad_cases=(\d+) stored=(\d+) not_compared=(\d+) leaf=(\S+)(?: reason=(.*))?$")


def main_checkout() -> Path:
    """Main checkout of the repository (holds tests/ and the bench references), also from a worktree."""
    out = subprocess.run(["git", "-C", str(tc.ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                         capture_output=True, text=True)
    if out.returncode == 0 and out.stdout.strip():
        return Path(out.stdout.strip()).parent
    return tc.ROOT


def game_name(arg: str | None) -> str:
    if arg:
        return arg
    games = sorted(p.name for p in (tc.ROOT / "games").iterdir() if (p / "game.toml").exists())
    if len(games) != 1:
        raise SystemExit(f"--game required (games: {', '.join(games)})")
    return games[0]


def versions_of(game_cfg: dict, arg: str | None) -> list[str]:
    all_versions = [v["game_id"] for v in game_cfg.get("versions", [])]
    if not arg or arg == "all":
        return all_versions
    out = [v for v in arg.split(",") if v]
    bad = [v for v in out if v not in all_versions]
    if bad:
        raise SystemExit(f"unknown versions {bad} (game.toml: {all_versions})")
    return out


def oracle_dir(bench: Path, game: str, label: str, version: str) -> Path:
    return bench / "tests" / game / "oracle" / label / version


# --- run -------------------------------------------------------------------------------------------------------------

def cmd_run(args) -> int:
    game = game_name(args.game)
    game_cfg = tc.load_game(game)
    bench = Path(args.bench_root).resolve() if args.bench_root else main_checkout()
    code = 0
    for version in versions_of(game_cfg, args.versions):
        exe = Path(args.exe.replace("{version}", version)).resolve()
        if not exe.exists():
            raise SystemExit(f"{exe}: not found (tools/build.py --oracle --tag ...)")
        out = oracle_dir(bench, game, args.label, version)
        out.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(bench / "tools" / "ab.py"), "--game", game, "--versions", version, "check",
               "--exe", str(exe), "--scenarios", args.scenarios, "--label", args.label,
               "--env", f"GCRT_DECOMP_ORACLE_OUT={out}"]
        for item in args.env:
            cmd += ["--env", item]
        print("+", " ".join(cmd), flush=True)
        rc = subprocess.run(cmd, cwd=bench).returncode
        print(f"== {version}: tools/ab.py check returned {rc} (bench verdict of the oracle executable)")
        code = code or rc
    print(f"== Oracle files: {oracle_dir(bench, game, args.label, '<ID>')}; next: "
          f"py -3 tools/decomp_oracle.py report --label {args.label}")
    return code


# --- report and promotion --------------------------------------------------------------------------------------------

def read_results(bench: Path, game: str, labels: list[str], version: str) -> tuple[dict, list[str]]:
    """{function: aggregate} over every oracle file of `labels` for `version`, and the problems found."""
    agg: dict[str, dict] = {}
    problems: list[str] = []
    for label in labels:
        folder = oracle_dir(bench, game, label, version)
        files = sorted(folder.glob("oracle-*.jsonl"))
        if not files:
            problems.append(f"{label}/{version}: no oracle file in {folder}")
        for path in files:
            header = None
            summaries = 0
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    problems.append(f"{path.name}: unreadable line (run cut short?)")
                    continue
                kind = rec.get("kind")
                if kind == "header":
                    header = rec
                    continue
                name = rec.get("function")
                a = agg.setdefault(name, {"calls": 0, "compared": 0, "mismatches": 0, "not_compared": 0,
                                          "fingerprints": set(), "bad": set(), "leaf": set(), "reasons": set(),
                                          "runs": 0, "address": rec.get("address"), "examples": [],
                                          "paths": set()})
                if kind == "summary":
                    summaries += 1
                    a["runs"] += 1
                    for k in ("calls", "compared", "mismatches", "not_compared"):
                        a[k] += int(rec[k])
                    a["fingerprints"].update(rec.get("fingerprints", []))
                    if int(rec["calls"]):  # a run that never called it says nothing about it
                        a["leaf"].add(rec["leaf"])
                    if rec.get("reason"):
                        a["reasons"].add(rec["reason"])
                    if rec.get("fingerprints_truncated"):
                        a["truncated"] = True
                elif kind == "case":
                    a["paths"].add(path_of(rec))
                elif kind == "mismatch":
                    a["bad"].add(rec["fingerprint"])
                    if len(a["examples"]) < 3:
                        a["examples"].append(f"{path.name}: " + "; ".join(rec.get("differences", [])[:4]))
            if header is None:
                problems.append(f"{path.name}: no header line")
            elif header.get("negative"):
                problems.append(f"{path.name}: negative-test executable (results never promote)")
            if summaries == 0:
                problems.append(f"{path.name}: no summary (the run did not exit normally)")
    return agg, problems


def path_of(rec: dict) -> str:
    """Rough path of a stored case, to show what the cases cover (never part of the verdict): result zero or not,
    number of reads of the decompiled version, number of bytes the original wrote."""
    result = "-" if rec.get("result_kind", 0) == 0 else ("0" if int(rec["original"]["gpr"][3], 16) == 0 else "nz")
    return f"r3={result}/reads={len(rec['reads'])}/written={sum(len(w[1]) // 2 for w in rec['original_writes'])}"


def verdict(a: dict | None, min_cases: int, saturation: int = 0) -> tuple[bool, str]:
    if not a or a["calls"] == 0:
        return False, "never called by these scenarios"
    if a["leaf"] - {"yes"}:
        return False, f"not comparable as a leaf ({', '.join(sorted(a['reasons'])) or ', '.join(sorted(a['leaf']))})"
    if a["compared"] == 0:
        return False, "called but never compared"
    if a["mismatches"]:
        return False, f"{a['mismatches']} compared calls with differences ({len(a['bad'])} distinct cases)"
    n = len(a["fingerprints"])
    if n < min_cases:
        # Fewer distinct states than asked: accepted only if the scenarios have saturated them (at least
        # `saturation` comparisons, all of them on these few states).
        if saturation and a["compared"] >= saturation:
            return True, (f"{a['compared']} comparisons, all on {n} distinct cases (< {min_cases}: every state "
                          f"these scenarios reach, saturated at >= {saturation} comparisons), 0 differences")
        return False, (f"{n} distinct cases < {min_cases} in {a['compared']} comparisons (< {saturation} to call "
                       f"them saturated): add a scenario that reaches other states")
    return True, f"{a['compared']} comparisons, {n} distinct cases, 0 differences"


def cmd_report(args) -> int:
    game = game_name(args.game)
    game_cfg = tc.load_game(game)
    bench = Path(args.bench_root).resolve() if args.bench_root else main_checkout()
    labels = [x for x in args.label.split(",") if x]
    versions = versions_of(game_cfg, args.versions)
    functions = df.read_table(game_cfg)
    results = {}
    problems: list[str] = []
    for v in versions:
        results[v], p = read_results(bench, game, labels, v)
        problems += p
    print(f"Oracle report, labels {', '.join(labels)}, minimum {args.min_cases} distinct cases per version "
          f"(or fewer, saturated, after {args.saturation} comparisons)")
    print(f"{'function':34} {'version':8} {'calls':>9} {'compared':>9} {'diff':>6} {'cases':>6}  verdict")
    proposed: dict[str, tuple[bool, list[str]]] = {}
    for f in functions:
        reasons = []
        ok_all = True
        for v in versions:
            a = results[v].get(f.name)
            ok, why = verdict(a, args.min_cases, args.saturation)
            ok_all &= ok
            if not ok:
                reasons.append(f"{v}: {why}")
            if a:
                print(f"{f.name:34} {v:8} {a['calls']:>9} {a['compared']:>9} {a['mismatches']:>6} "
                      f"{len(a['fingerprints']):>6}  {'pass' if ok else 'refused'}: {why}")
                print(f"{'':44}paths of the stored cases: {', '.join(sorted(a['paths'])) or '-'}")
                for e in a["examples"]:
                    print(f"{'':44}e.g. {e}")
            else:
                print(f"{f.name:34} {v:8} {'-':>9} {'-':>9} {'-':>6} {'-':>6}  refused: {why}")
        if set(versions) != {v["game_id"] for v in game_cfg.get("versions", [])}:
            ok_all = False
            reasons.append("not every version was read (--versions)")
        proposed[f.name] = (ok_all, reasons)
    print()
    for f in functions:
        ok, reasons = proposed[f.name]
        if f.kind == "listed":
            state = "row disabled: never promoted" + (" (would pass)" if ok else "")
        elif ok:
            state = "proposed status: verified"
        else:
            state = f"stays {f.status}: " + " | ".join(reasons)
        print(f"{f.name}: {state}")
    for p in problems:
        print(f"WARNING: {p}")
    if not args.promote:
        return 0
    if any("negative-test" in p for p in problems):
        raise SystemExit("--promote refused: results of a negative-test executable among the labels")
    promote = [f.name for f in functions if f.kind != "listed" and proposed[f.name][0]]
    if not promote:
        print("--promote: nothing to promote")
        return 1
    write_status(game_cfg, promote, versions)
    print(f"--promote: {', '.join(promote)} -> verified in {df.config(game_cfg)[0].name}")
    return 0


def write_status(game_cfg: dict, names: list[str], versions: list[str]) -> None:
    """Sets `verified` (and the oracle column of each version) on the rows `names` of the functions table, keeping
    every other byte of the file."""
    path, _ = df.config(game_cfg)
    oracle_cols = {v: c for v, c in dc.oracle_columns(game_cfg.get("decomp", {})).items() if v in versions}
    lines = path.read_text(encoding="utf-8").split("\n")
    header_index = next(i for i, raw in enumerate(lines) if raw.strip() and not raw.startswith("#"))
    header = lines[header_index].split("\t")
    for col in oracle_cols.values():
        if col not in header:
            header.append(col)
    lines[header_index] = "\t".join(header)
    for i in range(header_index + 1, len(lines)):
        raw = lines[i]
        if not raw.strip() or raw.startswith("#"):
            continue
        cells = raw.split("\t")
        cells += ["-"] * (len(header) - len(cells))
        if cells[header.index("name")] in names:
            cells[header.index("status")] = "verified"
            for col in oracle_cols.values():
                cells[header.index(col)] = "yes"
        lines[i] = "\t".join(cells)
    path.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    df.read_table(game_cfg)  # the table must still read


# --- offline replay --------------------------------------------------------------------------------------------------

def case_lines(rec: dict) -> list[str]:
    """Case record (oracle jsonl) -> text form read by runtime/core/decomp/oracle_replay.cpp."""
    out = [f"case {rec['function']} {rec['address']} {rec['result_kind']} {rec.get('fingerprint', '-')}"]
    for key, tag in (("entry", "I"), ("original", "O")):
        r = rec[key]
        out.append(f"{tag}G " + " ".join(r["gpr"]))
        out.append(f"{tag}F " + " ".join(r["fpr"]))
        out.append(f"{tag}P " + " ".join(r["ps1"]))
        out.append(f"{tag}C {r['cr']} {r['xer']} {r['lr']} {r['ctr']} {r['fpscr']}")
    for addr, size, value in rec["reads"]:
        out.append(f"R {addr} {size} {value}")
    for addr, before, after in rec["original_writes"]:
        out.append(f"W {addr} {before} {after}")
    out.append("end")
    return out


def build_replay(game: str, game_cfg: dict, out: Path, negative: Path | None, extra: list[Path]) -> Path:
    clang = tc.find_clang_cl()
    if not clang:
        raise SystemExit("clang-cl not found (tools/doctor.py)")
    core = tc.ROOT / "runtime" / "core"
    src = sorted((tc.ROOT / "games" / game / "decomp" / "src").rglob("*.cpp"))
    if negative:
        src += sorted(negative.rglob("*.cpp"))
    src += extra
    names = [f.name for f in df.read_table(game_cfg)]
    records = build_negative_records(negative)
    names += [r for r in records.values()]
    names += [m.group(1) for p in extra for m in re.finditer(r"GC_DECOMP_EXPORT\(\s*(\w+)\s*,", p.read_text("utf-8"))]
    table = out / "replay_table.cpp"
    lines = ['#include "decomp/gc_abi.h"', ""]
    lines += [f"extern const ::gc::abi::export_record gc_decomp_export_{n};" for n in names]
    lines += ["", "extern const ::gc::abi::export_record* const gc_replay_records[] = {"]
    lines += [f"    &gc_decomp_export_{n}," for n in names] + ["};",
              f"extern const unsigned gc_replay_record_count = {len(names)};", ""]
    out.mkdir(parents=True, exist_ok=True)
    table.write_text("\n".join(lines), encoding="utf-8")
    exe = out / f"oracle_replay{tc.EXE}"
    cmd = [str(clang), "/nologo", "/std:c++20", "/EHsc", "/O2", "/W4", "/clang:-ffp-contract=off",
           "/DGC_MEM_OBSERVER", *(["/DGC_DECOMP_ORACLE_NEGATIVE"] if negative else []), f"/I{core}",
           str(core / "decomp" / "oracle_replay.cpp"), str(table),
           *[str(s) for s in src], f"/Fo{out}\\", f"/Fe{exe}"]
    r = subprocess.run(cmd, capture_output=True, text=True, env=tc.msvc_env())
    if r.returncode != 0:
        raise SystemExit(f"replay build failed:\n{r.stdout}\n{r.stderr}")
    return exe


def build_negative_records(negative: Path | None) -> dict[str, str]:
    import build as b  # tools/build.py
    return b.negative_records(negative)


def cmd_replay(args) -> int:
    game = game_name(args.game)
    game_cfg = tc.load_game(game)
    bench = Path(args.bench_root).resolve() if args.bench_root else main_checkout()
    negative = Path(args.negative).resolve() if args.negative else None
    files: list[Path] = [Path(c).resolve() for c in args.cases]
    if args.label:
        for v in versions_of(game_cfg, args.versions):
            files += sorted(oracle_dir(bench, game, args.label, v).glob("oracle-*.jsonl"))
    if not files:
        raise SystemExit("no case file (--label or --cases)")
    out = Path(args.out).resolve() if args.out else tc.ROOT / "build" / "oracle-replay"
    exe = build_replay(game, game_cfg, out, negative, [Path(p).resolve() for p in args.extra_source])
    records = build_negative_records(negative)
    text: list[str] = []
    count = 0
    for path in files:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("kind") not in ("case", "mismatch") or (rec.get("kind") == "mismatch" and not args.mismatches):
                continue
            if negative and rec["function"] in records:
                rec = dict(rec, function=records[rec["function"]])
            text += case_lines(rec)
            count += 1
    cases = out / "cases.txt"
    cases.write_text("\n".join(text) + "\n", encoding="utf-8")
    print(f"== Replay of {count} cases from {len(files)} files ({exe.name})")
    r = subprocess.run([str(exe), str(cases)], capture_output=True, text=True)
    lines = r.stdout.splitlines()
    for line in lines:
        if not line.startswith("PASS ") or args.verbose:
            print(line)
    return r.returncode


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game")
    p.add_argument("--bench-root", help="checkout holding tests/ and tools/ab.py (default: the main checkout)")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="scenarios on the oracle executable, every version")
    r.add_argument("--exe", required=True, help='oracle executable; "{version}" replaced by the ID')
    r.add_argument("--label", required=True)
    r.add_argument("--versions")
    r.add_argument("--scenarios", default=DEFAULT_SCENARIOS)
    r.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    k = sub.add_parser("report", help="per-function report and proposed status (--promote writes it)")
    k.add_argument("--label", required=True, help="one or more labels, comma-separated (results are added up)")
    k.add_argument("--versions")
    k.add_argument("--min-cases", type=int, default=DEFAULT_MIN_CASES)
    k.add_argument("--saturation", type=int, default=DEFAULT_SATURATION,
                   help="comparisons from which fewer than --min-cases distinct cases count as all the reachable ones "
                        "(default %(default)s; 0: never)")
    k.add_argument("--promote", action="store_true")
    y = sub.add_parser("replay", help="offline replay of captured cases on the decompiled code")
    y.add_argument("--label")
    y.add_argument("--versions")
    y.add_argument("--cases", action="append", default=[], help="oracle jsonl file (repeatable)")
    y.add_argument("--negative", help="also build the wrong variants of DIR and replay them instead (negative test)")
    y.add_argument("--extra-source", action="append", default=[], help="more decompiled sources (tests)")
    y.add_argument("--mismatches", action="store_true", help="also replay the recorded mismatches")
    y.add_argument("--out", help="build folder of the replay executable (default build/oracle-replay)")
    y.add_argument("--verbose", action="store_true")
    args = p.parse_args()
    return {"run": cmd_run, "report": cmd_report, "replay": cmd_replay}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
