// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Emulated DSP microcodes (HLE): common base, recognition by hash, host services.
// Generic GameCube, no reference to any game.
//
// Adapted for gcrt from Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08:
//   Source/Core/Core/HW/DSPHLE/UCodes/UCodes.{h,cpp} (UCodeInterface, PrepareBootUCode, UCodeFactory),
//   UCodes/CARD.cpp, Source/Core/Common/Hash.cpp (HashEctor).
// Changes (2026): the microcodes talk to an abstract host (UcodeHost) instead of DSPHLE, so that they can be
// plugged into the SDK's native DSP library (dsp_glue.cpp, dsp_ucode.h; step 10d: no more emulated DSP);
// microcode switches are requested from the host and applied after the mail has been processed.
#pragma once

#include <cstdint>
#include <memory>

#include "guest_memory.hpp"

namespace gcrt::audio {

struct DspStats {
  uint64_t mails_to_dsp = 0;
  uint64_t mails_from_dsp = 0;
  uint64_t interrupts = 0;
  uint64_t ax_command_lists = 0;
  uint64_t ax_voices_processed = 0;
  uint32_t ucode_switches = 0;
};

// Services the emulated DSP offers to a microcode.
class UcodeHost {
public:
  virtual ~UcodeHost() = default;
  virtual const GuestMemory& memory() const = 0;
  // DSP -> CPU mail; `interrupt`: raise the DSP interrupt when it becomes visible (after
  // `delay_cycles` CPU cycles at 486 MHz if it is the first in the queue).
  virtual void push_mail(uint32_t mail, bool interrupt, uint32_t delay_cycles) = 0;
  // Microcode switches, applied after the current processing (mail or update) returns:
  // set = replace the microcode (e.g. back to the ROM); swap = start a task while keeping the current
  // microcode for a later resume (Dolphin SwapUCode).
  virtual void request_set_ucode(uint32_t crc) = 0;
  virtual void request_swap_ucode(uint32_t crc) = 0;
  // Polyphase table (2048 entries: 4 sets x 128 phases x 4 taps) or nullptr (linear interpolation).
  virtual const int16_t* polyphase_coefs() const = 0;
  virtual DspStats& stats() = 0;
};

class Ucode {
public:
  Ucode(UcodeHost& host, uint32_t crc) : m_host(host), m_crc(crc) {}
  virtual ~Ucode() = default;
  Ucode(const Ucode&) = delete;
  Ucode& operator=(const Ucode&) = delete;

  virtual const char* name() const = 0;
  // The microcode has just started (first mails).
  virtual void initialize() = 0;
  virtual void handle_mail(uint32_t mail) = 0;
  // Every millisecond of emulated time.
  virtual void update() = 0;
  uint32_t crc() const { return m_crc; }

  // Task management mails of the SDK's DSP library. DSP -> CPU: 0xDCD1xxxx.
  static constexpr uint32_t TASK_MAIL_MASK = 0xFFFF0000;
  static constexpr uint32_t TASK_MAIL_TO_CPU = 0xDCD10000;
  static constexpr uint32_t DSP_INIT = TASK_MAIL_TO_CPU | 0x0000;
  static constexpr uint32_t DSP_RESUME = TASK_MAIL_TO_CPU | 0x0001;
  static constexpr uint32_t DSP_YIELD = TASK_MAIL_TO_CPU | 0x0002;
  static constexpr uint32_t DSP_DONE = TASK_MAIL_TO_CPU | 0x0003;
  static constexpr uint32_t DSP_SYNC = TASK_MAIL_TO_CPU | 0x0004;
  static constexpr uint32_t DSP_FRAME_END = TASK_MAIL_TO_CPU | 0x0005;
  // CPU -> DSP: 0xCDD1xxxx.
  static constexpr uint32_t TASK_MAIL_TO_DSP = 0xCDD10000;
  static constexpr uint32_t MAIL_RESUME = TASK_MAIL_TO_DSP | 0x0000;
  static constexpr uint32_t MAIL_NEW_UCODE = TASK_MAIL_TO_DSP | 0x0001;
  static constexpr uint32_t MAIL_RESET = TASK_MAIL_TO_DSP | 0x0002;
  static constexpr uint32_t MAIL_CONTINUE = TASK_MAIL_TO_DSP | 0x0003;

protected:
  void push_mail(uint32_t mail, bool interrupt = false, uint32_t delay_cycles = 0) {
    m_host.push_mail(mail, interrupt, delay_cycles);
  }
  // After MAIL_NEW_UCODE, the CPU sends 10 mails describing the next microcode; on the 10th, the IRAM
  // hash is computed and a "swap" is requested (the current microcode is kept for a later resume).
  void prepare_boot_ucode(uint32_t mail);
  bool needs_resume_mail();

  UcodeHost& m_host;
  uint32_t m_crc;
  bool m_upload_setup_in_progress = false;

private:
  struct NextUcodeInfo {
    uint32_t mram_dest_addr = 0;
    uint16_t mram_size = 0;
    uint16_t mram_dram_addr = 0;
    uint32_t iram_mram_addr = 0;
    uint16_t iram_size = 0;
    uint16_t iram_dest = 0;
    uint16_t iram_startpc = 0;
    uint32_t dram_mram_addr = 0;
    uint16_t dram_size = 0;
    uint16_t dram_dest = 0;
  };
  NextUcodeInfo m_next{};
  int m_next_steps = 0;
  bool m_needs_resume_mail = false;
};

// Special hashes (Dolphin UCodes.h): the ROM and the OS initialisation microcode are not
// hashed, they are given these conventional values.
constexpr uint32_t UCODE_ROM = 0x00000000;
constexpr uint32_t UCODE_INIT_AUDIO_SYSTEM = 0x00000001;
constexpr uint32_t UCODE_NULL = 0xFFFFFFFF;

// "ector" hash of a microcode (3-bit rotation after each byte).
uint32_t hash_ector(const uint8_t* data, uint32_t len);

enum class UcodeKind { Rom, Init, Card, AxGc, Unknown };
UcodeKind classify_ucode(uint32_t crc);
// Hashes of the known GameCube AX microcodes (Dolphin's list, UCodes.cpp), terminated by 0.
extern const uint32_t kAxGcCrcs[];
constexpr uint32_t CARD_UCODE_CRC = 0x65d6cc6f;

// Factory of the audio microcodes implemented here (GameCube AX, CARD). For an unknown hash,
// `force_ax_if_unknown` reproduces Dolphin on GameCube (AX forced); otherwise nullptr. ROM/INIT: nullptr
// (provided by the host).
std::unique_ptr<Ucode> make_audio_ucode(UcodeHost& host, uint32_t crc, bool force_ax_if_unknown);

// Dolphin's free polyphase table (Data/Sys/GC/dsp_coef.bin), 2048 entries.
const int16_t* free_polyphase_coefs();

// Microcode statistics (dsp_glue.cpp), read by the report (host_audio.cpp).
DspStats& glue_stats();

} // namespace gcrt::audio
