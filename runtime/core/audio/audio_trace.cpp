// Diagnostic traces of the audio subsystem (see audio_trace.hpp).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "audio_trace.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gcrt::audio {

uint32_t g_audio_trace_mask = 1u << static_cast<uint32_t>(TraceCat::Warn);

namespace {

const char* const kNames[static_cast<uint32_t>(TraceCat::Count)] = {"audiowarn", "dsp",   "dspmail", "ax",
                                                                     "axvoice",   "aidma", "audioout"};
TraceSink s_sink = nullptr;
void* s_sink_user = nullptr;
std::once_flag s_env_once;
std::mutex s_print_mutex; // the SDL output may trace from its own thread

void set_bit(TraceCat cat, bool on) {
  const uint32_t bit = 1u << static_cast<uint32_t>(cat);
  if (on) {
    g_audio_trace_mask |= bit;
  } else {
    g_audio_trace_mask &= ~bit;
  }
}

} // namespace

bool trace_enable(const char* name, bool on) {
  if (std::strcmp(name, "all") == 0) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(TraceCat::Count); ++i) {
      set_bit(static_cast<TraceCat>(i), on);
    }
    return true;
  }
  if (std::strcmp(name, "none") == 0) {
    g_audio_trace_mask = 0;
    return true;
  }
  if (std::strcmp(name, "audio") == 0) {
    for (TraceCat c : {TraceCat::Warn, TraceCat::Dsp, TraceCat::Mail, TraceCat::Ax, TraceCat::AiDma, TraceCat::Output}) {
      set_bit(c, on);
    }
    return true;
  }
  for (uint32_t i = 0; i < static_cast<uint32_t>(TraceCat::Count); ++i) {
    if (std::strcmp(name, kNames[i]) == 0) {
      set_bit(static_cast<TraceCat>(i), on);
      return true;
    }
  }
  return false;
}

void trace_init_from_env() {
  std::call_once(s_env_once, [] {
    const char* spec = std::getenv("GCRT_TRACE");
    if (spec == nullptr) {
      return;
    }
    char buf[256];
    std::strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    for (char* tok = std::strtok(buf, ", ;"); tok != nullptr; tok = std::strtok(nullptr, ", ;")) {
      // The names of other subsystems (gx, dvd...) are ignored here.
      trace_enable(tok, true);
    }
  });
}

void set_trace_sink(TraceSink sink, void* user) {
  std::lock_guard<std::mutex> lock(s_print_mutex);
  s_sink = sink;
  s_sink_user = user;
}

void tracef(TraceCat cat, const char* fmt, ...) {
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  const uint32_t idx = static_cast<uint32_t>(cat);
  const char* name = idx < static_cast<uint32_t>(TraceCat::Count) ? kNames[idx] : "audio";
  std::lock_guard<std::mutex> lock(s_print_mutex);
  if (s_sink != nullptr) {
    s_sink(s_sink_user, name, msg);
  } else {
    std::fprintf(stderr, "[%s] %s\n", name, msg);
  }
}

} // namespace gcrt::audio
