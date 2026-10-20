"""Builds the player installer of a game (Inno Setup 6): payload assembly, licence notices, ISCC.

Usage: py -3 tools/installer/build.py <game> [--build ID=DIR ...] [--gcextract EXE] [--iscc ISCC.exe]
                                      [--vc-redist DIR] [--out build/installer] [--no-disc-arg]

  <game>        folder in games/ (game.toml: name, slug, [[versions]], [diagnostics] user_org / user_app / exe_name)
  --build       game build folder of one version (contains bin/ and gcrt_build_info.c); repeatable. Default: every
                version of game.toml whose build/<slug>-<ID>-Release/bin exists.
  --gcextract   gcextract.exe (default build/gcextract/gcextract.exe, built with tools/gcextract/build.py if absent)
  --iscc        Inno Setup compiler (default: ISCC.exe of an Inno Setup 6 installation, per-user or machine-wide)
  --vc-redist   folder of the Visual C++ runtime DLLs to copy next to the executable (default: the newest
                <Visual Studio>\\VC\\Redist\\MSVC\\<version>\\x64\\Microsoft.VC143.CRT)
  --no-disc-arg the executables find disc\\ next to themselves: shortcuts without --disc
  --external    external/ folder (Aurora checkout, fetched licence files); default: the one of this checkout

Result: build/installer/<slug>-pc-setup-<commit>.exe, plus build/installer/payload/ (what the installer contains)
and build/installer/payload/BUILD-INFO.txt (commits, SHA-256 of every file).

One installer holds every version given: files identical across versions go to common\\, the others (the
executable, and DLLs built per version) to <ID>\\; the image chosen by the player selects which ones are installed.
The payload never contains game data (checked: no disc image, DOL, REL, PGO profile, no file of generated/ or orig/).
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
import uuid
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import notices  # noqa: E402

# Size of a full GameCube disc image (mini DVD, 1 459 978 240 bytes): disk space estimate of the image copy.
GAMECUBE_IMAGE_SIZE = 1_459_978_240
# Never in the payload (game data, PGO profiles, debug files).
FORBIDDEN_SUFFIXES = {".iso", ".gcm", ".rvz", ".dol", ".rel", ".profdata", ".profraw", ".pdb", ".ilk"}
# Never in a payload file name (see the check in main): quotes, the list separator, Inno Setup constants, ISPP
# directives, path separators. Control characters are refused too.
FORBIDDEN_NAME_CHARS = set("\"'|{}#\\/")


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_iscc() -> Path | None:
    candidates = []
    for var in ("LOCALAPPDATA", "ProgramFiles(x86)", "ProgramFiles"):
        base = os.environ.get(var)
        if base:
            candidates.append(Path(base) / ("Programs" if var == "LOCALAPPDATA" else "") / "Inno Setup 6" / "ISCC.exe")
    return next((c for c in candidates if c.is_file()), None)


def find_vc_redist() -> Path | None:
    found = []
    for var in ("ProgramFiles", "ProgramFiles(x86)"):
        base = os.environ.get(var)
        if not base:
            continue
        for crt in Path(base).glob("Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT"):
            found.append(crt)

    def key(p: Path):
        return tuple(int(x) if x.isdigit() else 0 for x in p.parent.parent.name.split("."))

    return max(found, key=key) if found else None


def build_commit(build: Path) -> str:
    info = build / "gcrt_build_info.c"
    m = re.search(r'gcrt_build_commit\[\]\s*=\s*"([^"]*)"', info.read_text(encoding="utf-8")) if info.is_file() else None
    if not m:
        raise SystemExit(f"{info}: commit of the build not found")
    return m.group(1)


def main() -> int:
    parser = argparse.ArgumentParser(description="Builds the player installer (Inno Setup).")
    parser.add_argument("game")
    parser.add_argument("--build", action="append", default=[], metavar="ID=DIR")
    parser.add_argument("--gcextract")
    parser.add_argument("--iscc")
    parser.add_argument("--vc-redist")
    parser.add_argument("--out", default=str(ROOT / "build" / "installer"))
    parser.add_argument("--no-disc-arg", action="store_true")
    parser.add_argument("--external", default=str(ROOT / "external"))
    args = parser.parse_args()

    game_dir = ROOT / "games" / args.game
    game_toml = game_dir / "game.toml"
    cfg = tomllib.loads(game_toml.read_text(encoding="utf-8"))
    slug = cfg["game"]["slug"]
    game_name = cfg["game"]["name"]
    diag = cfg.get("diagnostics", {})
    exe_name = diag.get("exe_name", f"{slug}.exe")
    versions = {v["game_id"]: v for v in cfg["versions"]}

    builds: dict[str, Path] = {}
    for item in args.build:
        gid, _, folder = item.partition("=")
        if gid not in versions or not folder:
            raise SystemExit(f"--build {item}: expected ID=DIR with ID in {', '.join(versions)}")
        builds[gid] = Path(folder).resolve()
    if not builds:
        for gid in versions:
            folder = ROOT / "build" / f"{slug}-{gid}-Release"
            if (folder / "bin" / exe_name).is_file():
                builds[gid] = folder
    if not builds:
        raise SystemExit("no game build found (--build ID=DIR)")
    for gid, folder in builds.items():
        if not (folder / "bin" / exe_name).is_file():
            raise SystemExit(f"{folder / 'bin' / exe_name}: missing")

    iscc = Path(args.iscc) if args.iscc else find_iscc()
    if not iscc or not iscc.is_file():
        raise SystemExit("ISCC.exe (Inno Setup 6) not found: install it (winget install JRSoftware.InnoSetup) or --iscc")
    vc_redist = Path(args.vc_redist) if args.vc_redist else find_vc_redist()
    if not vc_redist or not vc_redist.is_dir():
        raise SystemExit("Visual C++ runtime folder not found (--vc-redist)")
    # The notices name the version of the redistributable folder (VC\Redist\MSVC\<version>\x64\Microsoft.VC14x.CRT).
    if vc_redist.parent.parent.name != notices.VC_VERSION:
        raise SystemExit(f"{vc_redist}: Visual C++ runtime {vc_redist.parent.parent.name}, the notices say "
                         f"{notices.VC_VERSION}: update VC_VERSION in tools/installer/notices.py")
    gcextract = Path(args.gcextract) if args.gcextract else ROOT / "build" / "gcextract" / "gcextract.exe"
    if not gcextract.is_file():
        subprocess.run([sys.executable, str(ROOT / "tools" / "gcextract" / "build.py"), "--out", str(gcextract.parent)],
                       check=True)

    out = Path(args.out).resolve()
    payload = out / "payload"
    if payload.exists():
        shutil.rmtree(payload)
    (payload / "common").mkdir(parents=True)

    # --- 1. Files of each version (bin/: executable and DLLs; nothing else from the build folder).
    per_version: dict[str, dict[str, Path]] = {}
    for gid, folder in builds.items():
        files = {}
        for f in sorted((folder / "bin").iterdir()):
            if f.is_file() and (f.name == exe_name or f.suffix.lower() == ".dll"):
                files[f.name] = f
        per_version[gid] = files
    names = sorted({n for files in per_version.values() for n in files})
    layout: dict[str, list[str]] = {"common": []}
    for name in names:
        present = [gid for gid in builds if name in per_version[gid]]
        digests = {sha256(per_version[gid][name]) for gid in present}
        if len(present) == len(builds) and len(digests) == 1:
            shutil.copy2(per_version[present[0]][name], payload / "common" / name)
            layout["common"].append(name)
        else:
            for gid in present:
                (payload / gid).mkdir(exist_ok=True)
                shutil.copy2(per_version[gid][name], payload / gid / name)
                layout.setdefault(gid, []).append(name)

    # --- 2. Visual C++ runtime: the DLLs of the redistributable folder that a shipped binary imports.
    shipped = [p for p in payload.rglob("*") if p.suffix.lower() in (".dll", ".exe")]
    imported = notices.vc_runtime_imports(shipped)
    vc_files = [f for f in sorted(vc_redist.iterdir()) if f.name.lower() in imported]
    missing_vc = sorted(n for n in imported if n not in {f.name.lower() for f in vc_files})
    if missing_vc:
        raise SystemExit(f"Visual C++ runtime DLLs not found in {vc_redist}: {', '.join(missing_vc)}")
    for f in vc_files:
        shutil.copy2(f, payload / "common" / f.name.lower())
        layout["common"].append(f.name.lower())

    # --- 3. Extractor, game description, notices.
    shutil.copy2(gcextract, payload / "common" / "gcextract.exe")
    shutil.copy2(game_toml, payload / "common" / "game.toml")
    first_build = next(iter(builds.values()))
    text = notices.assemble(first_build, iscc.parent, allow_fetch=True, external=Path(args.external), vc_dlls=imported)
    committed = HERE / "THIRD_PARTY_LICENSES.txt"
    if committed.read_text(encoding="utf-8").replace("\r\n", "\n") != text:
        raise SystemExit(f"{committed} is out of date: run tools/installer/notices.py --build {first_build}")
    (payload / "common" / "THIRD_PARTY_LICENSES.txt").write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
    layout["common"] += ["gcextract.exe", "game.toml", "THIRD_PARTY_LICENSES.txt"]

    # --- 4. No game data, no profile, no toolchain.
    for p in payload.rglob("*"):
        if p.is_file() and p.suffix.lower() in FORBIDDEN_SUFFIXES:
            raise SystemExit(f"payload: forbidden file {p}")
        if p.is_file() and p.stat().st_size > 200 << 20:
            raise SystemExit(f"payload: unexpectedly large file {p}")

    # --- 5. Version, build information, version-specific [Files] entries.
    commits = {gid: build_commit(folder) for gid, folder in builds.items()}
    unique = sorted(set(commits.values()))
    commit_label = unique[0] if len(unique) == 1 else "+".join(unique)
    exe_commits = ", ".join(f"{gid} {c}" for gid, c in commits.items())
    date = dt.date.today().strftime("%Y.%m.%d")
    app_version = f"{date}+{commit_label}"
    region = {gid: v.get("region", gid) for gid, v in versions.items()}
    supported = ", ".join(f"{region[gid]} ({gid})" for gid in builds)
    # GcrtBuildInfo.cmake appends "-dirty" when the source tree had uncommitted changes.
    dirty = sorted(gid for gid, c in commits.items() if c.endswith("-dirty"))
    clean_text = ("yes (every game executable was built from a committed source tree)" if not dirty else
                  f"NO: built with uncommitted changes ({', '.join(dirty)})")
    info = [f"{game_name} PC: installer {app_version}", f"Built: {dt.datetime.now().isoformat(timespec='seconds')}",
            f"Source code: https://github.com/Armandpro12/chibi-robo-pc (game executable commit: {exe_commits})",
            f"Clean build: {clean_text}",
            f"Supported disc images: {supported}", ""]
    for p in sorted(payload.rglob("*")):
        if p.is_file():
            info.append(f"{sha256(p)}  {p.relative_to(payload).as_posix()}")
    (payload / "BUILD-INFO.txt").write_text("\r\n".join(info) + "\r\n", encoding="utf-8")
    lines = ["; Generated by tools/installer/build.py: files of each version, installed when the chosen image is it."]
    for gid in builds:
        if gid in layout:
            lines.append(f'Source: "{{#PayloadDir}}\\{gid}\\*"; DestDir: "{{app}}"; Flags: ignoreversion; '
                         f"Check: IsGameVersion('{gid}'); BeforeInstall: BackUpExisting")
    (payload / "versions.iss").write_text("\n".join(lines) + "\n", encoding="utf-8")
    # Names installed into {app} (relative to it): setup.iss keeps the previous file of each as <name>.old during a
    # reinstall, puts it back after a failure, and the uninstaller removes a leftover one.
    installed: set[str] = {"BUILD-INFO.txt"}
    for p in payload.rglob("*"):
        rel = p.relative_to(payload)
        if p.is_file() and len(rel.parts) > 1 and rel.parts[0] in ("common", *builds):
            installed.add("\\".join(rel.parts[1:]))
    names = sorted(installed, key=str.lower)
    # The names go into an ISPP string ("..."), a Pascal string ('...') split on "|", and [UninstallDelete] entries
    # where "{" starts a constant; "#" could start an ISPP directive. Each path component must avoid all of these (and
    # the path separators: "\" only joins components here).
    for n in names:
        for part in n.split("\\"):
            bad = sorted({c for c in part if c in FORBIDDEN_NAME_CHARS or ord(c) < 32 or ord(c) == 127})
            if not part or bad:
                raise SystemExit(f"payload: file name {n!r} has an empty component or a forbidden character "
                                 f"({', '.join(repr(c) for c in bad)})")
    payload_bytes = sum(p.stat().st_size for p in payload.rglob("*") if p.is_file())
    (payload / "payload.iss").write_text(
        "; Generated by tools/installer/build.py: the payload files installed into {app}, and their total size.\n"
        f'#define PayloadNames "{"".join(n + "|" for n in names)}"\n'
        f"#define PayloadBytes \"{payload_bytes}\"\n", encoding="utf-8")
    (payload / "uninstall-old.iss").write_text(
        "; Generated by tools/installer/build.py: previous payload files left by an interrupted install.\n"
        + "".join(f'Type: files; Name: "{{app}}\\{n}.old"\n' for n in names), encoding="utf-8")

    # --- 6. Compile.
    app_name = f"{game_name} PC"
    user_dir = "{userappdata}\\" + diag.get("user_org", app_name) + "\\" + diag.get("user_app", game_name)
    output_name = f"{slug}-pc-setup-{commit_label}"
    defines = {
        "AppName": app_name, "GameName": game_name, "AppSlug": f"{slug}-pc",
        "AppId": str(uuid.uuid5(uuid.NAMESPACE_URL, f"gcrt-player-installer/{slug}")).upper(),
        "AppVersion": app_version, "ExeCommits": exe_commits, "ExeName": exe_name, "PayloadDir": str(payload),
        "OutputDir": str(out), "OutputName": output_name, "ImageSize": str(GAMECUBE_IMAGE_SIZE),
        "UserDataDir": user_dir, "SupportedList": supported, "PassDiscArg": "0" if args.no_disc_arg else "1",
    }
    cmd = [str(iscc), "/Q"] + [f"/D{k}={v}" for k, v in defines.items()] + [str(HERE / "setup.iss")]
    t0 = time.time()
    subprocess.run(cmd, check=True)
    setup = out / f"{output_name}.exe"
    print(f"Installer: {setup} ({setup.stat().st_size / 1e6:.1f} MB, compiled in {time.time() - t0:.0f} s)")
    for key, files in layout.items():
        print(f"  {key}: {', '.join(files)}")
    # Files for the release page (no local path, no game data).
    import release_files  # noqa: E402 (tools/installer is on sys.path)
    release_files.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
