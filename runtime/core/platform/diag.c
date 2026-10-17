/*
 * Diagnostic event ring and heartbeat (see gcrt_diag.h).
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "platform/gcrt_diag.h"
#include "platform/trace.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

volatile uint64_t g_gcrt_heartbeat = 0;

static GcrtDiagSlot* s_slots = NULL;
static uint32_t s_mask = 0;
static volatile int64_t s_pos = 0;
static int s_init = 0;

enum { kDefaultEvents = 4096, kMaxEvents = 1 << 20 };

void gcrt_diag_init(void) {
  if (s_init) {
    return;
  }
  s_init = 1;
  long n = kDefaultEvents;
  const char* env = getenv("GCRT_DIAG_EVENTS");
  if (env != NULL && env[0] != '\0') {
    n = strtol(env, NULL, 0);
  }
  if (n <= 0) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "diagnostic event ring disabled (GCRT_DIAG_EVENTS=0)");
    return;
  }
  uint32_t size = 16;
  while (size < (uint32_t)n && size < (uint32_t)kMaxEvents) {
    size <<= 1;
  }
  s_slots = (GcrtDiagSlot*)calloc(size, sizeof(GcrtDiagSlot));
  if (s_slots == NULL) {
    return;
  }
  s_mask = size - 1u;
  if (env != NULL && env[0] != '\0') {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "diagnostic event ring: %u events (GCRT_DIAG_EVENTS)", size);
  }
}

void gcrt_diag_event(unsigned kind, uint64_t a, uint64_t b) {
  GcrtDiagSlot* slots = s_slots;
  if (slots == NULL) {
    return;
  }
#if defined(_WIN32)
  const uint64_t index = (uint64_t)InterlockedIncrement64((volatile LONG64*)&s_pos) - 1u;
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  const uint64_t qpc = (uint64_t)t.QuadPart;
  const uint32_t thread = (uint32_t)GetCurrentThreadId();
#else
  const uint64_t index = (uint64_t)__atomic_fetch_add(&s_pos, 1, __ATOMIC_RELAXED);
  const uint64_t qpc = 0;
  const uint32_t thread = 0;
#endif
  GcrtDiagSlot* s = &slots[index & s_mask];
  s->seq = 0; /* being written */
  s->qpc = qpc;
  s->a = a;
  s->b = b;
  s->kind = kind;
  s->thread = thread;
#if defined(_WIN32)
  MemoryBarrier();
#else
  __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
  s->seq = index + 1u; /* published */
}

const char* gcrt_diag_event_name(unsigned kind) {
  static const char* const names[GCRT_EV_COUNT] = {
      "none",        "frame presented", "frame skipped", "display copy", "EFB->texture copy", "pipeline wait",
      "disc read",   "save read",       "save write",    "save remove",  "save move",         "window",
      "setting",     "DSP task",        "audio",         "watchdog",     "note"};
  return kind < GCRT_EV_COUNT ? names[kind] : "?";
}

const GcrtDiagSlot* gcrt_diag_slots(uint32_t* capacity, uint64_t* recorded) {
  *capacity = s_slots != NULL ? s_mask + 1u : 0u;
  *recorded = (uint64_t)s_pos;
  return s_slots;
}
