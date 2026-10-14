"""Compares two images, or two folders of images, for the comparisons with Dolphin (milestones 5/9).

Usage:
  py -3 tools/compare_images.py <imgA.png> <imgB.png> [--psnr-threshold 35] [--ssim-threshold 0.95]
  py -3 tools/compare_images.py <folderA> <folderB> [--pattern "*.png"] [--limit N] [--json]

For two folders, the files are paired by position after a natural sort (numeric when the names
contain numbers, e.g. framedump_2.png before framedump_10.png); the folders do not need to have
the same number of files, the pairing stops at the shorter one (with a warning). To compare two
specific captures frame by frame, pass two files directly.

Metrics (documented choices):
- PSNR (Peak Signal-to-Noise Ratio), in dB, on the luma: 10*log10(255^2 / MSE), where MSE is the
  mean squared error between the two images converted to grayscale (BT.601 formula:
  Y = 0.299 R + 0.587 G + 0.114 B, the same as PIL `Image.convert("L")`). PSNR = +inf if the images
  are identical pixel for pixel.
- SSIM (Structural Similarity), simplified version: formula of Wang et al. 2004, but with a sliding
  *mean* window (box filter, computed with cumsum to stay fast without a scipy dependency) instead
  of the Gaussian window of the original paper. Default window: 7×7.
  Standard constants: C1 = (0.01*255)^2, C2 = (0.03*255)^2.
  This version is a recognized approximation, not a bit-exact implementation of scikit-image;
  do not compare it numerically with SSIM values published elsewhere without recomputing them
  with the same code.
- If the two images do not have the same resolution, the second one is resized to the size of the
  first one with bilinear resampling (PIL `Image.BILINEAR`) before the comparison. Recorded in the
  result (`resized`).

Dependencies: Pillow and numpy (installed locally with `py -3 -m pip install --user pillow numpy`,
see notes/decisions.md; no heavier dependency such as scikit-image or opencv).

Default thresholds (calibrated on real Dolphin GGTP01 captures, see notes/dolphin.md):
  PSNR >= 35 dB AND SSIM >= 0.95: considered "identical" (same scene, negligible noise).
  Below that, the comparison fails (non-zero exit code); the scores are printed whatever the
  result, to allow a human diagnosis.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

DEFAULT_PSNR_THRESHOLD = 35.0
DEFAULT_SSIM_THRESHOLD = 0.95


def _natural_key(path: Path):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r"(\d+)", path.name)]


def load_luma(path: Path) -> np.ndarray:
    """Loads an image and returns its luma (grayscale) as float64, BT.601 formula
    (the one used by PIL `convert("L")`)."""
    img = Image.open(path).convert("L")
    return np.asarray(img, dtype=np.float64)


def resize_to(ref_shape: tuple[int, int], path: Path) -> np.ndarray:
    img = Image.open(path).convert("L")
    if (img.height, img.width) != ref_shape:
        img = img.resize((ref_shape[1], ref_shape[0]), Image.BILINEAR)
    return np.asarray(img, dtype=np.float64)


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a - b) ** 2))
    if mse == 0.0:
        return float("inf")
    return 10.0 * np.log10((255.0 ** 2) / mse)


def _box_filter(img: np.ndarray, k: int) -> np.ndarray:
    """k×k mean filter using cumulative sums (no scipy dependency). 'valid' mode:
    the result is smaller than the input by (k-1) on each axis (no border handling)."""
    c = np.cumsum(np.cumsum(img, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)), mode="constant")
    s = c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]
    return s / (k * k)


def ssim(a: np.ndarray, b: np.ndarray, k: int = 7) -> float:
    C1 = (0.01 * 255) ** 2
    C2 = (0.03 * 255) ** 2
    if a.shape[0] < k or a.shape[1] < k:
        k = max(3, min(a.shape[0], a.shape[1]))
        if k % 2 == 0:
            k -= 1
    mu_a = _box_filter(a, k)
    mu_b = _box_filter(b, k)
    mu_a2 = mu_a * mu_a
    mu_b2 = mu_b * mu_b
    mu_ab = mu_a * mu_b
    sigma_a2 = _box_filter(a * a, k) - mu_a2
    sigma_b2 = _box_filter(b * b, k) - mu_b2
    sigma_ab = _box_filter(a * b, k) - mu_ab
    num = (2 * mu_ab + C1) * (2 * sigma_ab + C2)
    den = (mu_a2 + mu_b2 + C1) * (sigma_a2 + sigma_b2 + C2)
    return float(np.mean(num / den))


def compare_pair(path_a: Path, path_b: Path, mask: tuple[int, int, int, int] | None = None) -> dict:
    """`mask`, if given, is a rectangle (x0,y0,x1,y1) in the coordinates of image A: the area is
    copied from A to B before computing the metrics (so it is compared with itself, a neutral
    contribution), to isolate a known area of irrelevant difference (e.g. an unsynchronized
    animation) from the rest of the image. Document it explicitly at each use (see the internal differences log
    for an example): it must never be used to hide a real difference."""
    a = load_luma(path_a)
    resized = False
    b_img = Image.open(path_b).convert("L")
    if (b_img.height, b_img.width) != a.shape:
        b_img = b_img.resize((a.shape[1], a.shape[0]), Image.BILINEAR)
        resized = True
    b = np.asarray(b_img, dtype=np.float64)
    if mask:
        x0, y0, x1, y1 = mask
        b = b.copy()
        b[y0:y1, x0:x1] = a[y0:y1, x0:x1]

    return {
        "a": str(path_a),
        "b": str(path_b),
        "resized": resized,
        "mask": list(mask) if mask else None,
        "psnr_db": None if np.isinf(psnr(a, b)) else round(psnr(a, b), 2),
        "psnr_identical": np.isinf(psnr(a, b)),
        "ssim": round(ssim(a, b), 4),
    }


def verdict(result: dict, psnr_threshold: float, ssim_threshold: float) -> bool:
    if result["psnr_identical"]:
        return True
    psnr_ok = result["psnr_db"] is not None and result["psnr_db"] >= psnr_threshold
    ssim_ok = result["ssim"] >= ssim_threshold
    return psnr_ok and ssim_ok


def collect_images(d: Path, pattern: str) -> list[Path]:
    return sorted(d.glob(pattern), key=_natural_key)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compares two images or two folders of images (PSNR + SSIM).")
    parser.add_argument("a", type=Path)
    parser.add_argument("b", type=Path)
    parser.add_argument("--pattern", default="*.png", help="file pattern for a folder (default: *.png)")
    parser.add_argument("--limit", type=int, default=None, help="limits the number of compared pairs (folders)")
    parser.add_argument("--psnr-threshold", type=float, default=DEFAULT_PSNR_THRESHOLD)
    parser.add_argument("--ssim-threshold", type=float, default=DEFAULT_SSIM_THRESHOLD)
    parser.add_argument("--mask", default=None, metavar="x0,y0,x1,y1",
                         help="excludes this rectangle (coordinates of image A, files only) from the "
                              "comparison by copying it from A to B before computing the metrics; "
                              "see the docstring of compare_pair()")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    mask = tuple(int(v) for v in args.mask.split(",")) if args.mask else None
    if mask and len(mask) != 4:
        raise SystemExit("--mask expects x0,y0,x1,y1")

    if args.a.is_dir() and args.b.is_dir():
        if mask:
            raise SystemExit("--mask is only supported when comparing two files, not two folders.")
        files_a = collect_images(args.a, args.pattern)
        files_b = collect_images(args.b, args.pattern)
        n = min(len(files_a), len(files_b))
        if args.limit:
            n = min(n, args.limit)
        if len(files_a) != len(files_b):
            print(f"[warning] {len(files_a)} files in {args.a}, {len(files_b)} in {args.b}"
                  f": comparison limited to the first {n} pairs, paired by position.", file=sys.stderr)
        results = [compare_pair(files_a[i], files_b[i]) for i in range(n)]
        all_ok = all(verdict(r, args.psnr_threshold, args.ssim_threshold) for r in results)
        summary = {
            "pairs": results,
            "count": n,
            "psnr_threshold": args.psnr_threshold,
            "ssim_threshold": args.ssim_threshold,
            "all_pass": all_ok,
        }
        if args.json:
            print(json.dumps(summary, indent=2))
        else:
            for r in results:
                ok = verdict(r, args.psnr_threshold, args.ssim_threshold)
                psnr_s = "inf (identical)" if r["psnr_identical"] else f"{r['psnr_db']} dB"
                print(f"{'OK  ' if ok else 'DIFF'}  {Path(r['a']).name} vs {Path(r['b']).name}  "
                      f"PSNR={psnr_s}  SSIM={r['ssim']}" + ("  [resized]" if r["resized"] else ""))
            print(f"\n{sum(verdict(r, args.psnr_threshold, args.ssim_threshold) for r in results)}/{n} pairs "
                  f"above the threshold (PSNR>={args.psnr_threshold} dB and SSIM>={args.ssim_threshold}).")
        return 0 if all_ok else 1

    if args.a.is_file() and args.b.is_file():
        r = compare_pair(args.a, args.b, mask=mask)
        ok = verdict(r, args.psnr_threshold, args.ssim_threshold)
        if args.json:
            print(json.dumps({**r, "pass": ok}, indent=2))
        else:
            psnr_s = "inf (identical)" if r["psnr_identical"] else f"{r['psnr_db']} dB"
            print(f"PSNR = {psnr_s}")
            print(f"SSIM = {r['ssim']}")
            if r["resized"]:
                print("(image B resized to match A)")
            if r["mask"]:
                print(f"(masked area, copied from A to B before the comparison: {r['mask']})")
            print("Verdict:", "IDENTICAL/VERY CLOSE" if ok else "DIFFERENT",
                  f"(thresholds PSNR>={args.psnr_threshold} dB, SSIM>={args.ssim_threshold})")
        return 0 if ok else 1

    raise SystemExit("Both arguments must be either two files or two folders.")


if __name__ == "__main__":
    sys.exit(main())
