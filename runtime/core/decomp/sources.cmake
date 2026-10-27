# Decompiled-code support (stage 2): gc_mem.h (guest memory types, header only) and the gateway between recompiled
# and decompiled functions (gc_abi.h, gc_abi.cpp). The gateway leaves the executable with the recompiled code (D3).
target_sources(gcrt PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/gc_abi.cpp"
)
# Decompilation oracle (option GCRT_DECOMP_ORACLE, tools/build.py --oracle): gc_oracle.cpp and the gc_mem.h observer,
# in the oracle executable only. Without the option, neither the file nor the definitions are built (no code, no cost).
if(GCRT_DECOMP_ORACLE)
  target_sources(gcrt PRIVATE "${CMAKE_CURRENT_LIST_DIR}/gc_oracle.cpp")
  target_compile_definitions(gcrt PUBLIC GC_DECOMP_ORACLE GC_MEM_OBSERVER)
  if(GCRT_DECOMP_NEGATIVE_DIR)
    target_compile_definitions(gcrt PUBLIC GC_DECOMP_ORACLE_NEGATIVE)
  endif()
endif()
