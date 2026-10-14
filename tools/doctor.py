"""Environment check: verifies each tool the project needs.

Usage: py -3 tools/doctor.py
Exit code 0 if every required tool is present, 1 otherwise. Optional tools (Dolphin, used only for comparisons
with the original behavior, and its DolphinTool, used only to convert disc images that are not .iso/.gcm) are
reported with a warning when missing, never as a failure.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402

GREEN, RED, YELLOW, RESET = "\033[32m", "\033[31m", "\033[33m", "\033[0m"


def _output(cmd: list) -> str:
    try:
        p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=60)
        return (p.stdout or "") + (p.stderr or "")
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"<error: {exc}>"


def _version(text: str) -> tuple[int, ...] | None:
    m = re.search(r"(\d+)\.(\d+)(?:\.(\d+))?", text)
    return tuple(int(g) for g in m.groups() if g is not None) if m else None


def check_python():
    v = sys.version_info
    return v >= (3, 11), sys.executable, f"{v.major}.{v.minor}.{v.micro} (3.11+ required for tomllib)"


def check_versioned(path, args, minimum, label):
    if not path:
        return False, None, f"{label} not found"
    text = _output([path, *args])
    ver = _version(text)
    if ver is None:
        return False, path, f"unreadable version: {text.strip()[:80]}"
    ok = minimum is None or ver >= minimum
    need = f" (min {'.'.join(map(str, minimum))})" if minimum else ""
    return ok, path, ".".join(map(str, ver)) + need


def check_java():
    """JDK used by Ghidra: JAVA_HOME_OVERRIDE from support/launch.properties if set, otherwise the system Java.

    Ghidra 12.1 requires at least Java 21; JDK 25 makes its OSGi framework fail (observed on 2026-10-09),
    hence the upper bound (versions 22 to 24 have not been tried).
    """
    path = None
    headless = tc.find_ghidra_headless()
    props = headless.parent / "launch.properties" if headless else None
    if props and props.exists():
        m = re.search(r"^JAVA_HOME_OVERRIDE=(.+)$", props.read_text(encoding="utf-8"), re.M)
        if m and m.group(1).strip():
            path = Path(m.group(1).strip().replace("\\\\", "\\")) / "bin" / f"java{tc.EXE}"
    path = path or tc.find_java()
    if not path or not path.exists():
        return False, path, "java not found (JDK 21 required by Ghidra; run tools/bootstrap.py --only jdk21)"
    text = _output([path, "-version"])
    m = re.search(r'version "(\d+)', text)
    major = int(m.group(1)) if m else 0
    return 21 <= major < 25, path, f"JDK {major} for Ghidra (21 to 24 accepted)"


def check_ghidra():
    path = tc.find_ghidra_headless()
    if not path:
        return False, None, "Ghidra not found (run tools/bootstrap.py)"
    props = path.parent.parent / "Ghidra" / "application.properties"
    ver = "?"
    if props.exists():
        m = re.search(r"application\.version=(\S+)", props.read_text(encoding="utf-8"))
        ver = m.group(1) if m else "?"
    return True, path, f"Ghidra {ver}"


def check_dolrecomp():
    path = tc.find_dolrecomp()
    if not path:
        return False, None, "DolRecomp not built (run tools/bootstrap.py)"
    text = _output([path])
    ok = "--gamecube" in text
    return ok, path, "responds and accepts --gamecube" if ok else "does not respond as expected"


def check_file(path, label, missing_note=""):
    if path is not None:
        return True, path, label
    return False, None, f"{label} not found" + (f" ({missing_note})" if missing_note else "")


def check_msvc():
    path = tc.find_vcvars()
    if not path:
        return False, None, "vcvars64.bat not found (Windows headers needed by clang-cl)"
    env = tc.msvc_env()
    ok = bool(env.get("INCLUDE")) and bool(env.get("LIB"))
    return ok, path, f"MSVC {env.get('VCToolsVersion', '?').strip()} / SDK {env.get('WindowsSDKVersion', '?').strip(chr(92))}"


def check_disc_images():
    """Informational: disc images present in orig/ (reads the header only)."""
    found = []
    for p in sorted(tc.ORIG.rglob("*")):
        if p.suffix.lower() in (".iso", ".gcm", ".rvz", ".gcz", ".ciso", ".wia") and p.is_file():
            with p.open("rb") as f:
                head = f.read(6)
            game_id = head.decode("ascii", "replace") if p.suffix.lower() in (".iso", ".gcm") else "(compressed)"
            found.append(f"{game_id}  {p.relative_to(tc.ROOT)}")
    return bool(found), tc.ORIG, ("\n" + " " * 16).join(found) if found else "no image in orig/"


# (name, check, required): a missing optional tool only gives a warning.
CHECKS = [
    ("Git", lambda: check_versioned(tc.find_git(), ["--version"], (2, 0), "git"), True),
    ("Python", check_python, True),
    ("CMake", lambda: check_versioned(tc.find_cmake(), ["--version"], (3, 25), "cmake"), True),
    ("Ninja", lambda: check_versioned(tc.find_ninja(), ["--version"], (1, 10), "ninja"), True),
    ("clang-cl", lambda: check_versioned(tc.find_clang_cl(), ["--version"], (16, 0), "clang-cl"), True),
    ("MSVC/SDK", check_msvc, True),
    ("JDK", check_java, True),
    ("Ghidra", check_ghidra, True),
    ("DolRecomp", check_dolrecomp, True),
    ("Dolphin", lambda: check_file(tc.find_dolphin(), "Dolphin (path in config/local.toml [paths] dolphin)",
                                   "optional, only for comparisons with the original behavior"), False),
    ("DolphinTool", lambda: check_file(tc.find_dolphin_tool(), "DolphinTool",
                                       "optional, only to convert disc images other than .iso/.gcm"), False),
]


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    all_ok = True
    warnings = 0
    for name, fn, required in CHECKS:
        ok, path, detail = fn()
        if required:
            all_ok &= ok
            mark = f"{GREEN}OK   {RESET}" if ok else f"{RED}FAIL {RESET}"
        else:
            warnings += not ok
            mark = f"{GREEN}OK   {RESET}" if ok else f"{YELLOW}WARN {RESET}"
        print(f"[{mark}] {name:<10} {detail}")
        if path:
            print(f"{'':16}{path}")
    ok, _, detail = check_disc_images()
    mark = f"{GREEN}info {RESET}" if ok else f"{YELLOW}info {RESET}"
    print(f"[{mark}] {'Images':<10} {detail}")
    print()
    if not all_ok:
        print(f"{RED}Environment incomplete.{RESET}")
    elif warnings:
        print(f"{GREEN}Environment ready{RESET} ({YELLOW}{warnings} optional tool(s) missing{RESET}: "
              "building and playing do not need them).")
    else:
        print(f"{GREEN}Environment ready.{RESET}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
