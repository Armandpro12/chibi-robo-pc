# Boot: DOL loading, HLE boot (low globals), gcrt_main entry point.
target_sources(gcrt PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/dol.c"
  "${CMAKE_CURRENT_LIST_DIR}/boot.c"
)
