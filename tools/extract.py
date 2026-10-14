"""Extracts the disc image of a GameCube game from `orig/` to `generated/<game>/<ID>/`.

Usage: py -3 tools/extract.py <game> [--version GGTP01] [--force]

Steps:
  1. finds in `orig/` the image whose ID matches the requested version from `games/<game>/game.toml`;
  2. converts it to .iso with DolphinTool if the image is compressed (.rvz, .gcz, .wia, .ciso);
  3. checks the SHA-1 fingerprint of the .iso (rejects an unknown image);
  4. extracts `sys/` (boot.bin, bi2.bin, apploader, main.dol, fst.bin) and `files/`;
  5. unpacks the U8 archives listed in [extract] u8_archives under `unpacked/<archive>/`;
  6. writes `manifest.json` (fingerprints of each code file) for the following steps.
`orig/` is never modified.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gcdisc  # noqa: E402
import toolchain as tc  # noqa: E402
import u8arc  # noqa: E402

COMPRESSED = (".rvz", ".gcz", ".wia", ".ciso")
RAW = (".iso", ".gcm")


def sha1_file(path: Path) -> str:
    h = hashlib.sha1()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    return h.hexdigest()


def read_game_id(path: Path) -> str | None:
    if path.suffix.lower() in RAW:
        with path.open("rb") as f:
            return f.read(6).decode("ascii", "replace")
    tool = tc.find_dolphin_tool()
    if not tool:
        return None
    out = tc.subprocess.run([str(tool), "header", "-i", str(path)], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    for line in out.stdout.splitlines():
        if line.startswith("Game ID:"):
            return line.split(":", 1)[1].strip()
    return None


def find_image(game_id: str) -> Path:
    candidates = [p for p in sorted(tc.ORIG.rglob("*"))
                  if p.is_file() and p.suffix.lower() in RAW + COMPRESSED]
    raw_first = sorted(candidates, key=lambda p: p.suffix.lower() not in RAW)
    for p in raw_first:
        if read_game_id(p) == game_id:
            return p
    raise SystemExit(f"No {game_id} image in {tc.ORIG} (accepted formats: {', '.join(RAW + COMPRESSED)}).")


def ensure_iso(image: Path, work: Path) -> Path:
    if image.suffix.lower() in RAW:
        return image
    iso = work / "disc.iso"
    if not iso.exists():
        tool = tc.find_dolphin_tool()
        if not tool:
            raise SystemExit("Compressed image but DolphinTool not found (see tools/doctor.py).")
        tc.run([tool, "convert", "-i", image, "-o", iso, "-f", "iso"])
    return iso


def write_file(dest: Path, data: bytes) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="Extracts the disc of a GameCube game.")
    parser.add_argument("game", help="folder in games/ (e.g. chibi-robo)")
    parser.add_argument("--version", help="disc ID (default: default_version from game.toml)")
    parser.add_argument("--force", action="store_true", help="extract again even if the manifest exists")
    args = parser.parse_args()

    game = tc.load_game(args.game)
    game_id = args.version or game["game"]["default_version"]
    version = next((v for v in game["versions"] if v["game_id"] == game_id), None)
    if version is None:
        raise SystemExit(f"Version {game_id} missing from games/{args.game}/game.toml")

    out = tc.GENERATED / args.game / game_id
    manifest_path = out / "manifest.json"
    if manifest_path.exists() and not args.force:
        print(f"Already extracted: {out} (--force to start over)")
        return 0

    image = find_image(game_id)
    print(f"Image: {image}")
    out.mkdir(parents=True, exist_ok=True)
    iso = ensure_iso(image, out)
    digest = sha1_file(iso)
    if digest != version["iso_sha1"]:
        raise SystemExit(f"Unexpected fingerprint for {game_id}: {digest} (expected {version['iso_sha1']}). "
                         "Modified image or unsupported version.")
    print(f"SHA-1 verified: {digest}")

    disc_dir = out / "disc"
    shutil.rmtree(disc_dir, ignore_errors=True)
    files_written = {}
    with gcdisc.GCDisc(iso) as disc:
        if disc.game_id != game_id:
            raise SystemExit(f"ID read {disc.game_id} ≠ {game_id}")
        for entry in disc.system_files() + disc.files():
            data = disc.read(entry.offset, entry.size)
            write_file(disc_dir / entry.path, data)
            files_written[entry.path] = len(data)
    print(f"{len(files_written)} files extracted to {disc_dir}")

    for rel_path in game.get("extract", {}).get("u8_archives", []):
        archive = disc_dir / rel_path
        data = archive.read_bytes()
        target = disc_dir / "unpacked" / Path(rel_path).name
        shutil.rmtree(target, ignore_errors=True)
        entries = u8arc.parse(data)
        for e in entries:
            write_file(target / e.path, data[e.offset:e.offset + e.size])
        print(f"{rel_path}: {len(entries)} files unpacked to {target}")

    code = game.get("code", {})
    code_files = {}
    dol = disc_dir / code["dol"]
    code_files[code["dol"]] = sha1_file(dol)
    rel_dir = disc_dir / code.get("rel_dir", "")
    if code.get("rel_dir") and rel_dir.is_dir():
        for rel in sorted(rel_dir.glob("*.rel")):
            code_files[rel.relative_to(disc_dir).as_posix()] = sha1_file(rel)

    manifest = {
        "game": args.game,
        "game_id": game_id,
        "image": str(image),
        "iso_sha1": digest,
        "disc_root": str(disc_dir),
        "code_sha1": code_files,
        "file_count": len(files_written),
    }
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"Manifest: {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
