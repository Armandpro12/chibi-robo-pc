# Decompiled-code support (stage 2): gc_mem.h (guest memory types, header only) and the gateway between recompiled
# and decompiled functions (gc_abi.h, gc_abi.cpp). The gateway leaves the executable with the recompiled code (D3).
target_sources(gcrt PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/gc_abi.cpp"
)
