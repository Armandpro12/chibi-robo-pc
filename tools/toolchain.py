"""Discovery of the project's tools (generic, no game-specific parameter).

Search order for each tool:
  1. explicit path in `config/local.toml`, section [paths] (machine-specific, ignored by Git);
  2. portable install in `external/` (done by `tools/bootstrap.py`);
  3. the system PATH.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXTERNAL = ROOT / "external"
GENERATED = ROOT / "generated"
ORIG = ROOT / "orig"
IS_WINDOWS = os.name == "nt"
EXE = ".exe" if IS_WINDOWS else ""

# The Windows console is not always UTF-8: force the encoding for messages with non-ASCII characters.
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8")


def build_dir(game: str, version: str, config: str, tag: str | None = None) -> Path:
    """Build directory: build/<game>-<ID>-<config>[-<tag>].

    The `tag` suffix lets several people or agents each build and run their own copy in parallel
    (on Windows, a running .exe cannot be replaced)."""
    name = f"{game}-{version}-{config}" + (f"-{tag}" if tag else "")
    return ROOT / "build" / name


def load_local_config() -> dict:
    path = ROOT / "config" / "local.toml"
    if not path.exists():
        return {}
    with path.open("rb") as f:
        return tomllib.load(f)


def load_game(game: str) -> dict:
    """Reads `games/<game>/game.toml`."""
    path = ROOT / "games" / game / "game.toml"
    with path.open("rb") as f:
        data = tomllib.load(f)
    data["_dir"] = path.parent
    return data


def _configured(name: str) -> Path | None:
    value = load_local_config().get("paths", {}).get(name)
    return Path(value) if value else None


def _first_existing(*candidates: Path | None) -> Path | None:
    for c in candidates:
        if c and c.exists():
            return c
    return None


def _which(name: str) -> Path | None:
    found = shutil.which(name)
    return Path(found) if found else None


@dataclass
class Tool:
    name: str
    path: Path | None


def find_git() -> Path | None:
    return _first_existing(_configured("git"), _which("git"))


def find_cmake() -> Path | None:
    return _first_existing(_configured("cmake"), _which("cmake"))


def find_ninja() -> Path | None:
    return _first_existing(_configured("ninja"), EXTERNAL / "bin" / f"ninja{EXE}", _which("ninja"))


def find_clang_cl() -> Path | None:
    return _first_existing(_configured("clang_cl"), EXTERNAL / "llvm" / "bin" / f"clang-cl{EXE}", _which("clang-cl"))


def find_clang() -> Path | None:
    return _first_existing(_configured("clang"), EXTERNAL / "llvm" / "bin" / f"clang{EXE}", _which("clang"))


def find_java() -> Path | None:
    java_home = os.environ.get("JAVA_HOME")
    return _first_existing(
        _configured("java"),
        Path(java_home) / "bin" / f"java{EXE}" if java_home else None,
        _which("java"),
    )


def find_ghidra_headless() -> Path | None:
    script = "analyzeHeadless.bat" if IS_WINDOWS else "analyzeHeadless"
    configured = _configured("ghidra")
    return _first_existing(
        configured / "support" / script if configured else None,
        EXTERNAL / "ghidra" / "support" / script,
    )


def find_dolrecomp() -> Path | None:
    build = EXTERNAL / "DolRecomp" / "build"
    return _first_existing(
        _configured("dolrecomp"),
        build / "Release" / f"dolrecomp{EXE}",
        build / f"dolrecomp{EXE}",
    )


def find_dolphin() -> Path | None:
    name = "Dolphin.exe" if IS_WINDOWS else "dolphin-emu"
    return _first_existing(_configured("dolphin"), _which(name))


def find_dolphin_tool() -> Path | None:
    dolphin = find_dolphin()
    name = "DolphinTool.exe" if IS_WINDOWS else "dolphin-tool"
    return _first_existing(_configured("dolphin_tool"), dolphin.parent / name if dolphin else None, _which(name))


def find_vcvars() -> Path | None:
    """MSVC environment script (provides the Windows headers and libraries to clang-cl)."""
    if not IS_WINDOWS:
        return None
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None
    out = subprocess.run(
        [str(vswhere), "-latest", "-products", "*", "-requires",
         "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-format", "json"],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )
    try:
        installs = json.loads(out.stdout or "[]")
    except json.JSONDecodeError:
        return None
    for inst in installs:
        candidate = Path(inst["installationPath"]) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat"
        if candidate.exists():
            return candidate
    return None


_msvc_env_cache: dict | None = None


def msvc_env() -> dict:
    """Environment of the current process, extended by vcvars64 (Windows); unchanged elsewhere."""
    global _msvc_env_cache
    if _msvc_env_cache is not None:
        return dict(_msvc_env_cache)
    env = dict(os.environ)
    vcvars = find_vcvars()
    if vcvars:
        out = subprocess.run(
            f'cmd /s /c ""{vcvars}" >nul 2>&1 && set"',
            capture_output=True, text=True, shell=True, encoding="utf-8", errors="replace",
        )
        for line in out.stdout.splitlines():
            key, sep, value = line.partition("=")
            if sep and key:
                env[key] = value
    _msvc_env_cache = env
    return dict(env)


def run(cmd: list, **kwargs) -> subprocess.CompletedProcess:
    """Runs a command, printing the command line; raises an exception if it fails."""
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def python_exe() -> str:
    return sys.executable
