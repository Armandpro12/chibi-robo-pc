"""Builds the native executable of a recompiled GameCube game (generic, parameterized by games/<game>/game.toml).

Usage: py -3 tools/build.py <game> [--version ID] [--config Release|Debug] [--jobs N]
                            [--force-recomp] [--recomp-only] [--no-symbols] [--tag T]
                            [--pgo auto|off|use|train] [--pgo-dir FOLDER] [--pgo-scenarios a,b]

Steps:
  1. disc extraction if `generated/<game>/<ID>/manifest.json` is missing (tools/extract.py);
  2. recompilation by DolRecomp (C backend) of the DOL then of the REL folder, into
     `generated/<game>/<ID>/recomp/` (`dol/` and `rel/`). The C is only regenerated if the fingerprint changes
     (input files, DolRecomp executable and commit, options, symbol map);
  3. CMake configuration (Ninja, clang-cl) in `build/<game>-<ID>-<config>/`;
  4. compilation.
Profile-guided optimization (`--pgo`, default auto: local profile build/pgo/<game>-<ID>/ if it is up to date, otherwise
a build without PGO with a message; production and checks: tools/pgo.py; profiles never go into Git).
Nothing produced here must be versioned (`generated/` and `build/` are ignored by Git).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402

# Increment when the way DolRecomp is called changes (forces the regeneration).
RECOMP_SCHEME = 2
MEM1_BASE = 0x80000000
MEM1_SIZE = 24 * 1024 * 1024


def sha1_file(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def fnv1a64_file(path: Path) -> int:
    """64-bit FNV-1a: the runtime computes the same value to check that it loads the right DOL."""
    h = 0xCBF29CE484222325
    data = path.read_bytes()
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def git_head(repo: Path) -> str:
    git = tc.find_git()
    if not git:
        return "?"
    out = subprocess.run([str(git), "-C", str(repo), "rev-parse", "HEAD"], capture_output=True, text=True)
    head = out.stdout.strip() or "?"
    diff = subprocess.run([str(git), "-C", str(repo), "diff", "HEAD"], capture_output=True)
    if diff.stdout:
        head += "+" + hashlib.sha1(diff.stdout).hexdigest()[:12]
    return head


def parse_int(value) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def run_logged(cmd: list, log: Path, env: dict | None = None) -> float:
    """Runs a command, writes its output to `log`, fails with the end of the log if it fails."""
    print("+", " ".join(f'"{c}"' if " " in str(c) else str(c) for c in cmd), flush=True)
    start = time.perf_counter()
    with log.open("w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.run([str(c) for c in cmd], stdout=f, stderr=subprocess.STDOUT, env=env)
    elapsed = time.perf_counter() - start
    if proc.returncode != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-30:]
        print("\n".join(tail))
        raise SystemExit(f"Failed (code {proc.returncode}), full log: {log}")
    return elapsed


def summarize_recomp_log(log: Path) -> dict:
    text = log.read_text(encoding="utf-8", errors="replace")
    decoded = known = embedded = unknown = 0
    for m in re.finditer(r"(\d+) decoded, (\d+) known, (?:(\d+) embedded data, )?(\d+) unknown", text):
        decoded += int(m.group(1))
        known += int(m.group(2))
        embedded += int(m.group(3) or 0)
        unknown += int(m.group(4))
    smc = "may patch executable memory" in text
    return {"decoded": decoded, "known": known, "embedded_data": embedded, "unknown": unknown, "smc_warning": smc}


def rel_layout_end(log: Path) -> int:
    """End address of the REL area from the DolRecomp log (sections and BSS of each module)."""
    end = 0
    text = log.read_text(encoding="utf-8", errors="replace")
    for m in re.finditer(r"addr:0x([0-9A-Fa-f]{8})\s+size:0x([0-9A-Fa-f]+)", text):
        end = max(end, int(m.group(1), 16) + int(m.group(2), 16))
    return end


PATCHES = tc.ROOT / "tools" / "patches" / "dolrecomp"
AURORA_PATCHES = tc.ROOT / "tools" / "patches" / "aurora"
HLE_CATALOG = tc.ROOT / "runtime" / "core" / "hle" / "sdk_signatures.tsv"
HLE_NATIVE_HOOKS = tc.ROOT / "runtime" / "core" / "hle" / "native_hooks.txt"
REL_AUTO_ALIGN = 0x10000  # DolRecomp, src/app/pipeline.h
TAIL_ALIGN = 32


def apply_patches(repo: Path, patch_dir: Path, name: str) -> set[Path] | None:
    """Applies the patches patch_dir/*.patch to the checkout repo when they are missing (a patch that can be
    reversed is already there). Returns the files they touch, or None without patches or without Git."""
    git = tc.find_git()
    patches = sorted(patch_dir.glob("*.patch"))
    if not patches or not git:
        return None
    touched: set[Path] = set()
    for patch in patches:
        for line in patch.read_text(encoding="utf-8").splitlines():
            if line.startswith("+++ b/"):
                touched.add(repo / line[6:])
        applied = subprocess.run([str(git), "-C", str(repo), "apply", "--reverse", "--check", str(patch)],
                                 capture_output=True).returncode == 0
        if applied:
            continue
        if subprocess.run([str(git), "-C", str(repo), "apply", "--check", str(patch)], capture_output=True).returncode:
            raise SystemExit(f"Patch {patch.name} does not apply to {repo} "
                             f"(different commit or local changes?).")
        print(f"== {name} patch: {patch.name}")
        tc.run([git, "-C", repo, "apply", patch])
    return touched


def aurora_dir() -> Path:
    """Aurora sources used by the build: external/aurora, or the folder named by the GCRT_AURORA_DIR environment
    variable (a private copy, e.g. to try a change of Aurora without touching the shared checkout)."""
    custom = os.environ.get("GCRT_AURORA_DIR")
    return Path(custom).resolve() if custom else tc.EXTERNAL / "aurora"


def ensure_aurora_patches() -> None:
    """Applies the Aurora patches (tools/patches/aurora/*.patch) to the Aurora sources of the build. Aurora is built
    by our CMake project (runtime/core/platform/sources.cmake): Ninja recompiles the patched files."""
    repo = aurora_dir()
    if (repo / "CMakeLists.txt").exists():
        apply_patches(repo, AURORA_PATCHES, "Aurora")


def ensure_dolrecomp_patches() -> None:
    """Applies the patches of the local DolRecomp fork (tools/patches/dolrecomp/*.patch) if they are missing,
    then rebuilds DolRecomp if a modified source is newer than the executable."""
    repo = tc.EXTERNAL / "DolRecomp"
    touched = apply_patches(repo, PATCHES, "DolRecomp")
    if touched is None:
        return
    exe = tc.find_dolrecomp()
    if exe and all(p.stat().st_mtime <= exe.stat().st_mtime for p in touched if p.exists()):
        return
    print("== Rebuilding DolRecomp (local patches)")
    env = tc.msvc_env()
    tc.run([tc.find_cmake(), "--build", repo / "build", "--config", "Release", "--parallel"], env=env,
           stdout=subprocess.DEVNULL)


def rel_layout(rels: list[Path], rel_base: int) -> list[dict]:
    """Virtual addresses that DolRecomp gives the RELs in folder mode (same rules as src/frontend/container/
    rel.c and src/app/pipeline.c: modules laid out in path order, aligned on 64 KiB; BSS at
    align(base + fixSize)). Adds the location where the runtime moves the end of the file (imports and relocations)
    when the BSS overlaps it (see notes/hle.md § 4, REL and OSLink)."""
    out = []
    cursor = rel_base
    for path in rels:
        data = path.read_bytes()
        be = lambda o: int.from_bytes(data[o:o + 4], "big")  # noqa: E731
        size = len(data)
        version = be(0x1C)
        fix_size = be(0x48) if version >= 3 and size >= 0x4C else size
        bss_align = (be(0x44) if size >= 0x48 else 4) or 4
        align_up = lambda v, a: (v + a - 1) // a * a  # noqa: E731
        base = cursor
        sections = []
        bss_cursor = align_up(base + fix_size, bss_align)
        bss_addr, bss_size = 0, 0
        for i in range(be(0x0C)):
            raw, sec_size = be(be(0x10) + 8 * i), be(be(0x10) + 8 * i + 4)
            offset, executable = raw & ~1, bool(raw & 1)
            if not sec_size:
                continue
            if offset == 0 and not executable:
                if not bss_addr:
                    bss_addr = bss_cursor
                bss_size = bss_cursor + sec_size - bss_addr
                sections.append((i, bss_cursor, sec_size, False, data[0:0]))
                bss_cursor = align_up(bss_cursor + sec_size, bss_align)
            else:
                sections.append((i, base + offset, sec_size, executable, data[offset:offset + sec_size]))
        slot_end = align_up(base + size + be(0x20), REL_AUTO_ALIGN)
        tail_size = size - fix_size
        tail_addr = base + fix_size
        bss_end = bss_addr + bss_size
        if bss_size and tail_size and bss_addr < base + size and bss_end > base + fix_size:
            tail_addr = align_up(bss_end, TAIL_ALIGN)
            if tail_addr + tail_size > slot_end:
                raise SystemExit(f"{path.name}: no room for the relocation table after the BSS "
                                 f"({tail_addr:08X}+{tail_size:X} > {slot_end:08X}).")
        out.append({"name": path.stem, "id": be(0), "base": base, "file_size": size, "fix_size": fix_size,
                    "bss_addr": bss_addr, "bss_size": bss_size, "tail_addr": tail_addr, "slot_end": slot_end,
                    "crc32": zlib.crc32(data), "sections": sections})
        cursor = slot_end
    return out


def check_rel_layout(layout: list[dict], log: Path) -> None:
    """Compares the computed addresses with those DolRecomp actually used (rel.log)."""
    text = log.read_text(encoding="utf-8", errors="replace")
    bases = [int(m, 16) for m in re.findall(r"-> base 0x([0-9A-Fa-f]{8})", text)]
    if bases != [m["base"] for m in layout]:
        raise SystemExit(f"REL layout differs from DolRecomp's: {bases} (see {log}).")
    used = {(int(a, 16), int(s, 16)) for a, s in re.findall(r"addr:0x([0-9A-Fa-f]{8})\s+size:0x([0-9A-Fa-f]+)", text)}
    mine = {(addr, size) for m in layout for (_, addr, size, _, _) in m["sections"]}
    if used != mine:
        raise SystemExit(f"REL sections differ from DolRecomp's (see {log}).")


def read_catalog() -> list[tuple]:
    """Runtime catalog: (name, size, crc32, [(key, mask)], flags)."""
    out = []
    for line in HLE_CATALOG.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        f = line.split("\t")
        out.append((f[0], int(f[1], 16), int(f[2], 16), [(int(f[3], 16), int(f[4], 16))], f[5] if len(f) > 5 else "-"))
    return out


def hle_hooks(game_cfg: dict, version: str, dol: Path, layout: list[dict]) -> list[tuple[int, str]]:
    """Entry points to compile with an HLE hook: catalog functions marked "hook", found by signature in the
    DOL and the RELs (same algorithm as the runtime scan), plus the game-specific addresses declared in
    game.toml ([hle.hooks] <ID> = [...])."""
    import sdksig  # noqa: E402  (generic tool of the repository)
    import symbols as sy  # noqa: E402
    # Native replacements of milestone 10: names listed in runtime/core/hle/native_hooks.txt (hook needed
    # even if the catalog does not mark the function "hook").
    native = set()
    if HLE_NATIVE_HOOKS.exists():
        for line in HLE_NATIVE_HOOKS.read_text(encoding="utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                native.add(line)
    catalog = [c for c in read_catalog() if "hook" in c[4].split(",") or c[0] in native]
    sigs = [c[:4] for c in catalog]
    hooks: dict[int, str] = {}
    images = [sy.Image.from_dol(dol)]
    for m in layout:
        code = [(addr, blob) for (_, addr, _, ex, blob) in m["sections"] if ex]
        if code:
            images.append(sy.Image(code))
    for img in images:
        for name, addrs in sdksig.find_in_image(img, sigs).items():
            for a in addrs:
                hooks.setdefault(a, name)
    for a in game_cfg.get("hle", {}).get("hooks", {}).get(version, []):
        hooks.setdefault(parse_int(a), "game")
    return sorted(hooks.items())


def ensure_extracted(game: str, version: str, out: Path) -> dict:
    manifest_path = out / "manifest.json"
    if not manifest_path.exists():
        print(f"== Extraction ({version})")
        tc.run([tc.python_exe(), tc.ROOT / "tools" / "extract.py", game, "--version", version])
    return json.loads(manifest_path.read_text(encoding="utf-8"))


def recompile(game_cfg: dict, version: str, disc_root: Path, recomp_dir: Path, args) -> dict:
    """Runs DolRecomp if the fingerprint changed; returns the description of the output (recomp/state.json)."""
    ensure_dolrecomp_patches()
    dolrecomp = tc.find_dolrecomp()
    if not dolrecomp:
        raise SystemExit("DolRecomp not found (see tools/bootstrap.py).")
    code = game_cfg.get("code", {})
    rc = game_cfg.get("recomp", {})
    dol = disc_root / code["dol"]
    rel_dir = disc_root / code["rel_dir"] if code.get("rel_dir") else None
    rels = sorted(rel_dir.rglob("*.rel")) if rel_dir and rel_dir.exists() else []
    rel_base = parse_int(rc["rel_base"]) if "rel_base" in rc else None
    symbol_map = None
    if not args.no_symbols and rc.get("symbol_map"):
        candidate = game_cfg["_dir"] / rc["symbol_map"].format(id=version)
        if candidate.exists():
            symbol_map = candidate
    jobs = args.jobs or os.cpu_count() or 4
    layout = rel_layout(rels, rel_base if rel_base is not None else 0x80500000) if rels else []
    hooks = hle_hooks(game_cfg, version, dol, layout)

    fingerprint = {
        "scheme": RECOMP_SCHEME,
        "dolrecomp_exe": sha1_file(dolrecomp),
        "dolrecomp_commit": git_head(tc.EXTERNAL / "DolRecomp"),
        "dol": sha1_file(dol),
        "rels": {str(p.relative_to(rel_dir)).replace("\\", "/"): sha1_file(p) for p in rels},
        "rel_base": rel_base,
        "symbol_map": sha1_file(symbol_map) if symbol_map else None,
        "hle_hooks": [f"{a:08X} {n}" for a, n in hooks],
    }
    state_path = recomp_dir / "state.json"
    if state_path.exists() and not args.force_recomp:
        state = json.loads(state_path.read_text(encoding="utf-8"))
        if state.get("fingerprint") == fingerprint:
            print(f"== Recompilation: up to date ({recomp_dir})")
            return state

    print(f"== DolRecomp recompilation ({version})")
    if recomp_dir.exists():
        shutil.rmtree(recomp_dir)
    recomp_dir.mkdir(parents=True)
    timings = {}
    # Entry points compiled with an HLE hook (local DolRecomp patch: --hle-hooks).
    hooks_file = recomp_dir / "hle_hooks.txt"
    hooks_file.write_text("# address name (generated by tools/build.py, catalog runtime/core/hle/sdk_signatures.tsv)\n"
                          + "".join(f"{a:08X} {n}\n" for a, n in hooks), encoding="utf-8", newline="\n")
    base_cmd = [dolrecomp, "--gamecube", f"-j{jobs}", "--hle-hooks", hooks_file]
    print(f"   HLE hooks: {len(hooks)} ({', '.join(sorted({n for _, n in hooks}))})")

    # DOL. If it fails with the symbol map (produced in parallel by the analysis), retry without it.
    cmd = list(base_cmd)
    if symbol_map:
        cmd += ["--map", symbol_map]
    cmd += [dol, recomp_dir / "dol"]
    try:
        timings["dol"] = run_logged(cmd, recomp_dir / "dol.log")
    except SystemExit:
        if not symbol_map:
            raise
        print(f"WARNING: DolRecomp failed with the map {symbol_map}; retrying without a map.")
        shutil.rmtree(recomp_dir / "dol", ignore_errors=True)
        symbol_map = None
        fingerprint["symbol_map"] = "rejected"
        timings["dol"] = run_logged(base_cmd + [dol, recomp_dir / "dol"], recomp_dir / "dol.log")
    stats = {"dol": summarize_recomp_log(recomp_dir / "dol.log")}

    rel_end = None
    if rels:
        cmd = list(base_cmd)
        if rel_base is not None:
            cmd += ["--rel-base", f"0x{rel_base:08X}"]
        cmd += [rel_dir, recomp_dir / "rel"]
        timings["rel"] = run_logged(cmd, recomp_dir / "rel.log")
        stats["rel"] = summarize_recomp_log(recomp_dir / "rel.log")
        check_rel_layout(layout, recomp_dir / "rel.log")
        rel_end = max(max(m["tail_addr"] + m["file_size"] - m["fix_size"], m["bss_addr"] + m["bss_size"],
                          m["base"] + m["file_size"]) for m in layout)
    # REL layout for the runtime (OSLink replacement): one line per module.
    (recomp_dir / "rel_layout.txt").write_text(
        "# id base file_size fixSize bss bss_size moved_file_end slot_end crc32 name\n"
        + "".join(f"{m['id']} {m['base']:08X} {m['file_size']:X} {m['fix_size']:X} {m['bss_addr']:08X} "
                  f"{m['bss_size']:X} {m['tail_addr']:08X} {m['slot_end']:08X} {m['crc32']:08X} {m['name']}\n"
                  for m in layout), encoding="utf-8", newline="\n")

    chunks = sorted(recomp_dir.rglob("chunks/*.c"))
    state = {
        "fingerprint": fingerprint,
        "dol_path": str(dol),
        "dol_fnv64": f"0x{fnv1a64_file(dol):016X}",
        "rel_base": rel_base,
        "rel_end": rel_end,
        "symbol_map": str(symbol_map) if symbol_map else None,
        "chunk_files": len(chunks),
        "c_bytes": sum(p.stat().st_size for p in chunks),
        "stats": stats,
        "timings_s": {k: round(v, 1) for k, v in timings.items()},
    }
    state_path.write_text(json.dumps(state, indent=2), encoding="utf-8")
    for name, s in stats.items():
        print(f"   {name}: {s['decoded']} instructions, {s['unknown']} unknown, "
              f"{s['embedded_data']} embedded data{', SMC reported' if s['smc_warning'] else ''}")
    print(f"   {len(chunks)} C files, {state['c_bytes'] / 1e6:.0f} MB, "
          f"{sum(timings.values()):.1f} s")
    return state


def host_ram_size(state: dict) -> int:
    """Size of the host RAM: 24 MiB, enlarged (to the next MiB) to cover the virtual REL area."""
    size = MEM1_SIZE
    end = state.get("rel_end")
    if end and end > MEM1_BASE:
        size = max(size, ((end - MEM1_BASE) + 0xFFFFF) & ~0xFFFFF)
    return size


def configure_and_build(game: str, version: str, config: str, disc_root: Path, recomp_dir: Path,
                        state: dict, jobs: int | None, tag: str | None = None,
                        pgo: tuple[str, Path | None] = ("OFF", None), compiler: Path | None = None) -> Path:
    """compiler: explicit clang-cl (production of a PGO profile with another toolchain, tools/pgo.py --clang);
    default: the development one. lld-link, llvm-lib and llvm-rc are taken next to it."""
    cmake = tc.find_cmake()
    ninja = tc.find_ninja()
    clang_cl = compiler or tc.find_clang_cl()
    missing = [n for n, p in (("cmake", cmake), ("ninja", ninja), ("clang-cl", clang_cl)) if not p]
    if missing:
        raise SystemExit(f"Missing tools: {', '.join(missing)} (see tools/doctor.py).")
    build_dir = tc.build_dir(game, version, config, tag)
    build_dir.mkdir(parents=True, exist_ok=True)
    env = tc.msvc_env()
    llvm_bin = clang_cl.parent
    defs = {
        "CMAKE_BUILD_TYPE": config,
        "CMAKE_MAKE_PROGRAM": ninja.as_posix(),
        "CMAKE_C_COMPILER": clang_cl.as_posix(),
        "CMAKE_CXX_COMPILER": clang_cl.as_posix(),
        "GCRT_GAME": game,
        "GCRT_GAME_VERSION": version,
        "GCRT_RECOMP_DIR": recomp_dir.as_posix(),
        "GCRT_DISC_ROOT": disc_root.as_posix(),
        "GCRT_DOL_FNV64": state["dol_fnv64"],
        "GCRT_RAM_SIZE": f"0x{host_ram_size(state):08X}",
        "DOLRECOMP_DIR": (tc.EXTERNAL / "DolRecomp").as_posix(),
        "GCRT_AURORA_DIR": aurora_dir().as_posix(),
        # Always set: otherwise a build folder already configured with a profile goes back to its old setting.
        "GCRT_PGO": pgo[0],
        "GCRT_PGO_PROFILE": pgo[1].as_posix() if pgo[0] == "USE" and pgo[1] else "",
        "GCRT_PGO_RUNTIME": pgo[1].as_posix() if pgo[0] == "GENERATE" and pgo[1] else "",
    }
    for tool, var in (("lld-link", "CMAKE_LINKER"), ("llvm-lib", "CMAKE_AR"), ("llvm-rc", "CMAKE_RC_COMPILER")):
        exe = llvm_bin / f"{tool}{tc.EXE}"
        if exe.exists():
            defs[var] = exe.as_posix()
    cmd = [cmake, "-S", tc.ROOT, "-B", build_dir, "-G", "Ninja"] + [f"-D{k}={v}" for k, v in defs.items()]
    ensure_aurora_patches()
    print("== CMake configuration")
    t0 = time.perf_counter()
    run_logged(cmd, build_dir / "configure.log", env=env)
    t1 = time.perf_counter()
    print(f"== Compilation ({config})")
    build_cmd = [cmake, "--build", build_dir]
    if jobs:
        build_cmd += ["--parallel", str(jobs)]
    log = build_dir / "build.log"
    print("+", " ".join(str(c) for c in build_cmd), flush=True)
    with log.open("w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.Popen([str(c) for c in build_cmd], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=env, text=True, encoding="utf-8", errors="replace")
        for line in proc.stdout:
            f.write(line)
            # Ninja prints "[n/N] ...": only show the progress every 20 targets, and the errors.
            m = re.match(r"\[(\d+)/(\d+)\]", line)
            if (m and (int(m.group(1)) % 20 == 0 or m.group(1) == m.group(2))) or "error" in line.lower():
                print(line.rstrip()[:200], flush=True)
        proc.wait()
    t2 = time.perf_counter()
    errors = sum(1 for line in log.read_text(encoding="utf-8", errors="replace").splitlines()
                 if re.search(r"\berror\b", line, re.IGNORECASE) and "warning" not in line.lower())
    if proc.returncode != 0:
        raise SystemExit(f"Compilation failed ({errors} error lines), log: {log}")
    exes = sorted(build_dir.rglob(f"{game}{tc.EXE}"))
    if not exes:
        raise SystemExit(f"Compilation finished but executable {game}{tc.EXE} not found in {build_dir}.")
    exe = exes[0]
    summary = {"exe": str(exe), "exe_bytes": exe.stat().st_size,
               "configure_s": round(t1 - t0, 1), "build_s": round(t2 - t1, 1)}
    (build_dir / "build-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"== Executable: {exe} ({exe.stat().st_size / 1e6:.1f} MB), "
          f"configuration {t1 - t0:.1f} s, compilation {t2 - t1:.1f} s")
    return exe


# --- Profile-guided optimization (PGO) -------------------------------------------------------------------------------
# Profiles produced and checked by tools/pgo.py (profile-info.json, staleness), stored locally in build/pgo/<game>-<ID>/
# (never in Git: they only serve our builds). Here:
#   --pgo auto (default): local profile build/pgo/<game>-<ID>/ if it exists, in Release configuration, and if it
#                         exactly matches the current code and compiler (profile-info.json); otherwise a build without
#                         PGO, with the reason;
#   --pgo off           : never;
#   --pgo use           : profile from --pgo-dir (default: the local profile), even if stale (warning): measurements
#                         during development;
#   --pgo train         : tools/pgo.py train (local profile), then a build with this profile.
# After a build with a profile, the functions whose profile clang considers stale are counted (build-summary.json).


def prepare(game: str, game_cfg: dict, version: str, force_recomp: bool = False, no_symbols: bool = False,
            jobs: int | None = None) -> tuple[Path, Path, dict]:
    """Extraction (if needed) and up-to-date generated code: (disc root, generated code folder, state)."""
    out = tc.GENERATED / game / version
    manifest = ensure_extracted(game, version, out)
    disc_root = Path(manifest["disc_root"])
    recomp_dir = out / "recomp"
    args = argparse.Namespace(force_recomp=force_recomp, no_symbols=no_symbols, jobs=jobs)
    return disc_root, recomp_dir, recompile(game_cfg, version, disc_root, recomp_dir, args)


def pgo_profile_for_build(game: str, version: str, config: str, state: dict, mode: str, directory: str | None,
                          build_dir: Path) -> Path | None:
    """Profile to pass to CMake (copy named after its fingerprint in the build folder, so that Ninja recompiles when
    the profile changes), or None (build without PGO)."""
    import pgo as pgo_tools  # tools/pgo.py
    if mode == "off":
        return None
    if mode == "auto":
        profile, note = pgo_tools.select(game, version, config, state, None, require_fresh=True)
    else:
        d = Path(directory).resolve() if directory else pgo_tools.local_dir(game, version)
        profile, note = pgo_tools.select(game, version, config, state, d, require_fresh=False)
        if profile is None:
            raise SystemExit(f"--pgo {mode}: {note}")
    print(f"== PGO: {note}")
    if profile is None:
        return None
    build_dir.mkdir(parents=True, exist_ok=True)
    copy = build_dir / f"pgo-{sha1_file(profile)[:12]}.profdata"
    if not copy.exists():
        for old in build_dir.glob("pgo-*.profdata"):
            old.unlink()
        shutil.copyfile(profile, copy)
    return copy


def main() -> int:
    parser = argparse.ArgumentParser(description="Builds the native executable of a recompiled game.")
    parser.add_argument("game", help="folder in games/ (e.g. chibi-robo)")
    parser.add_argument("--version", help="disc ID (default: default_version from game.toml)")
    parser.add_argument("--config", default="Release", choices=["Release", "Debug", "RelWithDebInfo"])
    parser.add_argument("--jobs", type=int, help="parallel jobs (DolRecomp and compilation)")
    parser.add_argument("--force-recomp", action="store_true", help="regenerates the C even if the fingerprint is identical")
    parser.add_argument("--recomp-only", action="store_true", help="stops after generating the C")
    parser.add_argument("--no-symbols", action="store_true", help="ignores the symbol map even if it exists")
    parser.add_argument("--tag", help="suffix of the build folder (build/<game>-<ID>-<config>-<tag>) for parallel copies")
    parser.add_argument("--pgo", choices=["auto", "off", "use", "train"], default="auto",
                        help="profile-guided optimization: auto (default: local profile build/pgo/<game>-<ID>/ "
                             "if it is up to date, otherwise without PGO), off, use (profile from --pgo-dir or local "
                             "profile, even if stale), train (tools/pgo.py train then a build with this profile)")
    parser.add_argument("--pgo-dir", help="profile folder for --pgo use (profile.profdata + profile-info.json; default: "
                                          "build/pgo/<game>-<ID>/)")
    parser.add_argument("--pgo-scenarios", help="training scenarios for --pgo train (default: [pgo] from game.toml)")
    args = parser.parse_args()

    game_cfg = tc.load_game(args.game)
    version = args.version or game_cfg["game"]["default_version"]
    if version not in {v["game_id"] for v in game_cfg.get("versions", [])}:
        raise SystemExit(f"Unknown version {version} in games/{args.game}/game.toml.")
    disc_root, recomp_dir, state = prepare(args.game, game_cfg, version, args.force_recomp, args.no_symbols, args.jobs)
    if args.recomp_only:
        return 0
    mode = args.pgo
    if mode == "train":
        import pgo as pgo_tools  # tools/pgo.py
        if args.config != pgo_tools.CONFIG:
            raise SystemExit(f"--pgo train: {pgo_tools.CONFIG} configuration only")
        pgo_tools.train(args.game, version, args.tag,
                        [n for n in (args.pgo_scenarios or "").split(",") if n], jobs=args.jobs)
        mode = "use"
    build_dir = tc.build_dir(args.game, version, args.config, args.tag)
    profile = pgo_profile_for_build(args.game, version, args.config, state, mode, args.pgo_dir, build_dir)
    exe = configure_and_build(args.game, version, args.config, disc_root, recomp_dir, state, args.jobs, args.tag,
                              pgo=("USE", profile) if profile else ("OFF", None))
    if profile:
        import pgo as pgo_tools
        stale = pgo_tools.count_out_of_date(build_dir / "build.log")
        summary_path = build_dir / "build-summary.json"
        summary = json.loads(summary_path.read_text(encoding="utf-8"))
        summary["pgo"] = {"profile": str(profile), **stale}
        summary_path.write_text(json.dumps(summary, indent=2, ensure_ascii=False), encoding="utf-8")
        if stale["stale_functions"]:
            print(f"WARNING: stale PGO profile for {stale['stale_functions']} function(s) in {stale['files']} "
                  f"file(s) (clang compiles them without a profile); regenerate: py -3 tools/pgo.py {args.game} train "
                  f"--version {version}")
        else:
            print("== PGO: no function with a stale profile")
    del exe
    return 0


if __name__ == "__main__":
    sys.exit(main())
