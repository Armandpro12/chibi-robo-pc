#!/usr/bin/env python3
"""Residual between two recordings once aligned: where they differ, and which side holds the extra sound.

B is shifted by a fixed offset (B time = A time + offset, as printed by audio_align.py), then for each window the
script prints the level of A, of B and of A - B (dBFS, mono mix). Where A - B is far below both, the recordings
match; where A - B is close to one side's level, that side holds a sound the other lacks (or plays it elsewhere).
Consecutive differing windows are grouped into events; with --events only those are printed.

  py -3 tools/audio_residual.py a.wav b.wav --offset-samples -259784 --start 80 --end 420 [--win 0.25] [--events]
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from compare_ab import read_wav  # noqa: E402


def db(v):
    return 20 * np.log10(np.maximum(v, 1e-9) / 32768.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--offset-samples", type=int, required=True, help="B index = A index + offset")
    ap.add_argument("--start", type=float, default=0.0, help="A time (s)")
    ap.add_argument("--end", type=float, default=None, help="A time (s)")
    ap.add_argument("--win", type=float, default=0.25, help="window length (s)")
    ap.add_argument("--margin", type=float, default=20.0,
                    help="a window differs when A - B is less than this many dB below the louder side")
    ap.add_argument("--events", action="store_true", help="print the differing events only")
    args = ap.parse_args()

    ra, xa = read_wav(args.a)
    rb, xb = read_wav(args.b)
    if ra != rb:
        sys.exit(f"different rates: {ra} and {rb}")
    a = xa.mean(axis=1)
    b = xb.mean(axis=1)
    w = int(args.win * ra)
    i0 = int(args.start * ra)
    i1 = len(a) if args.end is None else min(len(a), int(args.end * ra))
    i1 = min(i1, len(b) - args.offset_samples)
    i0 = max(i0, -args.offset_samples)
    rows = []
    for i in range(i0, i1 - w + 1, w):
        sa = a[i:i + w]
        sb = b[i + args.offset_samples:i + args.offset_samples + w]
        la = db(np.sqrt((sa ** 2).mean()))
        lb = db(np.sqrt((sb ** 2).mean()))
        ld = db(np.sqrt(((sa - sb) ** 2).mean()))
        differs = ld > max(la, lb) - args.margin and max(la, lb) > -70
        rows.append((i / ra, la, lb, ld, differs))
    print(f"{args.a} vs {args.b}: {ra} Hz, offset {args.offset_samples} samples, windows of {args.win} s, "
          f"{sum(r[4] for r in rows)}/{len(rows)} differ (A - B within {args.margin} dB of the louder side)")
    events = []
    for t, la, lb, ld, d in rows:
        if d:
            if events and t - events[-1]["end"] <= args.win * 1.01:
                e = events[-1]
            else:
                e = {"start": t, "end": t, "a": [], "b": [], "d": []}
                events.append(e)
            e["end"] = t + args.win
            e["a"].append(la)
            e["b"].append(lb)
            e["d"].append(ld)
        if not args.events:
            print(f"  {t:9.3f} s  A {la:6.1f}  B {lb:6.1f}  A-B {ld:6.1f}{'  *' if d else ''}")
    for e in events:
        ma, mb, md = max(e["a"]), max(e["b"]), max(e["d"])
        side = "A louder" if ma > mb + 1 else ("B louder" if mb > ma + 1 else "same level")
        print(f"  event {e['start']:9.3f}-{e['end']:9.3f} s ({e['end'] - e['start']:.2f} s): "
              f"max A {ma:6.1f}, B {mb:6.1f}, A-B {md:6.1f} dBFS ({side})")


if __name__ == "__main__":
    main()
