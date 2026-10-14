// Sample queue of the host audio output (generic, without SDL, testable offline).
//
// Producer: the game thread (native audio DMA of hle/native_audio.c, through SdlAudioOutput::write) pushes
// 16-bit stereo frames at the console rate, in irregular packets (bursts on every video frame when the game is
// ahead, nothing during a slowdown). Consumer: the audio device thread pulls exactly what it needs,
// at the pace of the sound card clock. A single producer, a single consumer:
// lock-free ring.
//
// Rules (notes/audio.md § 13, step 0 of the audio work: the host output):
//   - every pushed frame is played at most once, in order: never replayed;
//   - underrun (the game did not provide enough sound): fade-out over the last available
//     frames, then silence (continuous decay from the last value, never a step);
//     resume once the queue is back at its target latency, with a crossfade from that silence;
//   - queue too long (beyond `max_ms`, e.g. a stalled device): skip to the target with a
//     crossfade; pushing into a full ring drops the new frames (counted);
//   - drift between the console clock (paced on the host clock) and the sound card clock:
//     suggested rate ratio, proportional to the difference between the AVERAGE queue level (exponential
//     average over `level_tau_s`, insensitive to bursts) and the target, capped at +/-`max_ratio_deviation`.
//     The resampler (SDL) applies it continuously: no discontinuity, and +/-0.1 % = 1.7 cents at most
//     (in steady state, the actual clock difference: a few hundredths of a percent).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace gcrt::audio {

struct OutputQueueConfig {
  uint32_t rate_hz = 32028;          // frame rate (used to convert durations)
  uint32_t target_ms = 64;           // target latency of the queue (and re-priming level)
  uint32_t max_ms = 250;             // beyond it, crossfaded skip down to the target
  uint32_t fade_ms = 4;              // fade duration (fade-out, resume, skip)
  float release_ms = 2.0f;           // time constant of the decay that follows the last sample
  bool drift_control = true;         // suggested rate ratio (otherwise: always 1)
  double max_ratio_deviation = 0.001; // +/-0.1 % (1.7 cents; typical difference between two crystals: ~0.01 %)
  double drift_gain = 0.004;         // ratio deviation for a level deviation of 100 % of the target
  double level_tau_s = 2.0;          // time constant of the level average
};

// Events recorded by the consumer (diagnostics, see OutputQueue::set_event_hook).
enum class QueueEvent : uint32_t {
  Primed = 1,   // playback (re)started: a = level
  Underrun = 2, // queue empty: a = real frames provided in the request, b = fade applied (frames)
  Skip = 3,     // queue too long: a = frames skipped, b = level before
};

class OutputQueue {
public:
  explicit OutputQueue(const OutputQueueConfig& config = {});
  OutputQueue(const OutputQueue&) = delete;
  OutputQueue& operator=(const OutputQueue&) = delete;

  // Producer. Returns the number of frames accepted (the others are dropped: ring full).
  uint32_t push(const int16_t* lr, uint32_t frames);
  // Consumer: writes exactly `frames` frames into `out` (sound, fades or silence).
  void pull(int16_t* out, uint32_t frames);
  // Reset: only while the consumer is stopped or locked out (SDL stream lock).
  void reset();
  // Changes the frame rate (durations recomputed); same conditions as reset(), which it calls.
  void set_rate(uint32_t rate_hz);

  // Rate ratio suggested to the resampler (> 1: consume faster). Read by the consumer.
  double ratio() const { return m_ratio; }
  uint32_t level() const { return static_cast<uint32_t>(m_head.load(std::memory_order_acquire) - m_tail.load(std::memory_order_acquire)); }
  uint32_t target_frames() const { return m_target; }
  const OutputQueueConfig& config() const { return m_config; }

  using EventHook = void (*)(void* user, QueueEvent event, uint32_t a, uint32_t b);
  void set_event_hook(EventHook hook, void* user) {
    m_hook = hook;
    m_hook_user = user;
  }

  // Counters readable from any thread (relaxed).
  struct Stats {
    uint64_t frames_in = 0;       // frames accepted
    uint64_t frames_rejected = 0; // frames dropped on input (ring full)
    uint64_t frames_played = 0;   // real frames output (fades included)
    uint64_t frames_silence = 0;  // silence or decay frames inserted
    uint64_t frames_skipped = 0;  // frames skipped (queue too long)
    uint64_t underruns = 0;
    uint64_t primes = 0;
    uint64_t skips = 0;
    uint32_t level_min = 0;       // minimum level while playing since the last reset_window(), after each
                                  // device request (margin before an underrun)
    uint32_t level_max = 0;       // maximum level, before each request
    double level_avg = 0.0;       // exponential average (frames)
    double ratio = 1.0;
    bool playing = false;
  };
  Stats stats() const;
  // Resets the min/max level window (periodic traces).
  void reset_window() { m_window_reset.store(true, std::memory_order_relaxed); }

private:
  enum class State { Starved, Playing };

  void recompute_sizes();
  void emit(QueueEvent e, uint32_t a, uint32_t b) {
    if (m_hook != nullptr) {
      m_hook(m_hook_user, e, a, b);
    }
  }
  // Silence: decay of the last values towards 0 (continuity).
  void fill_release(int16_t* out, uint32_t frames);
  void step_release();

  OutputQueueConfig m_config;
  uint32_t m_capacity = 0; // power of 2
  uint32_t m_mask = 0;
  std::unique_ptr<int16_t[]> m_ring;
  std::atomic<uint64_t> m_head{0}; // written by the producer
  std::atomic<uint64_t> m_tail{0}; // written by the consumer

  // Sizes in frames (derived from the configuration and the rate).
  uint32_t m_target = 0;
  uint32_t m_max = 0;
  uint32_t m_fade = 0;
  float m_release_mul = 0.0f;

  // Consumer state.
  State m_state = State::Starved;
  uint32_t m_fade_in_pos = 0; // position in the resume fade (== m_fade: finished)
  float m_last_l = 0.0f;      // last output value
  float m_last_r = 0.0f;
  float m_rel_l = 0.0f;       // silence: decay from the last output value
  float m_rel_r = 0.0f;
  double m_level_avg = 0.0;
  double m_ratio = 1.0;

  // Counters.
  std::atomic<uint64_t> m_frames_in{0};
  std::atomic<uint64_t> m_frames_rejected{0};
  std::atomic<uint64_t> m_frames_played{0};
  std::atomic<uint64_t> m_frames_silence{0};
  std::atomic<uint64_t> m_frames_skipped{0};
  std::atomic<uint64_t> m_underruns{0};
  std::atomic<uint64_t> m_primes{0};
  std::atomic<uint64_t> m_skips{0};
  std::atomic<uint32_t> m_level_min{0};
  std::atomic<uint32_t> m_level_max{0};
  std::atomic<double> m_level_avg_pub{0.0};
  std::atomic<double> m_ratio_pub{1.0};
  std::atomic<bool> m_playing_pub{false};
  std::atomic<bool> m_window_reset{true};

  EventHook m_hook = nullptr;
  void* m_hook_user = nullptr;
};

} // namespace gcrt::audio
