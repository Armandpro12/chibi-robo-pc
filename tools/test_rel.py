"""REL module link test (generic): for each .rel of the game, the built executable loads the file into
guest RAM as the game would, calls the game's OSLink then OSUnlink (recompiled code) and checks the result
(DolRecomp fixed addresses, relocations, BSS, the OS module list). See notes/hle.md.

Usage: py -3 tools/test_rel.py <game> [--version ID] [--config Release|Debug] [--rel FILE ...]
Prerequisite: py -3 tools/build.py <game> [--version ID]. Exit code 0 if every module passes.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="REL link test (the game's OSLink/OSUnlink).")
    parser.add_argument("game")
    parser.add_argument("--version")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--rel", action="append", help="restrict to this file (repeatable)")
    parser.add_argument("-v", "--verbose", action="store_true", help="show the details of each test")
    args = parser.parse_args()

    game_cfg = tc.load_game(args.game)
    version = args.version or game_cfg["game"]["default_version"]
    manifest = json.loads((tc.GENERATED / args.game / version / "manifest.json").read_text(encoding="utf-8"))
    rel_dir = Path(manifest["disc_root"]) / game_cfg["code"]["rel_dir"]
    exe = tc.ROOT / "build" / f"{args.game}-{version}-{args.config}" / "bin" / f"{args.game}{tc.EXE}"
    if not exe.exists():
        raise SystemExit(f"Executable missing: {exe} (run tools/build.py {args.game} --version {version}).")
    rels = [Path(r) for r in args.rel] if args.rel else sorted(rel_dir.rglob("*.rel"))
    failures = 0
    for rel in rels:
        proc = subprocess.run([str(exe), "--io-log", "0", "--rel-test", str(rel)], capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=300)
        ok = proc.returncode == 0
        failures += not ok
        n_ok = proc.stdout.count("[OK]")
        print(f"{'OK    ' if ok else 'FAIL  '} {rel.name:16} {n_ok} checks passed")
        if args.verbose or not ok:
            for line in proc.stdout.splitlines():
                if line.startswith("  ") or line.startswith("REL test"):
                    print("   ", line)
            if not ok:
                print("   ", proc.stderr.strip().splitlines()[-5:] if proc.stderr.strip() else "")
    print(f"{len(rels) - failures}/{len(rels)} modules linked and unlinked correctly ({version})")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
