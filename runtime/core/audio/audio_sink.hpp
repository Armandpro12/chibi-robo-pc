// Output sample sink (generic): the audio DMA pushes into it what the console would send to the
// digital-to-analog converter. Implementations: SdlAudioOutput (speakers), WavWriter (file),
// AudioFanout (several at once).
#pragma once

#include <cstdint>
#include <vector>

namespace gcrt::audio {

// Actual console rates (Dolphin AudioInterface.cpp / Mixer.h, commit e6f3ae17): divisors of
// 108 MHz, 2248 for "48 kHz" (48042.7 Hz) and 3372 for "32 kHz" (32028.5 Hz). Rounded down to the integer,
// like Dolphin's WAV files (FIXED_SAMPLE_RATE_DIVIDEND / divisor).
constexpr uint32_t kRateDividend = 108000000;
constexpr uint32_t kDivisor48k = 2248;
constexpr uint32_t kDivisor32k = 3372;
constexpr uint32_t kRate32k = kRateDividend / kDivisor32k; // 32028
constexpr uint32_t kRate48k = kRateDividend / kDivisor48k; // 48042

class AudioSink {
public:
  virtual ~AudioSink() = default;
  // `lr`: `frames` stereo frames, host 16-bit integers, interleaved left then right.
  // `rate_hz`: rate of these frames (kRate32k or kRate48k for the audio DMA).
  virtual void write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) = 0;
};

// Broadcasts to several sinks (e.g. speakers + WAV recording).
class AudioFanout final : public AudioSink {
public:
  void add(AudioSink* sink) {
    if (sink != nullptr) {
      m_sinks.push_back(sink);
    }
  }
  void clear() { m_sinks.clear(); }
  void write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) override {
    for (AudioSink* s : m_sinks) {
      s->write(lr, frames, rate_hz);
    }
  }

private:
  std::vector<AudioSink*> m_sinks;
};

} // namespace gcrt::audio
