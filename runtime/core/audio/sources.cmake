# Audio: HLE of the DSP microcodes (GameCube AX, CARD, ROM) plugged into the SDK's native DSP library
# (hle/native_audio.c, dsp_ucode.h), host output (SDL3 through Aurora, WAV recording). Nothing runs until the
# platform plugs them in (see notes/audio.md § 10, integration).
# SDL3: headers and library obtained from aurora::core (PUBLIC link to SDL3), already linked to gcrt.
target_sources(gcrt PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/audio_trace.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/adpcm.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/accelerator.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/ucode.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/ax_gc.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/dsp_glue.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/wav_writer.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/output_queue.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/output_capture.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/sdl_output.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/host_audio.cpp"
)
