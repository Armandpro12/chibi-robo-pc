// Sample queue of the host audio output (see output_queue.hpp).
#include "output_queue.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gcrt::audio {

namespace {

constexpr double kPi = 3.14159265358979323846;

int16_t to_s16(float v) {
  const float r = std::nearbyint(v);
  return static_cast<int16_t>(std::clamp(r, -32768.0f, 32767.0f));
}

// Gain of a raised-cosine fade: 0 -> 1 over `n` frames, value at the centre of frame `i`.
float rise(uint32_t i, uint32_t n) {
  return static_cast<float>(0.5 - 0.5 * std::cos(kPi * (static_cast<double>(i) + 0.5) / static_cast<double>(n)));
}

uint32_t next_pow2(uint32_t v) {
  uint32_t p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

} // namespace

OutputQueue::OutputQueue(const OutputQueueConfig& config) : m_config(config) {
  // Ring: at least twice the maximum latency and one second of sound, so that the skip (queue too
  // long) always triggers before the ring is full.
  const uint32_t rate = std::max<uint32_t>(config.rate_hz, 8000);
  const uint32_t max_frames = static_cast<uint32_t>(uint64_t(rate) * std::max<uint32_t>(config.max_ms, 1) / 1000);
  m_capacity = next_pow2(std::max<uint32_t>(max_frames * 2, 65536));
  m_mask = m_capacity - 1;
  m_ring = std::make_unique<int16_t[]>(size_t(m_capacity) * 2);
  recompute_sizes();
}

void OutputQueue::recompute_sizes() {
  const uint32_t rate = std::max<uint32_t>(m_config.rate_hz, 8000);
  m_fade = std::max<uint32_t>(static_cast<uint32_t>(uint64_t(rate) * m_config.fade_ms / 1000), 8);
  m_target = std::max<uint32_t>(static_cast<uint32_t>(uint64_t(rate) * m_config.target_ms / 1000), m_fade * 2);
  m_max = std::max<uint32_t>(static_cast<uint32_t>(uint64_t(rate) * m_config.max_ms / 1000), m_target * 2);
  m_max = std::min<uint32_t>(m_max, m_capacity / 2);
  const double release_frames = std::max(0.05, static_cast<double>(m_config.release_ms)) * rate / 1000.0;
  m_release_mul = static_cast<float>(std::exp(-1.0 / release_frames));
}

void OutputQueue::reset() {
  m_head.store(0, std::memory_order_relaxed);
  m_tail.store(0, std::memory_order_relaxed);
  m_state = State::Starved;
  m_fade_in_pos = 0;
  m_last_l = m_last_r = 0.0f;
  m_rel_l = m_rel_r = 0.0f;
  m_level_avg = 0.0;
  m_playing_pub.store(false, std::memory_order_relaxed);
}

void OutputQueue::set_rate(uint32_t rate_hz) {
  // The ring capacity stays the one set at construction (sized for the initial rate; the
  // durations in frames are only recomputed, and capped at half the ring).
  m_config.rate_hz = rate_hz;
  recompute_sizes();
  reset();
}

uint32_t OutputQueue::push(const int16_t* lr, uint32_t frames) {
  const uint64_t head = m_head.load(std::memory_order_relaxed);
  const uint64_t tail = m_tail.load(std::memory_order_acquire);
  const uint32_t free_frames = m_capacity - static_cast<uint32_t>(head - tail);
  const uint32_t n = std::min(frames, free_frames);
  const uint32_t start = static_cast<uint32_t>(head) & m_mask;
  const uint32_t first = std::min(n, m_capacity - start);
  std::memcpy(&m_ring[size_t(start) * 2], lr, size_t(first) * 4);
  if (n > first) {
    std::memcpy(&m_ring[0], lr + size_t(first) * 2, size_t(n - first) * 4);
  }
  m_head.store(head + n, std::memory_order_release);
  m_frames_in.fetch_add(n, std::memory_order_relaxed);
  if (n < frames) {
    m_frames_rejected.fetch_add(frames - n, std::memory_order_relaxed);
  }
  return n;
}

void OutputQueue::step_release() {
  m_rel_l *= m_release_mul;
  m_rel_r *= m_release_mul;
  if (std::fabs(m_rel_l) < 0.5f) {
    m_rel_l = 0.0f;
  }
  if (std::fabs(m_rel_r) < 0.5f) {
    m_rel_r = 0.0f;
  }
}

void OutputQueue::fill_release(int16_t* out, uint32_t frames) {
  for (uint32_t i = 0; i < frames; ++i) {
    step_release();
    out[2 * i] = to_s16(m_rel_l);
    out[2 * i + 1] = to_s16(m_rel_r);
  }
  m_last_l = m_rel_l;
  m_last_r = m_rel_r;
}

void OutputQueue::pull(int16_t* out, uint32_t frames) {
  const uint64_t head = m_head.load(std::memory_order_acquire);
  uint64_t tail = m_tail.load(std::memory_order_relaxed);
  const bool window_reset = m_window_reset.exchange(false, std::memory_order_relaxed);
  uint32_t lmin = window_reset ? UINT32_MAX : m_level_min.load(std::memory_order_relaxed);
  uint32_t lmax = window_reset ? 0 : m_level_max.load(std::memory_order_relaxed);
  uint32_t done = 0;
  uint64_t played = 0, silence = 0;
  while (done < frames) {
    const uint32_t level = static_cast<uint32_t>(head - tail);
    if (m_state == State::Starved) {
      if (level >= m_target) {
        m_state = State::Playing;
        m_fade_in_pos = 0;
        // The average restarts from the target, not from the priming level (inflated by the burst that
        // triggered it): otherwise the ratio would needlessly drift away from 1 for a few seconds after each resume.
        m_level_avg = m_target;
        m_primes.fetch_add(1, std::memory_order_relaxed);
        emit(QueueEvent::Primed, level, 0);
        continue;
      }
      fill_release(out + size_t(done) * 2, frames - done);
      silence += frames - done;
      done = frames;
      break;
    }
    lmax = std::max(lmax, level); // maximum: before the device request
    // Queue too long: skip to the target, crossfade between the old and the new position.
    if (level > m_max) {
      const uint32_t skip = level - m_target;
      const uint32_t n = std::min(m_fade, frames - done);
      for (uint32_t i = 0; i < n; ++i) {
        const float w = rise(i, n);
        const uint32_t a = static_cast<uint32_t>(tail + i) & m_mask;
        const uint32_t b = static_cast<uint32_t>(tail + skip + i) & m_mask;
        const float l = m_ring[size_t(a) * 2] * (1.0f - w) + m_ring[size_t(b) * 2] * w;
        const float r = m_ring[size_t(a) * 2 + 1] * (1.0f - w) + m_ring[size_t(b) * 2 + 1] * w;
        out[size_t(done + i) * 2] = to_s16(l);
        out[size_t(done + i) * 2 + 1] = to_s16(r);
        m_last_l = l;
        m_last_r = r;
      }
      tail += uint64_t(skip) + n;
      done += n;
      played += n;
      m_skips.fetch_add(1, std::memory_order_relaxed);
      m_frames_skipped.fetch_add(skip, std::memory_order_relaxed);
      emit(QueueEvent::Skip, skip, level);
      continue;
    }
    const uint32_t want = frames - done;
    const uint32_t take = std::min(level, want);
    const bool underrun = take < want;
    // Underrun in this request: fade-out over the last available frames.
    const uint32_t fade_out = underrun ? std::min(take, m_fade) : 0;
    for (uint32_t i = 0; i < take; ++i) {
      const uint32_t idx = static_cast<uint32_t>(tail + i) & m_mask;
      const int16_t sl = m_ring[size_t(idx) * 2];
      const int16_t sr = m_ring[size_t(idx) * 2 + 1];
      float g_in = 1.0f, g_out = 1.0f;
      if (m_fade_in_pos < m_fade) {
        g_in = rise(m_fade_in_pos, m_fade);
        ++m_fade_in_pos;
      }
      if (fade_out != 0 && i >= take - fade_out) {
        g_out = 1.0f - rise(i - (take - fade_out), fade_out);
      }
      int16_t* o = out + size_t(done + i) * 2;
      if (g_in == 1.0f && g_out == 1.0f) {
        o[0] = sl; // untouched frame, bit-exact
        o[1] = sr;
        m_last_l = sl;
        m_last_r = sr;
      } else {
        // Resume: crossfade from the silence decay (continuity), then fade-out.
        step_release();
        const float l = (sl * g_in + m_rel_l * (1.0f - g_in)) * g_out;
        const float r = (sr * g_in + m_rel_r * (1.0f - g_in)) * g_out;
        o[0] = to_s16(l);
        o[1] = to_s16(r);
        m_last_l = l;
        m_last_r = r;
      }
    }
    tail += take;
    done += take;
    played += take;
    if (underrun) {
      m_state = State::Starved;
      m_rel_l = m_last_l; // the silence starts from the last output value (~0 after the fade)
      m_rel_r = m_last_r;
      m_underruns.fetch_add(1, std::memory_order_relaxed);
      emit(QueueEvent::Underrun, take, fade_out);
      // The rest of the request is filled by the decay (next loop iteration, Starved state).
    }
  }
  m_tail.store(tail, std::memory_order_release);
  if (m_state == State::Playing) {
    lmin = std::min(lmin, static_cast<uint32_t>(head - tail)); // minimum: after the request (remaining margin)
  }
  m_frames_played.fetch_add(played, std::memory_order_relaxed);
  m_frames_silence.fetch_add(silence, std::memory_order_relaxed);
  if (lmin != UINT32_MAX) {
    m_level_min.store(lmin, std::memory_order_relaxed);
    m_level_max.store(lmax, std::memory_order_relaxed);
  }
  // Level average and rate ratio, only while playing (no runaway during a
  // silence: the game slowing down is not a clock drift).
  if (m_state == State::Playing) {
    const uint32_t level_after = static_cast<uint32_t>(head - tail);
    const double dt = static_cast<double>(frames) / std::max<uint32_t>(m_config.rate_hz, 1);
    const double a = 1.0 - std::exp(-dt / std::max(0.01, m_config.level_tau_s));
    m_level_avg += a * (static_cast<double>(level_after) - m_level_avg);
    if (m_config.drift_control) {
      const double err = (m_level_avg - m_target) / m_target;
      const double dev = std::clamp(m_config.drift_gain * err, -m_config.max_ratio_deviation,
                                    m_config.max_ratio_deviation);
      m_ratio = 1.0 + dev;
    } else {
      m_ratio = 1.0;
    }
  }
  m_level_avg_pub.store(m_level_avg, std::memory_order_relaxed);
  m_ratio_pub.store(m_ratio, std::memory_order_relaxed);
  m_playing_pub.store(m_state == State::Playing, std::memory_order_relaxed);
}

OutputQueue::Stats OutputQueue::stats() const {
  Stats s;
  s.frames_in = m_frames_in.load(std::memory_order_relaxed);
  s.frames_rejected = m_frames_rejected.load(std::memory_order_relaxed);
  s.frames_played = m_frames_played.load(std::memory_order_relaxed);
  s.frames_silence = m_frames_silence.load(std::memory_order_relaxed);
  s.frames_skipped = m_frames_skipped.load(std::memory_order_relaxed);
  s.underruns = m_underruns.load(std::memory_order_relaxed);
  s.primes = m_primes.load(std::memory_order_relaxed);
  s.skips = m_skips.load(std::memory_order_relaxed);
  s.level_min = m_level_min.load(std::memory_order_relaxed);
  s.level_max = m_level_max.load(std::memory_order_relaxed);
  s.level_avg = m_level_avg_pub.load(std::memory_order_relaxed);
  s.ratio = m_ratio_pub.load(std::memory_order_relaxed);
  s.playing = m_playing_pub.load(std::memory_order_relaxed);
  return s;
}

} // namespace gcrt::audio
