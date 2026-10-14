// Diagnostic capture of the host audio output (generic, without SDL).
//
// Enabled by GCRT_AUDIO_CAPTURE=<prefix> (see host_audio.hpp). Writes on close:
//   <prefix>-feed.wav    frames handed to the resampler/device (16 bits, console rate):
//                        the game's sound as played, with the fades and silences inserted by the queue;
//   <prefix>-device.wav  what the device actually receives (32-bit floats, device
//                        rate), sampled just before it is sent to the driver;
//   <prefix>-device.csv  one line per device buffer: time (us), frames, queue level;
//   <prefix>-events.csv  queue events: priming, underrun, skip (time in us);
//   <prefix>-t0.txt      origin of the times above, in steady_clock us (to correlate with
//                        the platform's GCRT_TIMING_LOG);
//   <prefix>-push.csv    producer bursts (game thread): start (us), frames, gap from the previous one
//                        (us); a burst = submissions less than 2 ms apart.
// Audio thread data is kept in memory (preallocated chunks, no disk writes on that thread)
// and written on close. Meant for recordings of a few minutes (~0.5 MiB/s).
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gcrt::audio {

// Chunked storage: never copies nor reallocates a large array on the audio thread.
template <typename T, size_t kChunk = (size_t(1) << 20)>
class ChunkStore {
public:
  void append(const T* data, size_t n) {
    while (n > 0) {
      if (m_chunks.empty() || m_used == kChunk) {
        if (m_chunks.size() == m_chunks.capacity()) {
          return; // pointer capacity reached: recording stops (no reallocation here)
        }
        m_chunks.push_back(std::unique_ptr<T[]>(new T[kChunk]));
        m_used = 0;
      }
      const size_t k = std::min(n, kChunk - m_used);
      std::copy(data, data + k, m_chunks.back().get() + m_used);
      m_used += k;
      data += k;
      n -= k;
    }
  }
  void reserve_chunks(size_t n) { m_chunks.reserve(n); }
  size_t size() const { return m_chunks.empty() ? 0 : (m_chunks.size() - 1) * kChunk + m_used; }
  template <typename F>
  void for_each_block(F&& f) const {
    for (size_t i = 0; i < m_chunks.size(); ++i) {
      f(m_chunks[i].get(), i + 1 == m_chunks.size() ? m_used : kChunk);
    }
  }

private:
  std::vector<std::unique_ptr<T[]>> m_chunks;
  size_t m_used = 0;
};

class OutputCapture {
public:
  explicit OutputCapture(std::string prefix);
  ~OutputCapture() { close(); }
  OutputCapture(const OutputCapture&) = delete;
  OutputCapture& operator=(const OutputCapture&) = delete;

  int64_t now_us() const;

  // Device (or queue) thread: frames handed to the resampler.
  void feed(const int16_t* lr, uint32_t frames, uint32_t rate_hz);
  // Device thread: final buffer (interleaved floats) and its format.
  void device(const float* samples, uint32_t frames, int channels, int freq);
  // Device thread: one data request (input frames requested, queue level before it).
  void device_request(uint32_t frames, uint32_t level);
  // Queue event (device thread).
  void event(uint32_t code, uint32_t a, uint32_t b);
  // Game thread: one submission of frames.
  void producer_push(uint32_t frames);

  void close();

private:
  struct Tick {
    int64_t t_us;
    uint32_t frames;
    uint32_t level;
  };
  struct Ev {
    int64_t t_us;
    uint32_t code, a, b;
  };
  struct Burst {
    int64_t t_us;
    int64_t gap_us;
    uint32_t frames;
  };

  std::string m_prefix;
  std::chrono::steady_clock::time_point m_t0;
  bool m_closed = false;

  ChunkStore<int16_t> m_feed;
  uint32_t m_feed_rate = 0;
  ChunkStore<float> m_device;
  int m_dev_channels = 0;
  int m_dev_freq = 0;
  std::atomic<bool> m_dev_format_changed{false};
  ChunkStore<Tick, 65536> m_requests;
  ChunkStore<Tick, 65536> m_buffers;
  ChunkStore<Ev, 4096> m_events;
  std::vector<Burst> m_bursts; // game thread only
  int64_t m_last_push_us = -1;
};

} // namespace gcrt::audio
