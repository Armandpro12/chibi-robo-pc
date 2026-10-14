# Generic GameCube runtime (static library gcrt). No reference to any specific game here.
#
# Each subsystem (cpu/, boot/, hle/, gx/, audio/, platform/...) declares its own sources in
# `<subfolder>/sources.cmake` (e.g. `target_sources(gcrt PRIVATE ...)`), included automatically:
# adding a subsystem does not require changing this file.
if(NOT EXISTS "${DOLRECOMP_DIR}/src/cpu/cpu.c")
  message(FATAL_ERROR "DolRecomp not found in ${DOLRECOMP_DIR} (see tools/bootstrap.py)")
endif()

set(GCRT_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Root of the generic runtime")

add_library(gcrt STATIC)
# Order: runtime/core first ("cpu/gcrt_cpu.h", "boot/gcrt_boot.h"...), then the DolRecomp sources
# ("cpu/cpu.h", "common/types.h"). Do not create a runtime/core/cpu/cpu.h file (it would hide DolRecomp's).
target_include_directories(gcrt PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${DOLRECOMP_DIR}/src"
)

file(GLOB GCRT_SUBSYSTEM_LISTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/sources.cmake")
list(SORT GCRT_SUBSYSTEM_LISTS)
foreach(subsystem_list IN LISTS GCRT_SUBSYSTEM_LISTS)
  include("${subsystem_list}")
endforeach()
