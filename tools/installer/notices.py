"""Assembles THIRD_PARTY_LICENSES.txt, the licence notices shipped with the player installer.

Usage: py -3 tools/installer/notices.py --build build/<game>-<ID>-Release [--iscc-dir DIR] [--out FILE] [--fetch]
                                        [--external DIR]

The texts come from the real licence files: the dependency sources fetched by the game build (<build>/_deps), the
Aurora checkout (external/aurora, including the licence comment of its vendored magic_enum.hpp), the DolRecomp
checkout (external/DolRecomp), the Inno Setup installation, and, for the components shipped as prebuilt DLLs
without their licence (Dawn, the Khronos headers and SPIRV-Tools compiled into Dawn), the official repositories at
the revisions listed below, fetched with --fetch into external/licenses-src/ (licence files only: partial, blob-less
git fetch).

Claims checked against the build, so that the notices cannot silently go stale:
- DolRecomp: the checkout the build compiled (DOLRECOMP_DIR of <build>/CMakeCache.txt, else external/DolRecomp) must
  be at DOLRECOMP_COMMIT with src/cpu and src/common unmodified (git rev-parse, git diff, git status); otherwise, or
  without that checkout (its LICENSE is needed anyway), the notices are not built;
- Visual C++ runtime: the DLLs named in its entry are the ones the shipped binaries import (PE import tables of
  <build>/bin, or the list given by build.py, which computes it from the payload).

Imported by tools/installer/build.py, which regenerates the file for every installer and checks that the copy in
Git is up to date.
"""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXTERNAL = ROOT / "external"

# Official repositories of the components whose prebuilt binaries ship without a licence file.
# name: (repository URL, revision, licence files)
FETCHED = {
    # Revision of the Dawn prebuilt used by Aurora (CMake cache AURORA_DAWN_REF, release v20260807.225922 of
    # github.com/encounter/dawn).
    "dawn": ("https://github.com/google/dawn.git", "1155e0ed531126f33a1279afa029349651ca1c93", ["LICENSE"]),
    # Compiled into webgpu_dawn.dll (strings of the DLL) or used through their headers; licence files read at the
    # head of the default branch on 2026-10-19.
    "SPIRV-Tools": ("https://github.com/KhronosGroup/SPIRV-Tools.git", "71f287f18ed78d496e724fbfb1c3feae3a2af75a",
                    ["LICENSE"]),
    "SPIRV-Headers": ("https://github.com/KhronosGroup/SPIRV-Headers.git", "86f980c731e62ae4eaf383d320449d71687936bf",
                      ["LICENSE"]),
    "Vulkan-Headers": ("https://github.com/KhronosGroup/Vulkan-Headers.git",
                       "acbb9391c0a597272800cd3f4363661c00a27c77", ["LICENSE.md", "LICENSES/MIT.txt"]),
}

# DolRecomp revision whose CPU runtime (src/cpu, src/common) is compiled into the game executable, unmodified (the
# project's patch, tools/patches/dolrecomp/, only touches the recompiler itself): checked by check_dolrecomp().
DOLRECOMP_COMMIT = "c876e2b9e022e084aa9a690fe15e2ad79a0706cd"
DOLRECOMP_UNMODIFIED = ["src/cpu", "src/common"]
# Visual C++ runtime: version of the redistributable folder (VC\Redist\MSVC\<version>, checked by build.py) and the
# names of the DLLs it may provide.
VC_VERSION = "14.44.35112"
VC_RUNTIME_PATTERN = re.compile(r"^(vcruntime140(_1)?|msvcp140(_\w+)?|concrt140|vccorlib140)\.dll$", re.I)
VC_ENTRY = "Microsoft Visual C++ Redistributable"

# (title, version, shipped in, licence, origin, licence files) with licence files a list of (base, relative path) or
# (base, relative path, mode), base in root | build | aurora | dolrecomp | fetched:<name> | iscc, or None for a
# statement-only entry. Modes: "header" = only the first block of "//" comment lines of a source file (a vendored
# header whose licence is in its own comment); "same:<base>:<path>" = the file must have the same text as that other
# file (white space aside), which is already in the notices: it is checked, not repeated.
COMPONENTS = [
    ("This project (native runtime, gcextract, installer script, build scripts)", "see BUILD-INFO.txt",
     "the game executable (its native runtime part, not the recompiled game code: see the next entry), "
     "gcextract.exe, the installer script", "GNU General Public License v3.0",
     "https://github.com/Armandpro12/chibi-robo-pc (source code of the exact build: commit in BUILD-INFO.txt)",
     [("root", "LICENSE")]),
    ("Recompiled game code (not licensed by this project)", "the version of the disc image the player chose",
     "the game executable",
     "None granted by this project. The game executable also contains the original game's program code, translated "
     "into C by DolRecomp and compiled for the PC. That code is derived from the original game, belongs to the game's "
     "rights holders, and is NOT covered by the GNU GPL v3.0 or by any other licence of this project. This package "
     "contains no game data (no disc image, and none of the game's files, textures, sounds or models): the player "
     "provides their own disc image.",
     "the original game, translated when the executable is built", None),
    ("DolRecomp (CPU runtime)", f"commit {DOLRECOMP_COMMIT[:12]} of github.com/ExpansionPak/DolRecomp: src/cpu/cpu.c, "
     "src/cpu/cpu.h and src/common/types.h, unmodified", "the game executable",
     "GNU General Public License v3.0 or later (SPDX header of src/cpu/cpu.c: GPL-3.0-or-later, "
     "Copyright (C) 2026 ExpansionPak); runtime/core/decomp/gc_float.h of this project's repository adapts "
     "floating-point helpers of src/cpu/cpu.c (force_25_bit, correct_single_fma_tie, force_single)",
     "https://github.com/ExpansionPak/DolRecomp",
     [("dolrecomp", "LICENSE", "same:root:LICENSE")]),
    ("Dolphin (adapted code)", "commit e6f3ae17627e of github.com/dolphin-emu/dolphin", "the game executable",
     "GNU General Public License v2.0 or later, used here under the GNU GPL v3.0 (text: first entry). Copyright 2008 "
     "Dolphin Emulator Project (DSP sample accelerator, AX microcode HLE, microcode base and hashes, AX structures, "
     "and the free DSP coefficient table of Dolphin's Data/Sys/GC/dsp_coef.bin: files of runtime/core/audio/ of this "
     "project's repository); Copyright 2010 and 2017 Dolphin Emulator Project (display copy filter coefficients and "
     "computation: runtime/core/gx/xfb_filter.cpp); Copyright 2009 Dolphin Emulator Project (floating-point "
     "rounding of the Gekko, Force25Bit, ForceSingle and NI_madd_msub of Interpreter_FPUtils.h, through DolRecomp: "
     "runtime/core/decomp/gc_float.h). Each adapted file names the Dolphin files it comes from.",
     "https://github.com/dolphin-emu/dolphin", None),
    ("Aurora", "commit 50e5be30ec12 with the project's patches (tools/patches/aurora/ of this project's repository)",
     "the game executable", "MIT License", "https://github.com/encounter/aurora", [("aurora", "LICENSE")]),
    ("magic_enum", "0.7.2 (include/magic_enum.hpp of the Aurora checkout)", "the game executable", "MIT License",
     "https://github.com/Neargye/magic_enum", [("aurora", "include/magic_enum.hpp", "header")]),
    ("SDL", "3.4.10", "SDL3.dll", "zlib License", "https://github.com/libsdl-org/SDL",
     [("build", "_deps/sdl3_prebuilt-src/licenses/SDL3/LICENSE.txt")]),
    ("Dawn (including Tint)", "prebuilt v20260807.225922 of github.com/encounter/dawn, google/dawn 1155e0ed5311",
     "webgpu_dawn.dll", "BSD 3-Clause License (the Apache-2.0 part of the file covers files that are not shipped)",
     "https://dawn.googlesource.com/dawn", [("fetched:dawn", "LICENSE")]),
    ("SPIRV-Tools", "as compiled into the Dawn prebuilt", "webgpu_dawn.dll", "Apache License 2.0",
     "https://github.com/KhronosGroup/SPIRV-Tools", [("fetched:SPIRV-Tools", "LICENSE")]),
    ("SPIRV-Headers", "as used by the Dawn prebuilt", "webgpu_dawn.dll", "MIT-style Khronos licence",
     "https://github.com/KhronosGroup/SPIRV-Headers", [("fetched:SPIRV-Headers", "LICENSE")]),
    ("Vulkan-Headers", "as used by the Dawn prebuilt", "webgpu_dawn.dll", "Apache License 2.0 OR MIT (MIT chosen)",
     "https://github.com/KhronosGroup/Vulkan-Headers",
     [("fetched:Vulkan-Headers", "LICENSE.md"), ("fetched:Vulkan-Headers", "LICENSES/MIT.txt")]),
    ("Abseil", "20240722.0", "the game executable, webgpu_dawn.dll", "Apache License 2.0",
     "https://github.com/abseil/abseil-cpp", [("build", "_deps/abseil-cpp-src/LICENSE")]),
    ("libpng", "1.6.58", "libpng16.dll", "PNG Reference Library License version 2",
     "https://github.com/pnggroup/libpng", [("build", "_deps/png-src/LICENSE")]),
    ("zlib-ng", "2.3.3", "zlib1.dll", "zlib License", "https://github.com/zlib-ng/zlib-ng",
     [("build", "_deps/zlib-src/LICENSE.md")]),
    ("FreeType", "2.14.3", "the game executable",
     "FreeType License (FTL). Portions of this software are copyright (c) 2026 The FreeType Project "
     "(https://freetype.org). All rights reserved.", "https://freetype.org",
     [("build", "_deps/freetype-src/docs/FTL.TXT")]),
    ("Dear ImGui", "1.91.9b-docking", "the game executable", "MIT License", "https://github.com/ocornut/imgui",
     [("build", "_deps/imgui-src/LICENSE.txt")]),
    ("{fmt}", "12.1.0", "the game executable", "MIT License with the {fmt} binary exception",
     "https://github.com/fmtlib/fmt", [("build", "_deps/fmt-src/LICENSE")]),
    ("Zstandard", "1.5.7", "the game executable", "BSD License (chosen of BSD OR GPLv2)",
     "https://github.com/facebook/zstd", [("build", "_deps/zstd-src/LICENSE")]),
    ("xxHash", "0.8.3", "the game executable", "BSD 2-Clause License", "https://github.com/Cyan4973/xxHash",
     [("build", "_deps/xxhash-src/LICENSE")]),
    ("Tracy Profiler (client)", "0.14.1", "the game executable", "BSD 3-Clause License",
     "https://github.com/wolfpld/tracy", [("build", "_deps/tracy-src/LICENSE")]),
    ("SQLite", "3.51.3", "the game executable",
     "Public domain: \"The author disclaims copyright to this source code.\" (header of sqlite3.c)",
     "https://sqlite.org", None),
    (VC_ENTRY, VC_VERSION, "the files of that name in the installation folder",
     "Microsoft Software License Terms: Distributable Code of Visual Studio 2022 (files of VC\\redist, "
     "copied unmodified). Copyright (c) Microsoft Corporation. All rights reserved.",
     "https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution", None),
    ("Inno Setup", "6.7.3", "the installer and uninstaller (unins000.exe)", "Inno Setup License",
     "https://jrsoftware.org/isinfo.php", [("iscc", "license.txt")]),
]


def pe_imports(path: Path) -> list[str]:
    """Names of the DLLs imported by a PE file (import directory only; delay-loaded DLLs are not listed)."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: not a PE file")
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    dirs = opt + (112 if magic == 0x20B else 96)
    imp_rva = struct.unpack_from("<I", data, dirs + 8)[0]
    sections = []
    for i in range(nsec):
        s = opt + opt_size + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, s + 8)
        sections.append((va, max(vsize, rsize), raw))

    def off(rva: int) -> int:
        for va, size, raw in sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise SystemExit(f"{path}: RVA {rva:#x} outside the sections")

    names = []
    if imp_rva:
        p = off(imp_rva)
        while True:
            name_rva = struct.unpack_from("<I", data, p + 12)[0]
            if name_rva == 0:
                break
            q = off(name_rva)
            names.append(data[q:data.index(b"\0", q)].decode("ascii"))
            p += 20
    return names


def vc_runtime_imports(binaries) -> list[str]:
    """Visual C++ runtime DLLs (lower case, sorted) imported by the given PE files."""
    return sorted({n.lower() for p in binaries for n in pe_imports(p) if VC_RUNTIME_PATTERN.match(n)})


def build_binaries(build: Path) -> list[Path]:
    """Executables and DLLs of a game build (<build>/bin), as build.py ships them."""
    return sorted(p for p in (build / "bin").iterdir() if p.is_file() and p.suffix.lower() in (".exe", ".dll"))


def dolrecomp_dir(build: Path, external: Path) -> Path:
    """The DolRecomp checkout a game build compiled: DOLRECOMP_DIR of its CMake cache, else external/DolRecomp."""
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        m = re.search(r"^DOLRECOMP_DIR:\w+=(.*)$", cache.read_text(encoding="utf-8", errors="replace"), re.M)
        if m:
            return Path(m.group(1).strip())
    return external / "DolRecomp"


def check_dolrecomp(build: Path, external: Path) -> None:
    """Fails unless the DolRecomp checkout of the build is at DOLRECOMP_COMMIT with DOLRECOMP_UNMODIFIED unmodified,
    as its entry says. Without that checkout the claim cannot be checked: fails too."""
    repo = dolrecomp_dir(build, external)
    if not (repo / ".git").exists():
        raise SystemExit(f"notices: DolRecomp checkout not found ({repo}): its entry cannot be checked")

    def git(*args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)

    head = git("rev-parse", "HEAD").stdout.strip()
    if head != DOLRECOMP_COMMIT:
        raise SystemExit(f"notices: DolRecomp is at {head or '?'} in {repo}, its entry says {DOLRECOMP_COMMIT}: "
                         "update DOLRECOMP_COMMIT in tools/installer/notices.py")
    diff = git("diff", "--quiet", "HEAD", "--", *DOLRECOMP_UNMODIFIED)
    status = git("status", "--porcelain", "--untracked-files=all", "--", *DOLRECOMP_UNMODIFIED)
    if diff.returncode != 0 or status.returncode != 0 or status.stdout.strip():
        raise SystemExit(f"notices: {', '.join(DOLRECOMP_UNMODIFIED)} of DolRecomp are modified in {repo}, its entry "
                         f"says \"unmodified\":\n{status.stdout.strip()}")


def fetch(name: str, external: Path = EXTERNAL) -> Path:
    """Fetches the licence files of FETCHED[name] into external/licenses-src/<name> (no other blob)."""
    url, rev, files = FETCHED[name]
    dest = external / "licenses-src" / name
    if all((dest / f).is_file() for f in files):
        return dest
    dest.mkdir(parents=True, exist_ok=True)

    def git(*args: str, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(dest), *args], check=check, capture_output=True, text=True)

    if not (dest / ".git").exists():
        git("init", "-q")
        git("remote", "add", "origin", url)
    git("config", "remote.origin.promisor", "true")
    git("config", "remote.origin.partialclonefilter", "blob:none")
    git("fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", rev)
    for f in files:
        # The first checkout after a shallow blob-less fetch sometimes fails to fetch the blob: one more fetch
        # of the same revision fixes it.
        if git("checkout", "-q", rev, "--", f, check=False).returncode != 0:
            git("fetch", "-q", "--filter=blob:none", "origin", rev)
            git("checkout", "-q", rev, "--", f)
    return dest


def resolve(base: str, rel: str, build: Path, iscc_dir: Path | None, allow_fetch: bool, external: Path) -> Path:
    if base == "root":
        return ROOT / rel
    if base == "build":
        return build / rel
    if base == "aurora":
        return external / "aurora" / rel
    if base == "dolrecomp":
        return external / "DolRecomp" / rel
    if base == "iscc":
        if iscc_dir is None:
            raise SystemExit("notices: the Inno Setup folder is needed (--iscc-dir)")
        return iscc_dir / rel
    if base.startswith("fetched:"):
        name = base.split(":", 1)[1]
        path = external / "licenses-src" / name / rel
        if not path.is_file() and allow_fetch:
            fetch(name, external)
        return path
    raise ValueError(base)


def read_licence(base: str, rel: str, build: Path, iscc_dir: Path | None, allow_fetch: bool, external: Path) -> str:
    path = resolve(base, rel, build, iscc_dir, allow_fetch, external)
    if not path.is_file():
        raise SystemExit(f"notices: licence file missing: {path} (fetch with --fetch, or build the game "
                         "so that <build>/_deps exists)")
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n").rstrip()


def comment_header(text: str, rel: str) -> str:
    """First block of "//" comment lines of a source file, without the comment markers."""
    lines = []
    for line in text.split("\n"):
        if not line.startswith("//"):
            if lines:
                break
            continue
        lines.append(line[3:] if line.startswith("// ") else line[2:])
    body = "\n".join(lines).rstrip()
    if "Permission is hereby" not in body:
        raise SystemExit(f"notices: no licence text in the first comment block of {rel}")
    return body


def assemble(build: Path, iscc_dir: Path | None, allow_fetch: bool = False, external: Path = EXTERNAL,
             vc_dlls: list[str] | None = None) -> str:
    """The notices text. vc_dlls: the Visual C++ runtime DLLs shipped (default: those imported by <build>/bin)."""
    check_dolrecomp(build, external)
    if vc_dlls is None:
        vc_dlls = vc_runtime_imports(build_binaries(build))
    if not vc_dlls:
        raise SystemExit("notices: no Visual C++ runtime DLL is imported: check the build, or drop its entry")
    components = [(f"{c[0]} ({', '.join(vc_dlls)})",) + c[1:] if c[0] == VC_ENTRY else c for c in COMPONENTS]
    sep = "=" * 100
    out = [
        "THIRD-PARTY SOFTWARE NOTICES AND LICENCES",
        "",
        "This package contains the native PC executable of a recompiled GameCube game and the components listed",
        "below with their licences. The executable is made of this project's native runtime, the third-party code",
        "listed below, and the original game's program code translated by DolRecomp: that recompiled game code is",
        "derived from the original game and is not licensed by this project (see \"Recompiled game code\"). The",
        "package contains no game data: the player provides their own disc image.",
        "",
        "Contents:",
    ]
    out += [f"  - {c[0]} ({c[1]})" for c in components]
    out.append("")
    for title, version, shipped, licence, origin, files in components:
        out += [sep, title, sep, f"Version: {version}", f"Shipped in: {shipped}", f"Licence: {licence}",
                f"Origin: {origin}", ""]
        for base, rel, *mode in files or []:
            text = read_licence(base, rel, build, iscc_dir, allow_fetch, external)
            mode = mode[0] if mode else ""
            if mode == "header":
                text = comment_header(text, rel)
            elif mode.startswith("same:"):
                _, other_base, other_rel = mode.split(":", 2)
                other = read_licence(other_base, other_rel, build, iscc_dir, allow_fetch, external)
                if text.split() != other.split():
                    raise SystemExit(f"notices: {rel} of {title} differs from {other_rel}: ship its text")
                text = (f"Same text (white space aside) as the {other_rel} file reproduced earlier in this document, "
                        "not repeated.")
            elif mode:
                raise ValueError(mode)
            out += [f"----- {rel} -----", text, ""]
    return "\n".join(out).rstrip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Assembles the licence notices of the player installer.")
    parser.add_argument("--build", required=True, help="game build folder (contains _deps/)")
    parser.add_argument("--iscc-dir", help="Inno Setup installation folder (license.txt)")
    parser.add_argument("--out", default=str(Path(__file__).with_name("THIRD_PARTY_LICENSES.txt")))
    parser.add_argument("--fetch", action="store_true", help="fetch missing licence files (git, licence files only)")
    parser.add_argument("--external", default=str(EXTERNAL), help="external/ folder (default: the one of this checkout)")
    args = parser.parse_args()
    text = assemble(Path(args.build), Path(args.iscc_dir) if args.iscc_dir else None, args.fetch, Path(args.external))
    Path(args.out).write_text(text, encoding="utf-8", newline="\r\n")
    print(f"Written: {args.out} ({len(text)} characters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
