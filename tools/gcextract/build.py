"""Builds the native disc extractor tools/gcextract (gcextract.exe) with the project's toolchain.

Usage: py -3 tools/gcextract/build.py [--out build/gcextract] [--config Release]
Result: <out>/gcextract.exe (static C/C++ runtime: Windows system DLLs only).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import toolchain as tc  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Builds gcextract.")
    parser.add_argument("--out", default=str(HERE.parent.parent / "build" / "gcextract"), help="build folder")
    parser.add_argument("--config", default="Release", choices=["Release", "Debug"])
    args = parser.parse_args()

    cmake, ninja, clang_cl = tc.find_cmake(), tc.find_ninja(), tc.find_clang_cl()
    if not cmake or not ninja or not clang_cl:
        raise SystemExit("cmake, ninja or clang-cl not found (see tools/doctor.py).")
    env = tc.msvc_env()
    out = Path(args.out)
    tc.run([cmake, "-S", HERE, "-B", out, "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
            f"-DCMAKE_MAKE_PROGRAM={ninja}", f"-DCMAKE_CXX_COMPILER={clang_cl}"], env=env)
    tc.run([cmake, "--build", out], env=env)
    exe = out / ("gcextract.exe" if tc.IS_WINDOWS else "gcextract")
    print(f"Built: {exe}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
