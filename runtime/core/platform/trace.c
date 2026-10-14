/*
 * Switchable diagnostic traces (see trace.h).
 */
/* Standard C functions (fopen, getenv, strtok): no "unsafe" warning from MSVC. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enabled by default: "unhandled" (an access or a command that nothing handles must show up), "osreport"
   (game messages) and "platform" (run report). Can be disabled: GCRT_TRACE=none,... */
uint32_t g_gcrt_trace_mask = (1u << GCRT_TRACE_UNHANDLED) | (1u << GCRT_TRACE_OSREPORT) | (1u << GCRT_TRACE_PLATFORM) |
                             (1u << GCRT_TRACE_NATIVE);

/* One name per category, in the order of GcrtTraceCat (trace.h). */
static const char* const s_names[] = {"gx",  "unhandled", "irq",    "vi", "dsp", "ai",    "osreport", "platform", "nirq",
                                      "dvd", "pad",       "native", "ar", "vin", "cache", "card",     "os"};
_Static_assert(sizeof(s_names) / sizeof(s_names[0]) == GCRT_TRACE_COUNT, "one name per trace category");
/* Old categories of the emulated hardware removed in milestone 10a, kept as aliases of their native replacement. */
static const char* const s_aliases[][2] = {{"di", "dvd"}, {"si", "pad"}};

static FILE* s_out = NULL;
static bool s_init = false;
static double (*s_clock)(void*) = NULL;
static void* s_clock_user = NULL;

void gcrt_trace_set_clock(double (*seconds)(void* user), void* user) {
  s_clock = seconds;
  s_clock_user = user;
}

FILE* gcrt_trace_stream(void) {
  if (!s_init) {
    gcrt_trace_init();
  }
  return s_out;
}

/* Categories read by other modules (audio/audio_trace.hpp): accepted without an error message. */
static const char* const s_external[] = {"audio", "audiowarn", "dspmail", "ax", "axvoice", "aidma", "audioout"};

bool gcrt_trace_enable(const char* name) {
  for (unsigned i = 0; i < sizeof(s_aliases) / sizeof(s_aliases[0]); i++) {
    const char* bare = name[0] == '-' ? name + 1 : name;
    if (strcmp(bare, s_aliases[i][0]) == 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%s%s", name[0] == '-' ? "-" : "", s_aliases[i][1]);
      return gcrt_trace_enable(buf);
    }
  }
  for (unsigned i = 0; i < sizeof(s_external) / sizeof(s_external[0]); i++) {
    if (strcmp(name[0] == '-' ? name + 1 : name, s_external[i]) == 0) {
      return true;
    }
  }
  if (name[0] == '-') { /* "-osreport": disables a category */
    for (unsigned i = 0; i < GCRT_TRACE_COUNT; i++) {
      if (strcmp(name + 1, s_names[i]) == 0) {
        g_gcrt_trace_mask &= ~(1u << i);
        return true;
      }
    }
    return false;
  }
  if (strcmp(name, "all") == 0) {
    g_gcrt_trace_mask = (1u << GCRT_TRACE_COUNT) - 1u;
    return true;
  }
  if (strcmp(name, "none") == 0) {
    g_gcrt_trace_mask = 0;
    return true;
  }
  for (unsigned i = 0; i < GCRT_TRACE_COUNT; i++) {
    if (strcmp(name, s_names[i]) == 0) {
      g_gcrt_trace_mask |= 1u << i;
      return true;
    }
  }
  return false;
}

void gcrt_trace_init(void) {
  if (s_init) {
    return;
  }
  s_init = true;
  s_out = stderr;
  const char* file = getenv("GCRT_TRACE_FILE");
  if (file != NULL && file[0] != '\0') {
    FILE* f = fopen(file, "w");
    if (f != NULL) {
      s_out = f;
    }
  }
  const char* spec = getenv("GCRT_TRACE");
  if (spec == NULL) {
    return;
  }
  char buf[256];
  strncpy(buf, spec, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  for (char* tok = strtok(buf, ",; "); tok != NULL; tok = strtok(NULL, ",; ")) {
    if (!gcrt_trace_enable(tok)) {
      fprintf(stderr, "[trace] unknown category: %s\n", tok);
    }
  }
}

void gcrt_trace_printf(GcrtTraceCat cat, const char* fmt, ...) {
  if (!s_init) {
    gcrt_trace_init();
  }
  if ((unsigned)cat >= GCRT_TRACE_COUNT) {
    return;
  }
  if (s_clock != NULL) {
    fprintf(s_out, "[%9.3f] ", s_clock(s_clock_user));
  }
  fprintf(s_out, "[%s] ", s_names[cat]);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(s_out, fmt, ap);
  va_end(ap);
  fputc('\n', s_out);
  fflush(s_out);
}
