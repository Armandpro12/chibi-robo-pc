"""Reads the `trace.log` of a run (tools/run.py) for the A/B bench: captures, reports, counters.

Tolerant by design: the format of the report lines changes over milestone 10 (the DI/SI/EXI counters
disappear with the emulated hardware). Each field is therefore looked up independently and only reported if
present; `compare_ab.py` only compares what exists on both sides.

Expected format for the hardware access counter (milestone 10, provided by the executable, see
`notes/runtime.md`, requests from the tester agent, 2026-10-10): a trace line containing "hardware accesses"
followed by `NAME n` or `NAME=n` pairs, for example:
    [ 61.500] [platform] hardware accesses (0xCC00xxxx) report: total 123456; CP=0/25 PE=4153/4157 WGPIPE=0/283875871
The last such line in the file is authoritative (final report). `run.json["summary"]["hw_access"]` is its copy.
"""

from __future__ import annotations

import re
from pathlib import Path

CAPTURE_RE = re.compile(r"capture requested \(frame (\d+), VI field (\d+)\)\s*:\s*(.+\.png)\s*$")
REPORT_HEAD_RE = re.compile(r"^\[\s*([\d.]+)\]\s*\[platform\]\s*(report|FINAL REPORT)\s+t=([\d.]+)\s*s")
HITS_RE = re.compile(r"hits\s*:(.*)$")
WATCH_RE = re.compile(r"\b([0-9A-Fa-f]{8})=(\d+)")
GX_RE = re.compile(r"\[platform\] GX: (\d+) commands")
UNHANDLED_RE = re.compile(r"\[unhandled\]\s*(.*)$")
HW_LINE_RE = re.compile(r"\[platform\] hardware accesses\b")
TOTAL_RE = re.compile(r":\s*(\d+) total\b")
BLOCK_RE = re.compile(r"\b([A-Z][A-Z_]{1,15})\s+reads\s+(\d+)\s+writes\s+(\d+)")
RW_RE = re.compile(r"\b([A-Za-z_]\w*)=(\d+)/(\d+)\b")
PAIR_RE = re.compile(r"([A-Za-z_][\w./-]*(?: [A-Za-z_][\w./-]*)?)\s*[= ]\s*(\d+)\b")
FIELD_RES = {
    "images": re.compile(r"images (\d+)"),
    "vi_fields": re.compile(r"VI fields (\d+)"),
    "vi_interrupts": re.compile(r"VI interrupts (\d+)"),
    "di": re.compile(r"\bDI (\d+)"),
    "si": re.compile(r"\bSI (\d+)"),
    "exi": re.compile(r"\bEXI (\d+)"),
    "dsp": re.compile(r"\bDSP (\d+)"),
    "ai": re.compile(r"\bAI (\d+)"),
    "disc_reads": re.compile(r"disc reads (\d+)"),
    "si_polls": re.compile(r"SI polls (\d+)"),
    "blocks": re.compile(r"blocks (\d+)"),
}
GX_FIELDS = {
    "commands": r"(\d+) commands", "draws": r"(\d+) draws", "vertices": r"(\d+) vertices",
    "display_lists": r"(\d+) display lists", "xfb_copies": r"(\d+) XFB copies", "texture_copies": r"(\d+) texture copies",
    "unknown_opcodes": r"(\d+) unknown opcodes", "invalid_addresses": r"(\d+) invalid addresses",
    "skipped_frames": r"skipped frames (\d+)", "captures_written": r"captures written (\d+)",
    "filtered_frames": r"filtered frames \(display copy\): (\d+)",
}


def parse_hw_access(line: str) -> dict[str, int]:
    """`NAME n` pairs of a "hardware accesses" line. The text before the colon of the label is ignored."""
    m = HW_LINE_RE.search(line)
    tail = line[m.end():] if m else line
    colon = tail.find(":")
    tail = tail[colon + 1:] if colon >= 0 else tail
    out: dict[str, int] = {}
    # NAME=reads/writes (gcrt_hw_count_summary): sum of both.
    for name, reads, writes in RW_RE.findall(tail):
        out[name.lower()] = int(reads) + int(writes)
    tail = RW_RE.sub(" ", tail)
    for name, value in PAIR_RE.findall(tail):
        key = name.strip().lower().replace(" ", "_")
        out[key] = (out[key] + int(value)) if key in out else int(value)
    return out


def parse_trace(text: str) -> dict:
    """Summary of a `trace.log`. Keys: captures [{file, image, field}], reports [{t, final, ...fields}], final
    {...fields, hits{address: n}}, gx {...}, hw_access {name: n}|None, unhandled {normalized message: n},
    stop_lines [...]."""
    captures: list[dict] = []
    reports: list[dict] = []
    gx: dict[str, int] = {}
    hw: dict[str, int] | None = None
    unhandled: dict[str, int] = {}
    stop_lines: list[str] = []
    for line in text.splitlines():
        m = CAPTURE_RE.search(line)
        if m:
            captures.append({"file": Path(m.group(3).strip()).name, "image": int(m.group(1)), "field": int(m.group(2))})
            continue
        m = REPORT_HEAD_RE.match(line)
        if m:
            entry: dict = {"t": float(m.group(3)), "final": m.group(2) == "FINAL REPORT"}
            for key, rx in FIELD_RES.items():
                mm = rx.search(line)
                if mm:
                    entry[key] = int(mm.group(1))
            mp = HITS_RE.search(line)
            entry["hits"] = {a.upper(): int(n) for a, n in WATCH_RE.findall(mp.group(1))} if mp else {}
            reports.append(entry)
            continue
        if GX_RE.search(line):
            gx = {}
            for key, pat in GX_FIELDS.items():
                mm = re.search(pat, line)
                if mm:
                    gx[key] = int(mm.group(1))
            continue
        if HW_LINE_RE.search(line):
            mt = TOTAL_RE.search(line)
            # Format of the readable report of gcrt_hw_count_report: "...: N total", then one line per subsystem.
            hw = {"total": int(mt.group(1))} if mt else parse_hw_access(line)
            continue
        mb = BLOCK_RE.search(line)
        if mb and hw is not None:
            hw[mb.group(1).lower()] = int(mb.group(2)) + int(mb.group(3))
            continue
        m = UNHANDLED_RE.search(line)
        if m:
            key = re.sub(r"0x[0-9A-Fa-f]+|\d+", "#", m.group(1)).strip()
            unhandled[key] = unhandled.get(key, 0) + 1
            continue
        if "stop requested" in line or "stop:" in line.lower():
            stop_lines.append(line.strip())
    final = next((b for b in reversed(reports) if b["final"]), reports[-1] if reports else {})
    return {"captures": captures, "reports": reports, "final": final, "gx": gx, "hw_access": hw,
            "unhandled": unhandled, "stop_lines": stop_lines}


def hw_total(hw: dict[str, int] | None) -> int | None:
    """Total of the hardware accesses: the `total` field if it exists, otherwise the sum of the other fields."""
    if hw is None:
        return None
    if "total" in hw:
        return hw["total"]
    return sum(hw.values())
