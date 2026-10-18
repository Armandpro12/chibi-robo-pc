/*
 * Diagnostic history of the runtime (generic GameCube): a ring of recent native events and a heartbeat of the game
 * loop, read by the crash handler and the hang watchdog (platform/gcrt_crash.h).
 *
 * Event ring: fixed-size ring of the last GCRT_DIAG_EVENTS events (default 4096, power of two, 0 disables it),
 * allocated once by gcrt_diag_init. Lock-free: a writer takes a slot with one atomic increment and publishes it by
 * writing its sequence number last; a reader (crash report) skips a slot whose sequence number does not match (slot
 * being written or already overwritten). Any thread may record. Cost of one event: one atomic increment, one time
 * stamp (QueryPerformanceCounter) and a 40-byte store; recorded only at coarse places (frames, copies, disc reads,
 * save files, window events, settings, audio tasks), never per draw or per block.
 *
 * Heartbeat: a counter incremented once per emulated frame by the platform (presented or skipped). The watchdog
 * compares it over time.
 */
#ifndef GCRT_DIAG_H
#define GCRT_DIAG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event kinds. `a` and `b` meanings are given in the comment; keep gcrt_diag_event_name in sync. */
typedef enum GcrtDiagKind {
  GCRT_EV_NONE = 0,
  GCRT_EV_FRAME_PRESENTED, /* a = frame number (1 = first), b = VI fields */
  GCRT_EV_FRAME_SKIPPED,   /* first skipped frame of a run, then one in 64: a = skipped frames so far,
                              b = length of the current run of skipped frames */
  GCRT_EV_DISPLAY_COPY,    /* a = XFB physical address, b = (width << 16) | height */
  GCRT_EV_EFB_COPY,        /* EFB -> texture copy: a = destination physical address, b = (width << 16) | height */
  GCRT_EV_PIPELINE_WAIT,   /* a = microseconds waited, b = pipelines still queued */
  GCRT_EV_DISC_READ,       /* a = disc offset, b = (guest address << 32) | length */
  GCRT_EV_SAVE_READ,       /* a = bytes, b = 1 ok / 0 failed */
  GCRT_EV_SAVE_WRITE,      /* a = bytes, b = 1 ok / 0 failed */
  GCRT_EV_SAVE_REMOVE,     /* b = 1 ok / 0 failed */
  GCRT_EV_SAVE_MOVE,       /* b = 1 ok / 0 failed */
  GCRT_EV_WINDOW,          /* a = window event (GcrtDiagWindow), b = (width << 32) | height when relevant */
  GCRT_EV_SETTING,         /* a = setting (GcrtDiagSetting), b = value (float settings: value x 1000) */
  GCRT_EV_DSP_TASK,        /* a = microcode fingerprint, b = guest task address */
  GCRT_EV_AUDIO,           /* a = audio event (GcrtDiagAudio), b = value */
  GCRT_EV_WATCHDOG,        /* a = seconds without progress */
  GCRT_EV_NOTE,            /* a, b: free (tests, temporary diagnostics) */
  GCRT_EV_COUNT
} GcrtDiagKind;

typedef enum GcrtDiagWindow {
  GCRT_EVW_RESIZED = 1,
  GCRT_EVW_SCALE_CHANGED = 2,
  GCRT_EVW_CLOSE_REQUESTED = 3,
  GCRT_EVW_OPENED = 4,
} GcrtDiagWindow;

typedef enum GcrtDiagSetting {
  GCRT_EVS_RENDER_SCALE = 1, /* b = scale x 1000 */
  GCRT_EVS_FULLSCREEN = 2,   /* b = 0/1 */
  GCRT_EVS_WINDOW_SIZE = 3,  /* b = (width << 32) | height */
  GCRT_EVS_EFB_SIZE = 4,     /* b = (width << 32) | height */
} GcrtDiagSetting;

typedef enum GcrtDiagAudio {
  GCRT_EVA_OUTPUT_OPENED = 1, /* host output opened: b = (input rate << 32) | device rate, in Hz */
  GCRT_EVA_DSP_INIT = 2,      /* first DSPInit of the game (native replacement) */
} GcrtDiagAudio;

/* One slot of the ring (40 bytes). `seq` = index + 1 once published. */
typedef struct GcrtDiagSlot {
  volatile uint64_t seq;
  uint64_t qpc; /* QueryPerformanceCounter (Windows), 0 elsewhere */
  uint64_t a;
  uint64_t b;
  uint32_t kind;
  uint32_t thread; /* host thread id */
} GcrtDiagSlot;

/* Reads GCRT_DIAG_EVENTS and allocates the ring. Called once at startup (idempotent). */
void gcrt_diag_init(void);
/* Records one event (no-op when the ring is disabled or not yet allocated). Thread-safe, lock-free. */
void gcrt_diag_event(unsigned kind, uint64_t a, uint64_t b);
/* Name of an event kind ("?" if unknown). */
const char* gcrt_diag_event_name(unsigned kind);
/* Read access for the crash report: slots, capacity (0 if disabled) and total number of events recorded. */
const GcrtDiagSlot* gcrt_diag_slots(uint32_t* capacity, uint64_t* recorded);

/* Heartbeat of the game loop (one per emulated frame). */
extern volatile uint64_t g_gcrt_heartbeat;
static inline void gcrt_diag_heartbeat(void) { g_gcrt_heartbeat = g_gcrt_heartbeat + 1; }

#ifdef __cplusplus
}
#endif

#endif /* GCRT_DIAG_H */
