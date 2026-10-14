# HLE: SDK functions recognized by signature (catalog sdk_signatures.tsv, without addresses) and replaced
# by the host (OSReport/vprintf/OSPanic log, OSLink/OSUnlink for fixed-address RELs).
# The TSV catalog is converted into a C array at configure time.
set(_hle_tsv "${CMAKE_CURRENT_LIST_DIR}/sdk_signatures.tsv")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_hle_tsv}")
file(STRINGS "${_hle_tsv}" _hle_lines REGEX "^[^#]")
set(GCRT_SDK_CATALOG "")
set(GCRT_SDK_CATALOG_COUNT 0)
foreach(_line IN LISTS _hle_lines)
  string(REPLACE "\t" ";" _f "${_line}")
  list(LENGTH _f _n)
  if(_n LESS 6)
    continue()
  endif()
  list(GET _f 0 _name)
  list(GET _f 1 _size)
  list(GET _f 2 _crc)
  list(GET _f 3 _key)
  list(GET _f 4 _mask)
  list(GET _f 5 _flags)
  set(_fl "0u")
  if(_flags MATCHES "hook")
    set(_fl "GCRT_SIG_HOOK")
  endif()
  string(APPEND GCRT_SDK_CATALOG "    { \"${_name}\", 0x${_size}u, 0x${_crc}u, 0x${_key}u, 0x${_mask}u, ${_fl} },\n")
  math(EXPR GCRT_SDK_CATALOG_COUNT "${GCRT_SDK_CATALOG_COUNT} + 1")
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/sdk_catalog.c.in" "${CMAKE_CURRENT_BINARY_DIR}/gcrt_sdk_catalog.c" @ONLY)

target_sources(gcrt PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/gcrt_sdk_catalog.c"
  "${CMAKE_CURRENT_LIST_DIR}/hle.c"
  "${CMAKE_CURRENT_LIST_DIR}/guest_printf.c"
  "${CMAKE_CURRENT_LIST_DIR}/os_report.c"
  "${CMAKE_CURRENT_LIST_DIR}/oslink.c"
  # Milestone 10: native replacements (installation, native interrupts, DVD, PAD).
  "${CMAKE_CURRENT_LIST_DIR}/native.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_irq.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_dvd.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_pad.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_ar.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_vi.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_cache.c"
  # Milestone 10b: native OS (threads, scheduling, contexts, lazy FPU, messages, idle; alarms).
  "${CMAKE_CURRENT_LIST_DIR}/native_os.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_os_alarm.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_os_sys.c"
  # Milestone 7: native memory card (card model over a folder of .gci files, CARD API).
  "${CMAKE_CURRENT_LIST_DIR}/card_model.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_card.c"
  # Milestone 10c: native GX drawing (SDK state kept by the host, command stream to the Aurora bridge).
  "${CMAKE_CURRENT_LIST_DIR}/native_gx.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_gx_attr.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_gx_tev.c"
  "${CMAKE_CURRENT_LIST_DIR}/native_gx_draw.c"
  # Step 10d: native audio (SDK AI and DSP functions; audio DMA and mailbox kept by the host).
  "${CMAKE_CURRENT_LIST_DIR}/native_audio.c"
)
