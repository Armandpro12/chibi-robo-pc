// Audio output to the host device through SDL3 (version provided by Aurora: 3.4.10).
//
// Layout (notes/audio.md § 13, step 0 of the audio work: the host output):
//   game thread  --write()-->  OutputQueue (lock-free ring)  --SDL callback-->  SDL_AudioStream  -->  sound card
// The device runs all the time; it is the one pulling the data, at the pace of its clock, in the
// request callback (SDL_OpenAudioDeviceStream with a "get" SDL_AudioStreamCallback). The queue itself
// handles underruns there (fade-out then silence, never a replay or a hard cut), resuming
// (fade), the queue being too long (crossfaded skip) and clock drift (smoothed rate ratio,
// +/-0.1 % at most, applied by SDL_SetAudioStreamFrequencyRatio: SDL's resampler stays
// continuous when the ratio glides in small steps, verified by tests/audio/test_output_chain.cpp).
// SDL converts the console rate (32028 or 48043 Hz) to the device rate.
//
// Diagnostics: capture (output_capture.hpp) of what is handed to SDL and of what the device receives
// ("postmix" callback, just before the driver); mute mode (the device receives silence, with the real
// driver pacing) for automatic measurements without sound.
//
// SDL3 API used (verified in external/aurora/build-clang/_deps/sdl3_prebuilt-src/include/SDL3/SDL_audio.h):
// SDL_InitSubSystem(SDL_INIT_AUDIO), SDL_OpenAudioDeviceStream ("get" callback: amount in bytes of the stream's
// INPUT format; SDL_PutAudioStreamData allowed in the callback, stream lock already held; SDL locks
// are reentrant, SDL_mutex.h), SDL_ResumeAudioStreamDevice, SDL_LockAudioStream/SDL_UnlockAudioStream,
// SDL_SetAudioStreamFormat, SDL_SetAudioStreamFrequencyRatio, SDL_SetAudioStreamGain,
// SDL_SetAudioPostmixCallback (the buffer contents after this callback are what is passed along to the hardware),
// SDL_GetAudioDeviceFormat, SDL_DestroyAudioStream.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "audio_sink.hpp"
#include "output_capture.hpp"
#include "output_queue.hpp"

struct SDL_AudioStream;
struct SDL_AudioSpec;

namespace gcrt::audio {

struct SdlOutputConfig {
  uint32_t target_latency_ms = 64;   // GCRT_AUDIO_LATENCY_MS: target latency of the queue (and resume threshold)
  uint32_t max_latency_ms = 250;     // beyond it: crossfaded skip down to the target
  bool drift_control = true;         // GCRT_AUDIO_DRIFT=0: ratio always 1
  float max_ratio_deviation = 0.001f; // +/-0.1 %
  float gain = 1.0f;                 // SDL software gain (1 = unchanged)
  std::string capture_prefix;        // GCRT_AUDIO_CAPTURE: diagnostic capture (output_capture.hpp)
  bool mute = false;                 // GCRT_AUDIO_MUTE: the device receives silence (measurements without sound)
};

class SdlAudioOutput final : public AudioSink {
public:
  SdlAudioOutput() = default;
  ~SdlAudioOutput() override { close(); }
  SdlAudioOutput(const SdlAudioOutput&) = delete;
  SdlAudioOutput& operator=(const SdlAudioOutput&) = delete;

  // Opens the default device and starts playback (silence until the queue is primed).
  // False (and a trace) if SDL or the device are unavailable, or if mute mode cannot be
  // guaranteed: the runtime then continues without sound.
  bool open(const SdlOutputConfig& config = {}, uint32_t rate_hz = kRate32k);
  void close();
  bool is_open() const { return m_stream != nullptr; }
  // Game thread: frames at the console rate.
  void write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) override;
  // Compatibility: nothing to do, the device pulls the data itself.
  void flush() {}

  struct Stats {
    uint64_t frames_in = 0;
    uint64_t frames_dropped = 0; // frames lost: ring full + skipped (queue too long)
    uint64_t underruns = 0;      // underruns (fade-out then silence)
    uint64_t primes = 0;         // playback starts or resumes
    uint64_t skips = 0;          // skips (queue too long)
    uint64_t frames_silence = 0; // silence frames inserted (startup included)
    int queued_frames = 0;
    float ratio = 1.0f;
  };
  Stats stats() const;
  int queued_frames() const;

private:
  static void get_callback(void* user, SDL_AudioStream* stream, int additional_amount, int total_amount);
  static void postmix(void* user, const SDL_AudioSpec* spec, float* buffer, int buflen);
  static void on_queue_event(void* user, QueueEvent event, uint32_t a, uint32_t b);
  void trace_periodic();

  SDL_AudioStream* m_stream = nullptr;
  SdlOutputConfig m_config;
  uint32_t m_rate = kRate32k;
  std::unique_ptr<OutputQueue> m_queue;
  std::unique_ptr<OutputCapture> m_capture;
  bool m_mute = false;
  float m_applied_ratio = 1.0f; // device thread

  // Game thread: regularity of the submissions (traces).
  std::chrono::steady_clock::time_point m_last_write{};
  bool m_have_last_write = false;
  double m_window_max_gap_ms = 0.0;
  uint64_t m_frames_since_trace = 0;
  uint64_t m_last_underruns = 0;
  uint64_t m_gaps_over_40ms = 0;
};

} // namespace gcrt::audio
