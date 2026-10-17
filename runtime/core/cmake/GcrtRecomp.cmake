# gcrt_add_recompiled_executable(<target> [RECOMP_DIR dir] [SOURCES ...] [LIBRARIES ...])
#
# Creates the executable of a recompiled game:
#   - each folder of `RECOMP_DIR` containing a DolRecomp `generated.h` is a module (the DOL, a REL);
#     its `chunks/*.c` go into the static library `<target>_recomp`;
#   - for each module, a small glue file (cpu/module_glue.c.in) exposes its block lookup;
#   - a registry (cpu/module_registry.c.in) lists the modules and the build parameters
#     (GCRT_GAME, GCRT_GAME_VERSION, GCRT_DISC_ROOT, GCRT_DOL_FNV64, GCRT_RAM_SIZE);
#   - the executable links gcrt, the generated code, the game SOURCES (which define main) and LIBRARIES.
# Modules are discovered at configure time: rerun CMake after a regeneration (tools/build.py does it).
function(gcrt_add_recompiled_executable target)
  cmake_parse_arguments(ARG "" "RECOMP_DIR" "SOURCES;LIBRARIES" ${ARGN})
  if(NOT ARG_RECOMP_DIR)
    set(ARG_RECOMP_DIR "${GCRT_RECOMP_DIR}")
  endif()
  if(NOT ARG_RECOMP_DIR OR NOT IS_DIRECTORY "${ARG_RECOMP_DIR}")
    message(FATAL_ERROR "Recompiled code not found (\"${ARG_RECOMP_DIR}\"): run tools/build.py <game>")
  endif()

  file(GLOB_RECURSE headers LIST_DIRECTORIES false "${ARG_RECOMP_DIR}/*/generated.h")
  list(SORT headers)
  set(glue_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_glue")
  set(glue_sources "")
  set(chunk_sources "")
  set(GCRT_MODULE_DECLS "")
  set(GCRT_MODULE_ENTRIES "")
  set(GCRT_MODULE_COUNT 0)
  foreach(header IN LISTS headers)
    get_filename_component(module_dir "${header}" DIRECTORY)
    file(GLOB chunks LIST_DIRECTORIES false "${module_dir}/chunks/*.c")
    if(NOT chunks)
      continue()
    endif()
    # Module name: folder of the generated.h ("ahk_1" for a REL), or its parent if the folder
    # is called "generated" (the DOL: <recomp>/dol/generated).
    get_filename_component(GCRT_MODULE_NAME "${module_dir}" NAME)
    if(GCRT_MODULE_NAME STREQUAL "generated")
      get_filename_component(parent_dir "${module_dir}" DIRECTORY)
      get_filename_component(GCRT_MODULE_NAME "${parent_dir}" NAME)
    endif()
    string(MAKE_C_IDENTIFIER "${GCRT_MODULE_NAME}" GCRT_MODULE_ID)
    set(GCRT_MODULE_HEADER "${header}")
    configure_file("${GCRT_CORE_DIR}/cpu/module_glue.c.in" "${glue_dir}/module_${GCRT_MODULE_ID}.c" @ONLY)
    list(APPEND glue_sources "${glue_dir}/module_${GCRT_MODULE_ID}.c")
    list(APPEND chunk_sources ${chunks})
    string(APPEND GCRT_MODULE_DECLS "extern const GcrtModule gcrt_module_${GCRT_MODULE_ID};\n")
    string(APPEND GCRT_MODULE_ENTRIES "    &gcrt_module_${GCRT_MODULE_ID},\n")
    math(EXPR GCRT_MODULE_COUNT "${GCRT_MODULE_COUNT} + 1")
  endforeach()
  if(GCRT_MODULE_COUNT EQUAL 0)
    message(FATAL_ERROR "No recompiled module in ${ARG_RECOMP_DIR}: run tools/build.py <game>")
  endif()
  list(LENGTH chunk_sources chunk_count)
  message(STATUS "${target}: ${GCRT_MODULE_COUNT} recompiled modules, ${chunk_count} generated C files")

  set(GCRT_GAME "${GCRT_GAME}")
  set(GCRT_GAME_VERSION "${GCRT_GAME_VERSION}")
  set(GCRT_DISC_ROOT "${GCRT_DISC_ROOT}")
  set(GCRT_DOL_FNV64 "${GCRT_DOL_FNV64}")
  set(GCRT_RAM_SIZE "${GCRT_RAM_SIZE}")

  # REL layout (one line: id base size fixSize bss bss_size relocated_tail slot_end crc32 name)
  # and HLE-hooked entry points (one address per line), written by tools/build.py.
  set(GCRT_REL_LAYOUT "")
  set(GCRT_REL_COUNT 0)
  set(rel_layout_file "${ARG_RECOMP_DIR}/rel_layout.txt")
  if(EXISTS "${rel_layout_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rel_layout_file}")
    file(STRINGS "${rel_layout_file}" rel_lines REGEX "^[0-9]")
    foreach(line IN LISTS rel_lines)
      separate_arguments(f UNIX_COMMAND "${line}")
      list(GET f 0 id)
      list(GET f 1 base)
      list(GET f 2 size)
      list(GET f 3 fix)
      list(GET f 4 bss)
      list(GET f 5 bss_size)
      list(GET f 6 tail)
      list(GET f 7 slot_end)
      list(GET f 8 crc)
      list(GET f 9 name)
      string(APPEND GCRT_REL_LAYOUT "    { ${id}u, 0x${base}u, 0x${size}u, 0x${fix}u, 0x${bss}u, 0x${bss_size}u, "
                                    "0x${tail}u, 0x${slot_end}u, 0x${crc}u, \"${name}\" },\n")
      math(EXPR GCRT_REL_COUNT "${GCRT_REL_COUNT} + 1")
    endforeach()
  endif()
  set(GCRT_HLE_HOOKS "")
  set(GCRT_HLE_HOOK_COUNT 0)
  set(hooks_file "${ARG_RECOMP_DIR}/hle_hooks.txt")
  if(EXISTS "${hooks_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${hooks_file}")
    file(STRINGS "${hooks_file}" hook_lines REGEX "^[0-9A-Fa-f]")
    foreach(line IN LISTS hook_lines)
      separate_arguments(f UNIX_COMMAND "${line}")
      list(GET f 0 address)
      string(APPEND GCRT_HLE_HOOKS "    0x${address}u,\n")
      math(EXPR GCRT_HLE_HOOK_COUNT "${GCRT_HLE_HOOK_COUNT} + 1")
    endforeach()
  endif()
  configure_file("${GCRT_CORE_DIR}/cpu/module_registry.c.in" "${glue_dir}/module_registry.c" @ONLY)

  # Generated code: always optimized (at -O0, the 16 KiB functions of guest code have huge stack
  # frames), without warnings (translated machine code, not hand-written code).
  add_library(${target}_recomp STATIC ${chunk_sources})
  target_link_libraries(${target}_recomp PRIVATE gcrt)
  # Inline fast paths (RAM accesses, non-quantized psq_l/psq_st): substitutable header provided for by DolRecomp,
  # included by each generated.h instead of cpu/cpu.h; bit-identical result (cpu/gcrt_cpu_inline.h).
  # -DGCRT_CPU_FAST_PATHS=OFF: calls into cpu.c as before (measurements, diagnostics).
  if(GCRT_CPU_FAST_PATHS)
    target_compile_definitions(${target}_recomp PRIVATE "DOLRECOMP_CPU_HEADER=\"cpu/gcrt_cpu_inline.h\"")
  endif()
  if(MSVC AND GCRT_PGO STREQUAL "USE")
    # With a profile: /w would also silence the stale-profile warning (verified); the other warnings are
    # turned off another way and that one is kept (counted by tools/build.py).
    target_compile_options(${target}_recomp PRIVATE /O2 /GS- -Wno-everything -Wprofile-instr-out-of-date)
  elseif(MSVC)
    target_compile_options(${target}_recomp PRIVATE /O2 /w /GS-)
  else()
    target_compile_options(${target}_recomp PRIVATE -O2 -w -fno-strict-aliasing)
  endif()

  add_executable(${target} ${ARG_SOURCES} ${glue_sources} "${glue_dir}/module_registry.c")
  target_link_libraries(${target} PRIVATE gcrt ${target}_recomp ${ARG_LIBRARIES})
  if(MSVC)
    # Main thread stack: 16 MiB (headroom for the nested calls of the generated code).
    target_link_options(${target} PRIVATE /STACK:16777216)
  endif()
  if(WIN32)
    # Windows-subsystem program: Windows opens no console before main(); the runtime opens one when the game starts,
    # if asked (platform/gcrt_console.h). /ENTRY:mainCRTStartup keeps main() as the entry point (the default entry of
    # /SUBSYSTEM:WINDOWS calls WinMain).
    set_target_properties(${target} PROPERTIES WIN32_EXECUTABLE ON)
    if(MSVC)
      target_link_options(${target} PRIVATE /ENTRY:mainCRTStartup)
    endif()
  endif()
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  # Platform-specific settings (Aurora DLLs copied next to the executable), platform/sources.cmake.
  if(COMMAND gcrt_platform_setup_executable)
    gcrt_platform_setup_executable(${target})
  endif()
endfunction()
