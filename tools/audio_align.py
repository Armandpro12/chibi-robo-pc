"""Aligns two audio recordings (or one recording with itself one period later) sample by sample, over time.

Usage:
  py -3 tools/audio_align.py <a.wav> <b.wav> [--start 0] [--end E] [--step 2] [--block 0.5] [--search 3]
                             [--offset S] [--search-first S] [--json]
  py -3 tools/audio_align.py <a.wav> <a.wav> --offset <period in s>      (repetition of a loop, see audio_loop.py)

For each anchor of A (a block of --block seconds every --step seconds), finds the position in B with the
highest cross-correlation (FFT), within +-search seconds around the offset of the previous anchor (the first
anchor is searched around --offset; with --search-first, within +-that many seconds until a first anchor matches, to find
an unknown starting offset). Each anchor gets its offset in samples and the SNR of B against A at that
offset ("inf" = identical samples). Then:
  - runs: consecutive anchors with the same offset; between two runs, a jump of N samples means that one side
    played N samples more (positive: B is later, e.g. a pause or a gap inserted in B) somewhere in between;
  - the jump positions are refined to 5 ms by a residual scan between the two anchors;
  - drift: slope of the offset over time (parts per million), zero when both play at the same rate; it is computed over
    the whole file, so it is only meaningful when there is no jump (with loop seams it reports the seams, not a drift).
Limits: the cross-correlation is not normalized by the local energy, so an anchor can lock onto a louder passage
(low SNR) although an exact match exists elsewhere in the search window: such an anchor is reported as not matched.
In JSON output an infinite SNR is the string "inf".
Rates must be equal (the comparison is in samples). No game data is read or written.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_ab import read_wav  # noqa: E402


def strict(o):
    """JSON-safe copy of a result: +inf / -inf become the strings "inf" / "-inf" and NaN becomes null
    (json.dumps would write the invalid tokens Infinity / NaN, which a strict parser refuses)."""
    if isinstance(o, dict):
        return {k: strict(v) for k, v in o.items()}
    if isinstance(o, (list, tuple)):
        return [strict(v) for v in o]
    if isinstance(o, (float, np.floating)):
        v = float(o)
        if v != v:
            return None
        return "inf" if v == float("inf") else "-inf" if v == float("-inf") else v
    if isinstance(o, np.integer):
        return int(o)
    return o


def xcorr_lag(ref: np.ndarray, cand: np.ndarray) -> int:
    nfft = 1 << (len(cand) + len(ref)).bit_length()
    xc = np.fft.irfft(np.fft.rfft(cand, nfft) * np.conj(np.fft.rfft(ref, nfft)), nfft)[: len(cand) - len(ref) + 1]
    return int(np.argmax(xc))


def snr(a: np.ndarray, b: np.ndarray) -> float:
    e = float(((a - b) ** 2).sum())
    s = float((a ** 2).sum())
    if s <= 0:
        return float("nan")
    return float("inf") if e == 0 else 10 * np.log10(s / e)


def refine_jump(ma, mb, i_lo, i_hi, off_before, off_after, rate):
    """First 5 ms window between two anchors where A no longer matches B at the old offset."""
    w = int(0.005 * rate)
    for i in range(i_lo, i_hi, w):
        j = i + off_before
        if j < 0 or j + w > len(mb) or i + w > len(ma):
            break
        if snr(ma[i:i + w], mb[j:j + w]) < 30:
            return i
    return None


def align(pa: Path, pb: Path, start: float, end: float | None, step: float, block: float, search: float,
          offset: float, search_first: float | None = None) -> dict:
    ra, xa = read_wav(pa)
    rb, xb = read_wav(pb)
    if ra != rb:
        raise SystemExit(f"different rates ({ra} / {rb} Hz): compare at equal rates")
    ma, mb = xa.mean(axis=1), xb.mean(axis=1)
    blk, sp = int(block * ra), int(search * ra)
    sp_first = sp if search_first is None else int(search_first * ra)
    found = False
    off = int(round(offset * ra))
    anchors = []
    t = start
    last = len(ma) if end is None else min(len(ma), int(end * ra))
    while int(t * ra) + blk <= last:
        i = int(t * ra)
        ref = ma[i:i + blk]
        if np.abs(ref).max() < 1.0:      # silent anchor: nothing to align
            t += step
            continue
        w = sp if found else sp_first
        lo = max(0, i + off - w)
        cand = mb[lo: i + off + blk + w]
        if len(cand) < blk:
            break
        j = lo + xcorr_lag(ref, cand)
        s = snr(ref, mb[j:j + blk])
        if s >= 10:                      # a real match: follow it
            off = j - i
            found = True
        anchors.append({"t": round(t, 3), "offset": j - i, "snr_db": s})
        t += step
    runs = []
    for a in anchors:
        if a["snr_db"] < 10:
            continue
        if runs and runs[-1]["offset"] == a["offset"]:
            runs[-1]["to"] = a["t"]
            runs[-1]["n"] += 1
            runs[-1]["min_snr"] = min(runs[-1]["min_snr"], a["snr_db"])
        else:
            runs.append({"from": a["t"], "to": a["t"], "offset": a["offset"], "n": 1, "min_snr": a["snr_db"]})
    jumps = []
    for r0, r1 in zip(runs, runs[1:]):
        pos = refine_jump(ma, mb, int(r0["to"] * ra), int(r1["from"] * ra) + blk, r0["offset"], r1["offset"], ra)
        jumps.append({"after_s": r0["to"], "before_s": r1["from"], "at_s": None if pos is None else round(pos / ra, 3),
                      "samples": r1["offset"] - r0["offset"]})
    good = [a for a in anchors if a["snr_db"] >= 10]
    drift = None
    if len(good) >= 3:
        tt = np.array([a["t"] for a in good])
        oo = np.array([a["offset"] for a in good], dtype=float)
        drift = float(np.polyfit(tt, oo, 1)[0] / ra * 1e6)
    finite = [a["snr_db"] for a in good if np.isfinite(a["snr_db"])]
    return {"a": str(pa), "b": str(pb), "rate": ra, "anchors": len(anchors), "matched": len(good),
            "identical": sum(1 for a in good if a["snr_db"] == float("inf")),
            "min_snr_db": min(finite) if finite else None, "drift_ppm": drift, "runs": runs, "jumps": jumps,
            "unmatched": [a["t"] for a in anchors if a["snr_db"] < 10], "detail": anchors}


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("a", type=Path)
    p.add_argument("b", type=Path)
    p.add_argument("--start", type=float, default=0.0)
    p.add_argument("--end", type=float)
    p.add_argument("--step", type=float, default=2.0)
    p.add_argument("--block", type=float, default=0.5)
    p.add_argument("--search", type=float, default=3.0)
    p.add_argument("--offset", type=float, default=0.0, help="expected offset of B against A at the first anchor (s)")
    p.add_argument("--search-first", type=float, default=None,
                   help="search window (s) around --offset until a first anchor matches (default: --search)")
    p.add_argument("--json", action="store_true")
    a = p.parse_args()
    r = align(a.a, a.b, a.start, a.end, a.step, a.block, a.search, a.offset, a.search_first)
    if a.json:
        print(json.dumps(strict(r), indent=1))
        return 0
    print(f"{r['a']} -> {r['b']} ({r['rate']} Hz): {r['matched']}/{r['anchors']} anchors matched, "
          f"{r['identical']} identical to the sample, lowest finite SNR {r['min_snr_db']}, drift {r['drift_ppm']} ppm")
    for x in r["runs"]:
        print(f"  {x['from']:8.1f}-{x['to']:8.1f} s: offset {x['offset']:+d} samples ({x['offset'] / r['rate']:+.4f} s), "
              f"{x['n']} anchors, min SNR {x['min_snr']:.1f} dB")
    for j in r["jumps"]:
        print(f"  jump {j['samples']:+d} samples ({j['samples'] / r['rate'] * 1000:+.1f} ms) at {j['at_s']} s "
              f"(between {j['after_s']} and {j['before_s']})")
    if r["unmatched"]:
        print(f"  unmatched anchors (SNR < 10 dB): {r['unmatched'][:20]}{' ...' if len(r['unmatched']) > 20 else ''}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
