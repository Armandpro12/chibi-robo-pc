# CPU layer: DolRecomp helpers (cpu.c, GPL-3.0) + chassis, interrupts, SPR, default bus (no more
# mini-interpreter since milestone 10b).
target_sources(gcrt PRIVATE
  "${DOLRECOMP_DIR}/src/cpu/cpu.c"
  "${CMAKE_CURRENT_LIST_DIR}/cpu_core.c"
  "${CMAKE_CURRENT_LIST_DIR}/chassis.c"
  "${CMAKE_CURRENT_LIST_DIR}/hw_counter.c"
  "${CMAKE_CURRENT_LIST_DIR}/host_profiler.c"
  "${CMAKE_CURRENT_LIST_DIR}/idle_skip.c"
)
if(WIN32)
  # Host profiler (--host-profile): symbols (DbgHelp) and 1 ms timer (winmm).
  target_link_libraries(gcrt PRIVATE dbghelp winmm)
endif()
