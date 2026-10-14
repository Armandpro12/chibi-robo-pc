"""Test scenarios: inputs and checkpoints anchored on PRESENTED FRAMES (`.scn` format).

Why presented frames (EFB-to-XFB copies, numbered from 1 like Dolphin's `framedump_N` and `GCRT_CAPTURE_FRAMES`)
rather than seconds or VI fields: the game advances frame by frame; its logical state at frame N depends neither on
the speed of the machine nor on the chosen clock (guest time in cycles, or the PC's clock).
Measured on the emulated version (notes/validation.md): at a given VI field, the number of frames already presented
varies by several dozen from one launch to the next as soon as the clock follows the PC (slow boot, shader
compilation); an input anchored on a VI field then lands on another logical moment and the animated title screen is
no longer comparable. Anchored on the frame, the same input gives the same game state. The VI field of each frame
is a MEASUREMENT (reported by the comparator: cadence difference), not an anchor.

No game data in a scenario: only buttons, frame numbers and names.

Format (one directive per line, `#` starts a comment up to the end of the line):

    name <identifier>                    optional, default: file name without extension
    description <free text>              optional
    version <ID>                         optional: disc on which the frames were recorded; a scenario that
                                         declares it only applies to that version (see "Several versions")
    window <n>                           optional: half-width (in frames) of the capture window around
                                         each checkpoint (default 4, 0 = no window)
    input <frame> <BUTTONS> [<hold>]     BUTTONS = A, B, X, Y, Z, L, R, START, UP, DOWN, LEFT, RIGHT
                                         joined by "+"; hold in frames (default 10)
    check <frame> <name> [soft]          capture of presented frame number <frame>;
                                         "soft": compared and reported but never fails (screen whose
                                         timing depends on simulated durations, e.g. a loading bar)
    end <frame>                          stops the scenario when this frame is presented (mandatory)
    env <GCRT_KEY>=<value>               optional, repeatable: environment variable of the executable specific to
                                         the scenario (e.g. `env GCRT_CARD=absent`: port A empty). Only GCRT_*
                                         variables are allowed; a value already set by the caller wins.
    after <scenario>                     optional: preparation. The scenario assumes the user folder
                                         (saves) as <scenario> left it: `tools/ab.py` gives it a copy
                                         (`tools/run.py --user-seed`), `tools/run.py` alone warns if it did not get one

Fingerprint of a scenario (`sha1`): computed on its interpreted content (name, version, window, inputs,
checkpoints, end, variables, preparation), not on the text of the file: comments, the description, spacing and
line endings do not enter it. The comparator only compares two runs with the same fingerprint.

Input convention: `input N B D` is held for every controller read that precedes the presentation of frames
N, N+1, ..., N+D-1 (the controller read belongs to the frame being prepared). An executable that can replay these
scenarios provides `GCRT_INPUT_FRAMES` ("N:BUTTONS:hold" separated by ";"), `GCRT_STOP_FRAME` and
`GCRT_CAPTURE_FRAMES` (notes/runtime.md, requests from the tester agent, 2026-10-10). `window W` adds the captures
of frames N-W ... N+W: the comparator (`tools/compare_ab.py`) looks for the best match within this window if the
two versions do not present exactly the same content at frame N, and reports the offset.

Several versions of the disc (regions, revisions). The screens, hence the frame numbers, differ from one version
to another (e.g. a version without the refresh-rate prompt or the language choice): the same logical scenario is
written once per version. Layout in `games/<game>/scenarios/`:

    <ID>/<name>.scn    scenario specific to version <ID> (takes precedence)
    <name>.scn         fallback scenario: applies to the version it declares with `version <ID>`, to all of them if
                       it declares none

The same checkpoint name in two versions designates the same logical screen. The folder of a version can only
contain a scenario of that version (`version` absent or equal to <ID>). The comparator only compares runs of the
same scenario (same fingerprint), hence always of the same version.
"""

from __future__ import annotations

import hashlib
import json
import re
from dataclasses import dataclass, field
from pathlib import Path

BUTTONS = ("A", "B", "X", "Y", "Z", "L", "R", "START", "UP", "DOWN", "LEFT", "RIGHT")
DEFAULT_HOLD = 10
DEFAULT_WINDOW = 4
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]*$")
_KEYWORDS = {"name", "description", "version", "window", "input", "check", "end", "env", "after"}
SOFT_FLAG = "soft"

ENV_RE = re.compile(r"^GCRT_[A-Z0-9_]+$")


class ScenarioError(ValueError):
    pass


@dataclass
class Input:
    frame: int
    buttons: str
    hold: int = DEFAULT_HOLD


@dataclass
class Check:
    frame: int
    name: str
    soft: bool = False


@dataclass
class Scenario:
    name: str
    path: Path | None = None
    sha1: str = ""
    description: str = ""
    version: str = ""
    window: int = DEFAULT_WINDOW
    inputs: list[Input] = field(default_factory=list)
    checks: list[Check] = field(default_factory=list)
    end: int = 0
    env: dict[str, str] = field(default_factory=dict)
    after: str = ""   # scenario whose user folder serves as the starting point (chaining, tools/ab.py)

    # -- conversions to the executable's settings ---------------------------------------------
    def capture_frames(self) -> list[int]:
        """Frames to capture (centers + windows), sorted, without duplicates, >= 1."""
        wanted: set[int] = set()
        for c in self.checks:
            for off in range(-self.window, self.window + 1):
                wanted.add(max(1, c.frame + off))
        return sorted(wanted)

    def input_spec_frames(self) -> str:
        """GCRT_INPUT_FRAMES: `frame:BUTTONS:hold_in_frames` separated by `;`."""
        return ";".join(f"{i.frame}:{i.buttons}:{i.hold}" for i in self.inputs)

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "file": str(self.path) if self.path else None,
            "sha1": self.sha1,
            "version": self.version,
            "window": self.window,
            "end": self.end,
            "inputs": [{"frame": i.frame, "buttons": i.buttons, "hold": i.hold} for i in self.inputs],
            "checks": [{"frame": c.frame, "name": c.name, "soft": c.soft} for c in self.checks],
            "env": dict(self.env),
            "after": self.after,
        }

    def fingerprint(self) -> str:
        """SHA-1 of the interpreted content (see the module docstring): comments, description, spacing and line endings
        do not change it."""
        content = {
            "name": self.name, "version": self.version, "window": self.window, "end": self.end,
            "inputs": [[i.frame, i.buttons, i.hold] for i in self.inputs],
            "checks": [[c.frame, c.name, c.soft] for c in self.checks],
            "env": dict(sorted(self.env.items())), "after": self.after,
        }
        return hashlib.sha1(json.dumps(content, sort_keys=True, separators=(",", ":")).encode("utf-8")).hexdigest()

    @staticmethod
    def from_dict(d: dict) -> "Scenario":
        return Scenario(
            name=d["name"], path=Path(d["file"]) if d.get("file") else None, sha1=d.get("sha1", ""),
            version=d.get("version", ""), window=int(d.get("window", DEFAULT_WINDOW)), end=int(d.get("end", 0)),
            inputs=[Input(int(i["frame"]), i["buttons"], int(i.get("hold", DEFAULT_HOLD))) for i in d.get("inputs", [])],
            checks=[Check(int(c["frame"]), c["name"], bool(c.get("soft", False))) for c in d.get("checks", [])],
            env=dict(d.get("env", {})), after=d.get("after", ""))


def parse(text: str, path: Path | None = None, default_name: str = "") -> Scenario:
    sc = Scenario(name=default_name or (path.stem if path else "scenario"), path=path)
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 1)
        key = parts[0].lower()
        rest = parts[1].strip() if len(parts) > 1 else ""
        where = f"{path or '<scenario>'}:{lineno}"
        if key not in _KEYWORDS:
            raise ScenarioError(f'{where}: unknown directive "{parts[0]}"')
        args = rest.split()
        try:
            if key == "name":
                if not NAME_RE.match(rest):
                    raise ScenarioError(f'{where}: invalid name "{rest}" (a-z, 0-9, - and _)')
                sc.name = rest
            elif key == "description":
                sc.description = rest
            elif key == "version":
                sc.version = rest
            elif key == "window":
                sc.window = int(rest)
                if sc.window < 0:
                    raise ScenarioError(f"{where}: the window must be an integer >= 0")
            elif key == "input":
                if len(args) not in (2, 3):
                    raise ScenarioError(f'{where}: "input <frame> <BUTTONS> [<hold>]" expected')
                names = [b.upper() for b in args[1].split("+")]
                bad = [b for b in names if b not in BUTTONS]
                if bad:
                    raise ScenarioError(f"{where}: unknown button {bad} (allowed: {', '.join(BUTTONS)})")
                hold = int(args[2]) if len(args) == 3 else DEFAULT_HOLD
                if int(args[0]) < 1 or hold < 1:
                    raise ScenarioError(f"{where}: frame >= 1 and hold >= 1 expected")
                sc.inputs.append(Input(int(args[0]), "+".join(names), hold))
            elif key == "check":
                if len(args) not in (2, 3) or (len(args) == 3 and args[2].lower() != SOFT_FLAG):
                    raise ScenarioError(f'{where}: "check <frame> <name> [soft]" expected')
                if not NAME_RE.match(args[1]):
                    raise ScenarioError(f'{where}: invalid checkpoint name "{args[1]}" (a-z, 0-9, - and _)')
                if any(c.name == args[1] for c in sc.checks):
                    raise ScenarioError(f'{where}: checkpoint "{args[1]}" already defined')
                if int(args[0]) < 1:
                    raise ScenarioError(f"{where}: frame >= 1 expected")
                sc.checks.append(Check(int(args[0]), args[1], len(args) == 3))
            elif key == "end":
                sc.end = int(rest)
            elif key == "env":
                k, sep, v = rest.partition("=")
                k, v = k.strip(), v.strip()
                if not sep or not ENV_RE.match(k):
                    raise ScenarioError(f'{where}: "env GCRT_KEY=value" expected (GCRT_* variables only)')
                sc.env[k] = v
            elif key == "after":
                if not NAME_RE.match(rest):
                    raise ScenarioError(f'{where}: "after <scenario>" expected (name a-z, 0-9, - and _)')
                if sc.after:
                    raise ScenarioError(f'{where}: "after" already defined ({sc.after})')
                sc.after = rest
        except ValueError as exc:
            if isinstance(exc, ScenarioError):
                raise
            raise ScenarioError(f"{where}: number expected ({exc})") from exc
    if sc.end <= 0:
        raise ScenarioError(f"{path or '<scenario>'}: directive \"end <frame>\" is mandatory")
    sc.inputs.sort(key=lambda i: i.frame)
    sc.checks.sort(key=lambda c: c.frame)
    if sc.checks:
        last = sc.checks[-1].frame + sc.window + 2
        if sc.end < last:
            raise ScenarioError(f"{path or '<scenario>'}: \"end {sc.end}\" too early: at least {last} "
                                f"(last checkpoint + window + 2) for the captures to be written")
    sc.sha1 = sc.fingerprint()
    return sc


def applies_to(sc: Scenario, version: str | None) -> bool:
    """A scenario that declares a version only applies to it; without a declaration, to all of them."""
    return not sc.version or not version or sc.version == version


def resolve(spec: str | Path, game: str | None = None, root: Path | None = None, version: str | None = None) -> Path:
    """Finds the file of a scenario given by a path, or by its name: `games/<game>/scenarios/<ID>/<name>.scn`
    (version-specific) then `games/<game>/scenarios/<name>.scn` (fallback)."""
    p = Path(spec)
    if p.exists():
        return p
    if game and root:
        base = root / "games" / game / "scenarios"
        cands = []
        if version:
            cands += [base / version / f"{spec}.scn", base / version / str(spec)]
        cands += [base / f"{spec}.scn", base / str(spec)]
        for cand in cands:
            if cand.is_file():
                return cand
    raise ScenarioError(f"scenario not found: {spec}" + (f" (version {version})" if version else ""))


def load(spec: str | Path, game: str | None = None, root: Path | None = None, version: str | None = None) -> Scenario:
    """Loads a scenario given by a path, or by its name (see `resolve`)."""
    p = resolve(spec, game, root, version)
    sc = parse(p.read_text(encoding="utf-8"), p.resolve(), p.stem)
    if version and p.resolve().parent.name == version and sc.version not in ("", version):
        raise ScenarioError(f'{p}: stored in the folder of {version} but declares "version {sc.version}"')
    return sc


def available(game: str, root: Path, version: str, names: str | None = None) -> tuple[list[Path], list[str]]:
    """Scenarios applicable to `version`, all of them or limited to `names` (comma-separated).

    Returns (files sorted by name, remarks). Fallback scenarios declared for another version are discarded
    (remark); a requested name that does not exist for this version raises `ScenarioError`."""
    base = root / "games" / game / "scenarios"
    found: dict[str, Path] = {}
    notes: list[str] = []
    for f in sorted(base.glob("*.scn")):
        sc = parse(f.read_text(encoding="utf-8"), f, f.stem)
        if applies_to(sc, version):
            found[f.stem] = f
        else:
            notes.append(f"{f.stem}: discarded (recorded on {sc.version}, no {version}/{f.stem}.scn)")
    for f in sorted((base / version).glob("*.scn")):  # version-specific: takes precedence
        found[f.stem] = f
        notes = [n for n in notes if not n.startswith(f"{f.stem}:")]
    if names and names != "all":
        wanted = [n.strip() for n in names.split(",") if n.strip()]
        missing = [n for n in wanted if n not in found]
        if missing:
            raise ScenarioError(f"scenario not found for {version} in {base}: {', '.join(missing)}"
                                + (f" ({'; '.join(notes)})" if notes else ""))
        return [found[n] for n in wanted], notes
    if not found:
        raise ScenarioError(f"no scenario for {version} in {base}" + (f" ({'; '.join(notes)})" if notes else ""))
    return [found[n] for n in sorted(found)], notes


def map_checkpoints(sc: Scenario, captures: list[dict]) -> list[dict]:
    """Maps the captures of a run (`{file, image, field}`: presented frame and VI field of each capture, read
    from `trace.log`) to the checkpoints of the scenario.

    Center = capture of the exact frame; window = captures of frames N-W ... N+W. A capture can serve several
    nearby checkpoints. Returns, per checkpoint: `{name, frame, soft, center: capture|None, window: [...]}`."""
    by_image = {c["image"]: c for c in captures}
    out = []
    for chk in sc.checks:
        window = [by_image[i] for i in range(chk.frame - sc.window, chk.frame + sc.window + 1) if i in by_image]
        out.append({"name": chk.name, "frame": chk.frame, "soft": chk.soft,
                    "center": by_image.get(chk.frame), "window": window})
    return out
