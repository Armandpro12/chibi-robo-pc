// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Emulated DSP microcodes: common base, hashes, factory, CARD microcode.
// Adapted from Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08: Source/Core/Core/HW/DSPHLE/UCodes/
// UCodes.cpp, CARD.cpp, and Source/Core/Common/Hash.cpp (see ucode.hpp for the changes).
#include "ucode.hpp"

#include "audio_trace.hpp"
#include "ax_gc.hpp"

namespace gcrt::audio {

namespace {

const int16_t kFreeCoefs[0x800] = {
#include "dsp_coef_free.inc"
};

// Microcode of the CARD library (unlock response of a memory card). Like Dolphin, nothing is
// computed: an emulated card reports itself as already unlocked and this microcode is not launched in practice.
// Difference from Dolphin: DSP_INIT is posted with an immediate interrupt; Dolphin posts it without interrupt
// then raises the interrupt on the next update (at most 1 ms later) while a mail is pending.
class CardUcode final : public Ucode {
public:
  using Ucode::Ucode;
  const char* name() const override { return "CARD"; }
  void initialize() override { push_mail(DSP_INIT, true); }
  void update() override {}
  void handle_mail(uint32_t m) override {
    if (m != 0xFF000000) {
      GCRT_AUDIO_TRACE(TraceCat::Warn, "CARD: unknown command 0x%08X", m);
    }
    push_mail(DSP_DONE);
    m_host.request_set_ucode(UCODE_ROM); // like Dolphin (the DSP_DONE mail is then discarded)
  }
};

} // namespace

const uint32_t kAxGcCrcs[] = {
    // 0x4e8a8b21: oldest version (PB without low-pass filter, different mixer_control encoding).
    0x4e8a8b21, 0x07f88145, 0xe2136399, 0x3ad3b7ac, 0x3daf59b9, 0x3389a79e, 0,
};

uint32_t hash_ector(const uint8_t* data, uint32_t len) {
  uint32_t crc = 0;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc << 3) | (crc >> 29);
  }
  return crc;
}

UcodeKind classify_ucode(uint32_t crc) {
  if (crc == UCODE_ROM) {
    return UcodeKind::Rom;
  }
  if (crc == UCODE_INIT_AUDIO_SYSTEM) {
    return UcodeKind::Init;
  }
  if (crc == CARD_UCODE_CRC) {
    return UcodeKind::Card;
  }
  for (const uint32_t* p = kAxGcCrcs; *p != 0; ++p) {
    if (*p == crc) {
      return UcodeKind::AxGc;
    }
  }
  return UcodeKind::Unknown;
}

std::unique_ptr<Ucode> make_audio_ucode(UcodeHost& host, uint32_t crc, bool force_ax_if_unknown) {
  switch (classify_ucode(crc)) {
  case UcodeKind::Card:
    return std::make_unique<CardUcode>(host, crc);
  case UcodeKind::AxGc:
    return std::make_unique<AxUcodeGc>(host, crc);
  case UcodeKind::Unknown:
    if (force_ax_if_unknown) {
      GCRT_AUDIO_TRACE(TraceCat::Warn, "unknown DSP microcode (hash 0x%08X): GameCube AX forced", crc);
      return std::make_unique<AxUcodeGc>(host, crc);
    }
    return nullptr;
  case UcodeKind::Rom:
  case UcodeKind::Init:
  default:
    return nullptr;
  }
}

const int16_t* free_polyphase_coefs() { return kFreeCoefs; }

DspStats& glue_stats() {
  static DspStats stats;
  return stats;
}

bool Ucode::needs_resume_mail() {
  if (m_needs_resume_mail) {
    m_needs_resume_mail = false;
    return true;
  }
  return false;
}

void Ucode::prepare_boot_ucode(uint32_t mail_value) {
  switch (m_next_steps) {
  case 0:
    m_next.mram_dest_addr = mail_value;
    break;
  case 1:
    m_next.mram_size = mail_value & 0xFFFF;
    break;
  case 2:
    m_next.mram_dram_addr = mail_value & 0xFFFF;
    break;
  case 3:
    m_next.iram_mram_addr = mail_value;
    break;
  case 4:
    m_next.iram_size = mail_value & 0xFFFF;
    break;
  case 5:
    m_next.iram_dest = mail_value & 0xFFFF;
    break;
  case 6:
    m_next.iram_startpc = mail_value & 0xFFFF;
    break;
  case 7:
    m_next.dram_mram_addr = mail_value;
    break;
  case 8:
    m_next.dram_size = mail_value & 0xFFFF;
    break;
  case 9:
    m_next.dram_dest = mail_value & 0xFFFF;
    break;
  default:
    break;
  }
  m_next_steps++;
  if (m_next_steps < 10) {
    return;
  }
  m_next_steps = 0;
  m_needs_resume_mail = true;
  m_upload_setup_in_progress = false;

  const uint8_t* ptr = m_host.memory().ram_ptr(m_next.iram_mram_addr, m_next.iram_size);
  const uint32_t crc = ptr ? hash_ector(ptr, m_next.iram_size) : 0;
  GCRT_AUDIO_TRACE(TraceCat::Dsp,
                   "new DSP task: IRAM 0x%08X (0x%04X bytes) -> 0x%04X, start 0x%04X, hash 0x%08X",
                   m_next.iram_mram_addr, m_next.iram_size, m_next.iram_dest, m_next.iram_startpc, crc);
  if (m_next.mram_size) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "new DSP task: DRAM -> MRAM save not emulated (like Dolphin)");
  }
  if (m_next.dram_size) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "new DSP task: MRAM -> DRAM load not emulated (like Dolphin)");
  }
  m_host.request_swap_ucode(crc);
}

} // namespace gcrt::audio
