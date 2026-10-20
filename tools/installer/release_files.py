"""Writes the files for a release page next to a built installer: payload-manifest.txt and SHA256SUMS.txt.

Usage: py -3 tools/installer/release_files.py <out folder of build.py>

  payload-manifest.txt  every file the installer can install (payload/common, payload/<game ID>, BUILD-INFO.txt), with
                        its size in bytes and SHA-256; the build-time .iss files are not listed (not installed)
  SHA256SUMS.txt        SHA-256 of each installer (*.exe) in the folder, in the format of sha256sum

Only names relative to the payload folder are written (no local path), and the payload never contains game data
(build.py checks it). tools/installer/build.py runs this after compiling.
"""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def write(out: Path) -> None:
    payload = out / "payload"
    if not payload.is_dir():
        raise SystemExit(f"{payload}: missing (run tools/installer/build.py first)")
    setups = sorted(out.glob("*.exe"))
    if not setups:
        raise SystemExit(f"{out}: no installer (*.exe)")
    files = sorted((p for p in payload.rglob("*") if p.is_file() and p.suffix.lower() != ".iss"),
                   key=lambda p: p.relative_to(payload).as_posix().lower())
    info = (payload / "BUILD-INFO.txt").read_text(encoding="utf-8").splitlines()
    lines = [f"Payload of {', '.join(s.name for s in setups)}",
             "Files the installer can install (common/: every version; <game ID>/: only the version of the player's",
             "disc image). No game data: the player provides their own disc image.", ""]
    lines += [l for l in info if l and "  " not in l]  # the header of BUILD-INFO.txt (not its hash lines)
    lines += ["", f"{'SHA-256':64}  {'bytes':>10}  file"]
    for p in files:
        lines.append(f"{sha256(p)}  {p.stat().st_size:>10}  {p.relative_to(payload).as_posix()}")
    lines.append(f"{'':64}  {sum(p.stat().st_size for p in files):>10}  total ({len(files)} files)")
    (out / "payload-manifest.txt").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    (out / "SHA256SUMS.txt").write_text("".join(f"{sha256(s)}  {s.name}\n" for s in setups), encoding="utf-8",
                                        newline="\n")
    print(f"Written: {out / 'payload-manifest.txt'} ({len(files)} files), {out / 'SHA256SUMS.txt'}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    write(Path(sys.argv[1]).resolve())
