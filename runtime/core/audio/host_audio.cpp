// Host audio output (see host_audio.hpp).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "host_audio.hpp"

#include <cstdlib>
#include <cstring>

#include "audio_hw.h"
#include "audio_trace.hpp"
#include "ucode.hpp"

namespace gcrt::audio {

HostAudio::Config HostAudio::config_from_env() {
  Config c;
  if (const char* v = std::getenv("GCRT_AUDIO")) {
    c.speakers = !(std::strcmp(v, "off") == 0 || std::strcmp(v, "0") == 0 || std::strcmp(v, "null") == 0);
  }
  if (const char* v = std::getenv("GCRT_AUDIO_WAV")) {
    c.wav_path = v;
  }
  if (const char* v = std::getenv("GCRT_AUDIO_LATENCY_MS")) {
    const long ms = std::strtol(v, nullptr, 10);
    if (ms >= 10 && ms <= 1000) {
      c.sdl.target_latency_ms = static_cast<uint32_t>(ms);
      if (c.sdl.max_latency_ms < c.sdl.target_latency_ms * 3) {
        c.sdl.max_latency_ms = c.sdl.target_latency_ms * 3;
      }
    }
  }
  if (const char* v = std::getenv("GCRT_AUDIO_DRIFT")) {
    c.sdl.drift_control = std::strcmp(v, "0") != 0;
  }
  if (const char* v = std::getenv("GCRT_AUDIO_CAPTURE")) {
    c.sdl.capture_prefix = v;
  }
  if (const char* v = std::getenv("GCRT_AUDIO_MUTE")) {
    c.sdl.mute = std::strcmp(v, "0") != 0 && v[0] != '\0';
  }
  return c;
}

bool HostAudio::open(const Config& config) {
  close();
  trace_init_from_env();
  if (config.speakers) {
    m_sdl.open(config.sdl, kRate32k);
  }
  if (!config.wav_path.empty()) {
    m_wav.open(config.wav_path);
  }
  return true;
}

void HostAudio::close() {
  m_sdl.close();
  m_wav.close();
}

void HostAudio::write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) {
  m_frames += frames;
  m_wav.write(lr, frames, rate_hz);
  m_sdl.write(lr, frames, rate_hz);
}

} // namespace gcrt::audio

// ---------------------------------------------------------------------------------------------
// C interface (audio_hw.h)

struct GcrtAudioOut {
  gcrt::audio::HostAudio audio;
};

extern "C" GcrtAudioOut* gcrt_audio_out_open(void) {
  auto* out = new GcrtAudioOut;
  out->audio.open(gcrt::audio::HostAudio::config_from_env());
  return out;
}

extern "C" void gcrt_audio_out_close(GcrtAudioOut* out) {
  if (out != nullptr) {
    out->audio.close();
    delete out;
  }
}

extern "C" void gcrt_audio_out_sink(void* user, const uint8_t* p, uint32_t rate) {
  auto* out = static_cast<GcrtAudioOut*>(user);
  if (out == nullptr || p == nullptr) {
    return;
  }
  // Frames in RAM: right then left, 16-bit big-endian -> left, right in host byte order.
  int16_t lr[16];
  for (int i = 0; i < 8; ++i) {
    lr[2 * i + 1] = static_cast<int16_t>((p[4 * i + 0] << 8) | p[4 * i + 1]);
    lr[2 * i] = static_cast<int16_t>((p[4 * i + 2] << 8) | p[4 * i + 3]);
  }
  out->audio.write(lr, 8, rate);
}

extern "C" void gcrt_audio_get_stats(const GcrtAudioOut* out, GcrtAudioStats* stats) {
  if (stats == nullptr) {
    return;
  }
  const gcrt::audio::DspStats& d = gcrt::audio::glue_stats();
  *stats = GcrtAudioStats{};
  stats->command_lists = d.ax_command_lists;
  stats->voices = d.ax_voices_processed;
  if (out != nullptr) {
    auto* o = const_cast<GcrtAudioOut*>(out);
    const auto s = o->audio.speakers().stats();
    stats->frames_out = o->audio.frames_written();
    stats->frames_dropped = s.frames_dropped;
    stats->underruns = s.underruns;
  }
}
