"""Installs the missing tools into `external/` (portable archives, no change to the system).

Usage: py -3 tools/bootstrap.py [--only ninja,llvm,ghidra,jdk21,ghidra_gc_loader,dolrecomp,aurora]

Versions are pinned here so that two machines get the same result.
Installed by this script, into external/: Ninja, clang/LLVM, Ghidra, a portable JDK 21 (used by Ghidra), the
GameCube loader extension for Ghidra, DolRecomp (cloned and built) and Aurora (cloned, then built once to check
the toolchain; that build downloads Aurora's own dependencies, such as Dawn and SDL3).
To install yourself (required): Git, Python 3.11+, CMake 3.25+, Visual Studio 2022 (MSVC and the Windows SDK).
Optional, not installed either: Dolphin and its DolphinTool (comparisons with the original behavior, conversion of
disc images that are not .iso/.gcm).
"""

from __future__ import annotations

import argparse
import shutil
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import toolchain as tc  # noqa: E402

NINJA_URL = "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip"
LLVM_VERSION = "23.1.3"
LLVM_URL = (f"https://github.com/llvm/llvm-project/releases/download/llvmorg-{LLVM_VERSION}/"
            f"clang+llvm-{LLVM_VERSION}-x86_64-pc-windows-msvc.tar.xz")
GHIDRA_URL = ("https://github.com/NationalSecurityAgency/ghidra/releases/download/"
              "Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip")
# JDK 21 for Ghidra: Ghidra 12.1 fails with JDK 25 when its OSGi framework starts
# (Felix: "The data file must be inside the data dir"), observed on 2026-10-09.
JDK21_URL = ("https://github.com/adoptium/temurin21-binaries/releases/download/jdk-21.0.12.1%2B1/"
             "OpenJDK21U-jdk_x64_windows_hotspot_21.0.12.1_1.zip")
# DOL/REL loader and Gekko/Broadway processor profile (paired singles) for Ghidra.
GC_LOADER_URL = ("https://github.com/Cuyler36/Ghidra-GameCube-Loader/releases/download/"
                 "1.3.1/GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip")
DOLRECOMP_REPO = "https://github.com/ExpansionPak/DolRecomp.git"
DOLRECOMP_COMMIT = "c876e2b9e022e084aa9a690fe15e2ad79a0706cd"
# Aurora (MIT): GX/WebGPU layer (Dawn), window and input (SDL3). Dawn and SDL3 come from
# encounter's prebuilt packages ("package" mode, selected by default on Windows x64).
AURORA_REPO = "https://github.com/encounter/aurora.git"
AURORA_COMMIT = "50e5be30ec12cc72768bc564bc016b55e82be868"
AURORA_CMAKE_OPTIONS = [
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
    "-DAURORA_ENABLE_DVD=OFF",       # Aurora's DVD layer (nod) is unused: the runtime replaces the DVD functions
    "-DAURORA_ENABLE_TESTS=OFF",
    "-DAURORA_ENABLE_EXAMPLES=ON",   # the "simple" example validates the whole toolchain
    # Only use the dependencies downloaded by Aurora: otherwise find_package()
    # finds libraries through the PATH (e.g. fmt and zstd from a conda install).
    "-DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF",
    "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF",
    "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
    "-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON",
]

DOWNLOADS = tc.EXTERNAL / "_downloads"


def download(url: str) -> Path:
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    dest = DOWNLOADS / url.rsplit("/", 1)[1]
    if dest.exists():
        print(f"already downloaded: {dest.name}")
        return dest
    print(f"downloading: {url}")
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(url) as resp, tmp.open("wb") as out:
        shutil.copyfileobj(resp, out, length=1 << 20)
    tmp.rename(dest)
    return dest


def extract_single_root(archive: Path, target: Path) -> None:
    """Extracts an archive whose content is a single root folder, renamed to `target`."""
    staging = target.parent / (target.name + "._staging")
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    if archive.suffix == ".zip":
        with zipfile.ZipFile(archive) as z:
            z.extractall(staging)
    else:
        with tarfile.open(archive) as t:
            t.extractall(staging, filter="data")
    roots = list(staging.iterdir())
    src = roots[0] if len(roots) == 1 and roots[0].is_dir() else staging
    src.rename(target)
    shutil.rmtree(staging, ignore_errors=True)


def install_ninja() -> None:
    if tc.find_ninja():
        print("ninja: present")
        return
    (tc.EXTERNAL / "bin").mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(download(NINJA_URL)) as z:
        z.extractall(tc.EXTERNAL / "bin")


def install_llvm() -> None:
    if (tc.EXTERNAL / "llvm" / "bin" / f"clang-cl{tc.EXE}").exists():
        print("llvm: present")
        return
    extract_single_root(download(LLVM_URL), tc.EXTERNAL / "llvm")


def install_ghidra() -> None:
    if (tc.EXTERNAL / "ghidra").exists():
        print("ghidra: present")
        return
    extract_single_root(download(GHIDRA_URL), tc.EXTERNAL / "ghidra")


def install_jdk21() -> None:
    """Portable JDK 21 in external/jdk21, forced on Ghidra through JAVA_HOME_OVERRIDE."""
    jdk = tc.EXTERNAL / "jdk21"
    if not (jdk / "bin" / f"java{tc.EXE}").exists():
        extract_single_root(download(JDK21_URL), jdk)
    props = tc.EXTERNAL / "ghidra" / "support" / "launch.properties"
    if props.exists():
        lines = props.read_text(encoding="utf-8").splitlines()
        value = str(jdk).replace("\\", "\\\\")  # escape the backslashes (.properties)
        lines = [f"JAVA_HOME_OVERRIDE={value}" if l.startswith("JAVA_HOME_OVERRIDE=") else l for l in lines]
        props.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"Ghidra will use {jdk}")


def install_ghidra_gc_loader() -> None:
    ext = tc.EXTERNAL / "ghidra" / "Ghidra" / "Extensions"
    lang = ext / "GameCubeLoader" / "data" / "languages"
    if not (lang / "ppc_gekko_broadway.sla").exists():
        with zipfile.ZipFile(download(GC_LOADER_URL)) as z:
            z.extractall(ext)
        # Compile the SLEIGH specification explicitly (otherwise headless Ghidra does not see the profile).
        sleigh = tc.EXTERNAL / "ghidra" / "support" / ("sleigh.bat" if tc.IS_WINDOWS else "sleigh")
        tc.run([sleigh, lang / "ppc_gekko_broadway.slaspec"])
    else:
        print("ghidra_gc_loader: present")


def install_dolrecomp() -> None:
    repo = tc.EXTERNAL / "DolRecomp"
    git = tc.find_git()
    if not repo.exists():
        tc.run([git, "clone", DOLRECOMP_REPO, repo])
    tc.run([git, "-C", repo, "fetch", "--quiet", "origin"])
    tc.run([git, "-C", repo, "checkout", "--quiet", DOLRECOMP_COMMIT])
    tc.run([git, "-C", repo, "submodule", "update", "--init", "--recursive"])
    cmake = tc.find_cmake()
    tc.run([cmake, "-S", repo, "-B", repo / "build"], env=tc.msvc_env())
    tc.run([cmake, "--build", repo / "build", "--config", "Release", "--parallel"], env=tc.msvc_env())


def install_aurora() -> None:
    """Pinned Aurora, built in external/aurora/build-clang (clang-cl + Ninja).

    The runtime includes it through add_subdirectory(); this separate build checks that the
    toolchain and the prebuilt packages (Dawn, SDL3) work on the machine.
    Expected result: external/aurora/build-clang/examples/simple.exe.
    """
    repo = tc.EXTERNAL / "aurora"
    git = tc.find_git()
    if not repo.exists():
        tc.run([git, "clone", AURORA_REPO, repo])
    tc.run([git, "-C", repo, "fetch", "--quiet", "origin"])
    tc.run([git, "-C", repo, "checkout", "--quiet", AURORA_COMMIT])
    clang_cl = tc.find_clang_cl()
    ninja = tc.find_ninja()
    if not clang_cl or not ninja:
        raise SystemExit("aurora: run the llvm and ninja steps first")
    build = repo / "build-clang"
    env = tc.msvc_env()
    cmake = tc.find_cmake()
    tc.run([cmake, "-S", repo, "-B", build, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={ninja}",
            f"-DCMAKE_C_COMPILER={clang_cl}", f"-DCMAKE_CXX_COMPILER={clang_cl}", *AURORA_CMAKE_OPTIONS], env=env)
    tc.run([cmake, "--build", build, "--parallel"], env=env)


STEPS = {"ninja": install_ninja, "llvm": install_llvm, "ghidra": install_ghidra, "jdk21": install_jdk21,
         "ghidra_gc_loader": install_ghidra_gc_loader, "dolrecomp": install_dolrecomp, "aurora": install_aurora}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--only", help="comma-separated list of steps: " + ",".join(STEPS))
    args = parser.parse_args()
    steps = args.only.split(",") if args.only else list(STEPS)
    for name in steps:
        print(f"== {name}")
        STEPS[name]()
    print("Done. Check with: py -3 tools/doctor.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
