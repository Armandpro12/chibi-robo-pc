"""Measures how a looping piece of music repeats in a WAV recording: period, seams, gaps, identity, drift.

Usage:
  py -3 tools/audio_loop.py <file.wav> [--start S] [--end E] [--min-period 8] [--max-period 240] [--json]
  py -3 tools/audio_loop.py <a.wav> --compare <b.wav> [--start-b S] [--end-b E] [...]

Meant for a scene recorded without any input (a menu or a room left alone), where only the music changes.
  1. coarse period: self-correlation of the envelope (dB per 100 ms window) between --min-period and
     --max-period, keeping the smallest lag within 0.01 of the best score (a multiple scores as high);
     (its resolution is 0.1 s: the exact period comes from the passes, see 3.);
  2. seams: runs of digital silence (|sample| < 1 on every channel, at least --gap-ms long) that come back once
     per period (+-2 s); a stream restarted by the game at its loop point leaves such a gap;
  3. each pass (sound between two seams): length in samples, and SNR of the pass against the previous one,
     aligned on the restart ("inf" = the pass repeats to the sample);
  4. each seam: gap length, level of the last and first 5 ms around the gap (dBFS: a cut at full level would
     click, a fade does not), level step between the 50 ms before the gap and the 50 ms after it;
  5. without any gap, the period comes from a sample alignment of the file with itself (tools/audio_align.py),
     and the loop is reported as seamless.
With --compare, the second file (e.g. a Dolphin recording) is measured the same way and the pass lengths and
gaps are put side by side: equal pass lengths (in samples) mean the same music at the same speed; the gaps tell
how long each side waits at the loop point. Rates must be equal for a comparison in samples.
No game data is read or written.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare_ab import read_wav  # noqa: E402
import audio_align  # noqa: E402


def env_db(m: np.ndarray, rate: int, win_s: float) -> np.ndarray:
    w = max(1, int(round(rate * win_s)))
    n = (len(m) // w) * w
    p = np.sqrt((m[:n] ** 2).reshape(-1, w).mean(axis=1))
    return np.maximum(20.0 * np.log10(np.maximum(p, 1e-9) / 32768.0), -80.0)


def level_db(x: np.ndarray) -> float:
    return float(20 * np.log10(max(float(np.sqrt((x ** 2).mean())) if len(x) else 0.0, 1e-9) / 32768.0))


def pearson(u, v) -> float:
    n = min(len(u), len(v))
    if n < 2 or np.std(u[:n]) < 1e-12 or np.std(v[:n]) < 1e-12:
        return 0.0
    return float(np.corrcoef(u[:n], v[:n])[0, 1])


def coarse_period(m, rate, pmin, pmax, win=0.1):
    e = env_db(m, rate, win)
    scores = [(lag, pearson(e[:-lag], e[lag:])) for lag in range(int(pmin / win), min(int(pmax / win), len(e) // 2) + 1)]
    if not scores:
        return None, None
    best = max(c for _, c in scores)
    for i, (lag, c) in enumerate(scores):
        nb = [scores[j][1] for j in (i - 1, i + 1) if 0 <= j < len(scores)]
        if c >= best - 0.01 and all(c >= v for v in nb):
            return lag * win, c
    return None, None


def silent_runs(x, rate, min_ms):
    z = (np.abs(x).max(axis=1) < 1).astype(np.int8)
    d = np.diff(np.concatenate([[0], z, [0]]))
    s, e = np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]
    return [(int(a), int(b)) for a, b in zip(s, e) if (b - a) * 1000.0 / rate >= min_ms]


def analyze(path: Path, start: float, end, pmin, pmax, gap_ms) -> dict:
    rate, x = read_wav(path)
    i0 = int(start * rate)
    i1 = len(x) if end is None else min(len(x), int(end * rate))
    x = x[i0:i1]
    m = x.mean(axis=1)
    res = {"file": str(path), "rate": rate, "region_s": [round(start, 3), round((i0 + len(m)) / rate, 3)]}
    nz = np.nonzero(np.abs(m) >= 1)[0]
    if not len(nz):
        res["silent"] = True
        return res
    first = int(nz[0])
    p0, score = coarse_period(m[first:], rate, pmin, pmax)
    res["coarse_period_s"], res["coarse_self_corr"] = p0, score
    runs = [(a, b) for a, b in silent_runs(x, rate, gap_ms) if a > first]   # leading silence is not a seam
    res["silences"] = [{"t_s": round((i0 + a) / rate, 3), "ms": round((b - a) * 1000 / rate, 1)} for a, b in runs]
    seams = []
    if p0:
        tol = int(2.0 * rate)
        for a, b in runs:
            if not seams or abs((a - seams[-1][0]) - p0 * rate) <= tol:
                seams.append((a, b))
            elif a - seams[-1][0] > p0 * rate + tol and len(seams) == 1:
                seams = [(a, b)]          # the first silence was not a seam
        if len(seams) < 2 and not (len(seams) == 1 and len(m) - seams[0][1] < p0 * rate + tol):
            seams = seams if len(seams) == 1 else []
    w5, w50 = int(0.005 * rate), int(0.05 * rate)
    res["seams"] = [{"t_s": round((i0 + a) / rate, 4), "gap_samples": b - a, "gap_ms": round((b - a) * 1000 / rate, 1),
                     "last_5ms_db": round(level_db(m[max(0, a - w5):a]), 1), "first_5ms_db": round(level_db(m[b:b + w5]), 1),
                     "step_db": round(level_db(m[b:b + w50]) - level_db(m[max(0, a - w50):a]), 1)} for a, b in seams]
    passes = []
    for k in range(len(seams) - 1):
        a0, b0 = seams[k]
        a1, b1 = seams[k + 1]
        p = {"pass": k + 1, "restart_sample": i0 + b0, "restart_s": round((i0 + b0) / rate, 4), "sound_samples": a1 - b0,
             "period_samples": b1 - b0, "period_s": round((b1 - b0) / rate, 4)}
        passes.append(p)
    # SNR of each full pass against the previous one, aligned on the restart.
    for k in range(1, len(passes)):
        prev, cur = passes[k - 1], passes[k]
        n = min(prev["sound_samples"], cur["sound_samples"])
        s0 = prev["restart_sample"] - i0
        s1 = cur["restart_sample"] - i0
        cur["snr_vs_previous_db"] = audio_align.snr(m[s0:s0 + n], m[s1:s1 + n])
    res["passes"] = passes
    if not seams and p0:
        al = audio_align.align(path, path, start + first / rate, end, 2.0, 0.5, 1.0, p0)
        res["seamless"] = {"runs": al["runs"], "jumps": al["jumps"], "identical_anchors": al["identical"],
                           "matched": al["matched"], "anchors": al["anchors"]}
    if passes:
        snd = [p["sound_samples"] for p in passes]
        res["sound_samples_spread"] = int(max(snd) - min(snd))
    return res


def fmt_snr(v):
    if v is None:
        return "-"
    return "inf" if v == float("inf") else f"{v:.1f}"


def summary(r: dict) -> str:
    if r.get("silent"):
        return f"{r['file']}: silent"
    out = [f"{r['file']}: {r['rate']} Hz, region {r['region_s'][0]}-{r['region_s'][1]} s, coarse period "
           f"{r['coarse_period_s']} s (self-correlation {r['coarse_self_corr'] and round(r['coarse_self_corr'], 3)})"]
    for s in r["seams"]:
        out.append(f"  seam @ {s['t_s']} s: gap {s['gap_ms']} ms ({s['gap_samples']} samples), last 5 ms {s['last_5ms_db']} dBFS, "
                   f"first 5 ms {s['first_5ms_db']} dBFS, step {s['step_db']} dB")
    for p in r["passes"]:
        out.append(f"  pass {p['pass']} from {p['restart_s']} s: sound {p['sound_samples']} samples, period "
                   f"{p['period_samples']} samples ({p['period_s']} s), SNR vs previous {fmt_snr(p.get('snr_vs_previous_db'))} dB")
    if "sound_samples_spread" in r:
        out.append(f"  spread of the sound length between passes: {r['sound_samples_spread']} samples")
    others = [s for s in r["silences"] if all(abs(s["t_s"] - z["t_s"]) > 0.001 for z in r["seams"])]
    if others:
        out.append(f"  other silences >= gap: {others[:10]}")
    if "seamless" in r:
        sl = r["seamless"]
        out.append(f"  no gap at the loop point: self-alignment {sl['matched']}/{sl['anchors']} anchors, "
                   f"{sl['identical_anchors']} identical; runs {[(x['from'], x['to'], x['offset']) for x in sl['runs']]}")
    return "\n".join(out)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("wav", type=Path)
    p.add_argument("--compare", type=Path)
    p.add_argument("--start", type=float, default=0.0)
    p.add_argument("--end", type=float)
    p.add_argument("--start-b", type=float)
    p.add_argument("--end-b", type=float)
    p.add_argument("--min-period", type=float, default=8.0)
    p.add_argument("--max-period", type=float, default=240.0)
    p.add_argument("--gap-ms", type=float, default=20.0, help="shortest digital silence counted as a seam (ms)")
    p.add_argument("--json", action="store_true")
    a = p.parse_args()
    out = {"a": analyze(a.wav, a.start, a.end, a.min_period, a.max_period, a.gap_ms)}
    if a.compare:
        out["b"] = analyze(a.compare, a.start if a.start_b is None else a.start_b, a.end if a.end_b is None else a.end_b,
                           a.min_period, a.max_period, a.gap_ms)
        if out["a"].get("rate") != out["b"].get("rate"):
            print(f"error: the sample rates differ ({out['a'].get('rate')} and {out['b'].get('rate')} Hz): "
                  "lengths are compared in samples", file=sys.stderr)
            return 1
        pa, pb = out["a"].get("passes", []), out["b"].get("passes", [])
        out["pairs"] = [{"pass": i + 1, "sound_a": x["sound_samples"], "sound_b": y["sound_samples"],
                         "sound_diff": y["sound_samples"] - x["sound_samples"]} for i, (x, y) in enumerate(zip(pa, pb))]
        sa, sb = out["a"].get("seams", []), out["b"].get("seams", [])
        out["gaps_ms"] = {"a": [s["gap_ms"] for s in sa], "b": [s["gap_ms"] for s in sb]}
    if a.json:
        print(json.dumps(audio_align.strict(out), indent=1))
        return 0
    print(summary(out["a"]))
    if "b" in out:
        print(summary(out["b"]))
        for q in out["pairs"]:
            print(f"  pass {q['pass']}: sound length a {q['sound_a']} / b {q['sound_b']} samples, difference {q['sound_diff']:+d}")
        print(f"  gaps at the seams (ms): a {out['gaps_ms']['a']}  b {out['gaps_ms']['b']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
