// Ready-to-use host audio output: speakers (SDL3) and/or WAV recording, configured through
// environment variables (generic). It receives the blocks of the native audio DMA (hle/native_audio.c).
//
//   GCRT_AUDIO=sdl|off        speaker output (default: sdl; "off" for tests without sound)
//   GCRT_AUDIO_WAV=file       records everything coming out of the audio DMA (audio milestone tests, diagnostics)
//   GCRT_AUDIO_LATENCY_MS=64  target latency of the output queue (10 to 1000); the maximum queue is
//                             max(250, 3 x target) ms
//   GCRT_AUDIO_DRIFT=1|0      clock drift compensation (smoothed rate ratio, +/-0.1 % at most)
//   GCRT_AUDIO_CAPTURE=prefix  diagnostics: <prefix>-feed.wav (handed to SDL), -device.wav (received by the
//                             device), -device.csv, -events.csv, -push.csv (output_capture.hpp)
//   GCRT_AUDIO_MUTE=1         the device receives silence (automatic measurements without sound, real driver
//                             pacing); if this silence cannot be guaranteed, no sound is opened at all
// Traces: GCRT_TRACE=audioout (one line per second: queue level, ratio, underruns, maximum gap
// between two submissions from the game).
#pragma once

#include <string>

#include "audio_sink.hpp"
#include "sdl_output.hpp"
#include "wav_writer.hpp"

namespace gcrt::audio {

class HostAudio final : public AudioSink {
public:
  struct Config {
    bool speakers = true;
    std::string wav_path;
    SdlOutputConfig sdl;
  };
  static Config config_from_env();

  HostAudio() = default;
  ~HostAudio() override { close(); }
  // Opens what is requested; returns true even if the speakers are unavailable (running continues without sound).
  bool open(const Config& config);
  void close();
  void write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) override;

  SdlAudioOutput& speakers() { return m_sdl; }
  uint64_t frames_written() const { return m_frames; }
  WavWriter& recorder() { return m_wav; }

private:
  SdlAudioOutput m_sdl;
  WavWriter m_wav;
  uint64_t m_frames = 0;
};

} // namespace gcrt::audio
