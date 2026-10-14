"""Headless Ghidra analysis (analyzeHeadless) of a GameCube game described by `games/<game>/game.toml`.

Usage:
  py -3 tools/ghidra/analyze.py <game> [--version ID] import [--force] [--map auto|none|<path>] [--rels]
  py -3 tools/ghidra/analyze.py <game> [--version ID] export          # re-exports functions and registers
  py -3 tools/ghidra/analyze.py <game> [--version ID] dump <target>... [-o file]
  py -3 tools/ghidra/analyze.py <game> [--version ID] script <Script.java> [args...]

Ghidra project: `generated/<game>/<ID>/ghidra/<ID>.gpr`, program `/main.dol`
(and `/rels/<module>.rel` with `--rels`: the GameCube loader then puts the DOL and every REL of
the folder into a single program, the RELs following the end of the DOL).
Outputs: `generated/<game>/<ID>/ghidra/out/` (ignored by Git: contains information taken from the game).
  functions.tsv: functions (entry, size, name, callees); mmio.tsv: accesses to the hardware registers
  and to the OS low globals; disasm.txt: disassemblies requested by `dump`.

Technical choices (see notes/decisions.md):
  - Ghidra scripts in Java (`tools/ghidra/scripts/`): Ghidra 12.1 compiles them itself in headless mode;
    Jython is now only shipped as an optional extension and PyGhidra requires installing
    `pyghidra` + JPype in a Python environment, which the Java version avoids;
  - "GameCubeLoader" loader (Cuyler36 extension) forced, with `-loader-autoloadMaps false`
    (otherwise a "Load Symbols?" dialog box blocks the import);
  - `stdin` closed: no prompt can wait for an answer.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import toolchain as tc  # noqa: E402

SCRIPTS = Path(__file__).resolve().parent / "scripts"
LOADER_ARGS = ["-loader", "GameCubeLoader", "-loader-autoloadMaps", "false"]


def version_of(game: dict, requested: str | None) -> str:
    vid = requested or game["game"]["default_version"]
    known = [v["game_id"] for v in game.get("versions", [])]
    if vid not in known:
        sys.exit(f"unknown version {vid} (known: {', '.join(known)})")
    return vid


def paths(slug: str, vid: str, game: dict) -> dict:
    base = tc.GENERATED / slug / vid
    code = game.get("code", {})
    return {
        "base": base,
        "disc": base / "disc",
        "dol": base / "disc" / code.get("dol", "sys/main.dol"),
        "rel_dir": base / "disc" / code.get("rel_dir", "rel"),
        "project_dir": base / "ghidra",
        "out": base / "ghidra" / "out",
        "stage": base / "ghidra" / "stage-rels",
    }


def default_map(game: dict, vid: str) -> Path | None:
    sym = game.get("symbols", {})
    rel = sym.get("map_dir", "symbols")
    candidate = game["_dir"] / rel / f"{vid}.map"
    return candidate if candidate.exists() else None


def headless(project_dir: Path, project: str, args: list, log: Path) -> int:
    exe = tc.find_ghidra_headless()
    if not exe:
        sys.exit("analyzeHeadless not found (run tools/bootstrap.py --only ghidra)")
    project_dir.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), str(project_dir), project, *[str(a) for a in args]]
    print("+", " ".join(f'"{c}"' if " " in c else c for c in cmd), flush=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8", errors="replace") as f:
        proc = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT)
    text = log.read_text(encoding="utf-8", errors="replace")
    # Useful summary: report lines, error lines and script outputs.
    for line in text.splitlines():
        if any(k in line for k in ("REPORT", "ERROR", "Exception", "ApplySymbolMap", "ExportFunctions",
                                   "ExportMmioRefs", "DumpDisasm", "SCRIPT")):
            print("  " + line.strip())
    if "ERROR" in text and "Abort" in text:
        return 1
    return proc.returncode


def program_exists(project_dir: Path, project: str) -> bool:
    # The project exists and contains at least one program file.
    rep = project_dir / f"{project}.rep" / "idata"
    return (project_dir / f"{project}.gpr").exists() and rep.exists() and any(rep.rglob("*.prp"))


def post_exports(out: Path) -> list:
    return ["-postScript", "ExportFunctions.java", out / "functions.tsv",
            "-postScript", "ExportMmioRefs.java", out / "mmio.tsv"]


def cmd_import(ns, game, vid, p) -> int:
    project = vid
    map_path = None
    if ns.map == "auto":
        map_path = default_map(game, vid)
    elif ns.map != "none":
        map_path = Path(ns.map).resolve()
    if program_exists(p["project_dir"], project) and not ns.force:
        print(f"project already present: {p['project_dir'] / (project + '.gpr')} (use --force to import again)")
    else:
        if not p["dol"].exists():
            sys.exit(f"DOL missing: {p['dol']} (run tools/extract.py {ns.game} --version {vid} first)")
        args = ["-import", p["dol"], "-overwrite", *LOADER_ARGS, "-scriptPath", SCRIPTS]
        if map_path:
            print(f"symbol map: {map_path}")
            args += ["-preScript", "ApplySymbolMap.java", map_path]
        args += post_exports(p["out"])
        rc = headless(p["project_dir"], project, args, p["out"] / "import.log")
        if rc != 0:
            return rc
    if ns.rels:
        return import_rels(ns, p, project, map_path)
    return 0


def import_rels(ns, p, project, map_path: Path | None) -> int:
    """Imports the DOL + every REL into a single program `/rels/<first>.rel`."""
    rels = sorted(p["rel_dir"].glob("*.rel"))
    if not rels:
        print(f"no REL in {p['rel_dir']}")
        return 0
    stage = p["stage"]
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    # Local copy (in generated/, ignored by Git): the loader reads the neighbors of the imported file.
    shutil.copy2(p["dol"], stage / p["dol"].name)
    for r in rels:
        shutil.copy2(r, stage / r.name)
    args = ["-import", stage / rels[0].name, "-overwrite", *LOADER_ARGS, "-scriptPath", SCRIPTS]
    if map_path:
        # The DOL addresses are the same in this program: the DOL map applies as is.
        args += ["-preScript", "ApplySymbolMap.java", map_path]
    args += ["-postScript", "ExportFunctions.java", p["out"] / "functions-rels.tsv"]
    return headless(p["project_dir"], f"{project}/rels", args, p["out"] / "import-rels.log")


def cmd_export(ns, game, vid, p) -> int:
    args = ["-process", "main.dol", "-noanalysis", "-readOnly", "-scriptPath", SCRIPTS, *post_exports(p["out"])]
    return headless(p["project_dir"], vid, args, p["out"] / "export.log")


def cmd_dump(ns, game, vid, p) -> int:
    out = Path(ns.output).resolve() if ns.output else p["out"] / "disasm.txt"
    args = ["-process", "main.dol", "-noanalysis", "-readOnly", "-scriptPath", SCRIPTS,
            "-postScript", "DumpDisasm.java", out, *ns.targets]
    rc = headless(p["project_dir"], vid, args, p["out"] / "dump.log")
    print(f"disassembly: {out}")
    return rc


def cmd_script(ns, game, vid, p) -> int:
    extra = [a.replace("{out}", str(p["out"])) for a in ns.args]
    args = ["-process", "main.dol", "-noanalysis", "-scriptPath", SCRIPTS]
    if ns.read_only:
        args.append("-readOnly")
    args += ["-postScript", ns.script, *extra]
    return headless(p["project_dir"], vid, args, p["out"] / "script.log")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game")
    ap.add_argument("--version")
    sub = ap.add_subparsers(dest="cmd")
    imp = sub.add_parser("import", help="imports and analyzes the DOL (default)")
    imp.add_argument("--force", action="store_true", help="imports again even if the project exists")
    imp.add_argument("--map", default="auto", help="auto (games/<game>/symbols/<ID>.map), none, or a path")
    imp.add_argument("--rels", action="store_true", help="also imports DOL + REL into /rels/")
    sub.add_parser("export", help="re-exports functions.tsv and mmio.tsv")
    dmp = sub.add_parser("dump", help="disassembles functions (address, name or start-end range)")
    dmp.add_argument("targets", nargs="+")
    dmp.add_argument("-o", "--output")
    scr = sub.add_parser("script", help="runs a script from tools/ghidra/scripts on the program")
    scr.add_argument("script")
    scr.add_argument("args", nargs=argparse.REMAINDER)
    scr.add_argument("--read-only", action="store_true")
    ns = ap.parse_args()
    if ns.cmd is None:
        ns = ap.parse_args(sys.argv[1:] + ["import"])
    game = tc.load_game(ns.game)
    vid = version_of(game, ns.version)
    p = paths(ns.game, vid, game)
    handler = {"import": cmd_import, "export": cmd_export, "dump": cmd_dump, "script": cmd_script}[ns.cmd]
    return handler(ns, game, vid, p)


if __name__ == "__main__":
    sys.exit(main())
