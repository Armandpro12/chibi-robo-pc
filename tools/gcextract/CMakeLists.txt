# gcextract: standalone native extractor for GameCube disc images (used by the player installer, no Python needed).
# Generic tool: no game is named here; the accepted images come from games/<game>/game.toml on the command line.
# Build (Windows, from a vcvars64 environment): cmake -S tools/gcextract -B build/gcextract -G Ninja
#   -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ; cmake --build build/gcextract
# tools/gcextract/build.py does exactly that with the project's toolchain.
cmake_minimum_required(VERSION 3.25)
project(gcextract LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C/C++ runtime: the executable depends on Windows system DLLs only (no Visual C++ Redistributable needed).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(gcextract gcextract.cpp sha1.hpp)
if(MSVC)
  target_compile_definitions(gcextract PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
  target_compile_options(gcextract PRIVATE /W4 /utf-8)
else()
  target_compile_options(gcextract PRIVATE -Wall -Wextra)
endif()
