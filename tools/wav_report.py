"""Short report on a WAV file: duration, format, RMS level per second.

Usage: py -3 tools/wav_report.py <file.wav> [--json]

Generic (does not depend on any game). Uses only the standard library (`wave`, `audioop`
if available to speed up the RMS computation, otherwise a pure Python fallback). Used to check that
a Dolphin audio dump is not silent/empty before a more thorough comparison.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import wave
from pathlib import Path


def rms_of_chunk(frames: bytes, sampwidth: int, n_channels: int) -> float:
    if sampwidth == 2:
        count = len(frames) // 2
        if count == 0:
            return 0.0
        samples = struct.unpack(f"<{count}h", frames[:count * 2])
    elif sampwidth == 1:
        count = len(frames)
        if count == 0:
            return 0.0
        samples = [b - 128 for b in frames[:count]]
    else:
        raise ValueError(f"Unsupported sample width: {sampwidth} bytes")
    sq_sum = sum(s * s for s in samples)
    return math.sqrt(sq_sum / len(samples))


def analyze(path: Path) -> dict:
    with wave.open(str(path), "rb") as w:
        n_channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        framerate = w.getframerate()
        n_frames = w.getnframes()
        duration = n_frames / framerate if framerate else 0.0

        per_second = []
        frames_per_second = framerate
        remaining = n_frames
        second = 0
        while remaining > 0:
            to_read = min(frames_per_second, remaining)
            raw = w.readframes(to_read)
            rms = rms_of_chunk(raw, sampwidth, n_channels)
            max_amplitude = (2 ** (8 * sampwidth - 1)) - 1
            per_second.append({
                "second": second,
                "rms": round(rms, 2),
                "rms_dbfs": round(20 * math.log10(rms / max_amplitude), 1) if rms > 0 else None,
            })
            remaining -= to_read
            second += 1

    overall_rms = math.sqrt(sum(p["rms"] ** 2 for p in per_second) / len(per_second)) if per_second else 0.0
    return {
        "path": str(path),
        "channels": n_channels,
        "sample_width_bytes": sampwidth,
        "framerate": framerate,
        "n_frames": n_frames,
        "duration_seconds": round(duration, 3),
        "overall_rms": round(overall_rms, 2),
        "silent": overall_rms < 1.0,
        "per_second": per_second,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Duration + per-second RMS report for a WAV file.")
    parser.add_argument("wav", type=Path)
    parser.add_argument("--json", action="store_true", help="raw JSON output")
    args = parser.parse_args()

    report = analyze(args.wav)
    if args.json:
        print(json.dumps(report, indent=2))
        return 0

    print(f"File: {report['path']}")
    print(f"Format: {report['channels']} channel(s), {report['sample_width_bytes'] * 8} bits, "
          f"{report['framerate']} Hz")
    print(f"Duration: {report['duration_seconds']} s ({report['n_frames']} samples)")
    print(f"Overall RMS: {report['overall_rms']} ({'SILENT' if report['silent'] else 'not silent'})")
    print("RMS per second (0 = total silence, ~32767 = 16-bit full scale):")
    for p in report["per_second"]:
        bar = "#" * min(60, int(p["rms"] / 200))
        print(f"  t={p['second']:>3}s  rms={p['rms']:>8.1f}  {bar}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
