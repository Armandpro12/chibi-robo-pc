"""Runs the native executable of a recompiled game and stores its outputs (log, traces, captures).

Usage:
  py -3 tools/run.py <game> [--version ID] [--seconds 60] [--capture 5,20,60] [--input "25:A"]
                     [--trace di,dsp] [--watch 80001234,...] [--config Release] [-- executable options]
  py -3 tools/run.py <game> --scenario <name|file.scn> [--exe PATH | --tag TAG] [--out FOLDER]
                     (inputs and checkpoints in presented frames: tools/scenario.py; A/B bench: tools/ab.py;
                     a name is looked up in games/<game>/scenarios/<ID>/ then games/<game>/scenarios/: one scenario
                     per version)

Start in a chosen stage (--stage NAME|NUMBER, names and addresses in games/<game>/game.toml [stage]): when the game
restores a save, the stage stored in the save is replaced once, then the game restores the rest of the save as usual
(counters, story flags...) and later stage changes work normally. A save must be loaded, e.g.:
  py -3 tools/run.py chibi-robo --version GGTE01 --stage backyard --user-seed FOLDER_WITH_A_SAVE --scenario FILE.scn
Without --stage, nothing changes.

User folder of the game (settings, saves): always isolated (<output>/user by default, --user-dir to reuse one).
--user-seed FOLDER first fills it with a COPY of FOLDER (e.g. the user folder left by another scenario: "after"
directive of tools/scenario.py, used by tools/ab.py); run.json stores the list of the folder's files before (seed)
and after the launch (name, size, sha1).

Aurora caches (GX pipelines, shaders: gcrt-data/*.db, SQLite databases): an automatic launch works on a PRIVATE COPY
of the executable's caches (<output>/gcrt-data, deleted after a success; --keep-data keeps it), never on the shared
caches of the executable. Reason: Aurora does not wait on SQLite locks, so simultaneous launches on the same folder
lose their cache and compile the pipelines on the fly (investigation of the intermittent crash, the internal open-issues log).
--data-dir FOLDER or GCRT_DATA_DIR set by the caller: that folder as is; --shared-data: the old behavior.

Outputs: tests/<game>/<ID>/run-<YYYYMMDD-HHMMSS>/
  stdout.log, stderr.log (output of the executable), trace.log (traces timestamped in guest time,
  the game's OSReport, reports), capture_XXXs.png, audio.wav, run.json (parameters and summary).
With --scenario: cp-<name>.png (checkpoint) and cp-<name>@<offset>.png (capture window around it),
  run.json gains "scenario", "checkpoints" (presented frame and VI field of each capture) and
  "summary" (final report, hits, counters, hardware access counter if it exists).
Generic: the addresses watched by default come from games/<game>/game.toml ([run.watch]).

Determinism of the scenarios (notes/validation.md § 10b):
  - CONSOLE CLOCK: fixed, GCRT_RTC = 800000000 s since January 1st 2000 (2025-05-08 06:13:20), for the reference
    as for the candidates (--rtc or an already set GCRT_RTC win). Since step 10b the console time base starts from this
    date (like Dolphin): it dates the saves (GCI header) and the game derives from it, for example, the initial cursor
    of "Load Game". A fixed date keeps the launches strictly reproducible.
  - START TIME: the game starts at cycle 0 (static boot, no interpreted apploader since step 10b), for the
    reference as for the candidates: the bench references are recorded by executables of this project.

Non-intrusive launches by default: no sound on the speakers (WAV in the run folder), minimized window, and the
"no focus" test mode (GCRT_NO_FOCUS=1, runtime/core/platform/gcrt_focus.h: no window of the game, nor its console,
ever takes the foreground) unless the caller set GCRT_NO_FOCUS itself (GCRT_NO_FOCUS=0 to let --window normal
activate the window).
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import abtrace  # noqa: E402
import scenario as scn  # noqa: E402
import toolchain as tc  # noqa: E402

# Fixed RTC of the scenario runs (seconds since January 1st 2000: 800000000 = 2025-05-08 06:13:20): the same value
# for the reference and the candidates, otherwise the console clock (visible on screen, used by the game and, since
# step 10b, the starting point of the time base, hence the date of the saves) makes the runs incomparable. A date
# after 2000, as on a console whose clock has been set: the game treats the origin date differently
# (0 = 2000-01-01 00:00:00: cursor of "Load Game", see notes/validation.md § 10b).
SCENARIO_RTC = "800000000"

# Optional features of the executable, detected by the presence of the string in the binary (the names of the
# environment variables and options are literals). See notes/runtime.md, requests from the tester agent.
CAPABILITY_TOKENS = {
    "input_frames": b"GCRT_INPUT_FRAMES",   # scripted inputs anchored on the presented frames
    "stop_frame": b"GCRT_STOP_FRAME",       # stop at a presented frame
    "capture_frames": b"GCRT_CAPTURE_FRAMES",  # captures at the exact frame
    "clock_flag": b"--clock",               # --clock cycles option (deterministic guest clock)
    "force_arg": b"GCRT_FORCE_ARG",         # value forced at the entry of a function (random seed)
    "force_mem": b"ADDRESS@RETURN",         # GCRT_FORCE_ARG memory items, &=, |=, @RETURN, #COUNT (--stage)
}


def stage_applied(stderr_log: Path, items: list[str]) -> dict[str, bool]:
    """--stage: for each function address of the stage items, whether the executable reported an entry into it
    ("GCRT_FORCE_ARG: entry N/M into ADDRESS" on stderr). False = the item never acted (no save restored).
    The addresses are compared as hexadecimal numbers."""
    text = stderr_log.read_text(encoding="utf-8", errors="replace") if stderr_log.exists() else ""
    seen = set()
    for line in text.splitlines():
        if "GCRT_FORCE_ARG: entry " in line and " into " in line:
            word = line.split(" into ", 1)[1].split()[0]
            try:
                seen.add(int(word, 16))
            except ValueError:
                pass
    result = {}
    for item in items:
        address = item.split(":", 1)[0].split("@", 1)[0]
        result[address] = int(address, 16) in seen
    return result


def stage_items(game_cfg: dict, version: str, choice: str) -> tuple[int, list[str]]:
    """--stage: stage number and GCRT_FORCE_ARG items, from game.toml [stage] (names, count, start.<version>)."""
    cfg = game_cfg.get("stage", {})
    names = {str(k).lower(): int(v) for k, v in cfg.get("names", {}).items()}
    key = choice.strip().lower()
    if key in names:
        number = names[key]
    elif re.fullmatch(r"\d+", key):
        number = int(key)
        count = int(cfg.get("count", 0))
        if count and number >= count:
            raise SystemExit(f"--stage {choice}: stage numbers go from 0 to {count - 1}")
    else:
        known = ", ".join(f"{n} ({v})" for n, v in sorted(names.items(), key=lambda kv: kv[1])) or "none"
        raise SystemExit(f"--stage {choice}: unknown stage. Names: {known}; or a number")
    templates = cfg.get("start", {}).get(version)
    if not templates:
        have = ", ".join(sorted(cfg.get("start", {}))) or "none"
        raise SystemExit(f"--stage is not available on {version} yet (game.toml [stage.start] has no addresses for "
                         f"this version; available: {have})")
    return number, [t.replace("{stage}", str(number)) for t in templates]


def find_exe(game: str, version: str, config: str | None, tag: str | None = None) -> Path:
    configs = [config] if config else ["Release", "RelWithDebInfo", "Debug"]
    candidates = []
    for c in configs:
        exe = tc.build_dir(game, version, c, tag) / "bin" / f"{game}{tc.EXE}"
        if exe.exists():
            candidates.append(exe)
    if not candidates:
        raise SystemExit(f"Executable not found for {game} {version}: run tools/build.py {game} first")
    return max(candidates, key=lambda p: p.stat().st_mtime)  # the most recent


def exe_info(exe: Path) -> dict:
    data = exe.read_bytes()
    return {
        "sha1": hashlib.sha1(data).hexdigest(),
        "size": len(data),
        "mtime": dt.datetime.fromtimestamp(exe.stat().st_mtime).isoformat(timespec="seconds"),
        "capabilities": {name: token in data for name, token in CAPABILITY_TOKENS.items()},
    }


def user_manifest(root: Path) -> list[dict]:
    """List of the files of a user folder: relative path (/), size, sha1. Used to compare the saves left by two
    launches (tools/compare_ab.py) and to check the preparation of a chained scenario."""
    out = []
    for p in sorted(root.rglob("*")):
        if p.is_file():
            data = p.read_bytes()
            out.append({"path": p.relative_to(root).as_posix(), "size": len(data), "sha1": hashlib.sha1(data).hexdigest()})
    return out


def prepare_private_data_dir(exe: Path, dest: Path) -> dict:
    """Aurora data folder PRIVATE to the launch: consistent copy of the executable's SQLite caches.

    Why: by default the executable stores its GX pipeline and shader caches (gcrt-data/*.db, SQLite databases in WAL mode)
    in <executable folder>/gcrt-data, so they are SHARED by every launch of the same executable. Aurora sets no busy
    timeout on them (no sqlite3_busy_timeout): when two launches start at the same time, one fails on
    "database is locked", loses its cache and recompiles its pipelines on the fly, up to the 3D scene (intermittent
    0xC0000005 crash of 2026-10-11 on a shared cache, the internal open-issues log). An automatic launch therefore works on its
    own copy, made with SQLite's backup API (consistent even if another process writes at the same time).
    The executable's caches are never modified by an automatic launch."""
    src = exe.parent / "gcrt-data"
    dest.mkdir(parents=True, exist_ok=True)
    info: dict = {"dir": str(dest), "source": str(src), "copied": [], "failed": []}
    if not src.is_dir():
        return info
    for db in sorted(src.glob("*.db")):
        target = dest / db.name
        try:
            s = sqlite3.connect(str(db), timeout=30)
            try:
                d = sqlite3.connect(str(target))
                try:
                    s.backup(d)
                finally:
                    d.close()
            finally:
                s.close()
            info["copied"].append(db.name)
        except sqlite3.Error as exc:  # unreadable cache: the launch starts from an empty cache (slower, correct)
            info["failed"].append(f"{db.name}: {exc}")
            target.unlink(missing_ok=True)
    return info


def store_png(src: Path, dst: Path) -> None:
    """Copies a capture, recompressing it losslessly (the raw captures of the runtime are not compressed:
    1.3 MB each); the pixels are strictly identical. Fallback: a plain copy."""
    try:
        from PIL import Image
        with Image.open(src) as im:
            im.load()
            im.save(dst, format="PNG", compress_level=6)
    except Exception:  # noqa: BLE001 - Pillow missing or unreadable image: keep the original
        shutil.copyfile(src, dst)


def build_checkpoints(out: Path, sc: scn.Scenario, captures: list[dict]) -> list[dict]:
    """Stores the captures of a scenario run under stable names and returns the description of the checkpoints
    (files, presented frame and VI field of each capture)."""
    mapped = scn.map_checkpoints(sc, captures)
    used: set[str] = set()
    result = []
    for m in mapped:
        entry: dict = {"name": m["name"], "frame": m["frame"], "soft": m["soft"], "center": None,
                       "center_image": None, "center_field": None, "window": []}
        for cap in m["window"]:
            src = out / cap["file"]
            if not src.exists():
                continue
            offset = cap["image"] - m["frame"]
            dst_name = f"cp-{m['name']}.png" if offset == 0 else f"cp-{m['name']}@{offset:+d}.png"
            store_png(src, out / dst_name)
            used.add(cap["file"])
            if offset == 0:
                entry["center"], entry["center_image"], entry["center_field"] = dst_name, cap["image"], cap["field"]
            entry["window"].append({"file": dst_name, "image": cap["image"], "field": cap["field"], "offset": offset})
        entry["window"].sort(key=lambda w: w["image"])
        result.append(entry)
    for name in used:  # the raw files were copied under their checkpoint names
        try:
            (out / name).unlink()
        except OSError:
            pass
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", help="folder in games/ (e.g. chibi-robo)")
    parser.add_argument("--version", help="disc ID (default: default_version from game.toml)")
    parser.add_argument("--config", help="build configuration (default: the most recent)")
    parser.add_argument("--exe", help="executable to run (default: build/<game>-<ID>-<config>/bin)")
    parser.add_argument("--tag", help="suffix of the build folder, like tools/build.py --tag")
    parser.add_argument("--seconds", type=float, default=60.0, help="duration in seconds (default 60)")
    parser.add_argument("--capture", default="", help="capture times in seconds, e.g. 5,20,60")
    parser.add_argument("--input", default="", help="scripted inputs of port 0, e.g. '25:A:0.2;30:START'")
    parser.add_argument("--trace", default="", help="additional trace categories (GCRT_TRACE)")
    parser.add_argument("--watch", help="hexadecimal addresses to count (default: game.toml [run.watch])")
    parser.add_argument("--language", help="SRAM language (0 English by default)")
    parser.add_argument("--dvd-rate", help="drive rate in bytes/s (0 = instantaneous)")
    parser.add_argument("--out", help="output folder (default: tests/<game>/<ID>/run-<timestamp>)")
    parser.add_argument("--user-dir", help="user folder of the game (settings, saves); default: <output>/user "
                                           "(never the real user folder during an automatic launch)")
    parser.add_argument("--user-seed", metavar="FOLDER",
                        help="copies this folder into the user folder of the launch before starting (preparation of a "
                             "chained scenario: saves left by another one); the user folder must be empty")
    parser.add_argument("--capture-frames", default="",
                        help="numbers of the presented frames to capture (1 = first copy to the XFB), e.g. 50,500")
    parser.add_argument("--capture-fields", default="",
                        help="VI field numbers: capture at the first frame presented afterwards, e.g. 250,1000")
    parser.add_argument("--clock", choices=["real", "cycles"],
                        help="game clock: real time (default of the executable) or cycles (reproducible)")
    parser.add_argument("--guest-time", action="store_true",
                        help="--seconds counted in guest time, the game's own clock (stop through GCRT_STOP_AT), "
                             "useful with --clock cycles")
    parser.add_argument("--rtc", help="fixed console clock (seconds since 2000), for reproducibility")
    parser.add_argument("--audio", choices=["wav", "sdl", "off"], default="wav",
                        help="sound: wav (default: recorded into <run folder>/audio.wav, nothing on the "
                             "speakers), sdl (speakers + WAV) or off (no sound and no WAV)")
    parser.add_argument("--audio-device", action="store_true",
                        help="also sends the sound to the speakers (same as --audio sdl)")
    parser.add_argument("--wav", action="store_true", help="(no effect: the WAV is recorded by default)")
    parser.add_argument("--window", choices=["minimized", "background", "normal"], default="minimized",
                        help="window: minimized (default: minimized, without stealing the foreground), background "
                             "(shown without activation) or normal (still not activated unless GCRT_NO_FOCUS=0 is set "
                             "by the caller)")
    parser.add_argument("--viewport", choices=["fit", "stretch"], default="fit",
                        help="rendering: fit (default of the runs: EFB at its exact size, e.g. 640x528, like Dolphin at 1x; "
                             "window at the EFB aspect ratio) or stretch (default of the executable: 528 lines, 4:3 width)")
    parser.add_argument("--copy-filter", choices=["unblended", "full", "off"],
                        help="display copy filter: unblended (default, like Dolphin), full (console), off")
    parser.add_argument("--pacing", choices=["throttle", "host"],
                        help="pacing: cycles throttled to real time (default) or host clock")
    parser.add_argument("--pipelines", choices=["sync", "frame", "async"],
                        help="sync (default): no draw skipped while the pipelines compile; frame: wait at the end "
                             "of each frame only (former behavior); async: Aurora's behavior (GCRT_PIPELINES)")
    parser.add_argument("--scenario", metavar="NAME|FILE",
                        help="scenario of inputs and checkpoints in presented frames "
                             "(games/<game>/scenarios/[<ID>/]<name>.scn, format: tools/scenario.py); "
                             "replaces --seconds/--capture*/--input")
    parser.add_argument("--force-arg", metavar="SPEC|none",
                        help='scenario: GCRT_FORCE_ARG ("ADDRESS:rN=VALUE;..."); default: game.toml [run.force] of the '
                             "version (fixed random seed); none = force nothing")
    parser.add_argument("--stage", metavar="NAME|NUMBER",
                        help="start in this stage: when the game restores a save, the saved stage is replaced once "
                             "(the rest of the save is restored, later stage changes are free); names and addresses "
                             "in game.toml [stage] (e.g. chibi-robo: backyard, debug, living-room). Needs a save that "
                             "gets loaded (--user-seed, scenario)")
    parser.add_argument("--data-dir", metavar="FOLDER",
                        help="Aurora data folder (GCRT_DATA_DIR: pipeline and shader caches); default: PRIVATE copy "
                             "of the executable's caches in <output>/gcrt-data (deleted after a successful launch), "
                             "so that simultaneous launches do not share the SQLite databases; a GCRT_DATA_DIR "
                             "already set by the caller is used as is")
    parser.add_argument("--shared-data", action="store_true",
                        help="old behavior: do not set GCRT_DATA_DIR (shared caches in <executable>/gcrt-data)")
    parser.add_argument("--keep-data", action="store_true",
                        help="keeps the private data folder of the launch even after a success (diagnosis)")
    parser.add_argument("--note", default="", help="free comment stored in run.json")
    argv = sys.argv[1:]
    extra = argv[argv.index("--") + 1:] if "--" in argv else []
    args = parser.parse_args(argv[:argv.index("--")] if "--" in argv else argv)

    game_cfg = tc.load_game(args.game)
    version = args.version or game_cfg["game"]["default_version"]
    exe = Path(args.exe) if args.exe else find_exe(args.game, version, args.config, args.tag)
    if not exe.exists():
        raise SystemExit(f"Executable not found: {exe}")
    stage = stage_items(game_cfg, version, args.stage) if args.stage else None
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = (Path(args.out) if args.out else tc.ROOT / "tests" / args.game / version / f"run-{stamp}").resolve()
    out.mkdir(parents=True, exist_ok=True)

    watch = args.watch
    if watch is None:
        watch = ",".join(game_cfg.get("run", {}).get("watch", {}).get(version, []))

    env = dict(os.environ)
    env["GCRT_TRACE_FILE"] = str(out / "trace.log")
    # User folder of the game (settings, controls.ini, saves): an automatic launch must never write into the
    # real user folder (%APPDATA%). By default, a new folder in the run folder;
    # --user-dir to reuse one (e.g. save then reload over two launches).
    # Priority: --user-dir, then GCRT_USER_DIR already set by the caller (e.g. a test), then <output>/user.
    if args.user_dir:
        user_dir = Path(args.user_dir)
    elif os.environ.get("GCRT_USER_DIR"):
        user_dir = Path(os.environ["GCRT_USER_DIR"])
    else:
        user_dir = out / "user"
    seed_files = None
    if args.user_seed:
        seed = Path(args.user_seed).resolve()
        if not seed.is_dir():
            raise SystemExit(f"--user-seed: folder not found: {seed}")
        if user_dir.exists() and any(user_dir.iterdir()):
            raise SystemExit(f"--user-seed requires an empty user folder ({user_dir} is not)")
        shutil.copytree(seed, user_dir, dirs_exist_ok=True)
        seed_files = user_manifest(user_dir)
        print(f"== user folder prepared from {seed} ({len(seed_files)} files)")
    user_dir.mkdir(parents=True, exist_ok=True)
    env["GCRT_USER_DIR"] = str(user_dir)
    # Aurora data folder: priority --data-dir, then GCRT_DATA_DIR set by the caller, then (unless --shared-data)
    # a private copy of the executable's caches in <output>/gcrt-data (see prepare_private_data_dir).
    data_info: dict | None = None
    private_data: Path | None = None
    if args.data_dir:
        env["GCRT_DATA_DIR"] = str(Path(args.data_dir).resolve())
        data_info = {"dir": env["GCRT_DATA_DIR"], "mode": "option --data-dir"}
    elif os.environ.get("GCRT_DATA_DIR"):
        data_info = {"dir": os.environ["GCRT_DATA_DIR"], "mode": "environment GCRT_DATA_DIR"}
    elif not args.shared_data:
        private_data = out / "gcrt-data"
        data_info = {**prepare_private_data_dir(exe, private_data), "mode": "private copy"}
        env["GCRT_DATA_DIR"] = str(private_data)
        print(f"== Aurora caches: private copy ({', '.join(data_info['copied']) or 'no cache to copy: cold start'})"
              + (f"; FAILURES: {data_info['failed']}" if data_info["failed"] else ""))
    else:
        data_info = {"dir": str(exe.parent / "gcrt-data"), "mode": "shared with the executable (--shared-data)"}
    if args.trace:
        env["GCRT_TRACE"] = args.trace
    if watch:
        env["GCRT_WATCH"] = watch
    if args.language is not None:
        env["GCRT_LANGUAGE"] = args.language
    if args.dvd_rate is not None:
        env["GCRT_DVD_RATE"] = args.dvd_rate
    if args.rtc is not None:
        env["GCRT_RTC"] = args.rtc
    if args.pipelines:
        env["GCRT_PIPELINES"] = args.pipelines
    if args.pacing:
        env["GCRT_PACING"] = args.pacing
    # Non-intrusive automatic launches: no sound on the speakers without --audio-device
    # (WAV recording in the run folder), a window that does not steal the foreground.
    audio = "sdl" if args.audio_device else args.audio
    env["GCRT_AUDIO"] = audio
    if audio != "off":
        env["GCRT_AUDIO_WAV"] = str(out / "audio.wav")
    else:
        env.pop("GCRT_AUDIO_WAV", None)
    env["GCRT_WINDOW"] = args.window
    env.setdefault("GCRT_NO_FOCUS", "1")  # "no focus" test mode, unless the caller chose (GCRT_NO_FOCUS=0)
    if args.viewport:
        env["GCRT_VIEWPORT"] = args.viewport
    if args.copy_filter:
        env["GCRT_COPY_FILTER"] = args.copy_filter
    clock = ["--clock", args.clock] if args.clock else []

    info = exe_info(exe)
    sc: scn.Scenario | None = None
    if args.scenario:
        if args.input or args.capture or args.capture_frames or args.capture_fields or args.guest_time:
            raise SystemExit("--scenario replaces --input, --capture, --capture-frames, --capture-fields and --guest-time")
        try:
            sc = scn.load(args.scenario, args.game, tc.ROOT, version)
        except scn.ScenarioError as exc:
            raise SystemExit(f"Invalid scenario: {exc}")
        if sc.version and sc.version != version:
            print(f"WARNING: scenario recorded on {sc.version}, run on {version} (frames possibly shifted)")
        if sc.after and not (args.user_seed or args.user_dir or os.environ.get("GCRT_USER_DIR")):
            print(f'WARNING: "{sc.name}" assumes the user folder left by "{sc.after}" (after directive): '
                  f"run with --user-seed <folder of {sc.after}>/user or --user-dir, or go through tools/ab.py")
        caps = info["capabilities"]
        missing = [name for name in ("input_frames", "stop_frame", "capture_frames") if not caps[name]]
        if missing:
            raise SystemExit(
                f"{exe.name} cannot replay a scenario: strings missing from the binary for {', '.join(missing)} "
                "(GCRT_INPUT_FRAMES, GCRT_STOP_FRAME, GCRT_CAPTURE_FRAMES: see notes/runtime.md, requests from the "
                "tester agent, 2026-10-10); every executable built from this repository since milestone 10a has them.")
        if "GCRT_RTC" not in env:
            env["GCRT_RTC"] = SCENARIO_RTC
        # Determinism: game-specific forced values (random seed), read from game.toml [run.force]; the
        # GCRT_FORCE_ARG environment variable already set, or --force-arg, win.
        force = args.force_arg
        if force is None and "GCRT_FORCE_ARG" not in env:
            force = ";".join(game_cfg.get("run", {}).get("force", {}).get(version, []))
        if force and force.lower() != "none":
            env["GCRT_FORCE_ARG"] = force
            if not caps["force_arg"]:
                print(f"WARNING: {exe.name} does not know GCRT_FORCE_ARG: the random seed is not fixed "
                      "(non-strict A/B comparison)")
        elif force and force.lower() == "none":
            env.pop("GCRT_FORCE_ARG", None)
        env["GCRT_CAPTURE_DIR"] = str(out)
        env["GCRT_CAPTURE_FRAMES"] = ",".join(str(f) for f in sc.capture_frames())
        if sc.inputs:
            env["GCRT_INPUT_FRAMES"] = sc.input_spec_frames()
        env["GCRT_STOP_FRAME"] = str(sc.end)
        # Scenario-specific variables ("env" directive); a value already set by the caller wins.
        for key, value in sc.env.items():
            if key not in os.environ:
                env[key] = value
        # Real-time limit (safety): 10 frames/s at worst, plus a margin.
        wall = max(900.0, sc.end / 10.0 + 300.0)
        if not clock and caps["clock_flag"]:
            clock = ["--clock", "cycles"]  # guest time = executed cycles: deterministic (executables that accept --clock)
        cmd = [str(exe), "--max-seconds", f"{wall:g}", *clock, *extra]
        seconds = 0.0
    else:
        captures = [float(c) for c in re.split(r"[,; ]+", args.capture) if c]
        # Captures at the end of the requested duration need one more frame: 1.5 s margin.
        seconds = args.seconds + (1.5 if captures and max(captures) >= args.seconds - 0.5 else 0.0)
        if captures:
            env["GCRT_CAPTURE"] = ",".join(f"{c:g}" for c in captures)
            env["GCRT_CAPTURE_DIR"] = str(out)
        if args.input:
            env["GCRT_INPUT"] = args.input
        if args.capture_frames or args.capture_fields:
            env["GCRT_CAPTURE_DIR"] = str(out)
        if args.capture_frames:
            env["GCRT_CAPTURE_FRAMES"] = args.capture_frames
        if args.capture_fields:
            env["GCRT_CAPTURE_FIELDS"] = args.capture_fields
        if args.guest_time:
            # Stop at the requested guest time; generous real-time limit for safety.
            env["GCRT_STOP_AT"] = f"{seconds:g}"
            cmd = [str(exe), "--max-seconds", f"{max(seconds * 10, 600):g}", *clock, *extra]
        elif captures:
            # The capture times are in guest time, which can lag behind on a loaded host:
            # stop at the guest time (after the last capture), real-time limit for safety.
            env["GCRT_STOP_AT"] = f"{seconds:g}"
            cmd = [str(exe), "--max-seconds", f"{seconds * 2 + 30:g}", *clock, *extra]
        else:
            cmd = [str(exe), "--max-seconds", f"{seconds:g}", *clock, *extra]

    if stage:
        # --stage: the stage items come after the forced values already chosen (random seed of the scenarios,
        # --force-arg, or GCRT_FORCE_ARG of the caller).
        if not info["capabilities"]["force_mem"]:
            raise SystemExit(f"--stage: {exe.name} is too old (its GCRT_FORCE_ARG lacks memory items, &=, |= or "
                             "@RETURN): rebuild it")
        env["GCRT_FORCE_ARG"] = ";".join([s for s in [env.get("GCRT_FORCE_ARG", "")] if s] + stage[1])
        print(f"Stage {stage[0]} ({args.stage}): GCRT_FORCE_ARG={env['GCRT_FORCE_ARG']}")

    label = f"scenario {sc.name}" if sc else f"{seconds:g} s"
    print(f"== {exe.name} {version}, {label} -> {out}")
    t0 = time.time()
    with (out / "stdout.log").open("w", encoding="utf-8", errors="replace") as so, \
            (out / "stderr.log").open("w", encoding="utf-8", errors="replace") as se:
        proc = subprocess.run(cmd, env=env, stdout=so, stderr=se, cwd=out)
    wall_s = time.time() - t0

    trace = (out / "trace.log").read_text(encoding="utf-8", errors="replace") if (out / "trace.log").exists() else ""
    stdout = (out / "stdout.log").read_text(encoding="utf-8", errors="replace")
    final = [l for l in trace.splitlines() if "FINAL REPORT" in l]
    stop = next((l for l in stdout.splitlines() if l.startswith("Stop:")), "")
    parsed = abtrace.parse_trace(trace)
    summary = {
        "game": args.game,
        "version": version,
        "exe": str(exe),
        "exe_info": info,
        "command": cmd,
        "environment": {k: v for k, v in env.items() if k.startswith("GCRT_")},
        "data": data_info,
        "user_dir": str(user_dir),
        "user_seed": str(Path(args.user_seed).resolve()) if args.user_seed else None,
        "user_seed_files": seed_files,
        "user_files": user_manifest(user_dir),
        "exit_code": proc.returncode,
        "wall_seconds": round(wall_s, 2),
        "stop": stop,
        "final_report": final[-1] if final else "",
        "captures": sorted(p.name for p in out.glob("capture_*.png")),
        "osreport_lines": sum(1 for l in trace.splitlines() if "[osreport]" in l),
        "unhandled_lines": sum(1 for l in trace.splitlines() if "[unhandled]" in l),
        "stage": ({"choice": args.stage, "number": stage[0], "force": stage[1],
                   "applied": stage_applied(out / "stderr.log", stage[1])} if stage else None),
        "note": args.note,
        "summary": {"final": parsed["final"], "gx": parsed["gx"], "hw_access": parsed["hw_access"],
                    "unhandled": parsed["unhandled"], "stop_lines": parsed["stop_lines"]},
    }
    wav = out / "audio.wav"
    summary["audio"] = {"file": wav.name, "bytes": wav.stat().st_size} if wav.exists() else None
    if sc is not None:
        summary["scenario"] = sc.to_dict()
        summary["checkpoints"] = build_checkpoints(out, sc, parsed["captures"])
        summary["captures"] = sorted(p.name for p in out.glob("cp-*.png"))
    (out / "run.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"exit code {proc.returncode}, {wall_s:.1f} s; {stop}")
    if final:
        print(final[-1])
    if sc is not None:
        missing = [c["name"] for c in summary["checkpoints"] if c["center"] is None]
        print(f"checkpoints: {len(sc.checks) - len(missing)}/{len(sc.checks)} captured"
              + (f" (missing: {', '.join(missing)})" if missing else ""))
        if parsed["hw_access"] is not None:
            print(f"hardware accesses: {parsed['hw_access']}")
    else:
        print(f"captures: {', '.join(summary['captures']) or 'none'}")
    if stage and not all(summary["stage"]["applied"].values()):
        missed = [a for a, ok in summary["stage"]["applied"].items() if not ok]
        print(f"WARNING: --stage {args.stage} did not take effect (no entry into {', '.join(missed)}): "
              "was a save loaded during the run?")
    # Private data folder: deleted after a successful launch (kept if it crashed, for the diagnosis).
    if private_data is not None and not args.keep_data and proc.returncode in (0, 2):
        shutil.rmtree(private_data, ignore_errors=True)
    # A requested stop (window closed, GCRT_STOP_AT) exits with code 2 of gcrt_main.
    if proc.returncode == 2 and "stop requested" in stop:
        return 0
    return 0 if proc.returncode in (0,) else proc.returncode


if __name__ == "__main__":
    sys.exit(main())
