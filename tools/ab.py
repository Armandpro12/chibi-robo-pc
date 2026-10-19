"""A/B validation bench: a candidate executable against native reference runs, on replayed scenarios.

Usage:
  py -3 tools/ab.py build-commit HEAD --tag cand    builds a commit in a clean worktree (build/wt-cand) when the working
                                                    tree is being modified by others; then
                                                    check --exe build/wt-cand/build/<game>-<ID>-Release-cand/bin/<game>.exe
  py -3 tools/ab.py record --ref-exe EXE --rev REV [--scenarios a,b] [--repeat 1] [--jitter 0] [--force] [--ref-dir DIR]
                                                    records the reference with the executable of a validated commit (built
                                                    by build-commit or tools/build.py) in
                                                    tests/<game>/<ID>/reference-native/<scenario>/, with the fingerprint of
                                                    the executable, of the scenario and of the saves left; reproducibility
                                                    checked (identical to the pixel and the bit, otherwise a failure);
                                                    --jitter: "host clock" reruns that measure the natural difference of a
                                                    real-time run
  py -3 tools/ab.py refs                            lists the references, their executable and whether the scenarios have
                                                    changed since (exit code 1 if a reference is stale)
  py -3 tools/ab.py compact <folder>                losslessly recompresses the raw captures of a folder of runs
  py -3 tools/ab.py check [--tag tester | --exe PATH] [--scenarios a,b] [--label NAME]
                          [--hw-zero all|DI,SI,...] [--hw-except ...] [--ignore-watch ADDRESS,...] [--profile exact|realtime]
                          [--ref-dir FOLDER] [--seed-from cand|ref]
                                                    runs the candidate on the scenarios and compares it with the reference
                                                    (reports in tests/<game>/<ID>/ab/<label>/), exit code 1 on a regression.
References are native only: runs of an executable of this project (static boot, cycle clock, fixed RTC), so the
reference and the candidate start the game at the same instant. The emulated reference of milestone 10 (the last
build that still emulated the hardware, kept outside this repository's public history) is no longer rebuilt: its game
started one VI field later (the apploader that executable interpreted), which a former start offset compensated. An older recording stays usable explicitly with --ref-dir.
Chained scenarios ("after <scenario>" directive of tools/scenario.py, e.g. load-game after new-game): the next
scenario starts with a COPY of the user folder left by the previous one (tools/run.py --user-seed):
  record: the one of the previous reference; check --seed-from cand (default): the candidate's, run first
  (added if needed); --seed-from ref: the reference's (the scenario is then judged independently of the previous one).
Several versions of the disc at once (regions):
  --version ID                       one version (default: default_version from game.toml)
  --versions GGTP01,GGTE01 | all     several versions, one after the other; all those of game.toml with "all"
                                     (build-commit, record, check, refs; exit code = the worst of the versions)
  --exe / --ref-exe / --ref-dir      accept "{version}" to target the file of each version, e.g.
                                     --exe "build/wt-us/build/chibi-robo-{version}-Release-us/bin/chibi-robo.exe"
Generic: the game comes from `--game` (optional if there is only one game in games/; mandatory otherwise), the scenarios
from games/<game>/scenarios/<ID>/*.scn (version-specific) then games/<game>/scenarios/*.scn (see tools/scenario.py).
Method, tolerances and results: notes/validation.md; tools: tools/run.py (--scenario), tools/compare_ab.py.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scenario as scn  # noqa: E402
import toolchain as tc  # noqa: E402

TOOLS = Path(__file__).resolve().parent


def sha1(path: Path) -> str:
    return hashlib.sha1(path.read_bytes()).hexdigest()


def git(*args: str, cwd: Path | None = None, check: bool = True) -> str:
    r = subprocess.run(["git", *args], cwd=cwd or tc.ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        raise SystemExit(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def make_link(target: Path, link: Path) -> None:
    """NTFS junction (Windows, no special rights needed) or symbolic link."""
    if link.exists() or link.is_symlink():
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        f"New-Item -ItemType Junction -Path '{link}' -Target '{target}' | Out-Null"], check=True)
    else:
        os.symlink(target, link, target_is_directory=True)


def scenario_files(game: str, version: str, names: str | None) -> list[Path]:
    """Scenarios applicable to `version` (games/<game>/scenarios/<ID>/ then games/<game>/scenarios/, tools/scenario.py)."""
    try:
        files, notes = scn.available(game, tc.ROOT, version, names)
    except scn.ScenarioError as exc:
        raise SystemExit(str(exc))
    for n in notes:
        print(f"== {version}: {n}")
    return files


def default_game() -> str:
    """The game of `games/` if there is only one; otherwise `--game` is mandatory (no hard-coded game name)."""
    games = sorted(p.parent.name for p in (tc.ROOT / "games").glob("*/game.toml"))
    if len(games) == 1:
        return games[0]
    raise SystemExit("--game is mandatory: " + (f"several games in games/ ({', '.join(games)})" if games else "no game in games/"))


def expand(path: str | None, version: str, multi: bool, what: str) -> str | None:
    """`{version}` is replaced by the ID; without the marker, a single path can only serve a single version."""
    if path is None:
        return None
    if "{version}" in path:
        return path.replace("{version}", version)
    if multi:
        raise SystemExit(f'{what}: several versions requested, put "{{version}}" in the path ({path})')
    return path


def run_py(game: str, version: str, scenario_file: Path, exe: Path, out: Path, extra: list[str],
           env_extra: dict[str, str] | None = None) -> int:
    cmd = [sys.executable, str(TOOLS / "run.py"), game, "--version", version, "--scenario", str(scenario_file),
           "--exe", str(exe), "--out", str(out), *extra]
    print("$", " ".join(f"{k}={v}" for k, v in (env_extra or {}).items()), " ".join(cmd))
    return subprocess.run(cmd, env={**os.environ, **(env_extra or {})}).returncode


def compare(ref_run: Path, cand_run: Path, out: Path, extra: list[str]) -> int:
    cmd = [sys.executable, str(TOOLS / "compare_ab.py"), str(ref_run), str(cand_run), "--out", str(out), "--json", *extra]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode not in (0, 1):
        print(r.stdout[-2000:], r.stderr[-2000:])
    return r.returncode


# --------------------------------------------------------------------------------------------------
def build_in_worktree(game: str, version: str, rev: str, wt: Path, tag: str) -> int:
    """Builds `rev` in a detached worktree `wt` (clean tree, independent of the ongoing work of the other agents):
    `external/` shared read-only (junction); `generated/` SPECIFIC to the worktree (the recompiled code depends on the
    signature catalog and the hooks of this commit: if shared, it would conflict with the one of the current
    branch), only the extracted disc is shared."""
    if not (wt / ".git").exists():
        print(f"== detached worktree {wt} on {rev}")
        git("worktree", "add", "--detach", str(wt), rev)
    else:
        git("checkout", "-f", "--detach", rev, cwd=wt)
    head = git("rev-parse", "HEAD", cwd=wt)
    print(f"== worktree on {head[:10]} ({rev})")
    make_link(tc.ROOT / "external", wt / "external")
    gen = wt / "generated" / game / version
    gen.mkdir(parents=True, exist_ok=True)
    make_link(tc.ROOT / "generated" / game / version / "disc", gen / "disc")
    print(f"== this worktree contains junctions to the main checkout; remove it with: py -3 tools/worktree.py remove {wt}")
    manifest = tc.ROOT / "generated" / game / version / "manifest.json"
    if manifest.exists() and not (gen / "manifest.json").exists():
        shutil.copyfile(manifest, gen / "manifest.json")
    cmd = [sys.executable, str(wt / "tools" / "build.py"), game, "--version", version, "--config", "Release", "--tag", tag]
    print("$", " ".join(cmd))
    rc = subprocess.run(cmd, cwd=wt).returncode
    exe = wt / "build" / f"{game}-{version}-Release-{tag}" / "bin" / f"{game}{tc.EXE}"
    if rc == 0 and exe.exists():
        print(f"== executable: {exe} (sha1 {sha1(exe)})")
    return rc


def cmd_build_commit(args) -> int:
    wt = tc.ROOT / "build" / f"wt-{args.tag}"
    return build_in_worktree(args.game, args.version, args.rev, wt, args.tag)


def stats_of(report_json: Path) -> dict:
    r = json.loads(report_json.read_text(encoding="utf-8"))
    cps = [c for c in r["sections"]["checkpoints"] if "ssim" in c]
    audio = r.get("audio") or {}
    saves = r["sections"].get("saves") or []
    return {
        "regression": r["regression"], "failures": r["failures"], "warnings": r["counts"].get("WARN", 0),
        "min_ssim": min((c["ssim"] for c in cps), default=None),
        "max_shift": max((abs(c.get("best", {}).get("shift", 0)) for c in cps), default=0),
        "max_delta_field": max((abs(c.get("delta_field", 0)) for c in cps), default=0),
        "identical_checkpoints": sum(1 for c in cps if c["identical"]), "checkpoints": len(cps),
        "audio_bit_exact": audio.get("bit_exact"), "audio_lag_ms": (audio.get("lag_s") or 0.0) * 1000,
        "audio_env_corr_db": audio.get("env_corr_db"), "audio_dev_p95_db": audio.get("dev_db_p95"),
        "audio_dev_max_db": audio.get("dev_db_max"), "audio_rms_delta_db": (audio.get("rms_db_b") - audio.get("rms_db_a")) if audio else None,
        "audio_duration_delta_s": (audio.get("duration_b") - audio.get("duration_a")) if audio else None,
        "saves": [c["status"] for c in saves],
    }


def strictly_identical(st: dict) -> bool:
    """Two launches of the same executable with the cycle clock must be identical: every checkpoint to the
    pixel, the audio bit-exact, the saves left identical, no warning and no failure."""
    return (not st["regression"] and st["warnings"] == 0 and st["identical_checkpoints"] == st["checkpoints"]
            and bool(st["audio_bit_exact"]) and all(s == "OK" for s in st["saves"]))


# --------------------------------------------------------------------------------------------------
# Scenario plan: preparation ("after"), native references
# --------------------------------------------------------------------------------------------------
REF_DIR = "reference-native"


def ref_root(game: str, version: str) -> Path:
    return tc.ROOT / "tests" / game / version / REF_DIR


def read_meta(game: str, version: str) -> dict | None:
    p = ref_root(game, version) / "reference.json"
    return json.loads(p.read_text(encoding="utf-8")) if p.exists() else None


def plan_scenarios(game: str, version: str, names: str | None, with_dependents: bool = False,
                   with_deps: bool = True) -> list[tuple[Path, scn.Scenario]]:
    """Scenarios to run, in an order where the preparation ("after <scenario>") comes before those that depend on it.

    `names`: selection (file names without .scn, comma-separated; None = all those of the version).
    `with_deps`: adds the scenarios the selected ones depend on (they are needed to run them);
    `with_dependents`: adds those that depend on the selection (they are stale if the selection is redone)."""
    try:
        every, _ = scn.available(game, tc.ROOT, version, None)
        selected, notes = scn.available(game, tc.ROOT, version, names)
    except scn.ScenarioError as exc:
        raise SystemExit(str(exc))
    for n in notes:
        print(f"== {version}: {n}")
    by_name: dict[str, tuple[Path, scn.Scenario]] = {}
    for f in every:
        s = scn.load(f)
        by_name[s.name] = (f, s)
        by_name.setdefault(f.stem, (f, s))
    chosen: dict[str, tuple[Path, scn.Scenario]] = {}
    for f in selected:
        s = scn.load(f)
        chosen[s.name] = (f, s)

    def parent_of(s: scn.Scenario) -> scn.Scenario | None:
        if not s.after:
            return None
        if s.after not in by_name:
            raise SystemExit(f'{version}/{s.name}: "after {s.after}": this scenario does not exist for this version')
        return by_name[s.after][1]

    changed = True
    while changed:
        changed = False
        for name, (f, s) in list(chosen.items()):
            dep = parent_of(s) if with_deps else None
            if dep is not None and dep.name not in chosen:
                print(f'== {version}: {name} assumes "{dep.name}" (after): added')
                chosen[dep.name] = by_name[dep.name]
                changed = True
        if with_dependents:
            for name, (f, s) in list(by_name.items()):
                dep = by_name[s.after][1] if s.after in by_name else None   # broken "after" of another one: no effect here
                if dep is not None and dep.name in chosen and s.name not in chosen:
                    print(f'== {version}: {s.name} depends on "{dep.name}": added (to redo with it)')
                    chosen[s.name] = (f, s)
                    changed = True
    depth: dict[str, int] = {}

    def depth_of(name: str, stack: tuple[str, ...] = ()) -> int:
        if name in stack:
            raise SystemExit(f"{version}: loop in the after directives: {' -> '.join(stack + (name,))}")
        if name not in depth:
            dep = parent_of(chosen[name][1])
            depth[name] = 1 + depth_of(dep.name, stack + (name,)) if dep is not None and dep.name in chosen else 0
        return depth[name]

    for name in chosen:
        depth_of(name)
    return [chosen[n] for n in sorted(chosen, key=lambda n: (depth[n], n))]


def saves_of(run: dict) -> list[dict]:
    """Save files left by a run (`user_files` list of run.json)."""
    return [f for f in (run.get("user_files") or []) if f["path"].startswith("saves/")]


def exe_source(exe: Path, rev: str | None) -> dict:
    """Where the reference executable comes from: announced revision, and the Git tree that contains it (HEAD, modified or not)."""
    info: dict = {}
    if rev:
        info["rev"] = rev
        info["git_commit"] = git("rev-parse", rev + "^{commit}")
    root = next((p for p in exe.resolve().parents if (p / ".git").exists()), None)
    if root is not None:
        info["tree"] = str(root)
        info["tree_head"] = git("rev-parse", "HEAD", cwd=root)
        info["tree_dirty"] = bool(git("status", "--porcelain", cwd=root, check=False))
    return info


def cmd_record(args) -> int:
    """Records the reference: runs of the executable given by `--ref-exe` (a validated commit, built by `build-commit`
    or tools/build.py), stored in reference-native/."""
    game, version = args.game, args.version
    ref_exe = expand(args.ref_exe, version, args.multi, "--ref-exe")
    if not ref_exe:
        raise SystemExit("record requires --ref-exe (executable of a validated commit, e.g. the one from: "
                         "ab.py build-commit <rev> --tag native)")
    exe = Path(ref_exe)
    if not exe.exists():
        raise SystemExit(f"Reference executable missing: {exe} (build it first: py -3 tools/ab.py build-commit <rev>)")
    ref_dir_arg = expand(args.ref_dir, version, args.multi, "--ref-dir")
    dest = Path(ref_dir_arg) if ref_dir_arg else ref_root(game, version)
    dest.mkdir(parents=True, exist_ok=True)
    meta_path = dest / "reference.json"
    meta = json.loads(meta_path.read_text(encoding="utf-8")) if meta_path.exists() else {}
    meta.update({
        "format": "ab-reference/2", "kind": "native", "updated": dt.datetime.now().isoformat(timespec="seconds"),
        "game": game, "version": version, "exe": str(exe.resolve()), "exe_sha1": sha1(exe),
        "run_options": "run.py --scenario ... (fixed RTC 800000000; cycle clock; GCRT_FORCE_ARG from game.toml [run.force])",
        "force_arg": tc.load_game(game).get("run", {}).get("force", {}).get(version, []),
    })
    meta["source"] = exe_source(exe, args.rev)
    meta["git_commit"] = meta["source"].get("git_commit") or meta["source"].get("tree_head", "")
    if not args.rev:
        print("WARNING: --rev missing: the revision of the reference is only inferred from the tree of the executable")
    meta.setdefault("scenarios", {})
    plan = plan_scenarios(game, version, args.scenarios, with_dependents=args.force)
    jitter_rows = []
    status = 0
    refreshed: set[str] = set()
    for sf, sc in plan:
        out = dest / sc.name
        entry_old = meta["scenarios"].get(sc.name, {})
        stale = bool(entry_old) and entry_old.get("sha1") != sc.sha1
        redo = args.force or (bool(sc.after) and sc.after in refreshed)
        if out.exists() and (out / "run.json").exists() and not redo:
            print(f"== {sc.name}: reference already recorded ({out}); --force to redo it"
                  + ("; WARNING: scenario modified since, STALE reference" if stale else ""))
        else:
            if out.exists():
                shutil.rmtree(out)
            seed = dest / sc.after / "user" if sc.after else None
            if seed is not None and not seed.is_dir():
                print(f'FAIL: {sc.name} assumes "{sc.after}" (after): {seed} missing')
                status = 1
                continue
            rc = run_py(game, version, sf, exe, out, ["--user-seed", str(seed)] if seed else [])
            if rc != 0 and not (out / "run.json").exists():
                print(f"FAIL: launch of {sc.name} failed (code {rc})")
                status = 1
                continue
            refreshed.add(sc.name)
        entry = meta["scenarios"].setdefault(sc.name, {})
        entry.update({"sha1": sc.sha1, "after": sc.after, "dir": str(out), "repro": [], "jitter": [], "env": dict(sc.env)})
        run = json.loads((out / "run.json").read_text(encoding="utf-8"))
        entry["final"] = run["summary"]["final"]
        entry["exit_code"] = run.get("exit_code")
        entry["saves"] = [{k: f[k] for k in ("path", "size", "sha1")} for f in saves_of(run)]
        entry["seed_saves"] = [{k: f[k] for k in ("path", "size", "sha1")}
                               for f in (run.get("user_seed_files") or []) if f["path"].startswith("saves/")]
        for i in range(1, args.repeat + 1):
            rdir = out / f"repro-{i}"
            if rdir.exists():
                shutil.rmtree(rdir)
            seed = dest / sc.after / "user" if sc.after else None
            if run_py(game, version, sf, exe, rdir, ["--user-seed", str(seed)] if seed else []) != 0:
                status = 1
                continue
            rc = compare(out, rdir, rdir, [])
            st = stats_of(rdir / "ab-report.json")
            strict = strictly_identical(st)
            entry["repro"].append({"dir": rdir.name, "strictly_identical": strict, **st})
            print(f"== reproducibility {sc.name} #{i}: "
                  f"{'identical to the pixel and the bit' if strict else ('OK but not strictly identical' if rc == 0 else 'DIFFERENCE')}")
            if not strict:
                status = 1
        for j in range(1, args.jitter + 1):
            jdir = out / f"jitter-host-{j}"
            if jdir.exists():
                shutil.rmtree(jdir)
            if run_py(game, version, sf, exe, jdir, ["--clock", "real", "--pacing", "host"]) != 0:
                status = 1
                continue
            # wide tolerances: we MEASURE the difference, we do not judge
            compare(out, jdir, jdir, ["--max-shift", "1000", "--ssim", "0", "--audio-max-lag", "10",
                                      "--audio-env-corr-db", "-1", "--audio-env-corr-lin", "-1", "--audio-rms-db", "100",
                                      "--audio-peak-db", "100", "--audio-p95-db", "100", "--audio-activity", "1",
                                      "--audio-onset", "100", "--audio-duration-abs", "100", "--hits-abs", "100000"])
            row = {"dir": jdir.name, **stats_of(jdir / "ab-report.json")}
            entry["jitter"].append(row)
            jitter_rows.append((sc.name, row))
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8")
    src = f"revision `{meta['git_commit'][:10]}` (native executable of a validated commit)"
    lines = ["# Reference (native): recording", "", f"- {src}, executable `{exe}` (sha1 {meta['exe_sha1'][:12]})", ""]
    for name, e in meta["scenarios"].items():
        lines.append(f"## {name}")
        lines.append(f"- images {e['final'].get('images')}, VI fields {e['final'].get('vi_fields')}, hits {e['final'].get('hits')}"
                     + (f"; prepared by \"{e['after']}\"" if e.get("after") else ""))
        if e.get("saves"):
            lines.append("- saves left: " + ", ".join(f"{s['path']} ({s['size']} bytes, sha1 {s['sha1'][:10]})" for s in e["saves"]))
        for r in e["repro"]:
            lines.append(f"- reproducibility {r['dir']}: {'**identical**' if r.get('strictly_identical') else 'difference'} "
                         f"(identical captures {r['identical_checkpoints']}/{r['checkpoints']}, bit-exact audio {r['audio_bit_exact']}"
                         f", saves {','.join(r.get('saves') or []) or '-'})")
        for r in e["jitter"]:
            lines.append(f"- host clock {r['dir']}: min SSIM {r['min_ssim']}, max shift {r['max_shift']} frames, "
                         f"max cadence difference {r['max_delta_field']} fields, audio lag {r['audio_lag_ms']:+.0f} ms, "
                         f"correlation {r['audio_env_corr_db']}, p95 {r['audio_dev_p95_db']} dB, duration {r['audio_duration_delta_s']:+.2f} s")
    (dest / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    return status


def pick_reference(game: str, version: str, name: str, ref_dir_arg: str | None) -> tuple[str, Path] | None:
    """(kind, reference run folder) of a scenario: the native reference (reference-native/), or the folder of
    references given by `--ref-dir` (kind read from its reference.json, otherwise the folder name; shown in the
    reports). None if missing."""
    d = Path(ref_dir_arg) if ref_dir_arg else ref_root(game, version)
    if not (d / name / "run.json").exists():
        return None
    meta = json.loads((d / "reference.json").read_text(encoding="utf-8")) if (d / "reference.json").exists() else {}
    return meta.get("kind") or d.name, d / name


def cmd_check(args) -> int:
    game, version = args.game, args.version
    ref_dir_arg = expand(args.ref_dir, version, args.multi, "--ref-dir")
    refs = Path(ref_dir_arg) if ref_dir_arg else ref_root(game, version)
    if not (refs / "reference.json").exists():
        raise SystemExit(f"No reference for {version} in {refs} (run: py -3 tools/ab.py --version {version} record "
                         f"--ref-exe EXE --rev REV)")
    exe_arg = expand(args.exe, version, args.multi, "--exe")
    if exe_arg:
        exe = Path(exe_arg)
    else:
        import run as run_mod
        exe = run_mod.find_exe(game, version, args.config, args.tag)
    if not exe.exists():
        raise SystemExit(f"Executable not found for {version}: {exe}")
    label = args.label or f"{args.tag or exe.parent.parent.name}-{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}"
    base = tc.ROOT / "tests" / game / version / "ab" / label
    base.mkdir(parents=True, exist_ok=True)
    env_extra = dict(item.split("=", 1) for item in args.env)
    cmp_extra: list[str] = []
    if args.hw_zero:
        cmp_extra += ["--hw-zero", args.hw_zero]
    if args.hw_except:
        cmp_extra += ["--hw-except", args.hw_except]
    if args.ignore_watch:
        cmp_extra += ["--ignore-watch", args.ignore_watch]
    cmp_extra += ["--profile", args.profile, *args.compare_arg]
    rows = []
    args.results[version] = {"label": label, "exe": str(exe), "exe_sha1": sha1(exe), "rows": rows}
    worst = 0
    plan = plan_scenarios(game, version, args.scenarios, with_deps=(args.seed_from == "cand"))
    needed_as_seed = {s.after for _, s in plan if s.after} if args.seed_from == "cand" else set()
    cand_dirs: dict[str, Path] = {}
    for sf, sc in plan:
        ref = pick_reference(game, version, sc.name, ref_dir_arg)
        if ref is None and sc.name not in needed_as_seed:
            print(f"== {sc.name}: no recorded reference, skipped")
            continue
        label_kind, ref_run = ref if ref else ("-", None)
        recorded: dict = {}
        if ref_run is not None:
            meta_file = ref_run.parent / "reference.json"
            recorded = json.loads(meta_file.read_text(encoding="utf-8")) if meta_file.exists() else {}
            old = recorded.get("scenarios", {}).get(sc.name, {}).get("sha1")
            if old and old != sc.sha1:
                print(f"WARNING: {sc.name}: scenario modified since the {label_kind} reference was recorded "
                      f"({old[:8]} -> {sc.sha1[:8]}): stale reference")
        seed = None
        if sc.after:
            if args.seed_from == "ref":
                seed = ref_run.parent / sc.after / "user" if ref_run is not None else None
            else:
                seed = cand_dirs[sc.after] / "user" if sc.after in cand_dirs else None
            if seed is None or not seed.is_dir():
                rows.append((sc.name, label_kind, "LAUNCH FAILED", f'preparation "{sc.after}" missing ({seed})', ""))
                worst = 1
                continue
        out = base / sc.name
        if out.exists():
            shutil.rmtree(out)
        rc = run_py(game, version, sf, exe, out, [*args.run_arg, *(["--user-seed", str(seed)] if seed else [])], env_extra)
        if rc != 0 and not (out / "run.json").exists():
            rows.append((sc.name, label_kind, "LAUNCH FAILED", "", ""))
            worst = 1
            continue
        cand_dirs[sc.name] = out
        if ref_run is None:
            rows.append((sc.name, "-", "preparation", "run to serve as a starting point (no reference)", ""))
            continue
        crc = compare(ref_run, out, out, cmp_extra)
        st = stats_of(out / "ab-report.json")
        verdict = "REGRESSION" if crc else "ok"
        worst = max(worst, 1 if crc else 0)
        saves_txt = ""
        if st["saves"]:
            saves_txt = ", saves " + ("identical" if all(s == "OK" for s in st["saves"]) else "/".join(sorted(set(st["saves"]))))
        rows.append((sc.name, label_kind, verdict,
                     f"min SSIM {st['min_ssim']}, identical {st['identical_checkpoints']}/{st['checkpoints']}, "
                     f"audio {'bit-exact' if st['audio_bit_exact'] else 'corr. ' + str(st['audio_env_corr_db'])}{saves_txt}",
                     "; ".join(st["failures"])))
    lines = [f"# A/B: {label} ({version})", "", f"- candidate: `{exe}` (sha1 {sha1(exe)[:12]})",
             f"- references: `{refs}`",
             "", "| scenario | reference | verdict | summary | failures |", "|---|---|---|---|---|"]
    lines += [f"| {a} | {b} | {c} | {d} | {e} |" for a, b, c, d, e in rows]
    lines += ["", "Detailed reports: `<scenario>/ab-report.md`."]
    (base / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))
    print(f"\nSummary: {base / 'summary.md'}")
    return worst


def cmd_refs(args) -> int:
    """Lists the recorded references (executable, revision) and reports those whose scenario has changed, and the
    scenarios of the version that have no reference."""
    game, version = args.game, args.version
    meta = read_meta(game, version)
    if meta is None:
        print(f"{version} reference: none (py -3 tools/ab.py --version {version} record --ref-exe EXE --rev REV)")
        return 1
    print(f"{version} reference ({meta.get('kind', 'native')}): executable sha1 {meta.get('exe_sha1', '')[:12]}, revision "
          f"{str(meta.get('git_commit', ''))[:10]}, recorded {meta.get('updated')}")
    current = {}
    try:
        for sf, sc in plan_scenarios(game, version, None, with_deps=False):
            current[sc.name] = sc
    except SystemExit as exc:
        print(f"   {exc}")
    stale = 0
    recorded = meta.get("scenarios", {})
    for name, e in sorted(recorded.items()):
        sc = current.get(name)
        state = "scenario missing" if sc is None else ("up to date" if sc.sha1 == e.get("sha1") else "STALE (scenario modified)")
        stale += state != "up to date"
        repro = e.get("repro") or []
        strict = [r.get("strictly_identical", bool(r.get("audio_bit_exact")) and r.get("identical_checkpoints") == r.get("checkpoints"))
                  for r in repro]
        rep = ("reproducible (identical)" if strict and all(strict) else ("repro not strict" if strict else "repro not done"))
        print(f"   {name:24s} {state:26s} {rep}" + (f"; prepared by {e['after']}" if e.get("after") else "")
              + (f"; saves {len(e.get('saves') or [])}" if e.get("saves") else ""))
    for name in sorted(set(current) - set(recorded)):
        print(f"   {name:24s} NOT RECORDED")
        stale += 1
    return 1 if stale else 0


def cmd_compact(args) -> int:
    """Losslessly recompresses the raw PNGs (1.3 MB) of a folder of runs; checks that the pixels are equal."""
    import numpy as np
    from PIL import Image
    before = after = n = 0
    for png in sorted(Path(args.dir).rglob("*.png")):
        size = png.stat().st_size
        if size < 500_000:
            continue
        with Image.open(png) as im:
            im.load()
            ref = np.asarray(im).copy()
            tmp = png.with_suffix(".tmp.png")
            im.save(tmp, format="PNG", compress_level=6)
        with Image.open(tmp) as im2:
            ok = np.array_equal(np.asarray(im2), ref)
        if not ok:
            tmp.unlink()
            raise SystemExit(f"pixels differ after recompression: {png}")
        tmp.replace(png)
        before += size
        after += png.stat().st_size
        n += 1
    print(f"{n} images recompressed: {before / 1e6:.0f} MB -> {after / 1e6:.0f} MB (identical pixels)")
    return 0


def resolve_versions(args, game_cfg: dict) -> list[str]:
    known = [v["game_id"] for v in game_cfg.get("versions", [])]
    if args.version and args.versions:
        raise SystemExit("--version and --versions are mutually exclusive")
    if args.versions:
        out = known if args.versions.strip().lower() == "all" else [v.strip() for v in args.versions.split(",") if v.strip()]
    else:
        out = [args.version or game_cfg["game"]["default_version"]]
    unknown = [v for v in out if known and v not in known]
    if unknown or not out:
        raise SystemExit(f"unknown version: {', '.join(unknown) or '(none)'} (game.toml: {', '.join(known)})")
    return list(dict.fromkeys(out))


def multi_summary(args, versions: list[str], codes: dict[str, int | str]) -> None:
    """Final table of a check over several versions (also written to tests/<game>/ab-multi/<label>.md)."""
    if not args.results:
        return
    lines = [f"# A/B multi-version: {args.label}", "",
             "| version | scenario | reference | verdict | summary | failures |", "|---|---|---|---|---|---|"]
    for v in versions:
        res = args.results.get(v)
        if not res or not res["rows"]:
            lines.append(f"| {v} | - | - | **{codes[v]}** | {'no scenario run' if res else 'not run'} | |")
            continue
        for a, k, b, c, d in res["rows"]:
            lines.append(f"| {v} | {a} | {k} | {b} | {c} | {d} |")
    lines += ["", "Executables: " + "; ".join(f"{v} `{r['exe']}` (sha1 {r['exe_sha1'][:12]})" for v, r in args.results.items()),
              "Details: tests/<game>/<ID>/ab/<label>/summary.md and <scenario>/ab-report.md."]
    text = "\n".join(lines) + "\n"
    print("\n" + text)
    out = tc.ROOT / "tests" / args.game / "ab-multi"
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{args.label}.md").write_text(text, encoding="utf-8")
    print(f"Multi-version summary: {out / (args.label + '.md')}")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--game", help="folder of games/ (default: the only game present; mandatory if there are several)")
    p.add_argument("--version", help="disc ID (default: default_version from game.toml)")
    p.add_argument("--versions", metavar="ID,ID|all",
                   help='several versions, one after the other ("all": all those of game.toml); '
                        '--exe, --ref-exe and --ref-dir must then contain "{version}"')
    sub = p.add_subparsers(dest="cmd", required=True)
    bc = sub.add_parser("build-commit", help="builds a commit in a clean worktree (build/wt-<tag>)")
    bc.add_argument("rev", help="commit, branch or tag (e.g. HEAD)")
    bc.add_argument("--tag", default="cand", help="build tag (default cand): build/wt-<tag>/build/<game>-<ID>-Release-<tag>")
    r = sub.add_parser("record", help="records the reference (reference-native/)")
    r.add_argument("--scenarios", help="comma-separated names (default: all)")
    r.add_argument("--ref-exe", help='reference executable (a validated commit); "{version}" replaced by the ID')
    r.add_argument("--repeat", type=int, default=1, help="reproducibility reruns (default 1)")
    r.add_argument("--jitter", type=int, default=0, help="reruns with the host clock (natural real-time difference)")
    r.add_argument("--force", action="store_true", help="redo an already recorded reference (and those that depend on it)")
    r.add_argument("--rev", help="Git revision of the reference executable (recorded in reference.json)")
    r.add_argument("--ref-dir", help="folder to record into instead of reference-native/ (e.g. new references kept "
                                     'apart until they replace the current ones; "{version}" replaced by the ID)')
    sub.add_parser("refs", help="lists the recorded references and reports those whose scenario has changed")
    k = sub.add_parser("compact", help="losslessly recompresses the captures of a folder of runs")
    k.add_argument("dir")
    c = sub.add_parser("check", help="compares a candidate with the reference")
    c.add_argument("--scenarios")
    c.add_argument("--tag", help="build tag of the candidate (build/<game>-<ID>-<config>-<tag>)")
    c.add_argument("--config")
    c.add_argument("--exe", help='candidate executable (default: the most recent for --tag); "{version}" replaced by the ID')
    c.add_argument("--label", help="name of the results folder (default: <tag>-<timestamp>)")
    c.add_argument("--ref-dir", help="folder of references to use instead of reference-native/ (e.g. an older recording)")
    c.add_argument("--seed-from", choices=["cand", "ref"], default="cand",
                   help="chained scenarios (after): starting user folder taken from the candidate (default, the previous "
                        "one is run first) or from the reference"),
    c.add_argument("--hw-zero", metavar="all|DI,SI,...")
    c.add_argument("--hw-except", default="")
    c.add_argument("--ignore-watch", default="")
    c.add_argument("--profile", choices=["exact", "realtime"], default="exact",
                   help="comparator tolerances (exact: deterministic runs; realtime: candidate paced by the PC)")
    c.add_argument("--env", action="append", default=[], metavar="KEY=VALUE",
                   help="environment variable of the candidate (e.g. GCRT_TRACE=dvd), repeatable")
    c.add_argument("--run-arg", action="append", default=[], help="option passed to tools/run.py (repeatable)")
    c.add_argument("--compare-arg", action="append", default=[], help="option passed to tools/compare_ab.py (repeatable)")
    args = p.parse_args()
    args.game = args.game or default_game()
    if not (tc.ROOT / "games" / args.game / "game.toml").exists():
        raise SystemExit(f"unknown game: {args.game} (no games/{args.game}/game.toml)")
    game_cfg = tc.load_game(args.game)
    fn = {"build-commit": cmd_build_commit, "record": cmd_record, "check": cmd_check,
          "compact": cmd_compact, "refs": cmd_refs}[args.cmd]
    if args.cmd == "compact":
        return fn(args)
    versions = resolve_versions(args, game_cfg)
    args.multi = len(versions) > 1
    args.results = {}
    if args.multi:  # a single path cannot target several versions: refuse before any launch
        for opt in ("exe", "ref_exe", "ref_dir"):
            expand(getattr(args, opt, None), versions[0], True, "--" + opt.replace("_", "-"))
    if args.cmd == "check" and not args.label:  # same results folder for all versions
        args.label = f"{args.tag or 'exe'}-{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}" if args.multi else None
    codes: dict[str, int | str] = {}
    for v in versions:
        if args.multi:
            print(f"\n######## version {v} ########")
        args.version = v
        try:
            codes[v] = fn(args)
        except SystemExit as exc:  # a failing version does not prevent processing the others
            if not args.multi:
                raise
            print(f"FAIL {v}: {exc}")
            codes[v] = f"failure ({exc})"
    if args.multi:
        if args.cmd == "check":
            multi_summary(args, versions, codes)
        print("\nExit codes per version: " + ", ".join(f"{v}={c}" for v, c in codes.items()))
    return max((c if isinstance(c, int) else 1) for c in codes.values())


if __name__ == "__main__":
    sys.exit(main())
