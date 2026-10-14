// WAV recorder (16-bit stereo PCM): used by the automatic audio tests (duration and envelope
// comparison with a Dolphin recording) and for diagnostics. Generic, no dependency.
//
// The header is rewritten periodically (every ~2 s of sound): the file stays readable if the runtime
// stops abruptly. The file's rate is that of the first block received; if it changes later
// (AICR.AIDFR), the frames are written as they are and a warning is traced.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "audio_sink.hpp"

namespace gcrt::audio {

class WavWriter final : public AudioSink {
public:
  WavWriter() = default;
  ~WavWriter() override { close(); }
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;

  // `rate_hz` = 0: taken from the first write().
  bool open(const std::string& path, uint32_t rate_hz = 0);
  void close();
  bool is_open() const { return m_file != nullptr; }
  void write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) override;

  uint64_t frames_written() const { return m_frames; }
  uint32_t rate() const { return m_rate; }

private:
  void write_header();

  std::FILE* m_file = nullptr;
  std::string m_path;
  uint32_t m_rate = 0;
  uint64_t m_frames = 0;
  uint64_t m_frames_at_last_header = 0;
  bool m_rate_warned = false;
};

} // namespace gcrt::audio
