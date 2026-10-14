// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GameCube DSP sample accelerator. Adapted from Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08:
// Source/Core/Core/DSP/DSPAccelerator.cpp (see accelerator.hpp for the changes).
#include "accelerator.hpp"

#include "adpcm.hpp"
#include "audio_trace.hpp"

namespace gcrt::audio {

namespace {
constexpr uint32_t kStartEndAddressMask = 0x3FFFFFFF;
constexpr uint32_t kCurrentAddressMask = 0xBFFFFFFF;
} // namespace

uint16_t Accelerator::current_sample() {
  uint16_t val = 0;
  switch (size_bits()) {
  case 0: // 4 bits
    val = read_memory(m_current_address >> 1);
    if (m_current_address & 1) {
      val &= 0xF;
    } else {
      val >>= 4;
    }
    break;
  case 1: // 8 bits
    val = read_memory(m_current_address);
    break;
  case 2: // 16 bits
    val = static_cast<uint16_t>((read_memory(m_current_address * 2) << 8) | read_memory(m_current_address * 2 + 1));
    break;
  default: // returns garbage on hardware, but the address still advances
    GCRT_AUDIO_TRACE(TraceCat::Warn, "accelerator: invalid format 0x%04X", m_sample_format);
    break;
  }
  return val;
}

uint16_t Accelerator::read_raw() {
  const uint16_t val = current_sample();
  if (size_bits() != 3) {
    m_current_address++;
  } else {
    m_current_address = (m_current_address & ~3u) | ((m_current_address + 1) & 3u);
  }
  // Normal case only (the hardware's u4/u8 edge cases are not emulated, as in Dolphin).
  if (m_current_address - 1 == m_end_address) {
    m_current_address = m_start_address; // back to the start address (verified on hardware by Dolphin)
    on_raw_read_end();
  }
  set_current_address(m_current_address);
  return val;
}

void Accelerator::write_raw(uint16_t value) {
  // Writes are accepted only when the most significant bit of the address is 1.
  if (m_current_address & 0x80000000u) {
    write_memory(m_current_address * 2, static_cast<uint8_t>(value >> 8));
    write_memory(m_current_address * 2 + 1, static_cast<uint8_t>(value & 0xFF));
    m_current_address++;
    on_raw_write_end();
  } else {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "accelerator: raw write without the high bit (address 0x%08X)", m_current_address);
  }
}

uint16_t Accelerator::read_sample(const int16_t* coefs) {
  if (m_reads_stopped) {
    return 0x0000;
  }
  if ((m_sample_format >> 6) != 0) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "accelerator: unknown bits in format 0x%04X", m_sample_format);
  }

  uint16_t val = 0;
  uint32_t step_size = 0;
  int16_t raw_sample;
  const uint32_t decode = decode_bits();
  if (decode == 1 || decode == 3) {
    raw_sample = static_cast<int16_t>(m_input); // addresses are meaningless in these modes
  } else {
    raw_sample = static_cast<int16_t>(current_sample());
  }

  const uint32_t coef_idx = (m_pred_scale >> 4) & 0x7;
  const int32_t coef1 = coefs[coef_idx * 2 + 0];
  const int32_t coef2 = coefs[coef_idx * 2 + 1];

  switch (decode) {
  case 0: { // ADPCM: only the 4 low bits matter, even for 8/16-bit reads
    val = static_cast<uint16_t>(adpcm::decode_nibble(static_cast<uint32_t>(raw_sample) & 0xF, m_pred_scale, coefs,
                                                     m_yn1, m_yn2));
    step_size = 2;
    m_current_address += 1;

    // Special cases, handled outside the normal overflow: no ACCOV exception, no header re-read, and if
    // the end address is 16-aligned, the DSP loops to start + 1 instead of start.
    if ((m_end_address & 0xF) == 0x0 && m_current_address == m_end_address) {
      m_current_address = m_start_address + 1;
    } else if ((m_end_address & 0xF) == 0x1 && m_current_address == m_end_address - 1) {
      m_current_address = m_start_address;
    } else if ((m_current_address & 15) == 0) {
      // New frame: read the predictor/scale byte.
      m_pred_scale = read_memory((m_current_address & ~15u) >> 1);
      m_current_address += 2;
      step_size += 2;
    }
    break;
  }
  case 1:
  case 2:
  case 3: { // PCM: the gain applies to PCM only
    uint32_t gain_shift = 0;
    switch (gain_scale_bits()) {
    case 0:
      gain_shift = 11;
      break;
    case 1:
      gain_shift = 0;
      break;
    case 2:
      gain_shift = 16;
      break;
    default:
      GCRT_AUDIO_TRACE(TraceCat::Warn, "accelerator: invalid gain scale (format 0x%04X)", m_sample_format);
      break;
    }
    const int32_t val32 = ((static_cast<int32_t>(m_gain) * raw_sample) >> gain_shift) +
                          (((coef1 * m_yn1) >> gain_shift) + ((coef2 * m_yn2) >> gain_shift));
    val = static_cast<uint16_t>(static_cast<int16_t>(val32));
    m_yn2 = m_yn1;
    m_yn1 = static_cast<int16_t>(val);
    step_size = 2;
    if (decode != 1) {
      m_current_address += 1;
    }
    break;
  }
  default:
    break;
  }

  // Looping: YN1/YN2 must be reset to their "loop" values, which the exception handler does.
  if (m_current_address == (m_end_address + step_size - 1)) {
    m_current_address = m_start_address;
    m_reads_stopped = true;
    on_sample_read_end();
  }

  set_current_address(m_current_address);
  return val;
}

void Accelerator::set_start_address(uint32_t address) { m_start_address = address & kStartEndAddressMask; }
void Accelerator::set_end_address(uint32_t address) { m_end_address = address & kStartEndAddressMask; }
void Accelerator::set_current_address(uint32_t address) { m_current_address = address & kCurrentAddressMask; }

} // namespace gcrt::audio
