// Diagnostic capture of the host audio output (see output_capture.hpp).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "output_capture.hpp"

#include <cstdio>
#include <cstring>

#include "audio_trace.hpp"

namespace gcrt::audio {

namespace {

void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
}

// Canonical WAV header (44 bytes). `format`: 1 = integer PCM, 3 = IEEE float.
void wav_header(std::FILE* f, uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits, uint64_t data_bytes) {
  uint8_t h[44];
  const uint32_t data = data_bytes > 0xFFFFFFF0ull ? 0xFFFFFFF0u : static_cast<uint32_t>(data_bytes);
  std::memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + data);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, format);
  put16(h + 22, channels);
  put32(h + 24, rate);
  put32(h + 28, rate * channels * (bits / 8));
  put16(h + 32, static_cast<uint16_t>(channels * (bits / 8)));
  put16(h + 34, bits);
  std::memcpy(h + 36, "data", 4);
  put32(h + 40, data);
  std::fwrite(h, 1, sizeof(h), f);
}

} // namespace

OutputCapture::OutputCapture(std::string prefix) : m_prefix(std::move(prefix)), m_t0(std::chrono::steady_clock::now()) {
  // Chunk pointers reserved in advance: no reallocation on the audio thread (1 Mi elements per chunk;
  // 512 chunks of device sound = ~45 min of 48 kHz stereo).
  m_feed.reserve_chunks(256);
  m_device.reserve_chunks(512);
  m_requests.reserve_chunks(64);
  m_buffers.reserve_chunks(64);
  m_events.reserve_chunks(64);
  m_bursts.reserve(1 << 20);
}

int64_t OutputCapture::now_us() const {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - m_t0).count();
}

void OutputCapture::feed(const int16_t* lr, uint32_t frames, uint32_t rate_hz) {
  if (m_feed_rate == 0) {
    m_feed_rate = rate_hz;
  }
  m_feed.append(lr, size_t(frames) * 2);
}

void OutputCapture::device(const float* samples, uint32_t frames, int channels, int freq) {
  if (m_dev_channels == 0) {
    m_dev_channels = channels;
    m_dev_freq = freq;
  }
  if (channels != m_dev_channels || freq != m_dev_freq) {
    m_dev_format_changed.store(true, std::memory_order_relaxed);
    return; // device format changed (device switch): recording stops
  }
  m_device.append(samples, size_t(frames) * channels);
  const Tick t{now_us(), frames, 0};
  m_buffers.append(&t, 1);
}

void OutputCapture::device_request(uint32_t frames, uint32_t level) {
  const Tick t{now_us(), frames, level};
  m_requests.append(&t, 1);
}

void OutputCapture::event(uint32_t code, uint32_t a, uint32_t b) {
  const Ev e{now_us(), code, a, b};
  m_events.append(&e, 1);
}

void OutputCapture::producer_push(uint32_t frames) {
  const int64_t t = now_us();
  if (m_last_push_us >= 0 && t - m_last_push_us < 2000 && !m_bursts.empty()) {
    m_bursts.back().frames += frames;
  } else if (m_bursts.size() < m_bursts.capacity()) {
    m_bursts.push_back(Burst{t, m_last_push_us >= 0 ? t - m_last_push_us : 0, frames});
  }
  m_last_push_us = t;
}

void OutputCapture::close() {
  if (m_closed) {
    return;
  }
  m_closed = true;
  const std::string feed_path = m_prefix + "-feed.wav";
  if (std::FILE* f = std::fopen(feed_path.c_str(), "wb")) {
    const uint64_t bytes = uint64_t(m_feed.size()) * 2;
    wav_header(f, 1, 2, m_feed_rate != 0 ? m_feed_rate : 32028, 16, bytes);
    m_feed.for_each_block([&](const int16_t* p, size_t n) { std::fwrite(p, sizeof(int16_t), n, f); });
    std::fclose(f);
  }
  const std::string dev_path = m_prefix + "-device.wav";
  if (std::FILE* f = std::fopen(dev_path.c_str(), "wb")) {
    const uint64_t bytes = uint64_t(m_device.size()) * 4;
    const int ch = m_dev_channels != 0 ? m_dev_channels : 2;
    wav_header(f, 3, static_cast<uint16_t>(ch), m_dev_freq != 0 ? static_cast<uint32_t>(m_dev_freq) : 48000, 32, bytes);
    m_device.for_each_block([&](const float* p, size_t n) { std::fwrite(p, sizeof(float), n, f); });
    std::fclose(f);
  }
  if (std::FILE* f = std::fopen((m_prefix + "-device.csv").c_str(), "w")) {
    std::fprintf(f, "kind,t_us,frames,level\n");
    m_buffers.for_each_block([&](const Tick* p, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        std::fprintf(f, "buffer,%lld,%u,\n", static_cast<long long>(p[i].t_us), p[i].frames);
      }
    });
    m_requests.for_each_block([&](const Tick* p, size_t n) {
      for (size_t i = 0; i < n; ++i) {
        std::fprintf(f, "request,%lld,%u,%u\n", static_cast<long long>(p[i].t_us), p[i].frames, p[i].level);
      }
    });
    std::fclose(f);
  }
  if (std::FILE* f = std::fopen((m_prefix + "-events.csv").c_str(), "w")) {
    std::fprintf(f, "t_us,event,a,b\n");
    m_events.for_each_block([&](const Ev* p, size_t n) {
      static const char* const kNames[] = {"?", "primed", "underrun", "skip"};
      for (size_t i = 0; i < n; ++i) {
        std::fprintf(f, "%lld,%s,%u,%u\n", static_cast<long long>(p[i].t_us), kNames[p[i].code < 4 ? p[i].code : 0],
                     p[i].a, p[i].b);
      }
    });
    std::fclose(f);
  }
  if (std::FILE* f = std::fopen((m_prefix + "-t0.txt").c_str(), "w")) {
    // Origin of the capture timestamps, in steady_clock us (same base as GCRT_TIMING_LOG).
    std::fprintf(f, "%lld\n", static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                       m_t0.time_since_epoch())
                                                       .count()));
    std::fclose(f);
  }
  if (std::FILE* f = std::fopen((m_prefix + "-push.csv").c_str(), "w")) {
    std::fprintf(f, "t_us,gap_us,frames\n");
    for (const Burst& b : m_bursts) {
      std::fprintf(f, "%lld,%lld,%u\n", static_cast<long long>(b.t_us), static_cast<long long>(b.gap_us), b.frames);
    }
    std::fclose(f);
  }
  GCRT_AUDIO_TRACE(TraceCat::Output,
                   "capture: %s-{feed,device}.wav (%zu frames delivered, %zu device frames at %d Hz%s), "
                   "%zu game bursts",
                   m_prefix.c_str(), m_feed.size() / 2, m_dev_channels != 0 ? m_device.size() / m_dev_channels : 0,
                   m_dev_freq, m_dev_format_changed.load() ? ", format changed during capture: capture stopped" : "",
                   m_bursts.size());
}

} // namespace gcrt::audio
