"""Provenance of the outputs of tools/ghidra/analyze.py: the header they start with, and the check made by the tools
that read them.

Why: a Ghidra output is only worth something for the executable it came from. A PAL `functions.tsv` read with the US
DOL gives plausible, wrong addresses, and the error shows up an hour later (or never). So every output says what it
was made from, and a reader refuses a file that does not match what it was given.

Header: lines `# ghidra-provenance: key=value` at the very start of the file, before the line of column names. They
start with `#` like that line, so every parser of these files already skips them. Values come from
tools/ghidra/scripts/AttestImage.java, that is, from the program Ghidra really has open: `language` is the language
observed after import, and `map_sha256` is the map applied at import, which may differ from the map file on disk today
(`unknown`: imported with no map, or before the map hash was recorded).

Readers compare `version`, `dol_sha256` and `attest`. They do not compare the map: its names are not what they use
(function starts and sizes), and it changes with every batch of names.

A file with no header (made before this check) cannot be checked: readers say so on stderr and carry on, so that the
existing outputs stay usable until the next `tools/ghidra/analyze.py <game> --version <ID> export`.
"""

from __future__ import annotations

import hashlib
import os
import sys
from pathlib import Path

MARK = "# ghidra-provenance:"
SCHEMA = "1"
REQUIRED = ("version", "dol_sha256", "attest")

# Report of AttestImage.java -> header, in the order of the header. Everything else in the report stays in the report.
REPORT_FIELDS = ("dol_sha256", "map_sha256", "ghidra", "created_with", "extension", "extension_jar_sha256",
                 "language", "language_version", "compiler")

_hashes: dict[Path, str] = {}
_warned: set[Path] = set()


def sha256(path: Path) -> str:
    path = Path(path).resolve()
    if path not in _hashes:
        _hashes[path] = hashlib.sha256(path.read_bytes()).hexdigest()
    return _hashes[path]


def files_sha256(paths: list[Path]) -> str:
    """One hash for a set of files: their names and contents, in name order."""
    h = hashlib.sha256()
    for p in sorted(paths, key=lambda p: p.name):
        h.update(p.name.encode("utf-8"))
        h.update(p.read_bytes())
    return h.hexdigest()


# --- Writing (tools/ghidra/analyze.py) ---------------------------------------------------------------------------

def read_report(path: Path) -> dict | None:
    """Report of AttestImage.java: {key: value, "blocks": [fields...], "failures": [text...]}; None if there is none."""
    if not path.exists():
        return None
    report: dict = {"blocks": [], "failures": []}
    for line in path.read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition("\t")
        if key == "block":
            report["blocks"].append(value.split("\t"))
        elif key == "failure":
            report["failures"].append(value)
        else:
            report[key] = value
    return report


def header(report: dict, version: str, extra: dict | None = None) -> list[str]:
    fields = {"schema": SCHEMA, "version": version}
    fields.update({k: report.get(k, "unknown") for k in REPORT_FIELDS})
    fields.update(extra or {})
    fields["attest"] = report.get("result", "unknown")
    return [f"{MARK} {k}={v}" for k, v in fields.items()]


def stamp(path: Path, lines: list[str]) -> None:
    """Puts the header at the start of `path`, in place of any previous one, keeping the file's line ending."""
    data = path.read_bytes()
    eol = b"\r\n" if b"\r\n" in data[:4096] else b"\n"
    mark = MARK.encode("utf-8")
    while data.startswith(mark):
        data = data[data.index(b"\n") + 1:] if b"\n" in data else b""
    tmp = path.with_name(path.name + ".stamp")
    tmp.write_bytes(eol.join(s.encode("utf-8") for s in lines) + eol + data)
    os.replace(tmp, path)


# --- Reading (every tool that uses these files) ------------------------------------------------------------------

def read_header(path: Path) -> dict | None:
    """The fields of the header of `path`, or None if it has none."""
    fields = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith(MARK):
                break
            key, _, value = line[len(MARK):].strip().partition("=")
            fields[key] = value
    return fields or None


def mismatches(fields: dict, dol: Path | None, version: str | None) -> list[str]:
    """Why a header does not fit the DOL and the version a tool was given; empty if it fits."""
    if fields.get("schema") != SCHEMA:
        return [f"unknown header schema {fields.get('schema')}"]
    missing = [k for k in REQUIRED if k not in fields]
    if missing:
        return [f"incomplete header: no {', '.join(missing)}"]
    out = []
    if fields["attest"] != "pass":
        out.append(f"made from a program that did not pass attestation (attest={fields['attest']})")
    if version and fields["version"] != version:
        out.append(f"made for {fields['version']}, not {version}")
    if dol is not None and dol.exists() and fields["dol_sha256"] != sha256(dol):
        out.append(f"made from the DOL with SHA-256 {fields['dol_sha256']}, but {dol} has {sha256(dol)}")
    return out


def require(path: Path, dol: Path | None = None, version: str | None = None) -> None:
    """Refuses (exit) a file whose header does not fit `dol` and `version`; says so on stderr if it has no header."""
    path = Path(path)
    fields = read_header(path)
    if fields is None:
        if path not in _warned:
            _warned.add(path)
            print(f"warning: {path} has no provenance header (old output): it cannot be checked against "
                  f"{version or 'the DOL'}. Run tools/ghidra/analyze.py <game> --version <ID> export to get one.",
                  file=sys.stderr)
        return
    problems = mismatches(fields, Path(dol) if dol else None, version)
    if problems:
        sys.exit(f"refused: {path}: " + "; ".join(problems) +
                 "\nRe-export it for this version: tools/ghidra/analyze.py <game> --version <ID> export")
