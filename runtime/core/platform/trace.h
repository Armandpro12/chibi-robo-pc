/*
 * Diagnostic traces that can be switched on per category (generic, no dependency).
 *
 * Activation, either:
 *   - environment variable GCRT_TRACE="gx,dvd,card" (or "all");
 *   - call to gcrt_trace_enable("card") from code (e.g. the game configuration).
 * Output: stderr, or the file designated by GCRT_TRACE_FILE.
 *
 * Cost when the category is disabled: one bit test (GCRT_TRACE macro).
 */
#ifndef GCRT_PLATFORM_TRACE_H
#define GCRT_PLATFORM_TRACE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GcrtTraceCat {
  GCRT_TRACE_GX = 0, /* GX command stream, bridge to Aurora */
  GCRT_TRACE_UNHANDLED = 1, /* anomalies that must show up: access outside RAM that nothing serves, data outside RAM,
                               unknown command (enabled by default) */
  GCRT_TRACE_IRQ = 2, /* interrupts (PI) */
  GCRT_TRACE_VI = 3, /* presentation and EFB captures */
  GCRT_TRACE_DSP = 4, /* DSP: mails, microcodes, ARAM DMA */
  GCRT_TRACE_AI = 5, /* audio interface */
  GCRT_TRACE_OSREPORT = 6, /* text sent by the game on the IPL's UART (OSReport), enabled by default */
  GCRT_TRACE_PLATFORM = 7, /* window, frames, captures, scripted inputs, report */
  GCRT_TRACE_NIRQ = 8, /* native interrupts (milestone 10): posted, delivered */
  GCRT_TRACE_DVD = 9, /* native disc drive (DVD* functions replaced; alias "di") */
  GCRT_TRACE_PAD = 10, /* native controllers (PAD* and SI* functions replaced; alias "si") */
  GCRT_TRACE_NATIVE = 11, /* native replacements: installation, replaced and missing functions, subsystems */
  GCRT_TRACE_AR = 12, /* native audio memory (AR* functions replaced) */
  GCRT_TRACE_NVI = 13, /* native display (VI* functions replaced): fields, retraces, modes */
  GCRT_TRACE_CACHE = 14, /* native cache operations (DC*, IC*, LC*) */
  GCRT_TRACE_CARD = 15, /* native memory card (CARD* functions replaced: calls, files, results) */
  GCRT_TRACE_OS = 16, /* native OS (milestone 10b): threads (creation, switches, idle), alarms, FPU, interrupts */
  GCRT_TRACE_COUNT
} GcrtTraceCat;

extern uint32_t g_gcrt_trace_mask;

/* Reads GCRT_TRACE / GCRT_TRACE_FILE. To be called once at runtime startup:
   as long as it has not been called, only the "unhandled" category is active. */
void gcrt_trace_init(void);
/* Enables a category by name ("gx", "unhandled", "irq", "vi", "dsp", "ai", "osreport", "platform", "nirq",
   "dvd" (alias "di"), "pad" (alias "si"), "native", "ar", "vin", "cache", "card", "os", "all", "none"). */
bool gcrt_trace_enable(const char* name);
/* Line timestamps (seconds of emulated time), provided by the platform. */
void gcrt_trace_set_clock(double (*seconds)(void* user), void* user);
/* Trace output stream (stderr or GCRT_TRACE_FILE). */
FILE* gcrt_trace_stream(void);
void gcrt_trace_printf(GcrtTraceCat cat, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define GCRT_TRACE_ON(cat) ((g_gcrt_trace_mask >> (unsigned)(cat)) & 1u)
#define GCRT_TRACE(cat, ...)                                                                                           \
  do {                                                                                                                 \
    if (GCRT_TRACE_ON(cat)) {                                                                                          \
      gcrt_trace_printf((cat), __VA_ARGS__);                                                                           \
    }                                                                                                                  \
  } while (0)

#ifdef __cplusplus
}
#endif

#endif /* GCRT_PLATFORM_TRACE_H */
