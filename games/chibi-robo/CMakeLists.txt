# Chibi-Robo!: native executable = generic runtime (gcrt) + code recompiled by DolRecomp + this folder.
gcrt_add_recompiled_executable(chibi-robo
  SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/chibi-robo.rc"
)
# The resource script embeds the icon (chibi-robo.ico, made by tools/make_icon.py from icon.toml): rebuild the
# resources when the icon changes (the resource compiler does not report the files it reads to the build tool).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/chibi-robo.rc" PROPERTIES
  OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/chibi-robo.ico")
