# Build identification (generic). Run in script mode at every build (target gcrt_build_info, platform/sources.cmake):
#   cmake -DSOURCE_DIR=<source tree> -DOUTPUT=<file.c> -P GcrtBuildInfo.cmake
# Writes a C file that defines `const char gcrt_build_commit[]`: the abbreviated commit of SOURCE_DIR, with "-dirty" when
# tracked files have uncommitted changes, or "unknown" without Git (source archive). Crash reports print it
# (crash_handler.cpp). The file is rewritten only when its content changes, so an unchanged commit recompiles nothing.
find_program(GCRT_GIT NAMES git)
set(commit "unknown")
if(GCRT_GIT)
  # --no-optional-locks: no index refresh, hence no index.lock conflict when several builds run at the same time.
  execute_process(COMMAND "${GCRT_GIT}" --no-optional-locks -C "${SOURCE_DIR}" rev-parse --short=12 HEAD
                  OUTPUT_VARIABLE head RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(rc EQUAL 0 AND NOT head STREQUAL "")
    set(commit "${head}")
    execute_process(COMMAND "${GCRT_GIT}" --no-optional-locks -C "${SOURCE_DIR}" status --porcelain --untracked-files=no
                    OUTPUT_VARIABLE changes RESULT_VARIABLE rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(rc EQUAL 0 AND NOT changes STREQUAL "")
      string(APPEND commit "-dirty")
    endif()
  endif()
endif()
set(content "/* Generated at build time by runtime/core/cmake/GcrtBuildInfo.cmake: do not edit. */\n")
string(APPEND content "const char gcrt_build_commit[] = \"${commit}\";\n")
set(old "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL content)
  file(WRITE "${OUTPUT}" "${content}")
endif()
