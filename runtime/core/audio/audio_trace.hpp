// Diagnostic traces of the audio subsystem (generic GameCube, no reference to any game).
//
// Enabled by the GCRT_TRACE environment variable (the same as platform/trace.h), with these names:
//   dsp       DSP control register, microcode changes
//   dspmail   every CPU <-> DSP mail
//   ax        AX command lists (one line per command)
//   axvoice   every voice processed (very verbose: 1 line per active voice every 5 ms)
//   aidma     audio DMA (RAM -> output), AICR register
//   audioout  host output (SDL, WAV): latency, underrun, losses
//   audio     = dsp + dspmail + ax + aidma + audioout (without axvoice)
//   all       everything
// The "audiowarn" category (anomalies: unknown microcode, unknown AX command, address outside RAM)
// is enabled by default; "none" turns everything off.
//
// Output: stderr by default. The integration can redirect to platform/trace (gcrt_trace_printf)
// with set_trace_sink() to get a single file (GCRT_TRACE_FILE).
//
// Self-contained module (no dependency on the rest of the runtime) so that it can be tested alone.
#pragma once

#include <cstdint>

namespace gcrt::audio {

enum class TraceCat : uint32_t {
  Warn = 0,   // anomalies (enabled by default)
  Dsp = 1,    // DSP control, microcodes
  Mail = 2,   // mails
  Ax = 3,     // AX commands
  Voice = 4,  // AX voices (very verbose)
  AiDma = 5,  // audio DMA, AICR
  Output = 6, // host output
  Count
};

extern uint32_t g_audio_trace_mask;

inline bool trace_on(TraceCat cat) { return ((g_audio_trace_mask >> static_cast<uint32_t>(cat)) & 1u) != 0; }

// Reads GCRT_TRACE. Called automatically by the microcodes (gcrt_audio_register_ucodes) and the outputs; idempotent.
void trace_init_from_env();
// Enables (or disables) a category by name (see the list above). Returns false if unknown.
bool trace_enable(const char* name, bool on = true);

// Output redirection. `category`: short name ("dsp", "ax"...). nullptr = stderr.
using TraceSink = void (*)(void* user, const char* category, const char* message);
void set_trace_sink(TraceSink sink, void* user);

void tracef(TraceCat cat, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

} // namespace gcrt::audio

#define GCRT_AUDIO_TRACE(cat, ...)                                                                                     \
  do {                                                                                                                 \
    if (::gcrt::audio::trace_on(cat)) {                                                                                \
      ::gcrt::audio::tracef((cat), __VA_ARGS__);                                                                       \
    }                                                                                                                  \
  } while (0)
