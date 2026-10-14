# Chibi-Robo!: native executable = generic runtime (gcrt) + code recompiled by DolRecomp + this folder.
gcrt_add_recompiled_executable(chibi-robo
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
)
