"""Decompilation oracle (stage 2, D1.3): runs scenarios on an oracle executable, collects the per-function report of
every version, proposes a status, promotes on request, and replays the captured cases offline.

The oracle executable (tools/build.py --oracle --tag T) runs, on every call of a decompiled function, the recompiled
original and then the decompiled version from the same state, compares them (runtime/core/decomp/gc_oracle.cpp:
result registers, registers the EABI preserves, bytes written; FPSCR not compared) and keeps the original's results,
so the game behaves as the recompiled one. It also records which basic blocks of the original ran (a hook at every
block start, oracle builds only) and runs the hand-written synthetic cases of games/<game>/decomp/oracle_cases/ (no
game data) from a scratch area. With GCRT_DECOMP_ORACLE_OUT it writes oracle-<ID>-<pid>-<time>.jsonl: a header (with
the hash of the sources the executable was built from), distinct starting states ("case": entry registers, the bytes
the decompiled version read, the original's registers, writes and blocks), differences ("mismatch"), one "summary"
per function, and an "end" line at a normal exit. These files hold game memory: they stay local
(tests/<game>/oracle/<label>/<ID>/ of the main checkout), never in Git.

Commands (run from any checkout; scenarios through tools/ab.py of the main checkout, which holds the references):
  py -3 tools/decomp_oracle.py run --exe "build/.../chibi-robo-{version}-Release-oracle/bin/chibi-robo.exe"
        --label L [--versions GGTE01,GGTP01] [--scenarios title-boot,new-game,...] [--no-synthetic]
  py -3 tools/decomp_oracle.py report --label L[,L2...] [--min-cases 10] [--saturation 1000] [--max-not-compared 0]
        [--promote [--dry-run]]
  py -3 tools/decomp_oracle.py replay --label L [--versions ...] [--negative DIR] [--extra-source FILE]
  py -3 tools/decomp_oracle.py replay --cases FILE.jsonl [...]      (replay of any case file)
  py -3 tools/decomp_oracle.py hash                                 (hashes the results are tied to)

`run` refuses a label folder that already holds results (one label = one run of one build), and writes run-<ID>.json
next to the results: executable and its SHA-1, hash of the sources and of the synthetic cases, scenarios, bench
verdict, and the GCRT_* environment variables it passed (inherited ones and --env). Without --scenarios it replays
the list `oracle_scenarios` of the [decomp] section of games/<game>/game.toml. It refuses, before any launch, an
inherited or --env variable that can change what the oracle compares or the starting states (RESTRICTED_ENV_*: names
starting with GCRT_DECOMP_ or GCRT_HLE_, GCRT_FORCE_ARG, GCRT_HOST_CALL_FILTER, GCRT_GUEST_HISTORY; the tool sets
GCRT_DECOMP_ORACLE_OUT and GCRT_DECOMP_ORACLE_SYNTHETIC itself) unless --allow-env is given; such results never
promote.

Promotion rule (report). A row of the game's functions table is proposed `verified` only if, in EVERY version:
  * the results are whole and current: every oracle file has a header, an end line and a summary per function, no
    unreadable line, the header's source hash is the current one (tools/decomp_functions.py oracle_sources_hash: the
    decompiled sources, runtime/core/decomp, the tools and runtime files of the block analysis and the hooks, the
    DolRecomp checkout, the address columns of the table), run-<ID>.json exists, its bench verdict is 0 and its
    synthetic-case hash is the current one, and the synthetic cases the executable loaded (header synthetic_content)
    are the current ones; every setting in the header is the default (DEFAULT_SETTINGS: dead stack, stored cases,
    cross-check); none comes from a negative-test executable or a test mode of the oracle, no self-check of the undo
    and redo failed, and run-<ID>.json lists no restricted environment variable (`run --allow-env`). Accidental
    copies of a run are refused: two labels naming one folder (by resolved, case-folded path or file identity: "L,l",
    "L,L/", a junction) are refused, and, across every label and version read, a result file is a problem when its
    content, or its records apart from the header line, are another file's, or when its run is another file's
    (executable SHA-1 + header run identifier, or + process and start time of its name oracle-<ID>-<pid>-<time>.jsonl);
    so are a header without a run identifier, a file name that disagrees with it, a header or records of another
    version than the folder's, and a summary address other than the table's (DOL rows). Forged evidence (a run
    identifier, executable hash or record written by hand) is out of scope: results are not signed. Any problem
    refuses every promotion;
  * the oracle compared the function at least once, never saw a difference (real calls and synthetic cases), found
    it to be a leaf in every run that called it, could compare every synthetic case, and left at most
    --max-not-compared real calls uncompared (default 0: a call whose original did not return, for instance, is not
    compared, and is not evidence). Two kinds of calls and cases are counted apart, not as uncompared: entries the
    gateway declined because MSR[FP] = 0 for a function whose original uses the FPU ("fpu_declined": the original
    ran, "FPU unavailable" exception included, runtime/core/decomp/gc_abi.h registration_uses_fpu), and synthetic
    cases of a DISABLED row the executable does not build (header synthetic_skipped_functions; skipping the cases of an
    enabled row or of a name that is not in the table is a problem). Also reported, never part of the verdict: the
    compared calls whose decompiled fmadds under NI = 1 Dolphin would flush to zero and DolRecomp does not
    (gc_float.h, "fmadds_dolphin_divergent");
  * block coverage: every basic block of the original reachable from its entry (tools/decomp_functions.py
    code_blocks) ran during at least one compared call or synthetic case; a block the analysis calls unreachable must
    never run (it would prove the analysis wrong);
  * at least --min-cases distinct real starting states (default 10; a distinct state = argument registers + bytes the
    decompiled version read), or fewer when they are saturated: at least --saturation comparisons (default 1000),
    all on those few states [hypothesis: heuristic thresholds, notes/decomp.md D1.3].
"verified" therefore means: no difference on every compared call and synthetic case, with every reachable block of
the original exercised. This is basic-block coverage (not edge or path coverage), and it does not prove equivalence
on states that were never compared.
--promote refuses thresholds weaker than the defaults (--min-cases below 10, --saturation between 1 and 999,
--max-not-compared above 0): the table is only ever promoted under the stated rule; the thresholds used are printed
on the first line of the report and on the --promote line.
A function never reached stays `unverified`, whatever its other versions show. --promote writes `verified` (and
"yes" in the oracle column of each version that has one, [decomp.oracle_columns] of game.toml: the PAL activation
lock of the levels position and sized) only for rows that pass in all versions; it never touches a disabled row.
--dry-run shows what --promote would write without writing it.
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

DEFAULT_MIN_CASES = 10
DEFAULT_SATURATION = 1000
DEFAULT_MAX_NOT_COMPARED = 0
# Settings of the oracle executable (environment variables read by runtime/core/decomp, written in the header of each
# result file): only results produced with these defaults promote. dead_stack: GCRT_DECOMP_ORACLE_DEAD_STACK (bytes
# under r1 not compared; 0xFFFFFFFF would hide every write below r1); case_limit: GCRT_DECOMP_ORACLE_CASES (cases
# stored per function, the ones replay rechecks); crosscheck: GCRT_DECOMP_CROSSCHECK (the gateway also reruns the
# original). GCRT_DECOMP_ORACLE_TEST_DETACHING_JOURNAL (test mode) is refused on its own, and the synthetic cases
# (GCRT_DECOMP_ORACLE_SYNTHETIC) are checked by content (header field synthetic_content).
DEFAULT_SETTINGS = {"dead_stack": "00010000", "case_limit": 64, "crosscheck": False}
# Environment variables `run` refuses (inherited or --env) without --allow-env: they change what the oracle compares
# or stores (GCRT_DECOMP_*), how the native SDK replacements and the stack behave (GCRT_HLE_*, e.g.
# GCRT_HLE_STACK_FILL), or the calls and arguments of the run (GCRT_FORCE_ARG, GCRT_HOST_CALL_FILTER,
# GCRT_GUEST_HISTORY), so the starting states would not be the ones of a normal run.
RESTRICTED_ENV_PREFIXES = ("GCRT_DECOMP_", "GCRT_HLE_")
RESTRICTED_ENV_NAMES = frozenset({"GCRT_FORCE_ARG", "GCRT_HOST_CALL_FILTER", "GCRT_GUEST_HISTORY"})
# Name of a result file, as runtime/core/decomp/gc_oracle.cpp writes it; its header run is "<pid>-<time>-<clock>".
ORACLE_FILE = re.compile(r"oracle-(?P<version>[^-]+)-(?P<pid>-?\d+)-(?P<time>-?\d+)\.jsonl")


def restricted_env(name: str) -> bool:
    upper = name.upper()  # Windows names are case-insensitive
    return upper.startswith(RESTRICTED_ENV_PREFIXES) or upper in RESTRICTED_ENV_NAMES


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

def synthetic_dir(game_cfg: dict) -> Path:
    return game_cfg["_dir"] / "decomp" / "oracle_cases"


def synthetic_hash(game_cfg: dict) -> str:
    """SHA-1 of the synthetic cases (every .txt of games/<game>/decomp/oracle_cases, line ends normalized)."""
    import hashlib
    h = hashlib.sha1()
    folder = synthetic_dir(game_cfg)
    for path in synthetic_files(game_cfg):
        h.update(path.name.encode() + b"\0" + path.read_bytes().replace(b"\r\n", b"\n") + b"\0")
    return h.hexdigest()


def synthetic_files(game_cfg: dict) -> list[Path]:
    """The .txt files of the synthetic cases, sorted by name as a case-sensitive string (the order of the oracle
    executable's std::sort, the same on Windows and POSIX)."""
    folder = synthetic_dir(game_cfg)
    return sorted(folder.glob("*.txt"), key=lambda p: p.name) if folder.is_dir() else []


def synthetic_content_hash(game_cfg: dict) -> str:
    """FNV-1a 64 of the synthetic files the way the oracle executable hashes the ones it loaded (header field
    synthetic_content, runtime/core/decomp/gc_oracle.cpp load_synthetic): name, 0, content with CRLF read as LF, 0."""
    h = 0xCBF29CE484222325
    for path in synthetic_files(game_cfg):
        for b in path.name.encode() + b"\0" + path.read_bytes().replace(b"\r\n", b"\n") + b"\0":
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016X}"


def sha1_file(path: Path) -> str:
    import hashlib
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def default_scenarios(game_cfg: dict) -> str:
    """Comma-separated scenarios of `oracle_scenarios` in the [decomp] section of game.toml."""
    names = game_cfg.get("decomp", {}).get("oracle_scenarios")
    if not names or not isinstance(names, list) or not all(isinstance(n, str) and n for n in names):
        raise SystemExit(f"{game_cfg['_dir'] / 'game.toml'}: [decomp] oracle_scenarios missing or not a list of "
                         f"scenario names; add it (the reference scenarios the oracle replays) or give --scenarios")
    return ",".join(names)


def run_environment(args) -> tuple[dict[str, str], dict[str, str], list[str]]:
    """({name: value} of the inherited GCRT_* variables, {name: value} of the --env items, [restricted names among
    them]). Refuses a malformed --env item."""
    import os
    inherited = {k: v for k, v in os.environ.items() if k.upper().startswith("GCRT_")}
    cli: dict[str, str] = {}
    for item in args.env:
        if "=" not in item or not item.split("=", 1)[0]:
            raise SystemExit(f"--env {item!r}: expected KEY=VALUE")
        key, value = item.split("=", 1)
        cli[key] = value
    return inherited, cli, sorted({k for k in [*inherited, *cli] if restricted_env(k)})


def cmd_run(args) -> int:
    env_inherited, env_cli, env_restricted = run_environment(args)
    if env_restricted and not args.allow_env:
        raise SystemExit(f"run refused: environment variables {', '.join(env_restricted)} (inherited or --env) can "
                         f"change what the oracle compares or the starting states; unset them, or pass --allow-env "
                         f"for an experiment (its results never promote)")
    game = game_name(args.game)
    game_cfg = tc.load_game(game)
    bench = Path(args.bench_root).resolve() if args.bench_root else main_checkout()
    versions = versions_of(game_cfg, args.versions)
    scenarios = args.scenarios or default_scenarios(game_cfg)
    exes = {}
    for version in versions:
        exe = Path(args.exe.replace("{version}", version)).resolve()
        if not exe.exists():
            raise SystemExit(f"{exe}: not found (tools/build.py --oracle --tag ...)")
        out = oracle_dir(bench, game, args.label, version)
        if out.exists() and any(out.iterdir()):
            raise SystemExit(f"{out} already holds results: one label per run (results of several builds or runs "
                             f"would be added up); choose another --label")
        exes[version] = exe
    synth = synthetic_dir(game_cfg)
    code = 0
    for version in versions:
        exe = exes[version]
        out = oracle_dir(bench, game, args.label, version)
        out.mkdir(parents=True, exist_ok=True)
        own = {"GCRT_DECOMP_ORACLE_OUT": str(out)}
        if not args.no_synthetic and synth.is_dir():
            own["GCRT_DECOMP_ORACLE_SYNTHETIC"] = str(synth)
        cmd = [sys.executable, str(bench / "tools" / "ab.py"), "--game", game, "--versions", version, "check",
               "--exe", str(exe), "--scenarios", scenarios, "--label", args.label]
        for key, value in own.items():
            cmd += ["--env", f"{key}={value}"]
        for item in args.env:
            cmd += ["--env", item]
        print("+", " ".join(cmd), flush=True)
        rc = subprocess.run(cmd, cwd=bench).returncode
        print(f"== {version}: tools/ab.py check returned {rc} (bench verdict of the oracle executable)")
        play = run_play(args, game, game_cfg, bench, version, exe, out, own) if args.play else {}
        if any(play.values()):
            rc = rc or max(play.values())
        info = {"version": version, "exe": str(exe), "exe_sha1": sha1_file(exe), "scenarios": scenarios,
                "bench_returncode": rc, "sources": df.oracle_sources_hash(game_cfg),
                "dolrecomp": df.dolrecomp_state(),
                "synthetic": None if args.no_synthetic or not synth.is_dir() else synthetic_hash(game_cfg),
                # GCRT_* variables passed to tools/ab.py (inherited, then this tool's, then --env: ab.py merges them
                # in this order over its own environment); tools/run.py adds its own (GCRT_USER_DIR, ...) below.
                "gcrt_env": {k: v for k, v in {**env_inherited, **own, **env_cli}.items()
                             if k.upper().startswith("GCRT_")},
                "env_restricted": env_restricted, "allow_env": bool(args.allow_env),
                # --play: progression scenarios run after the bench, {name: exit code of tools/run.py} (in rc).
                "play": play}
        (out / f"run-{version}.json").write_text(json.dumps(info, indent=2), encoding="utf-8")
        code = code or rc
    print(f"== Oracle files: {oracle_dir(bench, game, args.label, '<ID>')}; next: "
          f"py -3 tools/decomp_oracle.py report --label {args.label}")
    return code


def run_play(args, game: str, game_cfg: dict, bench: Path, version: str, exe: Path, out: Path,
             own: dict[str, str]) -> dict[str, int]:
    """--play: progression scenarios games/<game>/scenarios/play/<ID>/<name>.scn, in the order given, through
    tools/run.py of the bench checkout (isolated user folder, no sound), writing their oracle files into the same
    folder as the bench's. A scenario with an `after <name>` line starts from the user folder of that scenario's run,
    which must come earlier in the list. Verdict of each: the exit code of tools/run.py (0 = ok). A scenario whose
    header asks for other options (--stage, a save of its own) is not run here: give only self-contained ones."""
    import os
    folder = game_cfg["_dir"] / "scenarios" / "play" / version
    done: dict[str, Path] = {}
    codes: dict[str, int] = {}
    env = dict(os.environ)
    for key, value in own.items():
        env[key] = value
    for item in args.env:
        key, value = item.split("=", 1)
        env[key] = value
    for name in [n for n in args.play.split(",") if n]:
        scn = folder / f"{name}.scn"
        if not scn.exists():
            raise SystemExit(f"--play {name}: {scn} not found (scenarios/play/{version}/)")
        after = None
        for line in scn.read_text(encoding="utf-8").splitlines():
            words = line.split("#", 1)[0].split()
            if len(words) >= 2 and words[0] == "after":
                after = words[1]
        run_out = out / f"play-{name}"
        cmd = [sys.executable, str(bench / "tools" / "run.py"), game, "--version", version, "--exe", str(exe),
               "--audio", "off", "--scenario", str(scn), "--out", str(run_out)]
        if after:
            if after not in done:
                raise SystemExit(f"--play {name}: needs the user folder of {after!r} ('after' line): list {after} "
                                 f"before it")
            cmd += ["--user-seed", str(done[after] / "user")]
        print("+", " ".join(cmd), flush=True)
        codes[name] = subprocess.run(cmd, cwd=bench, env=env).returncode
        print(f"== {version}: play {name}: tools/run.py returned {codes[name]}")
        done[name] = run_out
    return codes


def cmd_hash(args) -> int:
    game_cfg = tc.load_game(game_name(args.game))
    print(f"sources {df.oracle_sources_hash(game_cfg)}")
    print(f"synthetic {synthetic_hash(game_cfg)}")
    print(f"synthetic_content {synthetic_content_hash(game_cfg)}")
    print(f"dolrecomp {df.dolrecomp_state()}")
    return 0


# --- report and promotion --------------------------------------------------------------------------------------------

def new_aggregate(rec: dict) -> dict:
    return {"calls": 0, "compared": 0, "mismatches": 0, "not_compared": 0, "fingerprints": set(), "bad": set(),
            "leaf": set(), "reasons": set(), "runs": 0, "address": rec.get("address"), "examples": [],
            "paths": set(), "synthetic_cases": 0, "synthetic_compared": 0, "synthetic_mismatches": 0,
            "synthetic_not_compared": 0, "blocks": None, "block_hits": {}, "dead_blocks": None,
            "dead_block_hits": {}, "results": set(), "results_count": 0, "path_count": 0, "synthetic_labels": set(),
            "journal_reattached": 0, "restore_errors": 0, "fpu_declined": 0, "fmadds_dolphin_divergent": 0}


def new_seen() -> dict[str, dict]:
    """Identities of the result files already read, shared by every label and version of one report (accidental
    copies of a run are refused, also across versions): "content" (SHA-1 of the file), "body" (SHA-1 of its records
    apart from the header line: a copy whose header was edited), "run" ((executable SHA-1, header run identifier)),
    "name" ((executable SHA-1, pid, start time) of the file name) -> label/version/file that has it."""
    return {"content": {}, "body": {}, "run": {}, "name": {}}


def read_results(bench: Path, game: str, labels: list[str], version: str,
                 current: dict | None = None, seen: dict | None = None,
                 addresses: dict[str, int] | None = None, kinds: dict[str, str] | None = None,
                 skipped: dict[str, int] | None = None) -> tuple[dict, list[str]]:
    """{function: aggregate} over every oracle file of `labels` for `version`, and the problems found. A problem is
    blocking: --promote refuses, and every verdict of the version fails. `current`: {"sources": hash, "synthetic":
    hash} the results must have been produced with (None: not checked). `seen`: new_seen(), shared by the calls of one
    report (None: this call only). `addresses`: {function: address in `version`} of the table's DOL rows, which every
    summary must carry (None: not checked). `kinds`: {row name: kind} of the functions table (tools/decomp_functions.py:
    done, replaced, listed): the synthetic cases an executable skipped because their function was not built
    (header synthetic_skipped_functions) are accepted, and counted into `skipped` ({function: cases, summed over the
    files}), only for a disabled row ("listed"); any other name is a problem (None: every skipped case is one)."""
    import hashlib
    agg: dict[str, dict] = {}
    problems: list[str] = []
    skipped = skipped if skipped is not None else {}
    seen = seen if seen is not None else new_seen()
    for label in labels:
        folder = oracle_dir(bench, game, label, version)
        files = sorted(folder.glob("oracle-*.jsonl"))
        if not files:
            problems.append(f"{label}/{version}: no oracle file in {folder}")
        info_path = folder / f"run-{version}.json"
        info: dict = {}
        if not info_path.exists():
            problems.append(f"{label}/{version}: no run-{version}.json (results not produced by "
                            f"tools/decomp_oracle.py run)")
        else:
            info = json.loads(info_path.read_text(encoding="utf-8"))
            if info.get("bench_returncode") != 0:
                problems.append(f"{label}/{version}: bench verdict of the oracle run {info.get('bench_returncode')} "
                                f"(scenario not ok: crash or regression)")
            if current is not None and info.get("synthetic") != current["synthetic"]:
                problems.append(f"{label}/{version}: synthetic cases {info.get('synthetic')} differ from the current "
                                f"ones {current['synthetic']} (or were not run)")
            if info.get("env_restricted"):
                problems.append(f"{label}/{version}: run with restricted environment variables "
                                f"{', '.join(info['env_restricted'])} (run --allow-env: an experiment, results never "
                                f"promote)")
        exe_sha1 = str(info.get("exe_sha1") or "?")
        for path in files:
            # Accidental copies of a run are refused: under two labels, under another name, in another version's
            # folder, with an edited header (forged evidence is out of scope: nothing is signed).
            where = f"{label}/{version}/{path.name}"
            data = path.read_bytes()
            content = hashlib.sha1(data).hexdigest()
            copied = content in seen["content"]
            if copied:
                problems.append(f"{where}: same content as {seen['content'][content]} (accidental copy of a run: it "
                                f"would be counted twice; nothing promotes)")
            seen["content"].setdefault(content, where)
            named = ORACLE_FILE.fullmatch(path.name)
            if not named:
                problems.append(f"{where}: file name not oracle-<version>-<pid>-<time>.jsonl (renamed file)")
            elif named["version"] != version:
                problems.append(f"{where}: file name of version {named['version']} in the {version} folder")
            if named:
                name_key = (exe_sha1, named["pid"], named["time"])
                if name_key in seen["name"]:
                    problems.append(f"{where}: same executable, process and start time as {seen['name'][name_key]} "
                                    f"(accidental copy of a run; nothing promotes)")
                seen["name"].setdefault(name_key, where)
            body = hashlib.sha1()
            header = None
            summaries = 0
            ended = False
            other_version = None
            for line in data.decode("utf-8", errors="replace").splitlines():
                try:
                    rec = json.loads(line)
                except json.JSONDecodeError:
                    problems.append(f"{path.name}: unreadable line (run cut short?)")
                    body.update(line.encode() + b"\n")
                    continue
                kind = rec.get("kind")
                if kind == "header" and header is None:
                    header = rec
                    continue
                body.update(line.encode() + b"\n")
                if kind == "header":
                    problems.append(f"{where}: second header line (two result files joined?)")
                    continue
                if kind == "end":
                    ended = True
                    continue
                if rec.get("version") not in (None, version) and other_version is None:
                    other_version = rec["version"]
                    problems.append(f"{where}: records of version {other_version} in the {version} folder")
                name = rec.get("function")
                a = agg.setdefault(name, new_aggregate(rec))
                if kind == "summary":
                    summaries += 1
                    expected = (addresses or {}).get(name)
                    if expected is not None and str(rec.get("address", "")).upper() != f"{expected:08X}":
                        problems.append(f"{where}: {name} at {rec.get('address')}, the functions table gives "
                                        f"{expected:08X} in {version} (results of another version or table)")
                    a["runs"] += 1
                    for k in ("calls", "compared", "mismatches", "not_compared", "synthetic_compared",
                              "synthetic_mismatches", "synthetic_not_compared", "journal_reattached", "fpu_declined",
                              "fmadds_dolphin_divergent"):
                        a[k] += int(rec.get(k, 0))
                    if "fpu_declined" not in rec:
                        problems.append(f"{path.name}: {name}: summary without fpu_declined (oracle executable older "
                                        f"than the FPU decline of the gateway: rebuild and rerun)")
                    if "restore_errors" not in rec:
                        problems.append(f"{path.name}: {name}: summary without restore_errors (old oracle executable)")
                    elif int(rec["restore_errors"]):
                        a["restore_errors"] += int(rec["restore_errors"])
                        problems.append(f"{path.name}: {name}: {rec['restore_errors']} failed self-checks of the undo "
                                        f"or redo of the original's writes (oracle fault: nothing promotes)")
                    a["synthetic_cases"] = max(a["synthetic_cases"], int(rec.get("synthetic_cases", 0)))
                    # Non-leaf comparison (D2-1): calls recorded, thread switches refused in the nested run,
                    # assertion blocks and the synthetic cases that ran each block.
                    a["recorded_calls"] = a.get("recorded_calls", 0) + int(rec.get("recorded_calls", 0))
                    a["max_calls"] = max(a.get("max_calls", 0), int(rec.get("max_calls", 0)))
                    a["refused_switches"] = a.get("refused_switches", 0) + int(rec.get("refused_switches", 0))
                    if rec.get("assert_blocks") is not None:
                        a["assert_blocks"] = list(rec["assert_blocks"])
                    for b, n in zip(rec.get("blocks") or [], rec.get("synthetic_block_hits") or []):
                        sh = a.setdefault("synthetic_block_hits", {})
                        sh[b] = sh.get(b, 0) + int(n)
                    a["fingerprints"].update(rec.get("fingerprints", []))
                    if int(rec["calls"]):  # a run that never called it says nothing about it
                        a["leaf"].add(rec["leaf"])
                    if rec.get("reason"):
                        a["reasons"].add(rec["reason"])
                    if rec.get("fingerprints_truncated"):
                        a["truncated"] = True
                    for key in ("blocks", "dead_blocks"):
                        listed = rec.get(key)
                        if listed is None:
                            problems.append(f"{path.name}: {name}: summary without {key} (old oracle executable)")
                            continue
                        if a[key] is None:
                            a[key] = list(listed)
                        elif a[key] != listed:
                            problems.append(f"{path.name}: {name}: {key} differ between runs")
                        for b, n in zip(listed, rec.get(key[:-1] + "_hits", [])):
                            a[key[:-1] + "_hits"][b] = a[key[:-1] + "_hits"].get(b, 0) + int(n)
                    a["results"].update(rec.get("results", []))
                    a["results_count"] = max(a["results_count"], int(rec.get("results_count", 0)), len(a["results"]))
                    a["path_count"] = max(a["path_count"], int(rec.get("paths", 0)))
                elif kind == "case":
                    a["paths"].add(path_of(rec))
                    if rec.get("synthetic"):
                        a["synthetic_labels"].add(rec["synthetic"])
                elif kind == "mismatch":
                    if not rec.get("synthetic"):
                        a["bad"].add(rec["fingerprint"])
                    if len(a["examples"]) < 3:
                        a["examples"].append(f"{path.name}{' synthetic ' + rec['synthetic'] if rec.get('synthetic') else ''}"
                                             f": " + "; ".join(rec.get("differences", [])[:4]))
            digest = body.hexdigest()
            if digest in seen["body"] and not copied:  # a whole-file copy is already reported
                problems.append(f"{where}: same records as {seen['body'][digest]} apart from the header line (a copy "
                                f"with an edited header, or a rerun of one executable on one scenario, which adds no "
                                f"evidence; nothing promotes)")
            seen["body"].setdefault(digest, where)
            if header is None:
                problems.append(f"{path.name}: no header line")
            else:
                if header.get("version") != version:
                    problems.append(f"{where}: header version {header.get('version')!r}, not {version} (a result "
                                    f"file of another version in this folder)")
                run = header.get("run")
                if not isinstance(run, str) or not run:
                    problems.append(f"{where}: header without run identifier (old oracle executable: rebuild and "
                                    f"rerun)")
                else:
                    if named and not run.startswith(f"{named['pid']}-{named['time']}-"):
                        problems.append(f"{where}: header run {run} does not match the file name (renamed file or "
                                        f"edited header)")
                    run_key = (exe_sha1, run)
                    if run_key in seen["run"]:
                        problems.append(f"{where}: same run as {seen['run'][run_key]} (executable {exe_sha1[:12]}, "
                                        f"run {run}): accidental copy of a run; nothing promotes")
                    seen["run"].setdefault(run_key, where)
                for key, default in DEFAULT_SETTINGS.items():
                    if key not in header:
                        problems.append(f"{path.name}: header without {key} (old oracle executable: rebuild and rerun)")
                    elif header[key] != default:
                        problems.append(f"{path.name}: setting {key} = {header[key]!r}, not the default {default!r} "
                                        f"(an environment variable of the run changed what the oracle compares or "
                                        f"stores: results never promote)")
                if current is not None and "synthetic_content" in current and \
                        header.get("synthetic_content") != current["synthetic_content"]:
                    problems.append(f"{path.name}: synthetic cases loaded by the executable "
                                    f"{header.get('synthetic_content') or '(no hash: old oracle executable)'} are not "
                                    f"the current ones {current['synthetic_content']} (GCRT_DECOMP_ORACLE_SYNTHETIC "
                                    f"pointed elsewhere, or the cases changed)")
                if header.get("negative"):
                    problems.append(f"{path.name}: negative-test executable (results never promote)")
                if int(header.get("test_detaching_journal", 0)):
                    problems.append(f"{path.name}: test mode GCRT_DECOMP_ORACLE_TEST_DETACHING_JOURNAL="
                                    f"{header['test_detaching_journal']} (results never promote)")
                if "test_no_store_hook" not in header:
                    problems.append(f"{path.name}: header without test_no_store_hook (oracle executable older than the "
                                    f"routing of decompiled stores: rebuild and rerun)")
                elif int(header["test_no_store_hook"]):
                    problems.append(f"{path.name}: test mode GCRT_DECOMP_ORACLE_TEST_NO_STORE_HOOK="
                                    f"{header['test_no_store_hook']} (results never promote)")
                if current is not None and header.get("sources") != current["sources"]:
                    problems.append(f"{path.name}: built from sources {header.get('sources') or '(no hash)'}, not the "
                                    f"current ones {current['sources']} (rebuild the oracle executable and rerun)")
                if int(header.get("synthetic_errors", 0)):
                    problems.append(f"{path.name}: {header['synthetic_errors']} errors in the synthetic cases "
                                    f"(stderr of the run)")
                # Synthetic cases of functions this executable does not register: accepted (and counted apart) only
                # for a disabled row of the table, which is not built; an enabled row or an unknown name is a problem.
                listed_skips = header.get("synthetic_skipped_functions")
                if listed_skips is None or "synthetic_skipped" not in header:
                    problems.append(f"{path.name}: header without synthetic_skipped (oracle executable older than the "
                                    f"skipping of unbuilt rows: rebuild and rerun)")
                else:
                    if sum(int(n) for n in listed_skips.values()) != int(header["synthetic_skipped"]):
                        problems.append(f"{path.name}: synthetic_skipped {header['synthetic_skipped']} is not the sum "
                                        f"of synthetic_skipped_functions")
                    for skipped_name, n in listed_skips.items():
                        kind = (kinds or {}).get(skipped_name)
                        if kind == "listed":
                            skipped[skipped_name] = skipped.get(skipped_name, 0) + int(n)
                        elif kind is None:
                            problems.append(f"{path.name}: {n} synthetic cases of {skipped_name}, which is not a row "
                                            f"of the functions table (misspelled name in decomp/oracle_cases?)")
                        else:
                            problems.append(f"{path.name}: {n} synthetic cases of {skipped_name} skipped: the row is "
                                            f"enabled but the executable does not register it (old executable, or "
                                            f"another table)")
                if summaries != int(header.get("functions", -1)):
                    problems.append(f"{path.name}: {summaries} summaries for {header.get('functions')} functions")
            if not ended:
                problems.append(f"{path.name}: no end line (the run did not exit normally: crash or cut short)")
            if summaries == 0:
                problems.append(f"{path.name}: no summary (the run did not exit normally)")
    return agg, problems


def path_of(rec: dict) -> str:
    """Rough path of a stored case, to show what the cases cover (never part of the verdict): result zero or not,
    number of reads of the decompiled version, number of bytes the original wrote."""
    result = "-" if rec.get("result_kind", 0) == 0 else ("0" if int(rec["original"]["gpr"][3], 16) == 0 else "nz")
    return f"r3={result}/reads={len(rec['reads'])}/written={sum(len(w[1]) // 2 for w in rec['original_writes'])}"


def coverage(a: dict) -> tuple[list[str], list[str], list[str]]:
    """(reachable blocks, those never run in a compared call, unreachable blocks that ran)."""
    blocks = a.get("blocks") or []
    missed = [b for b in blocks if not a["block_hits"].get(b)]
    dead_run = [b for b in (a.get("dead_blocks") or []) if a["dead_block_hits"].get(b)]
    return blocks, missed, dead_run


def verdict(a: dict | None, min_cases: int, saturation: int = 0, problems: list[str] | None = None,
            max_not_compared: int = DEFAULT_MAX_NOT_COMPARED) -> tuple[bool, str]:
    if problems:
        return False, f"{len(problems)} problems with the results (WARNING lines): nothing promotes"
    if not a or a["calls"] == 0:
        return False, "never called by these scenarios"
    # "yes": a leaf; "calls": compared with its calls recorded and replayed (gc_oracle.cpp, "Calls").
    if a["leaf"] - {"yes", "calls"}:
        return False, f"not comparable ({', '.join(sorted(a['reasons'])) or ', '.join(sorted(a['leaf']))})"
    if a.get("refused_switches"):
        return False, f"{a['refused_switches']} thread switches refused during the original's nested run"
    if a["compared"] == 0:
        return False, "called but never compared"
    if a["not_compared"] > max_not_compared:
        return False, (f"{a['not_compared']} calls not compared (original did not return, or not comparable) > "
                       f"{max_not_compared} allowed (--max-not-compared)")
    if a["mismatches"]:
        return False, f"{a['mismatches']} compared calls with differences ({len(a['bad'])} distinct cases)"
    if a["synthetic_mismatches"]:
        return False, f"{a['synthetic_mismatches']} synthetic cases with differences"
    if a["synthetic_not_compared"]:
        return False, f"{a['synthetic_not_compared']} synthetic cases not compared"
    blocks, missed, dead_run = coverage(a)
    if a.get("blocks") is None:
        return False, "no block coverage in the results"
    if dead_run:
        return False, f"blocks the analysis calls unreachable ran: {', '.join(dead_run)}"
    if missed:
        asserts = [b for b in missed if b in set(a.get("assert_blocks") or [])]
        hint = (f"; assertion blocks among them ({', '.join(asserts)}): only a synthetic case reaches them, with its "
                f"calls simulated (`call` lines)") if asserts else ""
        return False, (f"blocks of the original never compared: {', '.join(missed)} ({len(blocks) - len(missed)}/"
                       f"{len(blocks)} covered): add a synthetic case (games/<game>/decomp/oracle_cases) or a scenario "
                       f"that reaches them{hint}")
    n = len(a["fingerprints"])
    cov = f"{len(blocks)}/{len(blocks)} blocks"
    if a.get("assert_blocks"):
        cov += f" ({len(a['assert_blocks'])} assertion blocks, by synthetic cases: listed for review)"
    if n < min_cases:
        # Fewer distinct states than asked: accepted only if the scenarios have saturated them (at least
        # `saturation` comparisons, all of them on these few states).
        if saturation and a["compared"] >= saturation:
            return True, (f"{a['compared']} comparisons, all on {n} distinct cases (< {min_cases}: every state "
                          f"these scenarios reach, saturated at >= {saturation} comparisons), {cov}, 0 differences")
        return False, (f"{n} distinct cases < {min_cases} in {a['compared']} comparisons (< {saturation} to call "
                       f"them saturated): add a scenario that reaches other states")
    return True, f"{a['compared']} comparisons, {n} distinct cases, {cov}, 0 differences"


def folder_key(path: Path) -> str:
    """Identity of a folder by its path: resolved (links and junctions followed) and case-folded the way the system
    compares names (os.path.normcase: case-insensitive on Windows only)."""
    import os
    return os.path.normcase(str(path.resolve()))


def same_folders(bench: Path, game: str, labels: list[str], versions: list[str]) -> list[str]:
    """Pairs of labels that name one folder ("L" and "l" on Windows, "L/", "./L", "L.", a junction or a symbolic link
    to it, for the label or for one of its version folders), by resolved path or by file identity
    (os.path.samefile)."""
    import os
    out = []
    for i, a in enumerate(labels):
        for b in labels[i + 1:]:
            for sub in [None, *versions]:
                pa = oracle_dir(bench, game, a, sub) if sub else oracle_dir(bench, game, a, "x").parent
                pb = oracle_dir(bench, game, b, sub) if sub else oracle_dir(bench, game, b, "x").parent
                same = folder_key(pa) == folder_key(pb)
                if not same and pa.exists() and pb.exists():
                    try:
                        same = os.path.samefile(pa, pb)
                    except OSError:
                        pass
                if same:
                    out.append(f"labels {a!r} and {b!r} are the same folder {pa.resolve()}")
                    break
    return out


def cmd_report(args) -> int:
    game = game_name(args.game)
    game_cfg = tc.load_game(game)
    bench = Path(args.bench_root).resolve() if args.bench_root else main_checkout()
    labels = [x for x in args.label.split(",") if x]
    duplicates = sorted({x for x in labels if labels.count(x) > 1})
    if duplicates:
        raise SystemExit(f"--label {args.label}: {', '.join(duplicates)} given more than once (its results would be "
                         f"counted twice)")
    same = same_folders(bench, game, labels, versions_of(game_cfg, args.versions))
    if same:
        raise SystemExit(f"--label {args.label}: {'; '.join(same)} (one run would be counted twice)")
    weaker = weaker_thresholds(args)
    if args.promote and weaker:
        raise SystemExit(f"--promote refused: thresholds weaker than the promotion rule ({'; '.join(weaker)}); the "
                         f"table is only promoted under the defaults (or stricter ones)")
    versions = versions_of(game_cfg, args.versions)
    functions = df.read_table(game_cfg)
    current = {"sources": df.oracle_sources_hash(game_cfg), "synthetic": synthetic_hash(game_cfg),
               "synthetic_content": synthetic_content_hash(game_cfg)}
    results = {}
    problems: list[str] = []
    seen = new_seen()  # shared by every version: a copy into another version's folder is refused too
    kinds = {f.name: f.kind for f in functions}
    skipped: dict[str, dict[str, int]] = {}
    for v in versions:
        addresses = {f.name: f.addresses[v] for f in functions if f.module == "dol" and f.addresses.get(v) is not None}
        skipped[v] = {}
        results[v], p = read_results(bench, game, labels, v, current, seen, addresses, kinds, skipped[v])
        problems += p
    print(f"Oracle report, labels {', '.join(labels)}; thresholds: --min-cases {args.min_cases} distinct cases per "
          f"version (or fewer, saturated, after --saturation {args.saturation} comparisons), --max-not-compared "
          f"{args.max_not_compared}; every reachable block of the original compared (basic-block coverage, not edge "
          f"or path coverage)")
    print(f"{'function':34} {'version':8} {'calls':>9} {'compared':>9} {'notcmp':>6} {'diff':>6} {'cases':>6} "
          f"{'blocks':>7} {'synth':>6} {'results':>7}  verdict")
    proposed: dict[str, tuple[bool, list[str]]] = {}
    for f in functions:
        reasons = []
        ok_all = True
        for v in versions:
            a = results[v].get(f.name)
            ok, why = verdict(a, args.min_cases, args.saturation, problems, args.max_not_compared)
            ok_all &= ok
            if not ok:
                reasons.append(f"{v}: {why}")
            if a:
                blocks, missed, _ = coverage(a)
                print(f"{f.name:34} {v:8} {a['calls']:>9} {a['compared']:>9} {a['not_compared']:>6} "
                      f"{a['mismatches']:>6} {len(a['fingerprints']):>6} {len(blocks) - len(missed):>3}/{len(blocks):<3} "
                      f"{a['synthetic_compared']:>6} {a['results_count']:>7}  {'pass' if ok else 'refused'}: {why}")
                hits = ", ".join(f"{b}:{a['block_hits'].get(b, 0)}" for b in blocks)
                print(f"{'':44}blocks (compared calls + synthetic cases that ran each): {hits or '-'}")
                if a.get("dead_blocks"):
                    print(f"{'':44}unreachable blocks: {', '.join(a['dead_blocks'])}")
                if a.get("assert_blocks"):
                    sh = a.get("synthetic_block_hits", {})
                    listed = ", ".join(f"{b}:{sh.get(b, 0)} synthetic" for b in a["assert_blocks"])
                    print(f"{'':44}assertion blocks (only lead to [decomp.no_return]; covered by synthetic cases, "
                          f"to review): {listed}")
                if a.get("recorded_calls"):
                    print(f"{'':44}calls recorded and replayed: {a['recorded_calls']} (up to {a['max_calls']} per "
                          f"comparison)")
                print(f"{'':44}distinct results of the original (r3, r3:r4 or f1 bits): "
                      f"{', '.join(sorted(a['results'])[:12]) or '-'}{' ...' if len(a['results']) > 12 else ''}; "
                      f"distinct block sets per run: up to {a['path_count']}")
                print(f"{'':44}synthetic cases: {a['synthetic_compared']} compared of {a['synthetic_cases']} per run, "
                      f"{a['synthetic_mismatches']} with differences; runs of the original where another write "
                      f"journal detached itself (oracle journal kept): {a['journal_reattached']}")
                print(f"{'':44}declined, FPU off (MSR[FP] = 0: the original ran, not counted as calls nor as not "
                      f"compared): {a['fpu_declined']}; compared calls with an fmadds that Dolphin would flush "
                      f"under NI = 1 and DolRecomp does not: {a['fmadds_dolphin_divergent']}")
                print(f"{'':44}paths of the stored cases: {', '.join(sorted(a['paths'])) or '-'}")
                for e in a["examples"]:
                    print(f"{'':44}e.g. {e}")
            else:
                print(f"{f.name:34} {v:8} {'-':>9} {'-':>9} {'-':>6} {'-':>6} {'-':>6} {'-':>7} {'-':>6} {'-':>7}  "
                      f"refused: {why}")
        if set(versions) != {v["game_id"] for v in game_cfg.get("versions", [])}:
            ok_all = False
            reasons.append("not every version was read (--versions)")
        proposed[f.name] = (ok_all, reasons)
    print()
    for f in functions:
        ok, reasons = proposed[f.name]
        if f.kind == "listed":
            state = "row disabled: never promoted" + (" (would pass)" if ok else "")
            ignored = {v: skipped[v][f.name] for v in versions if skipped[v].get(f.name)}
            if ignored:
                state += ("; synthetic cases ignored (row not built in the executable): " +
                          ", ".join(f"{v} {n}" for v, n in ignored.items()))
        elif ok:
            state = "proposed status: verified"
        else:
            state = f"stays {f.status}: " + " | ".join(reasons)
        print(f"{f.name}: {state}")
    for p in problems:
        print(f"WARNING: {p}")
    if not args.promote:
        return 0
    if problems:
        print(f"--promote refused: {len(problems)} problems with the results (WARNING lines above)")
        return 2
    promote = [f.name for f in functions if f.kind != "listed" and proposed[f.name][0]]
    if not promote:
        print("--promote: nothing to promote")
        return 1
    rule = (f"--min-cases {args.min_cases}, --saturation {args.saturation}, --max-not-compared "
            f"{args.max_not_compared}, labels {', '.join(labels)}")
    if args.dry_run:
        print(f"--promote --dry-run: would set {', '.join(promote)} to verified (nothing written; {rule})")
        return 0
    write_status(game_cfg, promote, versions)
    print(f"--promote: {', '.join(promote)} -> verified in {df.config(game_cfg)[0].name} ({rule})")
    return 0


def weaker_thresholds(args) -> list[str]:
    """Thresholds of `args` weaker than the promotion rule's defaults (empty: the defaults or stricter ones)."""
    out = []
    if args.min_cases < DEFAULT_MIN_CASES:
        out.append(f"--min-cases {args.min_cases} < {DEFAULT_MIN_CASES}")
    if 0 < args.saturation < DEFAULT_SATURATION:  # 0 = never saturated: stricter
        out.append(f"--saturation {args.saturation} < {DEFAULT_SATURATION}")
    if args.saturation < 0:
        out.append(f"--saturation {args.saturation} < 0")
    if args.max_not_compared > DEFAULT_MAX_NOT_COMPARED:
        out.append(f"--max-not-compared {args.max_not_compared} > {DEFAULT_MAX_NOT_COMPARED}")
    return out


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
    # call_sites and original_entry_lr (the LR the original was entered with): the LR save word rule of
    # gc_oracle_compare.h (0 for a case written before these fields existed: the word is then compared, the strict
    # side).
    out = [f"case {rec['function']} {rec['address']} {rec['result_kind']} {rec.get('fingerprint', '-')} "
           f"{rec.get('dead_stack', '00010000')} {int(rec.get('call_sites', 0))} "
           f"{rec.get('original_entry_lr', '00000000')}"]
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
    # Oracle test functions (disabled rows of the table, oracle builds only): their records are in the table too.
    src += sorted((tc.ROOT / "games" / game / "decomp" / "oracle_tests").rglob("*.cpp"))
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
    # Identifiers of the game globals (decomp_globals.h, as tools/build.py writes it next to its registration table):
    # the decompiled sources include it. No address table is installed here: the cases of functions that use a
    # global are skipped by cmd_replay (globals_users).
    import decomp_globals  # noqa: E402
    (out / "decomp_globals.h").write_text(decomp_globals.header_source(decomp_globals.read_table(game_cfg)),
                                          encoding="utf-8")
    exe = out / f"oracle_replay{tc.EXE}"
    cmd = [str(clang), "/nologo", "/std:c++20", "/EHsc", "/O2", "/W4", "/clang:-ffp-contract=off",
           "/DGC_MEM_OBSERVER", *(["/DGC_DECOMP_ORACLE_NEGATIVE"] if negative else []), f"/I{core}", f"/I{out}",
           str(core / "decomp" / "oracle_replay.cpp"), str(table),
           *[str(s) for s in src], f"/Fo{out}\\", f"/Fe{exe}"]
    r = subprocess.run(cmd, capture_output=True, text=True, env=tc.msvc_env())
    if r.returncode != 0:
        raise SystemExit(f"replay build failed:\n{r.stdout}\n{r.stderr}")
    return exe


def globals_users(game_cfg: dict) -> set[str]:
    """Functions of the functions table whose original uses a game global (used_by of the globals table): the offline
    replay installs no version address table (gc::set_global_table), so their cases are skipped, counted."""
    import decomp_globals  # noqa: E402
    if decomp_globals.table_path(game_cfg) is None:
        return set()
    return {u for row in decomp_globals.read_table(game_cfg) for u in row.used_by}


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
    users = globals_users(game_cfg)
    text: list[str] = []
    count = skipped_calls = skipped_globals = 0
    for path in files:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            if rec.get("kind") not in ("case", "mismatch") or (rec.get("kind") == "mismatch" and not args.mismatches):
                continue
            if rec.get("calls"):
                # A case of a non-leaf function holds the calls the original made: the offline replay does not replay
                # calls yet (the in-game oracle does). Skipped, counted.
                skipped_calls += 1
                continue
            if rec.get("function") in users:
                # Its global has no address offline (no version address table installed): skipped, counted.
                skipped_globals += 1
                continue
            if negative and rec["function"] in records:
                rec = dict(rec, function=records[rec["function"]])
            text += case_lines(rec)
            count += 1
    cases = out / "cases.txt"
    cases.write_text("\n".join(text) + "\n", encoding="utf-8")
    print(f"== Replay of {count} cases from {len(files)} files ({exe.name}); {skipped_calls} cases with calls "
          f"skipped (not replayable offline yet), {skipped_globals} cases of functions that use a game global "
          f"skipped (no version address table offline)")
    r = subprocess.run([str(exe), str(cases)], capture_output=True, text=True)
    lines = r.stdout.splitlines()
    for line in lines:
        if not line.startswith("PASS ") or args.verbose:
            print(line)
    return r.returncode


# --- cost ------------------------------------------------------------------------------------------------------------

GUEST_COST_HEADER = """\
# Guest cost of the decompiled functions of {game} (stage 2): for each function and version, the guest cycles the
# gateway charges to the cycle clock per call (runtime/core/decomp/gc_abi.h, "Guest time"), so that the cycle clock
# (default pacing of normal play, and the A/B bench) runs as with the recompiled original. Without it a decompiled
# function costs no guest time: timed events (end of a load, audio) come earlier, and the A/B bench sees a frame shift
# that is not a change of behaviour. Written by tools/decomp_oracle.py cost from the oracle's measurement of the
# originals: the mean EXCLUSIVE cycles of each original over its game calls (its own cycles, without those of the calls
# it makes, charged where they run); a whole number when every call costs the same, otherwise the mean to 4 decimals
# (the gateway charges it in 1/256 cycle and carries the remainder: a mean is off by at most 1/512 cycle per call).
# Numbers about the game's code only, like the size column of functions.tsv. Path-dependent originals (least and most
# of one call differ) are listed below the table: their charge is a mean, exact only on average. Measured on: {labels}.
"""

DRIFT_ALERT = 1_000_000  # cycles: ~0.12 frame of guest time at 486 MHz / 60 Hz (8.1 M cycles a frame)


def read_cost_summaries(files: list[Path], problems: list[str]) -> dict[str, list[dict]]:
    """{function: [summary, ...]} of the cost fields of the oracle files (one summary per function and run). A summary
    without the exclusive cost fields (oracle executable older than the exclusive measurement) is a problem."""
    out: dict[str, list[dict]] = defaultdict(list)
    for file in files:
        for line in file.read_text(encoding="utf-8").splitlines():
            if '"kind":"summary"' not in line:
                continue
            rec = json.loads(line)
            if "cost_calls" not in rec:
                problems.append(f"{file.name}: {rec.get('function')}: summary without cost (old oracle executable)")
                continue
            if not rec.get("cost_exclusive"):
                problems.append(f"{file.name}: {rec.get('function')}: summary with an inclusive cost (oracle executable "
                                f"older than the exclusive measurement): run the oracle again")
                continue
            rec["_run"] = file.stem
            out[rec["function"]].append(rec)
    return out


def cost_of(summaries: list[dict]) -> dict:
    """Totals of one function in one version: calls, exclusive cycles, least and most of one call, unmeasured calls,
    nested runs, and the per-run (calls, cycles) for the drift."""
    m = {"calls": 0, "cycles": 0, "min": None, "max": 0, "unmeasured": 0, "nested_calls": 0, "nested_cycles": 0,
         "callee_cycles": 0, "runs": []}
    for rec in summaries:
        m["unmeasured"] += rec.get("cost_unmeasured", 0)
        m["nested_calls"] += rec.get("cost_nested_calls", 0)
        m["nested_cycles"] += rec.get("cost_nested_cycles", 0)
        m["callee_cycles"] += rec.get("cost_callee_cycles", 0)
        if not rec["cost_calls"]:
            continue
        m["calls"] += rec["cost_calls"]
        m["cycles"] += rec["cost_cycles"]
        m["min"] = rec["cost_min"] if m["min"] is None else min(m["min"], rec["cost_min"])
        m["max"] = max(m["max"], rec["cost_max"])
        m["runs"].append((rec["_run"], rec["cost_calls"], rec["cost_cycles"]))
    return m


def cmd_cost(args) -> int:
    """Guest cost table (decomp/guest_cost.tsv, [decomp] guest_cost of game.toml) from the cost fields of the oracle's
    summaries: the mean exclusive guest cycles of each original over its game calls, per version. Refused (code 1)
    when an enabled function has no measured call in a version (unless --allow-missing), or when a summary has no
    exclusive cost. Warns about table rows of disabled functions, unmeasured calls, and the drift of path-dependent
    functions: for each run (one oracle file, i.e. one scenario), its cycles minus its calls x the charged mean; beyond
    --drift-alert cycles (default 1 M) in one run, the table may not fit that scenario."""
    game = game_name(args.game)
    game_cfg = tc.load_game(game)
    bench = Path(args.bench_root) if args.bench_root else main_checkout()
    versions = versions_of(game_cfg, args.versions)
    labels = [x for x in args.label.split(",") if x]
    path = df.guest_cost_path(game_cfg)
    if not path:
        raise SystemExit("game.toml: [decomp] guest_cost is required")
    include = [x for x in (args.include or "").split(",") if x]
    if include and not args.dry_run:
        raise SystemExit("--include (disabled rows, e.g. oracle test functions) is for --dry-run only: the tracked "
                         "table lists the enabled functions")
    table = df.read_table(game_cfg)
    known = {f.name for f in table}
    unknown = [n for n in include if n not in known]
    if unknown:
        raise SystemExit(f"--include: not in the functions table: {', '.join(unknown)}")
    rows = [f for f in table if f.enabled or f.name in include]
    problems: list[str] = []
    measured: dict[tuple[str, str], dict] = {}
    for version in versions:
        files: list[Path] = []
        for label in labels:
            found = sorted(oracle_dir(bench, game, label, version).rglob("oracle-*.jsonl"))
            if not found:
                problems.append(f"{label}/{version}: no oracle file")
            files += found
        for name, summaries in read_cost_summaries(files, problems).items():
            measured[(name, version)] = cost_of(summaries)
    for p in problems:
        print(f"PROBLEM {p}")
    if problems:
        return 1
    out = [GUEST_COST_HEADER.format(game=game_cfg.get("name", game), labels=", ".join(labels)).rstrip("\n"),
           "\t".join(["name"] + versions)]
    notes: list[str] = []
    missing: list[str] = []
    warnings: list[str] = []
    drift: list[str] = []
    path_dependent: set[str] = set()
    for f in rows:
        cells = [f.name]
        for version in versions:
            m = measured.get((f.name, version))
            calls, cycles, lo, hi = (m["calls"], m["cycles"], m["min"], m["max"]) if m else (0, 0, None, 0)
            if m and not calls and m["nested_calls"]:
                # Only reached inside recorded callees of other comparisons (exclusive there: no call sites).
                calls, cycles = m["nested_calls"], m["nested_cycles"]
                warnings.append(f"{f.name} ({version}): measured only inside recorded callees ({calls} calls)")
            if not calls:
                cells.append("-")
                missing.append(f"{f.name} ({version})")
                continue
            if m["unmeasured"]:
                warnings.append(f"{f.name} ({version}): {m['unmeasured']} game calls not measured (run as the original "
                                f"alone by a function with call sites: no exclusive cost)")
            cell = df.format_guest_cost(cycles, calls)
            cells.append(cell)
            if lo is not None and lo != hi:
                path_dependent.add(f.name)
                notes.append(f"#   {f.name} {version}: {lo}..{hi} cycles per call, mean {cycles / calls:.4f} over "
                             f"{calls} calls")
                charged = df.guest_cost_256ths(cell)  # 1/256 cycle
                for run, n, c in m["runs"]:
                    d = (c * df.GUEST_COST_FRACTION - n * charged) / df.GUEST_COST_FRACTION
                    flag = "ALERT" if abs(d) > args.drift_alert else "drift"
                    drift.append(f"{flag} {f.name} {version} {run}: {n} calls, mean {c / n:.4f}, {d:+.0f} cycles "
                                 f"against the charged {cell}")
        out.append("\t".join(cells))
    if notes:
        out += ["# Path-dependent originals (charge exact only on average):"] + notes
    text = "\n".join(out) + "\n"
    exact = len(rows) - len(path_dependent)
    pairs = sum(1 for n in notes)
    refused = bool(missing) and not args.allow_missing
    if args.dry_run:
        print(text, end="")
    elif not refused:
        path.write_bytes(text.encode("utf-8"))
    print(f"{'would write' if args.dry_run or refused else 'written'} {path}: {len(rows)} functions, {exact} with an exact cost "
          f"and {len(path_dependent)} with a mean ({pairs} function x version pairs path-dependent)")
    tracked = {name for name in df.read_guest_costs(game_cfg)}
    for name in sorted(tracked - {f.name for f in rows}):
        print(f"WARNING table row without an enabled function: {name} (dropped from the table)")
    for w in warnings:
        print(f"WARNING {w}")
    for line in drift:
        print(line)
    if any(line.startswith("ALERT") for line in drift):
        print(f"WARNING drift beyond {args.drift_alert} cycles in a run: the charged mean may not fit that scenario")
    for name in missing:
        print(f"{'WARNING' if args.allow_missing else 'ERROR'} no measured call: {name} (nothing would be charged; "
              f"add a scenario that reaches it)")
    if refused:
        print("refused: nothing written (--allow-missing writes the table with '-' for these)")
        return 1
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--game")
    p.add_argument("--bench-root", help="checkout holding tests/ and tools/ab.py (default: the main checkout)")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="scenarios on the oracle executable, every version")
    r.add_argument("--exe", required=True, help='oracle executable; "{version}" replaced by the ID')
    r.add_argument("--label", required=True)
    r.add_argument("--versions")
    r.add_argument("--scenarios", help="comma-separated (default: [decomp] oracle_scenarios of game.toml)")
    r.add_argument("--env", action="append", default=[], metavar="KEY=VALUE")
    r.add_argument("--allow-env", action="store_true",
                   help="accept restricted GCRT_* variables (inherited or --env; see RESTRICTED_ENV_*): an experiment, "
                        "its results never promote")
    r.add_argument("--play", metavar="NAME,...",
                   help="also run these progression scenarios (scenarios/play/<ID>/<name>.scn, tools/run.py) on the "
                        "oracle executable, in order, after the bench: more real starting states")
    r.add_argument("--no-synthetic", action="store_true",
                   help="do not run the synthetic cases (the report then refuses the promotion)")
    k = sub.add_parser("report", help="per-function report and proposed status (--promote writes it)")
    k.add_argument("--label", required=True, help="one or more labels, comma-separated (results are added up)")
    k.add_argument("--versions")
    k.add_argument("--min-cases", type=int, default=DEFAULT_MIN_CASES)
    k.add_argument("--saturation", type=int, default=DEFAULT_SATURATION,
                   help="comparisons from which fewer than --min-cases distinct cases count as all the reachable ones "
                        "(default %(default)s; 0: never)")
    k.add_argument("--max-not-compared", type=int, default=DEFAULT_MAX_NOT_COMPARED,
                   help="real calls a function may leave uncompared (default %(default)s)")
    k.add_argument("--promote", action="store_true",
                   help="write the proposed statuses (refused with thresholds weaker than the defaults)")
    k.add_argument("--dry-run", action="store_true", help="with --promote: show what would be written, write nothing")
    sub.add_parser("hash", help="current hash of the sources and of the synthetic cases")
    c = sub.add_parser("cost", help="guest cost table of the enabled functions from the oracle's measurements")
    c.add_argument("--label", required=True, help="one or more labels, comma-separated (measurements added up)")
    c.add_argument("--versions")
    c.add_argument("--dry-run", action="store_true", help="print the table, write nothing")
    c.add_argument("--include", metavar="NAME,...",
                   help="with --dry-run: also show these disabled rows (oracle test functions)")
    c.add_argument("--allow-missing", action="store_true",
                   help="write the table even if an enabled function has no measured call ('-': nothing charged; "
                        "a production build then refuses it unless --allow-no-guest-cost)")
    c.add_argument("--drift-alert", type=int, default=DRIFT_ALERT, metavar="CYCLES",
                   help="alert when one run's cycles differ from its calls x the charged mean by more than this "
                        f"(default {DRIFT_ALERT})")
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
    return {"run": cmd_run, "report": cmd_report, "replay": cmd_replay, "hash": cmd_hash,
            "cost": cmd_cost}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
