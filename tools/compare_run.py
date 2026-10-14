"""Compares a native run (`tools/run.py`) and a Dolphin run (`tools/dolphin_ref.py`), frame by frame,
**aligned by presented-frame index (XFB copy)**, not by wall-clock second.

Why not by second: the native build runs locked to real time (verified: "t" = "host" in every
report of `trace.log`, without drift), but Dolphin in frame-dump mode runs **much slower than real
time** and at a speed that varies from one launch to the next (observed: ~26 to ~41 real frames/s
for a game that normally shows 50/s in PAL; see `notes/dolphin.md`). The real-clock second is
therefore not a reliable landmark to match a native capture with a Dolphin frame; the
presented-frame index (the Nth EFB->XFB copy since boot) is, because it depends only on the game
logic, not on the speed of the emulating machine.

Usage:
  py -3 tools/compare_run.py <native_run_folder> <dolphin_run_folder>
                             [--ssim-threshold 0.95] [--psnr-advisory 25] [--out DIR]

- `<native_run_folder>`: a `tests/<game>/<ID>/run-*/` folder produced by `tools/run.py --capture ...`
  (contains `trace.log` and `capture_XXXs.png` files).
- `<dolphin_run_folder>`: a `tests/dolphin/<ID>/run-*/` folder produced by `tools/dolphin_ref.py
  --frames` (contains `meta.json` and `frames/framedump_N.png`).

Alignment:
1. The `trace.log` of the native run gives the exact frame index of each capture **directly** since
   the follow-up of this milestone (line "capture requested (frame N, VI field M): ...", added by the
   runtime agent with `--capture-frames`/`--capture-fields` of `tools/run.py`): no estimate needed
   any more. For older runs (old format "capture requested at t=X s" without an index), we fall back
   on the old method: linear interpolation between the two surrounding report points (every 5 s) to
   estimate the number of frames presented at that instant; accuracy: within a few frames in general
   (see the internal differences log for the comparison of the two methods on the same runs, measured offset
   zero before any scripted input).
2. The name of the Dolphin files (`framedump_N.png`) already is this index on the Dolphin side
   (numbered at each XFB copy, see `notes/dolphin.md`). We take the available Dolphin frame closest
   to the target (exact or estimated); the offset (in frames) is recorded in the report.

Metric and thresholds: reuses `tools/compare_images.py` (PSNR + SSIM on the luma, Dolphin image
resized to the size of the native image by bilinear resampling; same method and same formulas,
see that file). **Known and measured resolution difference**: the native captures are 640x480,
the Dolphin dumps 640x468 (measured directly on the files of this repository; the runtime agent
had reported 640x528 for the raw PAL EFB before scaling on the native side; the difference probably
comes from XFB vs EFB). This scaling alone lowers the PSNR by ~10-15 dB even for a strictly
identical screen (measured: 28.9 dB on the Health/Safety warning, whereas two consecutive Dolphin
captures of the same static screen give 52 dB, see `notes/dolphin.md`). **The SSIM stays reliable**
(0.989 to 0.9996 on identical screens despite the resolution difference): it is therefore the main
verdict criterion here (`--ssim-threshold`, default 0.95, calibrated on these real measurements).
The PSNR is advisory only, with a low "for information" threshold (`--psnr-advisory`, default 25 dB)
that does not affect the verdict.

Outputs: `report.md` and `report.json` in `--out` (default: the native run folder), plus one
difference image (luma, absolute difference amplified x4 for readability) per compared pair,
in `<out>/diffs/`. Non-zero exit code if a pair is below the SSIM threshold.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_images as ci  # noqa: E402

REPORT_RE = re.compile(r"^\[\s*([\d.]+)\]\s*\[platform\]\s*(?:report|FINAL REPORT).*?images\s+(\d+)\b")
# Precise format (runtime, milestone 5 follow-up): the line directly gives the exact frame index.
CAPTURE_EXACT_RE = re.compile(
    r"^\[\s*([\d.]+)\]\s*\[platform\]\s*capture requested \(frame (\d+), VI field \d+\)\s*:\s*(.+\.png)\s*$")
# Old format (milestone 5, before the exact capture): only the instant, to interpolate from the reports.
CAPTURE_APPROX_RE = re.compile(r"^\[\s*[\d.]+\]\s*\[platform\]\s*capture requested at t=([\d.]+)\s*s\s*:\s*(.+\.png)\s*$")
DOLPHIN_FRAME_RE = re.compile(r"framedump_(\d+)\.png$")

DEFAULT_SSIM_THRESHOLD = 0.95
DEFAULT_PSNR_ADVISORY = 25.0


def parse_native_trace(trace_path: Path) -> tuple[list[tuple[float, int]], list[tuple[float, str, int | None]]]:
    """Returns (sorted (t, images) samples, captures (t, file_name, exact_image_or_None)
    in log order). `exact_image` comes directly from `trace.log` when the runtime provides it
    (no approximate format to interpolate any more, see the precise format above); otherwise None
    and the caller must interpolate with `interpolate_images()` (old format, kept for the runs
    captured before this runtime improvement)."""
    samples: list[tuple[float, int]] = []
    captures: list[tuple[float, str, int | None]] = []
    for line in trace_path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = REPORT_RE.match(line)
        if m:
            samples.append((float(m.group(1)), int(m.group(2))))
            continue
        m = CAPTURE_EXACT_RE.match(line)
        if m:
            captures.append((float(m.group(1)), Path(m.group(3)).name, int(m.group(2))))
            continue
        m = CAPTURE_APPROX_RE.match(line)
        if m:
            captures.append((float(m.group(1)), Path(m.group(2)).name, None))
    samples.sort(key=lambda s: s[0])
    return samples, captures


def interpolate_images(samples: list[tuple[float, int]], t: float) -> float:
    if not samples:
        raise ValueError("no report sample in trace.log")
    if t <= samples[0][0]:
        return float(samples[0][1])
    if t >= samples[-1][0]:
        return float(samples[-1][1])
    for (t0, i0), (t1, i1) in zip(samples, samples[1:]):
        if t0 <= t <= t1:
            if t1 == t0:
                return float(i0)
            frac = (t - t0) / (t1 - t0)
            return i0 + frac * (i1 - i0)
    return float(samples[-1][1])


def list_dolphin_frames(frames_dir: Path) -> dict[int, Path]:
    out = {}
    for p in frames_dir.glob("framedump_*.png"):
        m = DOLPHIN_FRAME_RE.search(p.name)
        if m:
            out[int(m.group(1))] = p
    return out


def nearest_frame(frames: dict[int, Path], target: int) -> tuple[int, Path] | None:
    if not frames:
        return None
    best = min(frames.keys(), key=lambda n: abs(n - target))
    return best, frames[best]


def save_diff_image(native_path: Path, dolphin_path: Path, out_path: Path) -> None:
    a = ci.load_luma(native_path)
    b_img = Image.open(dolphin_path).convert("L")
    if (b_img.height, b_img.width) != a.shape:
        b_img = b_img.resize((a.shape[1], a.shape[0]), Image.BILINEAR)
    b = np.asarray(b_img, dtype=np.float64)
    diff = np.clip(np.abs(a - b) * 4.0, 0, 255).astype(np.uint8)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(diff, mode="L").save(out_path)


def main() -> int:
    parser = argparse.ArgumentParser(description="Compares a native run and a Dolphin run, aligned by frame index.")
    parser.add_argument("native_run", type=Path)
    parser.add_argument("dolphin_run", type=Path)
    parser.add_argument("--ssim-threshold", type=float, default=DEFAULT_SSIM_THRESHOLD)
    parser.add_argument("--psnr-advisory", type=float, default=DEFAULT_PSNR_ADVISORY,
                         help="advisory threshold only (does not affect the verdict), cf. docstring")
    parser.add_argument("--out", type=Path, default=None, help="default: the native run folder")
    parser.add_argument("--manual-pair", action="append", default=[],
                         metavar="capture_XXXs.png=N",
                         help="forces Dolphin frame N for this capture instead of the interpolation "
                              "estimate (repeatable). Useful after a scripted input event: "
                              "the native build (locked to real time) and Dolphin (unstable dump speed, "
                              "see docstring) usually do not trigger the same action at the same "
                              "frame index, so the interpolation estimate can land far from the "
                              "right screen after that point; an offset to find and fix by hand "
                              "(see the internal differences log for an example on Chibi-Robo!).")
    args = parser.parse_args()
    manual_pairs = {}
    for item in args.manual_pair:
        name, _, frame_s = item.partition("=")
        if not frame_s:
            raise SystemExit(f"--manual-pair expects capture.png=N, got {item!r}")
        manual_pairs[name] = int(frame_s)

    trace_path = args.native_run / "trace.log"
    if not trace_path.exists():
        raise SystemExit(f"trace.log not found in {args.native_run}")
    samples, captures = parse_native_trace(trace_path)
    if not captures:
        raise SystemExit(f'No "capture requested" line in {trace_path}')

    dolphin_meta_path = args.dolphin_run / "meta.json"
    frames_dir = args.dolphin_run / "frames"
    if not frames_dir.exists():
        raise SystemExit(f"frames/ folder not found in {args.dolphin_run} "
                          "(run tools/dolphin_ref.py with --frames)")
    dolphin_meta = json.loads(dolphin_meta_path.read_text(encoding="utf-8")) if dolphin_meta_path.exists() else {}
    dolphin_frames = list_dolphin_frames(frames_dir)
    if not dolphin_frames:
        raise SystemExit(f"No framedump_*.png in {frames_dir}")
    dolphin_max = max(dolphin_frames)

    out_dir = args.out or args.native_run
    diffs_dir = out_dir / "diffs"

    results = []
    for t_capture, filename, exact_image in captures:
        native_path = args.native_run / filename
        if not native_path.exists():
            results.append({"capture": filename, "native_t_s": t_capture, "error": "native file missing"})
            continue
        if exact_image is not None:
            estimated = float(exact_image)
            exact = True
        else:
            estimated = interpolate_images(samples, t_capture)
            exact = False
        target = round(estimated)
        manual = filename in manual_pairs
        if manual:
            target = manual_pairs[filename]
        found = nearest_frame(dolphin_frames, target)
        entry: dict = {
            "capture": filename,
            "native_t_s": t_capture,
            "native_image": int(estimated) if exact else None,
            "native_image_estimated": None if exact else round(estimated, 1),
            "dolphin_image_target": target,
            "manual_target": manual,
        }
        if manual:
            entry["note"] = ("index forced by --manual-pair: the interpolation estimate "
                              f"({round(estimated)}) falls after a scripted input event, "
                              "where native and Dolphin diverge in frame index (see docstring).")
        if target > dolphin_max:
            entry["warning"] = (
                f"target ({target}) beyond the last available Dolphin frame ({dolphin_max})"
                ": the Dolphin run probably did not go that far."
            )
        if found is None:
            entry["error"] = "no Dolphin frame available"
            results.append(entry)
            continue
        dolphin_index, dolphin_path = found
        entry["dolphin_image_used"] = dolphin_index
        entry["image_offset"] = dolphin_index - target

        r = ci.compare_pair(native_path, dolphin_path)
        entry["psnr_db"] = r["psnr_db"]
        entry["psnr_identical"] = bool(r["psnr_identical"])
        entry["ssim"] = float(r["ssim"])
        entry["resized"] = bool(r["resized"])
        entry["ssim_pass"] = bool(r["ssim"] >= args.ssim_threshold)
        entry["psnr_advisory_pass"] = bool(r["psnr_identical"] or
                                                (r["psnr_db"] is not None and r["psnr_db"] >= args.psnr_advisory))

        diff_name = f"diff_{Path(filename).stem}_vs_framedump_{dolphin_index}.png"
        save_diff_image(native_path, dolphin_path, diffs_dir / diff_name)
        entry["diff_image"] = str((diffs_dir / diff_name).relative_to(out_dir))

        results.append(entry)

    all_ok = all(r.get("ssim_pass", False) for r in results)

    report = {
        "native_run": str(args.native_run),
        "dolphin_run": str(args.dolphin_run),
        "dolphin_meta": {k: dolphin_meta.get(k) for k in ("game_id", "region", "movie")},
        "ssim_threshold": args.ssim_threshold,
        "psnr_advisory_threshold": args.psnr_advisory,
        "results": results,
        "all_pass_ssim": all_ok,
    }

    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")

    lines = [
        "# Native / Dolphin comparison",
        "",
        f"- Native run: `{args.native_run}`",
        f"- Dolphin run: `{args.dolphin_run}`",
        f"- SSIM threshold (verdict criterion): {args.ssim_threshold}",
        f"- PSNR threshold (advisory only, see the docstring of `tools/compare_run.py`): {args.psnr_advisory} dB",
        "",
        "| native capture | t (s) | native frame | targeted Dolphin frame | Dolphin frame used | offset (frames) | PSNR | SSIM | verdict (SSIM) |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        if "error" in r and "ssim" not in r:
            lines.append(f"| {r['capture']} | {r['native_t_s']} | - | - | - | - | - | - | **ERROR: {r['error']}** |")
            continue
        psnr_s = "inf" if r["psnr_identical"] else f"{r['psnr_db']} dB"
        verdict = "OK" if r["ssim_pass"] else "**BELOW THRESHOLD**"
        native_image_s = r["native_image"] if r["native_image"] is not None else f"{r['native_image_estimated']} (estimated)"
        lines.append(f"| {r['capture']} | {r['native_t_s']} | {native_image_s} | "
                     f"{r['dolphin_image_target']} | {r['dolphin_image_used']} | {r['image_offset']} | "
                     f"{psnr_s} | {r['ssim']} | {verdict} |")
        if r.get("warning"):
            lines.append(f"  - WARNING: {r['warning']}")
    lines.append("")
    lines.append(f"**Overall verdict (SSIM ≥ {args.ssim_threshold}): "
                 f"{'ALL PAIRS PASS' if all_ok else 'AT LEAST ONE PAIR BELOW THRESHOLD'}**")
    (out_dir / "report.md").write_text("\n".join(lines), encoding="utf-8")

    print("\n".join(lines))
    print(f"\nReport: {out_dir / 'report.md'} / {out_dir / 'report.json'}")
    print(f"Difference images: {diffs_dir}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
