# Platform: window, rendering and input through Aurora (SDL3, Dawn), traces, PNG captures.
#
# Aurora (external/aurora, commit pinned by tools/bootstrap.py) is built as a sub-project.
# Only its own downloaded dependencies are used: without these settings, find_package()
# finds libraries through the PATH (e.g. fmt and zstd from a conda installation), hence a
# non-reproducible build (observed on 2026-10-09). Variables with a scope local to the
# runtime/core folder (include() does not create a scope, add_subdirectory inherits it).
set(GCRT_AURORA_DIR "${CMAKE_SOURCE_DIR}/external/aurora" CACHE PATH "Aurora sources")
if(NOT EXISTS "${GCRT_AURORA_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "Aurora not found in ${GCRT_AURORA_DIR} (py -3 tools/bootstrap.py --only aurora)")
endif()
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "Aurora DVD layer (nod) unused: DVD functions replaced natively" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${GCRT_AURORA_DIR}" "${CMAKE_BINARY_DIR}/aurora" EXCLUDE_FROM_ALL)

target_sources(gcrt PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/trace.c"
  "${CMAKE_CURRENT_LIST_DIR}/png_writer.c"
  "${CMAKE_CURRENT_LIST_DIR}/aurora_host.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/platform.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/input_config.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/input_host.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/user_dir.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/save_file.c"
  "${CMAKE_CURRENT_LIST_DIR}/crash_handler.cpp"
)
# PRIVATE: Aurora's compile options (C++20, /Zc:__cplusplus...) do not propagate
# to the generated code; linking, however, does follow (static library).
target_link_libraries(gcrt PRIVATE aurora::core aurora::gx aurora::vi aurora::pad)
target_compile_features(gcrt PRIVATE cxx_std_20)
if(WIN32)
  target_link_libraries(gcrt PRIVATE bcrypt) # SHA-1 of the executable in crash reports (crash_handler.cpp)
endif()

# Build identification printed by crash reports: commit of the source tree (cmake/GcrtBuildInfo.cmake). Written once at
# configuration (the file exists before the first build), then checked at every build; rewritten only when it changes.
set(GCRT_BUILD_INFO_C "${CMAKE_BINARY_DIR}/gcrt_build_info.c")
set(GCRT_BUILD_INFO_CMD "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DOUTPUT=${GCRT_BUILD_INFO_C}"
    -P "${GCRT_CORE_DIR}/cmake/GcrtBuildInfo.cmake")
execute_process(COMMAND ${GCRT_BUILD_INFO_CMD})
add_custom_target(gcrt_build_info COMMAND ${GCRT_BUILD_INFO_CMD} BYPRODUCTS "${GCRT_BUILD_INFO_C}" VERBATIM)
add_dependencies(gcrt gcrt_build_info)
target_sources(gcrt PRIVATE "${GCRT_BUILD_INFO_C}")

include("${GCRT_AURORA_DIR}/cmake/AuroraCopyRuntimeDLLs.cmake")
# Called by gcrt_add_recompiled_executable (cmake/GcrtRecomp.cmake): Aurora's DLLs (SDL3, Dawn,
# shader compiler, libpng, zlib) copied next to the executable.
function(gcrt_platform_setup_executable target)
  aurora_copy_runtime_dlls(${target})
endfunction()
