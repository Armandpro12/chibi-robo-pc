// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// GameCube DSP sample accelerator (ADPCM / PCM8 / PCM16 reads from ARAM, looping).
// Adapted for gcrt (generic GameCube runtime) from Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08:
//   Source/Core/Core/DSP/DSPAccelerator.h and DSPAccelerator.cpp.
// Changes (2026): comments translated, Dolphin save state and logging removed, ADPCM formula factored out
// into adpcm.hpp. Address and exception logic kept as is.
#pragma once

#include <cstdint>

namespace gcrt::audio {

class Accelerator {
public:
  virtual ~Accelerator() = default;

  // Reads one decoded sample (ADPCM with `coefs`, or PCM) and advances the current address.
  uint16_t read_sample(const int16_t* coefs);
  // Raw read/write (used by other microcodes; not used by AX).
  uint16_t read_raw();
  void write_raw(uint16_t value);

  uint32_t start_address() const { return m_start_address; }
  uint32_t end_address() const { return m_end_address; }
  uint32_t current_address() const { return m_current_address; }
  uint16_t sample_format() const { return m_sample_format; }
  int16_t gain() const { return m_gain; }
  int16_t yn1() const { return m_yn1; }
  int16_t yn2() const { return m_yn2; }
  uint16_t pred_scale() const { return m_pred_scale; }

  void set_start_address(uint32_t address);
  void set_end_address(uint32_t address);
  void set_current_address(uint32_t address);
  void set_sample_format(uint16_t format) { m_sample_format = format; }
  void set_gain(int16_t gain) { m_gain = gain; }
  void set_yn1(int16_t yn1) { m_yn1 = yn1; }
  // Writing YN2 restarts reads stopped by an end of sample (hardware behaviour).
  void set_yn2(int16_t yn2) {
    m_yn2 = yn2;
    m_reads_stopped = false;
  }
  void set_pred_scale(uint16_t pred_scale) { m_pred_scale = pred_scale & 0x7F; }
  void set_input(uint16_t input) { m_input = input; }

protected:
  virtual void on_raw_read_end() = 0;
  virtual void on_raw_write_end() = 0;
  virtual void on_sample_read_end() = 0;
  virtual uint8_t read_memory(uint32_t address) = 0;
  virtual void write_memory(uint32_t address, uint8_t value) = 0;

  uint16_t current_sample();

  // Format (accelerator FORMAT register): bits 0-1 size (0: 4 bits, 1: 8 bits, 2: 16 bits),
  // bits 2-3 decoding (0: ADPCM, 1: PCM from ACIN without increment, 2: PCM, 3: PCM from ACIN),
  // bits 4-5 PCM gain scale (0: /2048, 1: /1, 2: /65536).
  uint32_t size_bits() const { return m_sample_format & 3; }
  uint32_t decode_bits() const { return (m_sample_format >> 2) & 3; }
  uint32_t gain_scale_bits() const { return (m_sample_format >> 4) & 3; }

  uint32_t m_start_address = 0;
  uint32_t m_end_address = 0;
  uint32_t m_current_address = 0;
  uint16_t m_sample_format = 0;
  int16_t m_gain = 0;
  int16_t m_yn1 = 0;
  int16_t m_yn2 = 0;
  uint16_t m_pred_scale = 0;
  uint16_t m_input = 0;
  // After an end-of-sample exception (ACCOV), the accelerator reads nothing (returns 0) and does not
  // advance until YN2 is written again. Internal state, not a register.
  bool m_reads_stopped = false;
};

} // namespace gcrt::audio
