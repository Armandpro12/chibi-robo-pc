"""A/B comparator of two runs of the same scenario (milestone 10: validation of each native replacement).

Usage:
  py -3 tools/compare_ab.py <reference_run> <candidate_run> [--out DIR] [--profile exact|realtime]
                            [--hw-zero all|DI,SI,...] [--ignore-watch ADDRESS,...] [tolerance options]
  py -3 tools/compare_ab.py <a.wav> <b.wav>        (audio only: duration, envelope, correlation, SNR)

`<..._run>` is a folder produced by `tools/run.py --scenario ...` (run.json, trace.log, cp-*.png, audio.wav).
Convention: A = reference (native reference run, tools/ab.py record), B = candidate. Report written to
`<out>/ab-report.md` and `ab-report.json` (default: the candidate folder). Exit code 0 = no regression,
1 = regression (at least one "failure" criterion), 2 = unusable data (different scenarios, missing run...).

Criteria (default values documented in `notes/validation.md`, calibrated by `tools/ab.py record`):
 1. Execution: exit code, requested stop, same scenario (fingerprint), every checkpoint captured.
 2. Checkpoints (presented frame number N of both runs): SSIM on the luma at the milestone 5 threshold (0.95)
    between the center captures; levels IDENTICAL (equal pixels) / CLOSE (>= 0.99) / ACCEPTABLE (>= 0.95) /
    SHIFTED (the center capture of one side looks like a neighboring capture of the other: offset in FRAMES
    reported, failure beyond --max-shift) / DIFFERENT. The checkpoint must exist on both sides (arrival). The VI
    field difference at the same frame (cadence) is reported for information.
    "soft" checks are compared and reported but never fail.
 3. Audio: duration, audio onset, offset (correlation of the RMS envelope over 20 ms windows, within ±0.5 s around
    the onset offset), global envelope correlation and per 5 s segment (absorbs pauses and time stretching),
    levels (RMS of the aligned span, peak), per-window deviations, activity; bit-exactness and SNR when
    possible.
    Tolerance profiles: `exact` (default; deterministic runs: cycle clock, identical result expected) and
    `realtime` (candidate paced by the PC, provisional values); each tolerance also has its own option.
 4. Counted hits (`[run.watch]` of game.toml, key `hits`) in the final report.
 5. Hardware access counter (range 0xCC000000, milestone 10): reported if it exists; with --hw-zero, the
    named subsystems (or all of them) must be 0 in the candidate, and the counter must exist.
 6. Trace health: unknown GX opcodes, invalid addresses, skipped frames (no worse than the
    reference), new [unhandled] messages (warning), report counters (informational).
 7. Saves left in the user folder (milestone 7; "saves/" files of the `user_files` list of run.json): same files
    and same sizes (failure otherwise), identical content (otherwise a warning with the number of differing bytes
    and, for a GCI file, the affected area); the preparation of a chained scenario (`user_seed_files`) is compared
    the same way. A run recorded before this list existed (no `user_files`) is not compared on this point.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import wave
from pathlib import Path

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import abtrace  # noqa: E402
import compare_images as ci  # noqa: E402

OK, WARN, FAIL, INFO = "OK", "WARN", "FAIL", "INFO"

DEFAULTS = {
    # images (milestone 5: SSIM >= 0.95)
    "ssim": 0.95, "ssim_strict": 0.99, "max_shift": 2,
    # audio
    "audio_duration_abs": 0.25, "audio_duration_rel": 0.005, "audio_max_lag": 0.06, "audio_env_corr_db": 0.98,
    "audio_env_corr_lin": 0.99, "audio_rms_db": 0.5, "audio_peak_db": 1.0, "audio_p95_db": 3.0,
    "audio_activity": 0.02, "audio_onset": 0.10, "audio_seg_corr": 0.98,
    # hits and counters
    "hits_abs": 3, "hits_rel": 0.002, "cadence_warn_rel": 0.05, "counter_warn_rel": 0.05,
}

# Subsystems of the hardware access counter (runtime/core/cpu/hw_counter.c, gcrt_hw_kind_name).
KNOWN_HW = {"cp", "pe", "vi", "pi", "mi", "dsp", "ar", "di", "si", "exi", "ai", "wgpipe", "efb", "other",
            "wgpipe_outside"}  # wgpipe_outside (step 10c): pipe writes outside a draw opened by GXBegin

# Tolerance profiles. "exact": deterministic reference and candidate (cycle clock, or a candidate whose time does
# not depend on the machine): the expected result is IDENTICAL, the slightest difference is suspicious.
# "realtime": candidate paced by the PC clock (after milestone 10b): slower boot, pauses (shader compilation),
# animations tied to real time. PROVISIONAL values taken from the "host clock" reruns of the emulated version
# (`notes/validation.md`, natural difference section); to recalibrate on the first native version paced by the PC, on an
# idle machine.
PROFILES = {
    "exact": {},
    "realtime": {
        "realtime": True, "ssim": 0.90, "ssim_strict": 0.95, "max_shift": 4,
        "audio_duration_abs": 1e9, "audio_max_lag": 10.0, "audio_onset": 10.0, "audio_env_corr_db": 0.0,
        "audio_env_corr_lin": 0.0, "audio_seg_corr": 0.90, "audio_rms_db": 1.5, "audio_p95_db": 6.0, "audio_activity": 0.10,
        "cadence_warn_rel": 1e9,
    },
}


# --------------------------------------------------------------------------------------------------
# Utilities
# --------------------------------------------------------------------------------------------------
class Check:
    """An evaluated criterion: name, status (OK/WARN/FAIL/INFO), readable detail."""

    def __init__(self, name: str, status: str, detail: str, data: dict | None = None):
        self.name, self.status, self.detail, self.data = name, status, detail, data or {}

    def to_dict(self) -> dict:
        return {"name": self.name, "status": self.status, "detail": self.detail, **({"data": self.data} if self.data else {})}


def load_run(path: Path) -> dict:
    rj = path / "run.json"
    if not rj.exists():
        raise SystemExit(f"run.json not found in {path} (run tools/run.py --scenario ...)")
    data = json.loads(rj.read_text(encoding="utf-8"))
    data["_dir"] = path
    trace = path / "trace.log"
    text = trace.read_text(encoding="utf-8", errors="replace") if trace.exists() else ""
    data["_parsed"] = abtrace.parse_trace(text) if text else None
    m = re.search(r"\[native\] subsystems: (.*)", text)
    data["_native"] = m.group(1).strip() if m else None   # subsystems announced by the candidate ("[native] subsystems" line), if any
    return data


def _fmt(v, nd=4):
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.{nd}f}"
    return str(v)


# --------------------------------------------------------------------------------------------------
# Images
# --------------------------------------------------------------------------------------------------
def image_metrics(pa: Path, pb: Path) -> dict:
    r = ci.compare_pair(pa, pb)
    out = {"ssim": float(r["ssim"]), "psnr_db": r["psnr_db"], "resized": bool(r["resized"]),
           "identical": False, "max_diff": None, "diff_fraction": None}
    a = np.asarray(Image.open(pa).convert("RGB"), dtype=np.int16)
    b = np.asarray(Image.open(pb).convert("RGB"), dtype=np.int16)
    if a.shape == b.shape:
        d = np.abs(a - b)
        out["max_diff"] = int(d.max())
        out["diff_fraction"] = float((d.max(axis=2) > 0).mean())
        out["identical"] = out["max_diff"] == 0
    return out


def save_diff(pa: Path, pb: Path, out: Path) -> None:
    la = ci.load_luma(pa)
    ib = Image.open(pb).convert("L")
    if (ib.height, ib.width) != la.shape:
        ib = ib.resize((la.shape[1], la.shape[0]), Image.BILINEAR)
    diff = np.clip(np.abs(la - np.asarray(ib, dtype=np.float64)) * 4.0, 0, 255).astype(np.uint8)
    out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(diff, mode="L").save(out)


def compare_checkpoints(a: dict, b: dict, tol: dict, out_dir: Path, all_diffs: bool) -> list[dict]:
    """A checkpoint = presented frame number N of both runs (scenario anchor: frames, not seconds).

    Shift (in frames) = frame number of the candidate - frame number of the reference for the same content; negative:
    the candidate gets there earlier in its game logic. Cadence difference = VI field of the candidate - VI field of
    the reference at the same frame (information: the time the game took, not its content)."""
    cps_a = {c["name"]: c for c in a.get("checkpoints", [])}
    cps_b = {c["name"]: c for c in b.get("checkpoints", [])}
    rows = []
    for chk in a["scenario"]["checks"]:
        name = chk["name"]
        row: dict = {"name": name, "frame": chk["frame"], "soft": bool(chk.get("soft")), "status": FAIL, "notes": []}
        ca, cb = cps_a.get(name), cps_b.get(name)
        if not ca or not ca.get("center"):
            row["notes"].append("capture missing on the reference side")
            rows.append(row)
            continue
        if not cb or not cb.get("center"):
            row["notes"].append("checkpoint not reached on the candidate side (capture missing)")
            rows.append(row)
            continue
        pa, pb = a["_dir"] / ca["center"], b["_dir"] / cb["center"]
        row["ref"] = {"image": ca["center_image"], "field": ca["center_field"], "file": ca["center"]}
        row["cand"] = {"image": cb["center_image"], "field": cb["center_field"], "file": cb["center"]}
        row["delta_field"] = cb["center_field"] - ca["center_field"]
        m = image_metrics(pa, pb)
        row.update({"ssim": m["ssim"], "psnr_db": m["psnr_db"], "identical": m["identical"], "max_diff": m["max_diff"],
                    "diff_fraction": m["diff_fraction"], "resized": m["resized"]})
        best = {"ssim": m["ssim"], "shift": 0, "how": "center"}
        if not m["identical"] and m["ssim"] < tol["ssim_strict"]:
            # Best match within the window: (1) candidate center against the reference window,
            # (2) reference center against the candidate window; on a tie, the smallest shift.
            for w in ca.get("window", []):
                if w["file"] == ca["center"]:
                    continue
                s = image_metrics(a["_dir"] / w["file"], pb)["ssim"]
                shift = cb["center_image"] - w["image"]
                if s > best["ssim"] + 1e-9 or (abs(s - best["ssim"]) <= 1e-9 and abs(shift) < abs(best["shift"])):
                    best = {"ssim": s, "shift": shift, "how": "candidate center ~ reference window"}
            for w in cb.get("window", []):
                if w["file"] == cb["center"]:
                    continue
                s = image_metrics(pa, b["_dir"] / w["file"])["ssim"]
                shift = w["image"] - ca["center_image"]
                if s > best["ssim"] + 1e-9 or (abs(s - best["ssim"]) <= 1e-9 and abs(shift) < abs(best["shift"])):
                    best = {"ssim": s, "shift": shift, "how": "reference center ~ candidate window"}
        row["best"] = best
        status = OK
        if m["identical"]:
            level = "IDENTICAL"
        elif m["ssim"] >= tol["ssim_strict"]:
            level = "CLOSE"
        elif m["ssim"] >= tol["ssim"]:
            level = "ACCEPTABLE"
            status = WARN
        elif best["ssim"] >= tol["ssim"] and best["shift"] != 0:
            level = f"SHIFTED ({best['shift']:+d} images)"
            if abs(best["shift"]) <= tol["max_shift"]:
                status = WARN
            else:
                status = FAIL
                row["notes"].append(f"shift {best['shift']:+d} images > {tol['max_shift']}")
        else:
            level = "DIFFERENT"
            status = FAIL
        row["level"] = level
        row["status"] = status
        if not m["identical"] and (all_diffs or status in (WARN, FAIL)):
            dpath = out_dir / "diffs" / f"ab-{name}.png"
            save_diff(pa, pb, dpath)
            row["diff_image"] = str(dpath.relative_to(out_dir))
        rows.append(row)
    return rows


# --------------------------------------------------------------------------------------------------
# Audio
# --------------------------------------------------------------------------------------------------
def read_wav(path: Path) -> tuple[int, np.ndarray]:
    """(rate, float64 samples [n, channels]) of an 8/16/24/32-bit PCM WAV."""
    with wave.open(str(path), "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64)
    elif width == 1:
        x = (np.frombuffer(raw, dtype=np.uint8).astype(np.float64) - 128.0) * 256.0
    elif width == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 65536.0
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 256.0
    else:
        raise SystemExit(f"{path}: unsupported sample width {width}")
    return rate, x[: (len(x) // ch) * ch].reshape(-1, ch)


def _to_db(x: np.ndarray, floor: float = -100.0) -> np.ndarray:
    return np.maximum(20.0 * np.log10(np.maximum(x, 1e-9) / 32768.0), floor)


def _envelope(x: np.ndarray, rate: int, win_s: float) -> np.ndarray:
    w = max(1, int(round(rate * win_s)))
    n = (len(x) // w) * w
    if n == 0:
        return np.zeros(0)
    p = (x[:n] ** 2).mean(axis=1).reshape(-1, w).mean(axis=1)
    return np.sqrt(p)


def _resample(x: np.ndarray, rate_from: int, rate_to: int) -> np.ndarray:
    if rate_from == rate_to:
        return x
    n_to = int(round(len(x) * rate_to / rate_from))
    t = np.linspace(0, len(x) - 1, n_to)
    return np.stack([np.interp(t, np.arange(len(x)), x[:, c]) for c in range(x.shape[1])], axis=1)


def _pearson(u: np.ndarray, v: np.ndarray) -> float:
    if len(u) < 2 or u.std() < 1e-12 or v.std() < 1e-12:
        return 1.0 if (len(u) and np.allclose(u, v)) else 0.0
    return float(np.corrcoef(u, v)[0, 1])


def _best_lag(ea: np.ndarray, eb: np.ndarray, lo: int, hi: int) -> int:
    """Offset (in windows) k in [lo, hi] such that eb[i + k] ~ ea[i]: maximum Pearson correlation."""
    best, best_k = -2.0, 0
    for k in range(lo, hi + 1):
        if k >= 0:
            u, v = ea[: len(ea) - k], eb[k:]
        else:
            u, v = ea[-k:], eb[: len(eb) + k]
        n = min(len(u), len(v))
        if n < 50:
            continue
        c = _pearson(u[:n], v[:n])
        if c > best + 1e-12 or (abs(c - best) <= 1e-12 and abs(k) < abs(best_k)):
            best, best_k = c, k
    return best_k


def _segment_match(ca: np.ndarray, cb: np.ndarray, lag: int, win_s: float, seg_s: float = 5.0,
                   span_s: float = 3.0) -> list[dict]:
    """Local alignment by segments of `seg_s` seconds: for each active segment of A, best position of B
    within ±`span_s` around the expected position (global `lag`). Absorbs the pauses and time stretching
    (execution paced by the PC) that a global offset cannot correct."""
    seg, span = int(round(seg_s / win_s)), int(round(span_s / win_s))
    out = []
    for i0 in range(0, len(ca) - seg + 1, seg):
        a = ca[i0:i0 + seg]
        if a.max() <= -60.0 and a.std() < 1e-6:
            continue  # silent segment of A: nothing to find
        best, best_off = -2.0, 0
        for off in range(-span, span + 1):
            j0 = i0 + lag + off
            if j0 < 0 or j0 + seg > len(cb):
                continue
            c = _pearson(a, cb[j0:j0 + seg])
            if c > best + 1e-12 or (abs(c - best) <= 1e-12 and abs(off) < abs(best_off)):
                best, best_off = c, off
        if best > -2.0:
            out.append({"t": round(i0 * win_s, 2), "corr": best, "offset_s": round(best_off * win_s, 3)})
    return out


def compare_audio(pa: Path, pb: Path, tol: dict, win_s: float = 0.02) -> dict:
    """Compares two WAVs. Returns a dictionary of measurements (see `audio_checks` for the verdicts).

    Alignment: audio onset (first window above -50 dBFS) then a search for the envelope offset within ±0.5 s
    around it; at the sample level (SNR) only if both files are aligned to the sample (deterministic
    execution)."""
    ra, xa = read_wav(pa)
    rb, xb = read_wav(pb)
    res: dict = {"rate_a": ra, "rate_b": rb, "channels_a": xa.shape[1], "channels_b": xb.shape[1],
                 "duration_a": len(xa) / ra, "duration_b": len(xb) / rb}
    if rb != ra:  # comparison at the rate of A (e.g. Dolphin reference at 32 kHz against 44.1 kHz)
        xb = _resample(xb, rb, ra)
    if xa.shape[1] != xb.shape[1]:
        m = min(xa.shape[1], xb.shape[1])
        xa, xb = xa[:, :m], xb[:, :m]
    ea, eb = _envelope(xa, ra, win_s), _envelope(xb, ra, win_s)
    da, db = _to_db(ea), _to_db(eb)
    res["peak_db_a"] = float(_to_db(np.abs(xa).max())) if len(xa) else -100.0
    res["peak_db_b"] = float(_to_db(np.abs(xb).max())) if len(xb) else -100.0
    res["silent_a"] = bool(len(xa) == 0 or np.abs(xa).max() < 1.0)
    res["silent_b"] = bool(len(xb) == 0 or np.abs(xb).max() < 1.0)

    def onset(d):
        idx = np.nonzero(d > -50.0)[0]
        return float(idx[0] * win_s) if len(idx) else None

    res["onset_a"], res["onset_b"] = onset(da), onset(db)
    res["onset_delta_s"] = (res["onset_b"] - res["onset_a"]) if res["onset_a"] is not None and res["onset_b"] is not None else None
    ca, cb = np.maximum(da, -80.0), np.maximum(db, -80.0)   # clamped dB: the background noise does not count
    coarse = int(round(res["onset_delta_s"] / win_s)) if res["onset_delta_s"] is not None else 0
    half = int(round(0.5 / win_s))
    lag = _best_lag(ca, cb, coarse - half, coarse + half) if min(len(ca), len(cb)) > 100 else 0
    res["lag_windows"], res["lag_s"] = lag, lag * win_s
    if lag >= 0:
        ua, ub = ca[: len(ca) - lag], cb[lag:]
        la, lb = ea[: len(ea) - lag], eb[lag:]
    else:
        ua, ub = ca[-lag:], cb[: len(cb) + lag]
        la, lb = ea[-lag:], eb[: len(eb) + lag]
    n = min(len(ua), len(ub))
    ua, ub, la, lb = ua[:n], ub[:n], la[:n], lb[:n]
    # RMS level over the common aligned span (extra silence at the end of one side does not count).
    res["rms_db_a"] = float(_to_db(np.sqrt((la ** 2).mean()))) if n else -100.0
    res["rms_db_b"] = float(_to_db(np.sqrt((lb ** 2).mean()))) if n else -100.0
    res["env_corr_db"] = _pearson(ua, ub)
    res["env_corr_lin"] = _pearson(la, lb)
    active = np.maximum(ua, ub) > -60.0
    res["active_windows"] = int(active.sum())
    if active.any():
        dev = np.abs(ua[active] - ub[active])
        res["dev_db_p50"], res["dev_db_p95"], res["dev_db_max"] = (float(np.percentile(dev, 50)),
                                                                   float(np.percentile(dev, 95)), float(dev.max()))
    else:
        res["dev_db_p50"] = res["dev_db_p95"] = res["dev_db_max"] = 0.0
    mismatch = ((ua > -50.0) & (ub < -70.0)) | ((ub > -50.0) & (ua < -70.0))
    res["activity_mismatch"] = float(mismatch.mean()) if n else 0.0
    # Local alignment by segments (robust to pauses and time stretching).
    segs = _segment_match(ca, cb, lag, win_s) if min(len(ca), len(cb)) > 600 else []
    res["segments"] = segs
    if segs:
        corrs = np.array([s["corr"] for s in segs])
        offs = np.array([s["offset_s"] for s in segs])
        res["seg_corr_median"] = float(np.median(corrs))
        res["seg_corr_p10"] = float(np.percentile(corrs, 10))
        res["seg_corr_min"] = float(corrs.min())
        res["seg_offset_span_s"] = float(offs.max() - offs.min())
    # Sample level: bit-exactness, otherwise SNR at the best sample offset around the envelope offset
    # (searched over a loud 2 s segment with an FFT).
    res["bit_exact"] = bool(rb == ra and xa.shape == xb.shape and np.array_equal(xa, xb))
    res["snr_db"] = None
    res["sample_lag"] = None
    if res["bit_exact"]:
        res["snr_db"] = float("inf")
        res["sample_lag"] = 0
    elif len(xa) > ra * 3 and len(xb) > ra * 3 and not res["silent_a"] and not res["silent_b"]:
        ma, mb = xa.mean(axis=1), xb.mean(axis=1)
        seg = int(2 * ra)
        step = max(1, seg // 2)
        energy = [(float((ma[i:i + seg] ** 2).sum()), i) for i in range(0, len(ma) - seg, step)]
        i0 = max(energy)[1]
        guess = int(round(lag * win_s * ra))
        span = int(0.1 * ra)
        lo, hi = max(0, i0 + guess - span), min(len(mb), i0 + guess + seg + span)
        ref, cand = ma[i0:i0 + seg], mb[lo:hi]
        if len(cand) >= len(ref):
            nfft = 1 << (len(cand) + len(ref)).bit_length()
            fa, fb = np.fft.rfft(cand, nfft), np.fft.rfft(ref, nfft)
            xc = np.fft.irfft(fa * np.conj(fb), nfft)[: len(cand) - len(ref) + 1]
            k = int(np.argmax(xc))
            slag = lo + k - i0
            res["sample_lag"] = slag
            if slag >= 0:
                u, v = xa[: len(xa) - slag], xb[slag:]
            else:
                u, v = xa[-slag:], xb[: len(xb) + slag]
            nn = min(len(u), len(v))
            u, v = u[:nn], v[:nn]
            err = float(((u - v) ** 2).sum())
            sig = float((u ** 2).sum())
            res["snr_db"] = float("inf") if err == 0 else (float(10 * np.log10(sig / err)) if sig > 0 else None)
    return res


def audio_checks(res: dict, tol: dict) -> list[Check]:
    checks: list[Check] = []
    dur_tol = max(tol["audio_duration_abs"], tol["audio_duration_rel"] * res["duration_a"])
    dd = res["duration_b"] - res["duration_a"]
    dur_status = OK if abs(dd) <= dur_tol else (INFO if tol.get("realtime") else FAIL)
    checks.append(Check("duration", dur_status,
                        f"A {res['duration_a']:.3f} s, B {res['duration_b']:.3f} s, difference {dd:+.3f} s "
                        + ("(realtime profile: information only)" if tol.get("realtime") else f"(tolerance ±{dur_tol:.3f} s)")))
    if res["silent_a"] or res["silent_b"]:
        both = res["silent_a"] and res["silent_b"]
        checks.append(Check("non-empty audio", WARN if both else FAIL,
                            f"silence: A {'yes' if res['silent_a'] else 'no'}, B {'yes' if res['silent_b'] else 'no'}"
                            + (" (both: nothing to compare)" if both else "")))
        return checks
    if res["bit_exact"]:
        checks.append(Check("bit-exact", OK, "both files have exactly the same samples"))
    if res.get("onset_delta_s") is not None:
        checks.append(Check("audio onset", OK if abs(res["onset_delta_s"]) <= tol["audio_onset"] else FAIL,
                            f"A {res['onset_a']:.2f} s, B {res['onset_b']:.2f} s, difference {res['onset_delta_s']:+.2f} s (±{tol['audio_onset']} s)"))
    residual = res["lag_s"] - (res["onset_delta_s"] or 0.0)
    lag_ok = abs(res["lag_s"]) <= tol["audio_max_lag"]
    checks.append(Check("time offset", OK if lag_ok else FAIL,
                        f"{res['lag_s'] * 1000:+.0f} ms in total (audio onset {(res['onset_delta_s'] or 0.0) * 1000:+.0f} ms, "
                        f"refinement {residual * 1000:+.0f} ms; 20 ms windows; tolerance ±{tol['audio_max_lag'] * 1000:.0f} ms)"
                        + (f", {res['sample_lag']:+d} samples at the fine level" if res.get("sample_lag") not in (None, 0) else "")))
    checks.append(Check("envelope (dB)", OK if res["env_corr_db"] >= tol["audio_env_corr_db"] else FAIL,
                        f"correlation {res['env_corr_db']:.4f} (>= {tol['audio_env_corr_db']})"))
    checks.append(Check("envelope (linear)", OK if res["env_corr_lin"] >= tol["audio_env_corr_lin"] else FAIL,
                        f"correlation {res['env_corr_lin']:.4f} (>= {tol['audio_env_corr_lin']})"))
    if "seg_corr_p10" in res:
        st = OK if res["seg_corr_p10"] >= tol["audio_seg_corr"] else FAIL
        checks.append(Check("envelope per 5 s segment", st,
                            f"{len(res['segments'])} segments, median correlation {res['seg_corr_median']:.4f}, 10th percentile "
                            f"{res['seg_corr_p10']:.4f}, min {res['seg_corr_min']:.4f} (10th percentile >= {tol['audio_seg_corr']}); "
                            f"pauses and time stretching: local offsets spread over {res['seg_offset_span_s']:.2f} s"))
    drms = res["rms_db_b"] - res["rms_db_a"]
    checks.append(Check("RMS level (aligned span)", OK if abs(drms) <= tol["audio_rms_db"] else FAIL,
                        f"A {res['rms_db_a']:.2f} dBFS, B {res['rms_db_b']:.2f} dBFS, difference {drms:+.2f} dB (±{tol['audio_rms_db']})"))
    dpk = res["peak_db_b"] - res["peak_db_a"]
    checks.append(Check("peak", OK if abs(dpk) <= tol["audio_peak_db"] else FAIL,
                        f"A {res['peak_db_a']:.2f} dBFS, B {res['peak_db_b']:.2f} dBFS, difference {dpk:+.2f} dB (±{tol['audio_peak_db']})"))
    checks.append(Check("per-window deviation", OK if res["dev_db_p95"] <= tol["audio_p95_db"] else FAIL,
                        f"p50 {res['dev_db_p50']:.2f} dB, p95 {res['dev_db_p95']:.2f} dB, max {res['dev_db_max']:.2f} dB over "
                        f"{res['active_windows']} active windows (p95 <= {tol['audio_p95_db']} dB)"))
    checks.append(Check("activity (silence/sound)", OK if res["activity_mismatch"] <= tol["audio_activity"] else FAIL,
                        f"{res['activity_mismatch'] * 100:.2f} % of windows where one side is active and the other silent (<= {tol['audio_activity'] * 100:.0f} %)"))
    if res.get("snr_db") is not None and not res["bit_exact"]:
        snr = res["snr_db"]
        checks.append(Check("sample SNR", INFO, "infinite" if snr == float("inf") else f"{snr:.1f} dB after alignment"))
    return checks


# --------------------------------------------------------------------------------------------------
# Hits, counters, hardware, health
# --------------------------------------------------------------------------------------------------
def hits_checks(a: dict, b: dict, tol: dict, ignore: set[str]) -> list[Check]:
    pa = (a["summary"]["final"] or {}).get("hits", {})
    pb = (b["summary"]["final"] or {}).get("hits", {})
    checks = []
    if not pa:
        return [Check("hits", INFO, "no hit counted on the reference side (empty [run.watch]?)")]
    for addr in sorted(set(pa) | set(pb)):
        va, vb = pa.get(addr), pb.get(addr)
        if addr in ignore:
            checks.append(Check(f"hits {addr}", INFO, f"A {va}, B {vb} (ignored: --ignore-watch)"))
            continue
        if va is None or vb is None:
            checks.append(Check(f"hits {addr}", FAIL, f"counted on one side only (A {va}, B {vb})"))
            continue
        limit = max(tol["hits_abs"], tol["hits_rel"] * va)
        checks.append(Check(f"hits {addr}", OK if abs(vb - va) <= limit else FAIL,
                            f"A {va}, B {vb}, difference {vb - va:+d} (tolerance ±{limit:.1f})"))
    return checks


def counter_checks(a: dict, b: dict, tol: dict) -> list[Check]:
    fa, fb = a["summary"]["final"] or {}, b["summary"]["final"] or {}
    checks = []
    # The scenario stops at the same presented frame: both runs must get there (verdict). The number of
    # VI fields elapsed to get there measures the cadence (the candidate is slower or faster): warning.
    va, vb = fa.get("images"), fb.get("images")
    if va is None or vb is None:
        checks.append(Check("presented images", INFO, f"A {va}, B {vb} (missing on one side)"))
    else:
        checks.append(Check("presented images", OK if abs(vb - va) <= 3 else FAIL,
                            f"A {va}, B {vb}, difference {vb - va:+d} (the scenario stops at the same frame: ±3)"))
    va, vb = fa.get("vi_fields"), fb.get("vi_fields")
    if va is None or vb is None:
        checks.append(Check("cadence (VI fields)", INFO, f"A {va}, B {vb} (missing on one side)"))
    else:
        rel = (vb - va) / max(1, va)
        checks.append(Check("cadence (VI fields)", INFO if abs(rel) <= tol["cadence_warn_rel"] else WARN,
                            f"A {va} fields, B {vb} fields for the same number of frames, difference {vb - va:+d} ({rel * 100:+.1f} %"
                            f"; > 0: the candidate takes longer)"))
    for key in ("vi_interrupts", "di", "si", "exi", "dsp", "ai", "disc_reads", "si_polls"):
        va, vb = fa.get(key), fb.get(key)
        if va is None and vb is None:
            continue
        if va is None or vb is None:
            checks.append(Check(f"counter {key}", INFO, f"A {va}, B {vb} (missing on one side: builds from before milestone 10 "
                                                         f"reported per-device counters that native builds do not have)"))
            continue
        rel = abs(vb - va) / max(1, va)
        if vb == 0 and va > 0:   # e.g. a reference from before milestone 10, where this device was still simulated
            checks.append(Check(f"counter {key}", INFO,
                                f"A {va}, B 0 (no activity reported by the candidate: expected against a build from "
                                f"before the native replacement of this device)"))
            continue
        # Activity counters: information only (what they count changed with each native replacement).
        checks.append(Check(f"counter {key}", INFO, f"A {va}, B {vb}, difference {vb - va:+d} ({rel * 100:.1f} %)"))
    ga, gb = a["summary"].get("gx") or {}, b["summary"].get("gx") or {}
    for key, label in (("unknown_opcodes", "unknown GX opcodes"), ("invalid_addresses", "invalid GX addresses"),
                       ("skipped_frames", "skipped frames")):
        va, vb = ga.get(key), gb.get(key)
        if va is None or vb is None:
            continue
        checks.append(Check(label, OK if vb <= va else FAIL, f"A {va}, B {vb} (the candidate must not do worse)"))
    for key in ("commands", "draws", "vertices", "xfb_copies"):
        va, vb = ga.get(key), gb.get(key)
        if va is None or vb is None:
            continue
        rel = abs(vb - va) / max(1, va)
        checks.append(Check(f"GX {key}", INFO if rel <= tol["counter_warn_rel"] else WARN,
                            f"A {va}, B {vb}, difference {vb - va:+d} ({rel * 100:.1f} %)"))
    ua, ub = a["summary"].get("unhandled") or {}, b["summary"].get("unhandled") or {}
    new = sorted(set(ub) - set(ua))
    if new:
        checks.append(Check("new [unhandled] messages", WARN, "; ".join(f"{k} (x{ub[k]})" for k in new)))
    elif ub or ua:
        checks.append(Check("[unhandled] messages", INFO, f"A {sum(ua.values())}, B {sum(ub.values())} (same categories)"))
    return checks


def hw_checks(a: dict, b: dict, hw_zero: str | None, hw_except: set[str]) -> list[Check]:
    ha, hb = a["summary"].get("hw_access"), b["summary"].get("hw_access")
    ta, tb = abtrace.hw_total(ha), abtrace.hw_total(hb)
    checks = [Check("hardware accesses (reference)", INFO,
                    "counter missing from the reference executable (expected: it does not have the counter)" if ha is None else
                    f"total {ta}; " + ", ".join(f"{k} {v}" for k, v in sorted(ha.items())), {"values": ha})]
    if hb is None:
        status = FAIL if hw_zero else INFO
        checks.append(Check("hardware accesses (candidate)", status,
                            'counter missing: the candidate executable writes no "hardware accesses" line in the trace '
                            '(expected format: notes/runtime.md, requests from the tester agent, 2026-10-10)'))
        return checks
    detail = f"total {tb}; " + ", ".join(f"{k} {v}" for k, v in sorted(hb.items()))
    if not hw_zero:
        checks.append(Check("hardware accesses (candidate)", INFO, detail, {"values": hb}))
        return checks
    wanted = None if hw_zero.lower() == "all" else {s.strip().lower() for s in hw_zero.split(",") if s.strip()}
    bad = {k: v for k, v in hb.items() if k != "total" and v != 0 and k not in hw_except and (wanted is None or k in wanted)}
    if wanted is not None:
        # The counter only lists the subsystems that were touched: a known but missing name counts as 0. A name the
        # counter does not know at all (typo) is reported.
        unknown = sorted(w for w in wanted if w not in KNOWN_HW)
        if unknown:
            checks.append(Check("hardware accesses: unknown subsystems", WARN,
                                f"{', '.join(unknown)}: name unknown to the counter (names: {', '.join(sorted(KNOWN_HW))})"))
    if "total" in hb and wanted is None and not hw_except and hb["total"] != 0 and not bad:
        bad = {"total": hb["total"]}
    checks.append(Check("hardware accesses (candidate)", FAIL if bad else OK,
                        (f"NON-ZERO: {', '.join(f'{k}={v}' for k, v in sorted(bad.items()))}" if bad else
                         f"zero for {'all subsystems' if wanted is None else ', '.join(sorted(wanted))}")
                        + f" ({detail})", {"values": hb, "nonzero": bad}))
    return checks


SAVE_PREFIX = "saves/"
GCI_HEADER = 0x40      # directory entry of a GCI file (Dolphin format); the modification time is at 0x28
GCI_TIME = (0x28, 0x2C)


def _user_file(run: dict, rel: str) -> Path | None:
    """File of the user folder of a run: `<run>/user` (the default location of run.py, valid even if the run folder
    was copied or moved), otherwise the path recorded in run.json (launched with --user-dir elsewhere)."""
    for base in (run["_dir"] / "user", Path(run["user_dir"]) if run.get("user_dir") else None):
        if base is not None and (base / rel).is_file():
            return base / rel
    return None


def _diff_summary(pa: Path | None, pb: Path | None) -> str:
    if pa is None or pb is None:
        return "content not read (file not found)"
    da, db = pa.read_bytes(), pb.read_bytes()
    n = min(len(da), len(db))
    pos = [i for i in range(n) if da[i] != db[i]]
    if not pos:
        return "identical content"
    head = [i for i in pos if i < GCI_HEADER]
    body = [i for i in pos if i >= GCI_HEADER]
    parts = [f"{len(pos)} bytes differ (first at 0x{pos[0]:X})"]
    if head and not body and all(GCI_TIME[0] <= i < GCI_TIME[1] for i in head):
        parts.append("only the modification time of the GCI header (0x28-0x2B) differs")
    elif head:
        parts.append(f"{len(head)} in the GCI header (0x00-0x3F)")
    if body and head:
        parts.append(f"{len(body)} in the data")
    return ", ".join(parts)


def saves_checks(a: dict, b: dict) -> list[Check]:
    """Saves left (and preparation): `saves/*` files of the `user_files` list of run.json."""
    fa, fb = a.get("user_files"), b.get("user_files")
    if fa is None or fb is None:
        return []        # run recorded before the list existed: nothing to compare
    def only_saves(files):
        return {f["path"]: f for f in files if f["path"].startswith(SAVE_PREFIX)}
    sa, sb = only_saves(fa), only_saves(fb)
    checks: list[Check] = []
    seed_a, seed_b = a.get("user_seed_files"), b.get("user_seed_files")
    if seed_a is not None or seed_b is not None:
        ha = {f["path"]: f["sha1"] for f in (seed_a or []) if f["path"].startswith(SAVE_PREFIX)}
        hb = {f["path"]: f["sha1"] for f in (seed_b or []) if f["path"].startswith(SAVE_PREFIX)}
        if seed_a is None or seed_b is None:
            checks.append(Check("preparation (starting saves)", WARN,
                                f"on one side only (A {'yes' if seed_a is not None else 'no'}, B {'yes' if seed_b is not None else 'no'})"))
        elif ha == hb:
            checks.append(Check("preparation (starting saves)", OK,
                                f"identical on both sides ({len(ha)} file(s): {', '.join(sorted(ha)) or 'none'})"))
        else:
            diff = sorted(set(ha) ^ set(hb) | {k for k in set(ha) & set(hb) if ha[k] != hb[k]})
            checks.append(Check("preparation (starting saves)", WARN,
                                f"different: {', '.join(diff)} (the two runs do not start from the same state)"))
    if not sa and not sb:
        return checks
    names_a, names_b = set(sa), set(sb)
    if names_a != names_b:
        checks.append(Check("saves left", FAIL,
                            f"different files: A {sorted(names_a) or 'none'}, B {sorted(names_b) or 'none'}"))
        return checks
    for name in sorted(names_a):
        x, y = sa[name], sb[name]
        if x["size"] != y["size"]:
            checks.append(Check(f"save {name}", FAIL, f"different size: A {x['size']} bytes, B {y['size']} bytes"))
        elif x["sha1"] == y["sha1"]:
            checks.append(Check(f"save {name}", OK, f"identical ({x['size']} bytes, sha1 {x['sha1'][:10]})"))
        else:
            checks.append(Check(f"save {name}", WARN,
                                f"same size ({x['size']} bytes), {_diff_summary(_user_file(a, name), _user_file(b, name))}"))
    return checks


def _rtc_text(v: str | None) -> str:
    if v is None or not str(v).isdigit():
        return "not fixed" if v is None else str(v)
    import datetime as dt
    return f"{v} s ({(dt.datetime(2000, 1, 1) + dt.timedelta(seconds=int(v))).isoformat(sep=' ')})"


def clock_checks(a: dict, b: dict) -> list[Check]:
    """Console clock (GCRT_RTC) of both runs: the origin of the console time base since step 10b (like Dolphin). It
    changes what the game does or shows without any regression (save dates, "Load Game" cursor: notes/validation.md
    § 10b): it is therefore reported, and a disagreement is a warning, never a silent failure."""
    ra, rb = (a.get("environment") or {}).get("GCRT_RTC"), (b.get("environment") or {}).get("GCRT_RTC")
    if ra is not None and ra == rb:
        return [Check("console clock (GCRT_RTC)", INFO,
                      f'same time base origin on both sides: {_rtc_text(ra)}: save dates and "Load Game" cursor identical')]
    return [Check("console clock (GCRT_RTC)", WARN,
                  f"different time base origin: A {_rtc_text(ra)}, B {_rtc_text(rb)}: save dates (GCI header) and "
                  '"Load Game" cursor may differ without a regression')]


def run_checks(a: dict, b: dict) -> list[Check]:
    checks = []
    for tag, r in (("reference", a), ("candidate", b)):
        ok = r["exit_code"] == 0 or (r["exit_code"] == 2 and "stop requested" in (r.get("stop") or ""))
        stop = r.get("stop") or "no stop line"
        checks.append(Check(f"execution ({tag})", OK if ok else FAIL,
                            f"code {r['exit_code']}, {stop}, {r['wall_seconds']} s"))
    sa, sb = a["scenario"], b["scenario"]
    same = sa["sha1"] == sb["sha1"]
    checks.append(Check("same scenario", OK if same else FAIL,
                        f"{sa['name']} ({sa['sha1'][:8]}) / {sb['name']} ({sb['sha1'][:8]})"))
    fa = (a.get("environment") or {}).get("GCRT_FORCE_ARG", "")
    fb = (b.get("environment") or {}).get("GCRT_FORCE_ARG", "")
    if fa and fa == fb:
        checks.append(Check("forced values (determinism)", INFO, f"identical on both sides: {fa}"))
    elif not fa and not fb:
        checks.append(Check("forced values (determinism)", WARN,
                            "neither run fixes the random seed (GCRT_FORCE_ARG, game.toml [run.force]): "
                            "the random scenery of the title screen may differ without a regression"))
    else:
        checks.append(Check("forced values (determinism)", WARN,
                            f'different: A "{fa}", B "{fb}": runs not strictly comparable'))
    checks += clock_checks(a, b)
    for tag, r in (("reference", a), ("candidate", b)):
        if r.get("_native"):
            checks.append(Check(f"subsystems ({tag})", INFO, r["_native"]))
    checks.append(Check("executables", INFO,
                        f"A {Path(a['exe']).parent.parent.name} sha1 {a['exe_info']['sha1'][:10]}; "
                        f"B {Path(b['exe']).parent.parent.name} sha1 {b['exe_info']['sha1'][:10]}"))
    return checks


# --------------------------------------------------------------------------------------------------
# Report
# --------------------------------------------------------------------------------------------------
def worst(statuses: list[str]) -> str:
    return FAIL if FAIL in statuses else WARN if WARN in statuses else OK


def render_md(report: dict) -> str:
    L = [f"# A/B comparison: {report['scenario']}", ""]
    L.append(f"- Reference (A): `{report['ref_run']}`")
    L.append(f"- Candidate (B): `{report['cand_run']}`")
    L.append(f"- **Verdict: {'REGRESSION' if report['regression'] else 'NO REGRESSION'}** "
             f"({report['counts'][FAIL]} failure(s), {report['counts'][WARN]} warning(s))")
    if report["failures"]:
        L.append("- Failures: " + "; ".join(report["failures"]))
    L += ["", "## Execution", ""]
    for c in report["sections"]["execution"]:
        L.append(f"- [{c['status']}] **{c['name']}**: {c['detail']}")
    L += ["", "## Checkpoints", "",
          "| checkpoint | frame | VI field A/B | SSIM | pixels | level | best (shift in frames) | status |",
          "|---|---|---|---|---|---|---|---|"]
    for r in report["sections"]["checkpoints"]:
        if "ssim" not in r:
            L.append(f"| {r['name']}{' (info)' if r['soft'] else ''} | {r['frame']} | - | - | - | - | - | **{r['status']}** : {'; '.join(r['notes'])} |")
            continue
        px = "identical" if r["identical"] else (f"max {r['max_diff']}, {r['diff_fraction'] * 100:.2f} %" if r["max_diff"] is not None else "different sizes")
        best = r.get("best", {})
        bs = f"{best.get('ssim', 0):.4f} ({best.get('shift', 0):+d})" if best.get("how") != "center" else "-"
        L.append(f"| {r['name']}{' (info)' if r['soft'] else ''} | {r['frame']} | {r['ref']['field']}/{r['cand']['field']} "
                 f"({r['delta_field']:+d}) | {r['ssim']:.4f} | {px} | {r['level']} | {bs} | {r['status']}"
                 + (f" ({'; '.join(r['notes'])})" if r["notes"] else "") + (" — informational" if r["soft"] else "") + " |")
    L += ["", "## Audio", ""]
    for c in report["sections"]["audio"]:
        L.append(f"- [{c['status']}] **{c['name']}**: {c['detail']}")
    L += ["", "## Counted hits and counters", ""]
    for c in report["sections"]["counters"]:
        L.append(f"- [{c['status']}] **{c['name']}**: {c['detail']}")
    L += ["", "## Hardware accesses (0xCC000000)", ""]
    for c in report["sections"]["hardware"]:
        L.append(f"- [{c['status']}] **{c['name']}**: {c['detail']}")
    if report["sections"].get("saves"):
        L += ["", "## Saves left in the user folder", ""]
        for c in report["sections"]["saves"]:
            L.append(f"- [{c['status']}] **{c['name']}**: {c['detail']}")
    L += ["", "## Tolerances used", "", "```json", json.dumps(report["tolerances"], indent=1, ensure_ascii=False), "```", ""]
    return "\n".join(L)


def compare_runs(run_a: Path, run_b: Path, out: Path, tol: dict, hw_zero: str | None, hw_except: set[str],
                 ignore_watch: set[str], all_diffs: bool = False) -> dict:
    a, b = load_run(run_a), load_run(run_b)
    if "scenario" not in a or "scenario" not in b:
        raise SystemExit('Both runs must come from tools/run.py --scenario (run.json without "scenario")')
    out.mkdir(parents=True, exist_ok=True)
    sections: dict[str, list] = {}
    exec_checks = run_checks(a, b)
    sections["execution"] = [c.to_dict() for c in exec_checks]
    cp_rows = compare_checkpoints(a, b, tol, out, all_diffs)
    sections["checkpoints"] = cp_rows
    wa, wb = run_a / "audio.wav", run_b / "audio.wav"
    audio: list[Check] = []
    audio_res = None
    if wa.exists() and wb.exists():
        audio_res = compare_audio(wa, wb, tol)
        audio = audio_checks(audio_res, tol)
    elif wa.exists() and not wb.exists():
        audio = [Check("audio", FAIL, "audio.wav missing on the candidate side")]
    else:
        audio = [Check("audio", INFO, "no audio.wav on the reference side")]
    sections["audio"] = [c.to_dict() for c in audio]
    counters = hits_checks(a, b, tol, ignore_watch) + counter_checks(a, b, tol)
    sections["counters"] = [c.to_dict() for c in counters]
    hw = hw_checks(a, b, hw_zero, hw_except)
    sections["hardware"] = [c.to_dict() for c in hw]
    saves = saves_checks(a, b)
    sections["saves"] = [c.to_dict() for c in saves]

    statuses, failures = [], []
    for c in exec_checks + audio + counters + hw + saves:
        statuses.append(c.status)
        if c.status == FAIL:
            failures.append(f"{c.name}")
    for r in cp_rows:
        if r["soft"]:
            continue
        statuses.append(r["status"])
        if r["status"] == FAIL:
            failures.append(f"check {r['name']}")
    counts = {FAIL: statuses.count(FAIL), WARN: statuses.count(WARN)}
    report = {
        "scenario": a["scenario"]["name"], "ref_run": str(run_a), "cand_run": str(run_b),
        "regression": counts[FAIL] > 0, "counts": counts, "failures": failures, "tolerances": tol,
        "sections": sections, "audio": audio_res,
    }
    (out / "ab-report.json").write_text(json.dumps(report, indent=2, ensure_ascii=False, default=str), encoding="utf-8")
    (out / "ab-report.md").write_text(render_md(report), encoding="utf-8")
    return report


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("ref", type=Path, help="reference run (or WAV)")
    p.add_argument("cand", type=Path, help="candidate run (or WAV)")
    p.add_argument("--out", type=Path, help="report folder (default: the candidate folder)")
    p.add_argument("--hw-zero", metavar="all|DI,SI,...",
                   help="the hardware access counter of the candidate must be zero (all subsystems or the named ones)")
    p.add_argument("--hw-except", default="", help="subsystems allowed to be non-zero with --hw-zero all (e.g. wgp: write-gather pipe vertices)")
    p.add_argument("--ignore-watch", default="", help="[run.watch] addresses not to compare (function replaced by native code)")
    p.add_argument("--all-diffs", action="store_true", help="write a difference image for every checkpoint")
    p.add_argument("--json", action="store_true", help="print only the JSON")
    p.add_argument("--profile", choices=sorted(PROFILES), default="exact",
                   help="set of tolerances: exact (default, deterministic runs) or realtime (candidate paced by the PC)")
    for k, v in DEFAULTS.items():
        p.add_argument("--" + k.replace("_", "-"), type=type(v), default=None,
                       help=f"tolerance (default {v}, realtime profile: {PROFILES['realtime'].get(k, v)})")
    args = p.parse_args()
    tol = dict(DEFAULTS)
    tol.update(PROFILES[args.profile])
    for k in DEFAULTS:  # an explicit option wins over the profile
        if getattr(args, k) is not None:
            tol[k] = type(DEFAULTS[k])(getattr(args, k))
    tol["profile"] = args.profile

    if args.ref.suffix.lower() == ".wav" and args.cand.suffix.lower() == ".wav":
        res = compare_audio(args.ref, args.cand, tol)
        checks = audio_checks(res, tol)
        for c in checks:
            print(f"[{c.status}] {c.name}: {c.detail}")
        if args.out:
            args.out.mkdir(parents=True, exist_ok=True)
            (args.out / "audio-report.json").write_text(json.dumps(res, indent=2, default=str), encoding="utf-8")
        return 1 if any(c.status == FAIL for c in checks) else 0

    out = args.out or args.cand
    report = compare_runs(args.ref, args.cand, out, tol, args.hw_zero,
                          {s.strip().lower() for s in args.hw_except.split(",") if s.strip()},
                          {s.strip().upper() for s in args.ignore_watch.split(",") if s.strip()}, args.all_diffs)
    if args.json:
        print(json.dumps(report, indent=2, ensure_ascii=False, default=str))
    else:
        print((out / "ab-report.md").read_text(encoding="utf-8"))
        print(f"Report: {out / 'ab-report.md'} / {out / 'ab-report.json'}")
    return 1 if report["regression"] else 0


if __name__ == "__main__":
    sys.exit(main())
