# Optional standalone build of the CPU stress test (not part of the game build):
#   cmake -S tools/cpustress -B build/cpustress -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/cpustress
cmake_minimum_required(VERSION 3.20)
project(cpustress CXX)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "cpustress needs Windows x64 (it generates x86-64 code and uses structured exception handling)")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static runtime: the .exe runs on a machine without redistributables
add_executable(cpustress cpustress.cpp)
target_compile_options(cpustress PRIVATE /W4 /EHsc)
