# Build of a recompiled GameCube game: generic runtime (runtime/core) + generated code + game-specific code.
# Normally run by tools/build.py, which produces the C code (DolRecomp) and passes the variables below.
# 3.25: required by Aurora (added by runtime/core/platform/sources.cmake); an older CMake stops here, with a clear message.
cmake_minimum_required(VERSION 3.25)
project(gcrecomp LANGUAGES C CXX)

set(GCRT_GAME "" CACHE STRING "Game to build (folder games/<game>)")
set(GCRT_GAME_VERSION "" CACHE STRING "Disc identifier (e.g. GGTP01)")
set(GCRT_RECOMP_DIR "" CACHE PATH "DolRecomp output (generated/<game>/<ID>/recomp)")
set(GCRT_DISC_ROOT "" CACHE PATH "Root of the extracted disc, used by default at launch")
set(GCRT_DOL_FNV64 "0" CACHE STRING "FNV-1a 64 hash of the recompiled DOL (0: no check)")
set(GCRT_RAM_SIZE "0x01800000" CACHE STRING "Size of the host RAM (>= 24 MiB)")
set(DOLRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/DolRecomp" CACHE PATH "DolRecomp sources")
option(GCRT_CPU_FAST_PATHS "Inline fast paths for the generated code (runtime/core/cpu/gcrt_cpu_inline.h)" ON)
# Optional profile-guided optimization (GCRT_PGO, tools/build.py --pgo): before any add_subdirectory, so that the
# options apply to all the C code.
include(runtime/core/cmake/GcrtPgo.cmake)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

add_subdirectory(runtime/core)
include(runtime/core/cmake/GcrtRecomp.cmake)

if(GCRT_GAME)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/${GCRT_GAME}/CMakeLists.txt")
    message(FATAL_ERROR "games/${GCRT_GAME}/CMakeLists.txt not found")
  endif()
  add_subdirectory(games/${GCRT_GAME})
endif()
