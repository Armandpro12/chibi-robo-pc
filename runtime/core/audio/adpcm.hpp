// GameCube DSP ADPCM ("DSP-ADPCM"): decoding primitives (generic GameCube).
//
// Format: 8-byte frames = 1 header byte (predictor in bits 6-4, scale in bits 3-0)
// + 14 signed 4-bit samples. Sample addresses are expressed in nibbles:
// a frame starts at an address that is a multiple of 16, and its first 2 nibbles are the header.
//
// Formula (DSP hardware accelerator):
//   s = clamp16( n * 2^scale + ((0x400 + c1 * yn1 + c2 * yn2) >> 11) ),  (c1, c2) = coefs[2p], coefs[2p+1]
// Sources: Dolphin Source/Core/Core/DSP/DSPAccelerator.cpp (Accelerator::ReadSample, commit e6f3ae17,
// GPL-2.0-or-later) and unplug unplug/src/audio/format/adpcm/decode.rs (MIT); both give the same
// formula. Decoding an AX voice goes through the accelerator (accelerator.hpp), which calls decode_nibble().
#pragma once

#include <cstddef>
#include <cstdint>

namespace gcrt::audio::adpcm {

constexpr uint32_t kBytesPerFrame = 8;
constexpr uint32_t kSamplesPerFrame = 14;
constexpr uint32_t kNibblesPerFrame = 16;

inline int16_t clamp16(int64_t v) {
  return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

// Decodes one nibble (0..15). `pred_scale`: current header byte. Updates the history (yn1 = latest).
inline int16_t decode_nibble(uint32_t nibble, uint16_t pred_scale, const int16_t coefs[16], int16_t& yn1,
                             int16_t& yn2) {
  int32_t n = static_cast<int32_t>(nibble & 0xF);
  if (n >= 8) {
    n -= 16;
  }
  const int32_t scale = 1 << (pred_scale & 0xF);
  const uint32_t idx = (pred_scale >> 4) & 0x7;
  const int64_t c1 = coefs[idx * 2 + 0];
  const int64_t c2 = coefs[idx * 2 + 1];
  const int64_t v = int64_t(scale) * n + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
  const int16_t s = clamp16(v);
  yn2 = yn1;
  yn1 = s;
  return s;
}

// Decoding context (like the pred_scale/yn1/yn2 fields of an AX parameter block).
struct Context {
  uint16_t pred_scale = 0;
  int16_t yn1 = 0;
  int16_t yn2 = 0;
};

// Decodes the nibbles at addresses [start_addr, end_addr] (bounds included, in nibbles, relative to `data`)
// following the hardware convention: on reaching an address that is a multiple of 16, the header byte
// is re-read and its two nibbles skipped. `ctx.pred_scale` must be the header of the starting frame if
// `start_addr` is not the start of a frame. Returns the number of samples written (at most `out_cap`).
size_t decode_range(const uint8_t* data, size_t data_size, uint32_t start_addr, uint32_t end_addr,
                    const int16_t coefs[16], Context& ctx, int16_t* out, size_t out_cap);

// Conversions between nibble address <-> sample index (unplug's logic, GcAdpcm::address_to_sample).
size_t address_to_sample(size_t address);
size_t sample_to_address(size_t sample);

} // namespace gcrt::audio::adpcm
