// WAV recorder (see wav_writer.hpp).
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "wav_writer.hpp"

#include <cstring>

#include "../platform/gcrt_path.h"
#include "audio_trace.hpp"

namespace gcrt::audio {

namespace {

void put_le32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
void put_le16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

} // namespace

bool WavWriter::open(const std::string& path, uint32_t rate_hz) {
  close();
  m_file = gcrt_fopen_utf8(path.c_str(), "wb"); // UTF-8 path (host_audio.cpp)
  if (m_file == nullptr) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "WAV: cannot create %s", path.c_str());
    return false;
  }
  m_path = path;
  m_rate = rate_hz;
  m_frames = 0;
  m_frames_at_last_header = 0;
  m_rate_warned = false;
  write_header();
  GCRT_AUDIO_TRACE(TraceCat::Output, "WAV: recording to %s", path.c_str());
  return true;
}

void WavWriter::write_header() {
  if (m_file == nullptr) {
    return;
  }
  const uint64_t data_bytes64 = m_frames * 4;
  const uint32_t data_bytes = data_bytes64 > 0xFFFFFFF0ull ? 0xFFFFFFF0u : static_cast<uint32_t>(data_bytes64);
  const uint32_t rate = m_rate != 0 ? m_rate : kRate32k;
  uint8_t h[44];
  std::memcpy(h + 0, "RIFF", 4);
  put_le32(h + 4, 36 + data_bytes);
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put_le32(h + 16, 16);      // fmt chunk size
  put_le16(h + 20, 1);       // PCM
  put_le16(h + 22, 2);       // stereo
  put_le32(h + 24, rate);    // sample rate
  put_le32(h + 28, rate * 4); // bytes per second
  put_le16(h + 32, 4);       // bytes per frame
  put_le16(h + 34, 16);      // bits per sample
  std::memcpy(h + 36, "data", 4);
  put_le32(h + 40, data_bytes);
  const long pos = std::ftell(m_file);
  std::fseek(m_file, 0, SEEK_SET);
  std::fwrite(h, 1, sizeof(h), m_file);
  if (pos > 44) {
    std::fseek(m_file, pos, SEEK_SET);
  }
  std::fflush(m_file);
  m_frames_at_last_header = m_frames;
}

void WavWriter::write(const int16_t* lr, uint32_t frames, uint32_t rate_hz) {
  if (m_file == nullptr || frames == 0) {
    return;
  }
  if (m_rate == 0) {
    m_rate = rate_hz;
  } else if (rate_hz != m_rate && !m_rate_warned) {
    m_rate_warned = true;
    GCRT_AUDIO_TRACE(TraceCat::Warn, "WAV: rate changed during recording (%u -> %u Hz), ignored",
                     m_rate, rate_hz);
  }
  uint8_t buf[4 * 256];
  uint32_t done = 0;
  while (done < frames) {
    const uint32_t n = (frames - done) > 256 ? 256 : (frames - done);
    for (uint32_t i = 0; i < n * 2; ++i) {
      put_le16(buf + 2 * i, static_cast<uint16_t>(lr[2 * done + i]));
    }
    std::fwrite(buf, 1, n * 4, m_file);
    done += n;
  }
  m_frames += frames;
  if (m_frames - m_frames_at_last_header >= uint64_t(m_rate) * 2) {
    write_header();
  }
}

void WavWriter::close() {
  if (m_file == nullptr) {
    return;
  }
  write_header();
  std::fclose(m_file);
  m_file = nullptr;
  GCRT_AUDIO_TRACE(TraceCat::Output, "WAV: %s closed, %llu frames at %u Hz", m_path.c_str(),
                   static_cast<unsigned long long>(m_frames), m_rate);
}

} // namespace gcrt::audio
