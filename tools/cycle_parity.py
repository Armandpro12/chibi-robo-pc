"""Guest cycle parity of bench runs: the final cycle counter of each scenario against a reference run.

The A/B bench (tools/ab.py) compares images, audio and saves; it does not look at guest time. A decompiled function
that costs fewer guest cycles than its recompiled original (no guest cost charged, or a charged mean that does not fit
the scenario) makes guest time drift, which only shows on the bench once it shifts an event by a whole frame. This tool
reads the counters the executable prints at exit (stdout.log of tools/run.py: `blocks=N cycles=N ticks=N`) for each
scenario of a bench run and of the reference, and reports the difference of `cycles` (and of `ticks`).

The references (tests/<game>/<ID>/reference-native/, tools/ab.py record) are runs of a recompiled executable without
decompiled functions. Expected [hypothesis, not established]: with every decompiled function charging the exact cost of
its original, the final cycle count is the reference's, and functions charged a mean cost (path-dependent originals,
tools/decomp_oracle.py cost) leave a small difference. Measured so far: a few thousand cycles below the reference with
the charged costs; the cause (the means or something else) is not established. A function charging nothing leaves a
difference of millions of cycles.

Usage:
  py -3 tools/cycle_parity.py <game> --label L [--versions GGTE01,GGTP01] [--scenarios a,b] [--ref-dir DIR]
                              [--tolerance CYCLES]
      bench runs of tools/ab.py check --label L (tests/<game>/<ID>/ab/<L>/<scenario>/ of the main checkout) against the references
  py -3 tools/cycle_parity.py <game> --run RUN_DIR --ref REF_DIR [--tolerance CYCLES]
      one run folder against another (each holding stdout.log)
Exit code 0 if every |cycles difference| <= --tolerance (default 50 000 cycles, the drift alert of
tools/decomp_oracle.py cost), 1 otherwise, 2 if a counter is missing.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402

COUNTERS = re.compile(r"\bblocks=(\d+) cycles=(\d+) ticks=(\d+)")
DEFAULT_TOLERANCE = 50_000
REF_DIR = "reference-native"  # tools/ab.py REF_DIR


def main_checkout() -> Path:
    """Main checkout of the repository (holds tests/ and the bench references), also from a worktree."""
    out = subprocess.run(["git", "-C", str(tc.ROOT), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                         capture_output=True, text=True)
    if out.returncode == 0 and out.stdout.strip():
        return Path(out.stdout.strip()).parent
    return tc.ROOT


def final_counters(run_dir: Path) -> dict[str, int] | None:
    """Last `blocks= cycles= ticks=` line of the run's stdout.log, None if absent."""
    log = run_dir / "stdout.log"
    if not log.is_file():
        return None
    found = None
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = COUNTERS.search(line)
        if m:
            found = {"blocks": int(m.group(1)), "cycles": int(m.group(2)), "ticks": int(m.group(3))}
    return found


def compare(run: Path, ref: Path, tolerance: int) -> tuple[int, str]:
    """(code, line): 0 within tolerance, 1 beyond, 2 counter missing."""
    a, b = final_counters(run), final_counters(ref)
    if a is None or b is None:
        return 2, f"missing counters ({'run' if a is None else 'reference'}: no `cycles=` line in stdout.log)"
    d = a["cycles"] - b["cycles"]
    dt = a["ticks"] - b["ticks"]
    verdict = "identical" if d == 0 else ("ok" if abs(d) <= tolerance else "BEYOND TOLERANCE")
    return (0 if abs(d) <= tolerance else 1), (f"cycles {a['cycles']} vs {b['cycles']}: {d:+d} ({verdict}); "
                                               f"ticks {dt:+d}; blocks {a['blocks'] - b['blocks']:+d}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("game")
    p.add_argument("--label", help="bench label of tools/ab.py check")
    p.add_argument("--versions", help="comma-separated (default: every version of game.toml)")
    p.add_argument("--scenarios", help="comma-separated (default: every scenario of the bench run)")
    p.add_argument("--ref-dir", help="reference folder, '{version}' replaced (default: tests/<game>/<ID>/" + REF_DIR + ")")
    p.add_argument("--run", help="one run folder (with --ref)")
    p.add_argument("--ref", help="the reference run folder for --run")
    p.add_argument("--tolerance", type=int, default=DEFAULT_TOLERANCE, metavar="CYCLES")
    p.add_argument("--bench-root", help="checkout holding tests/ (default: the main checkout, also from a worktree)")
    args = p.parse_args()
    if args.run or args.ref:
        if not (args.run and args.ref) or args.label:
            p.error("--run and --ref go together, without --label")
        code, line = compare(Path(args.run), Path(args.ref), args.tolerance)
        print(line)
        return code
    if not args.label:
        p.error("--label or --run/--ref is required")
    game_cfg = tc.load_game(args.game)
    versions = args.versions.split(",") if args.versions else [v["game_id"] for v in game_cfg.get("versions", [])]
    root = Path(args.bench_root) if args.bench_root else main_checkout()
    worst = 0
    for version in versions:
        base = root / "tests" / args.game / version / "ab" / args.label
        refs = Path(args.ref_dir.replace("{version}", version)) if args.ref_dir else \
            root / "tests" / args.game / version / REF_DIR
        if not base.is_dir():
            print(f"{version}: no bench run {base}")
            worst = max(worst, 2)
            continue
        names = args.scenarios.split(",") if args.scenarios else sorted(d.name for d in base.iterdir() if d.is_dir())
        for name in names:
            code, line = compare(base / name, refs / name, args.tolerance)
            worst = max(worst, code)
            print(f"{version} {name:<16} {line}")
    print(f"tolerance {args.tolerance} cycles: {'ok' if worst == 0 else ('MISSING' if worst == 2 else 'BEYOND')}")
    return worst


if __name__ == "__main__":
    sys.exit(main())
