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

/* Tail of the trace output kept in memory for crash reports (gcrt_trace_tail): the last kTailSize bytes of the lines
   written by gcrt_trace_printf, whatever the output stream. Guarded by a tiny spin lock (lines come from several
   threads); the crash report reads it without the lock (a line being written may be cut). */
enum { kTailSize = 32 * 1024 };
static char s_tail[kTailSize];
static volatile long s_tail_pos = 0; /* total bytes ever appended, modulo 2^31 (kTailSize divides it) */
static volatile long s_tail_lock = 0;

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static void tail_lock(void) {
  while (InterlockedExchange(&s_tail_lock, 1) != 0) {
    YieldProcessor();
  }
}
static void tail_unlock(void) { InterlockedExchange(&s_tail_lock, 0); }
#else
static void tail_lock(void) {
  while (__atomic_exchange_n(&s_tail_lock, 1, __ATOMIC_ACQUIRE) != 0) {
  }
}
static void tail_unlock(void) { __atomic_store_n(&s_tail_lock, 0, __ATOMIC_RELEASE); }
#endif

static void tail_append(const char* text, size_t n) {
  tail_lock();
  long pos = s_tail_pos;
  for (size_t i = 0; i < n; i++) {
    s_tail[(unsigned long)pos % kTailSize] = text[i];
    pos = (pos + 1) & 0x7FFFFFFF;
  }
  s_tail_pos = pos;
  tail_unlock();
}

size_t gcrt_trace_tail(char* out, size_t size) {
  if (size == 0) {
    return 0;
  }
  const unsigned long pos = (unsigned long)s_tail_pos;
  const bool wrapped = s_tail[kTailSize - 1] != 0 || pos >= kTailSize;
  unsigned long avail = wrapped ? kTailSize : pos;
  if (avail > size - 1) {
    avail = (unsigned long)(size - 1);
  }
  const unsigned long start = (pos + 0x80000000ul - avail) % kTailSize;
  /* Start at a line boundary when the tail was cut. */
  size_t n = 0;
  bool skipping = wrapped || avail < pos;
  for (unsigned long i = 0; i < avail; i++) {
    const char c = s_tail[(start + i) % kTailSize];
    if (skipping) {
      skipping = c != '\n';
      continue;
    }
    out[n++] = c;
  }
  out[n] = 0;
  return n;
}

void gcrt_trace_printf(GcrtTraceCat cat, const char* fmt, ...) {
  if (!s_init) {
    gcrt_trace_init();
  }
  if ((unsigned)cat >= GCRT_TRACE_COUNT) {
    return;
  }
  /* Same bytes as before (prefix, message, newline), formatted once into a local buffer so that they can also go to
     the in-memory tail; a message too long for the buffer is written directly and only its beginning is kept. */
  char line[1536];
  int len = 0;
  if (s_clock != NULL) {
    len = snprintf(line, sizeof(line), "[%9.3f] ", s_clock(s_clock_user));
  }
  len += snprintf(line + len, sizeof(line) - (size_t)len, "[%s] ", s_names[cat]);
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  const int body = vsnprintf(line + len, sizeof(line) - (size_t)len, fmt, ap);
  va_end(ap);
  if (body >= 0 && (size_t)(len + body) < sizeof(line) - 1) {
    len += body;
    line[len++] = '\n';
    fwrite(line, 1, (size_t)len, s_out);
    tail_append(line, (size_t)len);
  } else {
    fwrite(line, 1, (size_t)len, s_out);
    vfprintf(s_out, fmt, ap2);
    fputc('\n', s_out);
    tail_append(line, strlen(line));
    tail_append(" [...]\n", 7);
  }
  va_end(ap2);
  fflush(s_out);
}
