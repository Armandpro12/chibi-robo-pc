// SDL3 audio output (see sdl_output.hpp).
#include "sdl_output.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "audio_trace.hpp"
#include "../platform/gcrt_diag.h"

namespace gcrt::audio {

namespace {
// Scratch buffer of the callback (device thread): 4096 frames per pass (128 ms at 32 kHz).
constexpr uint32_t kScratchFrames = 4096;
thread_local int16_t t_scratch[kScratchFrames * 2];
} // namespace

bool SdlAudioOutput::open(const SdlOutputConfig& config, uint32_t rate_hz) {
  close();
  trace_init_from_env();
  m_config = config;
  m_rate = rate_hz;
  OutputQueueConfig qc;
  qc.rate_hz = rate_hz;
  qc.target_ms = config.target_latency_ms;
  qc.max_ms = std::max(config.max_latency_ms, config.target_latency_ms * 2);
  qc.drift_control = config.drift_control;
  qc.max_ratio_deviation = config.max_ratio_deviation;
  m_queue = std::make_unique<OutputQueue>(qc);
  m_queue->set_event_hook(&SdlAudioOutput::on_queue_event, this);
  m_applied_ratio = 1.0f;
  m_have_last_write = false;
  m_window_max_gap_ms = 0.0;
  m_frames_since_trace = 0;
  m_last_underruns = 0;
  m_gaps_over_40ms = 0;
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "SDL: audio unavailable (%s), no sound", SDL_GetError());
    m_queue.reset();
    return false;
  }
  SDL_AudioSpec spec{};
  spec.format = SDL_AUDIO_S16; // 16-bit integers in host byte order
  spec.channels = 2;
  spec.freq = static_cast<int>(rate_hz);
  // The device opens paused (SDL_OpenAudioDeviceStream): nothing plays before it is resumed below.
  m_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &SdlAudioOutput::get_callback, this);
  if (m_stream == nullptr) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "SDL: cannot open the audio device (%s), no sound",
                     SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    m_queue.reset();
    return false;
  }
  if (config.gain != 1.0f) {
    SDL_SetAudioStreamGain(m_stream, config.gain);
  }
  m_mute = config.mute;
  if (!config.capture_prefix.empty()) {
    m_capture = std::make_unique<OutputCapture>(config.capture_prefix);
  }
  if ((m_capture || m_mute) &&
      !SDL_SetAudioPostmixCallback(SDL_GetAudioStreamDevice(m_stream), &SdlAudioOutput::postmix, this)) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "SDL: device capture/silence not possible (%s)", SDL_GetError());
    if (m_mute) {
      // Mute requested but impossible to guarantee: no sound at all.
      SDL_DestroyAudioStream(m_stream);
      m_stream = nullptr;
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      m_capture.reset();
      m_queue.reset();
      return false;
    }
  }
  SDL_AudioSpec dev{};
  int dev_frames = 0;
  SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(m_stream), &dev, &dev_frames);
  GCRT_AUDIO_TRACE(TraceCat::Output,
                   "SDL: output opened (%s), input %u Hz, device %d Hz %d channels, buffer %d frames; "
                   "queue: target %u ms, max %u ms, drift %s (+/-%.2f %%)%s%s",
                   SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", rate_hz, dev.freq, dev.channels,
                   dev_frames, qc.target_ms, qc.max_ms, qc.drift_control ? "compensated" : "not compensated",
                   qc.max_ratio_deviation * 100.0, m_mute ? ", MUTED" : "", m_capture ? ", capture" : "");
  gcrt_diag_event(GCRT_EV_AUDIO, GCRT_EVA_OUTPUT_OPENED, (static_cast<uint64_t>(rate_hz) << 32) |
                                                          static_cast<uint32_t>(dev.freq));
  // The device runs from now on: silence until the queue has reached its target.
  SDL_ResumeAudioStreamDevice(m_stream);
  return true;
}

void SdlAudioOutput::close() {
  if (m_stream == nullptr) {
    return;
  }
  SDL_DestroyAudioStream(m_stream); // also closes the device; no callback after this
  m_stream = nullptr;
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
  const Stats s = stats();
  GCRT_AUDIO_TRACE(TraceCat::Output,
                   "SDL: output closed (%llu frames received, %llu lost, %llu underruns, %llu resumes, "
                   "%llu skips, %.1f s of silence inserted, final ratio %.5f, %llu gaps > 40 ms between two game "
                   "submissions)",
                   static_cast<unsigned long long>(s.frames_in), static_cast<unsigned long long>(s.frames_dropped),
                   static_cast<unsigned long long>(s.underruns), static_cast<unsigned long long>(s.primes),
                   static_cast<unsigned long long>(s.skips), static_cast<double>(s.frames_silence) / m_rate, s.ratio,
                   static_cast<unsigned long long>(m_gaps_over_40ms));
  if (m_capture) {
    m_capture->close();
    m_capture.reset();
  }
  m_queue.reset();
}

// Device thread, stream lock held by SDL. `additional_amount`: bytes of the input format (16-bit
// stereo) to add to satisfy the current request.
void SdlAudioOutput::get_callback(void* user, SDL_AudioStream* stream, int additional_amount, int) {
  auto* self = static_cast<SdlAudioOutput*>(user);
  if (additional_amount <= 0 || !self->m_queue) {
    return;
  }
  uint32_t frames = static_cast<uint32_t>((additional_amount + 3) / 4);
  if (self->m_capture) {
    self->m_capture->device_request(frames, self->m_queue->level());
  }
  while (frames > 0) {
    const uint32_t n = std::min(frames, kScratchFrames);
    self->m_queue->pull(t_scratch, n);
    SDL_PutAudioStreamData(stream, t_scratch, static_cast<int>(n * 4));
    if (self->m_capture) {
      self->m_capture->feed(t_scratch, n, self->m_rate);
    }
    frames -= n;
  }
  // Rate ratio suggested by the queue (glides in small steps: no discontinuity in SDL).
  const float r = static_cast<float>(self->m_queue->ratio());
  if (std::fabs(r - self->m_applied_ratio) > 2e-6f) {
    SDL_SetAudioStreamFrequencyRatio(stream, r);
    self->m_applied_ratio = r;
  }
}

void SdlAudioOutput::postmix(void* user, const SDL_AudioSpec* spec, float* buffer, int buflen) {
  auto* self = static_cast<SdlAudioOutput*>(user);
  const int ch = spec->channels > 0 ? spec->channels : 2;
  if (self->m_capture) {
    self->m_capture->device(buffer, static_cast<uint32_t>(buflen / 4 / ch), ch, spec->freq);
  }
  if (self->m_mute) {
    std::memset(buffer, 0, static_cast<size_t>(buflen));
  }
}

void SdlAudioOutput::on_queue_event(void* user, QueueEvent event, uint32_t a, uint32_t b) {
  auto* self = static_cast<SdlAudioOutput*>(user);
  if (self->m_capture) {
    self->m_capture->event(static_cast<uint32_t>(event), a, b);
  }
}

SdlAudioOutput::Stats SdlAudioOutput::stats() const {
  Stats s;
  if (!m_queue) {
    return s;
  }
  const OutputQueue::Stats q = m_queue->stats();
  s.frames_in = q.frames_in + q.frames_rejected;
  s.frames_dropped = q.frames_rejected + q.frames_skipped;
  s.underruns = q.underruns;
  s.primes = q.primes;
  s.skips = q.skips;
  s.frames_silence = q.frames_silence;
  s.queued_frames = static_cast<int>(m_queue->level());
  s.ratio = static_cast<float>(q.ratio);
  return s;
}

int SdlAudioOutput::queued_frames() const { return m_queue ? static_cast<int>(m_queue->level()) : 0; }

void SdlAudioOutput::trace_periodic() {
  const OutputQueue::Stats q = m_queue->stats();
  const double ms = 1000.0 / m_rate;
  GCRT_AUDIO_TRACE(TraceCat::Output,
                   "SDL: queue %.0f ms (min %.0f, max %.0f, average %.1f), ratio %.5f, %s; underruns "
                   "%llu (+%llu), silence inserted %.2f s, skips %llu, rejected %llu; max gap between two game "
                   "submissions %.1f ms",
                   m_queue->level() * ms, q.level_min * ms, q.level_max * ms, q.level_avg * ms, q.ratio,
                   q.playing ? "playing" : "waiting", static_cast<unsigned long long>(q.underruns),
                   static_cast<unsigned long long>(q.underruns - m_last_underruns),
                   static_cast<double>(q.frames_silence) / m_rate, static_cast<unsigned long long>(q.skips),
                   static_cast<unsigned long long>(q.frames_rejected), m_window_max_gap_ms);
  m_last_underruns = q.underruns;
  m_queue->reset_window();
  m_window_max_gap_ms = 0.0;
}

void SdlAudioOutput::write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) {
  if (m_stream == nullptr || frames == 0) {
    return;
  }
  if (rate_hz != m_rate) {
    // DMA rate change (rare: game initialisation). The callback is locked out by the stream lock;
    // whatever is in the queue is dropped (a few tens of ms at most), playback resumes with a fade.
    SDL_LockAudioStream(m_stream);
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = static_cast<int>(rate_hz);
    SDL_SetAudioStreamFormat(m_stream, &spec, nullptr);
    m_queue->set_rate(rate_hz);
    SDL_UnlockAudioStream(m_stream);
    GCRT_AUDIO_TRACE(TraceCat::Output, "SDL: input rate %u -> %u Hz", m_rate, rate_hz);
    m_rate = rate_hz;
  }
  m_queue->push(lr, frames);
  // Regularity of the game's submissions (stutter diagnostics: a gap > 40 ms = the game fell behind).
  const auto now = std::chrono::steady_clock::now();
  if (m_have_last_write) {
    const double gap = std::chrono::duration<double, std::milli>(now - m_last_write).count();
    m_window_max_gap_ms = std::max(m_window_max_gap_ms, gap);
    if (gap > 40.0) {
      ++m_gaps_over_40ms;
    }
  }
  m_last_write = now;
  m_have_last_write = true;
  if (m_capture) {
    m_capture->producer_push(frames);
  }
  m_frames_since_trace += frames;
  if (m_frames_since_trace >= m_rate) { // one line per second of sound
    m_frames_since_trace = 0;
    if (trace_on(TraceCat::Output)) {
      trace_periodic();
    }
  }
}

} // namespace gcrt::audio
