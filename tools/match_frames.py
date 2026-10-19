"""Pairs native captures with Dolphin frame dumps by CONTENT, then measures each pair (milestone 9).

After a scripted input or a loading screen, the native build and Dolphin do not reach the same scene at the
same presented-frame index (inputs land at other frames, loading times differ), so pairing by index
(tools/compare_run.py) no longer works. This tool searches, for each native capture, the Dolphin frame
that looks the most like it, and reports how close they are.

Usage:
  py -3 tools/match_frames.py <native folder> <dolphin frames folder> --out <folder>
      [--pattern "cp-*.png"] [--window LO,HI] [--offset N] [--top 6] [--diffs] [--ssim-threshold 0.95]

Method (documented choices):
- Frame number of a native capture: the last integer in its file name (e.g. cp-f03060.png -> 3060).
  Dolphin frames: framedump_<n>.png (numbered from 1, one per presented frame, notes/dolphin.md).
- Coarse search: luma (BT.601, PIL "L") of every Dolphin frame, resized to the native capture size
  (bilinear, like tools/compare_images.py), then reduced 8x8 by block mean; mean squared error against
  the reduced native capture over the candidate frames. Candidates: all frames, or, with --window LO,HI,
  the frames n + offset + LO .. n + offset + HI (n = native frame number).
- Fine measure: the --top best candidates are measured at full size with tools/compare_images.py
  (PSNR on the luma, simplified SSIM, 7x7 box window); the kept pair is the best SSIM.
- "flat": number of candidates whose coarse error is within 1 % of the best one: a large value means the
  scene is static over that many frames (the pairing is then exact in content but not in time).
- --diffs: writes <out>/diffs/<capture>.png = native | Dolphin | |difference| x4 (RGB), to be read by eye.
- The reduced Dolphin frames are cached in <out>/dolphin-cache.npz (key: folder, count, size, newest file time).

Limits: the PSNR is only indicative once the Dolphin frame is resized (about 10 to 15 dB are lost even for
identical content: use the SSIM); the reference size is the size of the first native capture; on a static scene
the pairing is exact in content but not in time (see "flat").

Output: <out>/match.json (one entry per capture) and <out>/match.md (table). In match.json an infinite PSNR
(identical pair) is null and "identical" is true.
Exit code: 0 when at least one pair was measured (whether it passes or not), 1 when none could be measured,
2 for an invalid input (no capture, no Dolphin frame, bad --window or --top).
Dependencies: Pillow and numpy, as tools/compare_images.py.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_images as ci  # noqa: E402

BLOCK = 8


def frame_number(path: Path) -> int | None:
    nums = re.findall(r"\d+", path.stem)
    return int(nums[-1]) if nums else None


def reduce(luma: np.ndarray) -> np.ndarray:
    h, w = (luma.shape[0] // BLOCK) * BLOCK, (luma.shape[1] // BLOCK) * BLOCK
    x = luma[:h, :w].reshape(h // BLOCK, BLOCK, w // BLOCK, BLOCK)
    return x.mean(axis=(1, 3)).astype(np.float32)


def load_dolphin(frames_dir: Path, shape: tuple[int, int], cache: Path) -> tuple[np.ndarray, np.ndarray]:
    files = {}
    for p in frames_dir.glob("framedump_*.png"):
        n = frame_number(p)
        if n is not None:
            files[n] = p
    if not files:
        raise SystemExit(f"no framedump_<n>.png frame in {frames_dir}")
    nums = np.array(sorted(files), dtype=np.int64)
    newest = max(p.stat().st_mtime_ns for p in files.values())
    key = f"{frames_dir.resolve()}|{len(nums)}|{shape}|{newest}"
    if cache.exists():
        z = np.load(cache, allow_pickle=False)
        if str(z["key"]) == key:
            return z["nums"], z["small"]
    small = np.stack([reduce(ci.resize_to(shape, files[int(n)])) for n in nums])
    np.savez(cache, key=np.array(key), nums=nums, small=small)
    return nums, small


def save_diff(pa: Path, pb: Path, out: Path) -> None:
    a = Image.open(pa).convert("RGB")
    b = Image.open(pb).convert("RGB").resize(a.size, Image.BILINEAR)
    d = np.clip(np.abs(np.asarray(a, np.int16) - np.asarray(b, np.int16)) * 4, 0, 255).astype(np.uint8)
    sheet = Image.new("RGB", (a.width * 3, a.height))
    for i, im in enumerate((a, b, Image.fromarray(d))):
        sheet.paste(im, (i * a.width, 0))
    out.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Pairs native captures with Dolphin frames by content.")
    ap.add_argument("native", type=Path)
    ap.add_argument("dolphin", type=Path, help="folder of framedump_<n>.png")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--pattern", default="cp-*.png")
    ap.add_argument("--window", default=None, metavar="LO,HI", help="search n+offset+LO .. n+offset+HI only")
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--top", type=int, default=6)
    ap.add_argument("--diffs", action="store_true")
    ap.add_argument("--ssim-threshold", type=float, default=ci.DEFAULT_SSIM_THRESHOLD)
    args = ap.parse_args()

    if args.top < 1:
        print("--top must be at least 1", file=sys.stderr)
        return 2
    win = None
    if args.window:
        try:
            win = tuple(int(v) for v in args.window.split(","))
            if len(win) != 2 or win[0] > win[1]:
                raise ValueError
        except ValueError:
            print(f"--window expects LO,HI with LO <= HI (two integers), got {args.window!r}", file=sys.stderr)
            return 2
    caps = sorted(args.native.glob(args.pattern), key=ci._natural_key)
    if not caps:
        print(f"no capture matching {args.pattern} in {args.native}", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)
    shape = ci.load_luma(caps[0]).shape
    try:
        nums, small = load_dolphin(args.dolphin, shape, args.out / "dolphin-cache.npz")
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 2
    rows = []
    for cap in caps:
        n = frame_number(cap)
        luma = ci.load_luma(cap)
        if luma.shape != shape:
            luma = ci.resize_to(shape, cap)
        sel = np.ones(len(nums), bool)
        if win and n is not None:
            sel = (nums >= n + args.offset + win[0]) & (nums <= n + args.offset + win[1])
        if not sel.any():
            rows.append({"capture": cap.name, "frame": n, "dolphin": None})
            continue
        idx = np.nonzero(sel)[0]
        err = ((small[idx] - reduce(luma)[None]) ** 2).mean(axis=(1, 2))
        order = idx[np.argsort(err)]
        best_err = float(err.min())
        flat = int((err <= best_err * 1.01 + 1e-6).sum())
        best = None
        for i in order[: args.top]:
            p = args.dolphin / f"framedump_{int(nums[i])}.png"
            r = ci.compare_pair(cap, p)
            if best is None or r["ssim"] > best["ssim"]:
                best = dict(r, dolphin_frame=int(nums[i]))
        row = {"capture": cap.name, "frame": n, "dolphin": best["dolphin_frame"],
               "delta": None if n is None else best["dolphin_frame"] - n,
               "ssim": best["ssim"], "psnr_db": best["psnr_db"], "identical": best["psnr_identical"],
               "coarse_mse": round(best_err, 2), "flat": flat, "pass": best["ssim"] >= args.ssim_threshold}
        rows.append(row)
        if args.diffs:
            save_diff(cap, args.dolphin / f"framedump_{best['dolphin_frame']}.png", args.out / "diffs" / cap.name)
    (args.out / "match.json").write_text(json.dumps(rows, indent=1))
    lines = ["| capture | frame | Dolphin | delta | SSIM | PSNR (dB) | flat | pass |", "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        if r.get("dolphin") is None:
            lines.append(f"| {r['capture']} | {r['frame']} | - | - | - | - | - | - |")
            continue
        ps = "inf" if r["identical"] else r["psnr_db"]
        lines.append(f"| {r['capture']} | {r['frame']} | {r['dolphin']} | {r['delta']} | {r['ssim']:.4f} | {ps} | "
                     f"{r['flat']} | {'yes' if r['pass'] else 'NO'} |")
    (args.out / "match.md").write_text("\n".join(lines) + "\n")
    ok = sum(1 for r in rows if r.get("pass"))
    measured = sum(1 for r in rows if r.get("dolphin") is not None)
    print(f"{ok}/{len(rows)} pairs with SSIM >= {args.ssim_threshold} ({measured} measured); "
          f"table: {args.out / 'match.md'}")
    return 0 if measured else 1


if __name__ == "__main__":
    sys.exit(main())
