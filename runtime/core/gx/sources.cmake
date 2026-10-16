# Bridge from the GX command stream to Aurora's command processor, EFB capture.
target_sources(gcrt PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/gx_bridge.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/efb_capture.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/xfb_filter.cpp"
)
# gx_bridge.cpp, xfb_filter.cpp and efb_capture.cpp use Aurora's internal API (lib/gx/fifo.hpp, lib/webgpu/gpu.hpp,
# lib/dolphin/vi/vi_internal.hpp): include path limited to these files.
# (Same folder as in platform/sources.cmake, included later: the path is recomputed here.)
set(GCRT_AURORA_DIR "${CMAKE_SOURCE_DIR}/external/aurora" CACHE PATH "Aurora sources")
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/gx_bridge.cpp" "${CMAKE_CURRENT_LIST_DIR}/xfb_filter.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/efb_capture.cpp" PROPERTIES
  INCLUDE_DIRECTORIES "${GCRT_AURORA_DIR}/lib")
