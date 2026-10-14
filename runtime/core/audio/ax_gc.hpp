// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// HLE of the GameCube AX microcode (the SDK's AX library): command lists, voices, mixing.
// Adapted for gcrt from Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08:
//   Source/Core/Core/HW/DSPHLE/UCodes/AX.{h,cpp} and AXVoice.h (AX_GC part).
// Changes (2026): memory access through GuestMemory (explicit big-endian), safeguards against looped or
// out-of-bounds command or voice lists, gcrt traces, no Dolphin save state or
// statistics; polyphase table provided by the host (UcodeHost). Audio processing identical to Dolphin.
//
// Protocol (CPU <-> DSP), every 5 ms (on each audio DMA interrupt on the CPU side):
//   CPU: 0xBABE0000 | size (16-bit words)   then   address of the command list
//   DSP: runs the list (5 ms mix = 160 samples at 32 kHz, written to RAM by CMD_OUTPUT)
//   DSP: 0xDCD10002 (DSP_YIELD) with interrupt, 2500 CPU cycles later
//   CPU: 0xCDD10003 (MAIL_CONTINUE) if it has no other DSP task, or 0xCDD10001 + 10 mails to
//        start another task (e.g. CARD), or 0xCDD10002 (back to the ROM).
#pragma once

#include <cstdint>

#include "accelerator.hpp"
#include "ax_structs.hpp"
#include "ucode.hpp"

namespace gcrt::audio {

// Simulated accelerator for an AX voice: handles the end of sample (looping or stopping the voice).
class AxHleAccelerator final : public Accelerator {
public:
  explicit AxHleAccelerator(const GuestMemory& mem) : m_mem(mem) {}
  ax::AXPB* pb = nullptr;

protected:
  void on_raw_read_end() override {}
  void on_raw_write_end() override {}
  void on_sample_read_end() override;
  uint8_t read_memory(uint32_t address) override { return m_mem.aram_read8(address); }
  void write_memory(uint32_t address, uint8_t value) override { m_mem.aram_write8(address, value); }

private:
  const GuestMemory& m_mem;
};

class AxUcodeGc final : public Ucode {
public:
  AxUcodeGc(UcodeHost& host, uint32_t crc);
  const char* name() const override { return "AX"; }
  void initialize() override;
  void handle_mail(uint32_t mail) override;
  void update() override;

  static constexpr uint32_t MAIL_CMDLIST = 0xBABE0000;
  static constexpr uint32_t MAIL_CMDLIST_MASK = 0xFFFF0000;
  // 32 samples per ms, 5 ms per AX frame.
  static constexpr uint32_t kSpms = 32;
  static constexpr uint32_t kFrame = 32 * 5;

  enum MixControl : uint32_t {
    MIX_MAIN_L = 0x000001,
    MIX_MAIN_L_RAMP = 0x000002,
    MIX_MAIN_R = 0x000004,
    MIX_MAIN_R_RAMP = 0x000008,
    MIX_MAIN_S = 0x000010,
    MIX_MAIN_S_RAMP = 0x000020,
    MIX_AUXA_L = 0x000040,
    MIX_AUXA_L_RAMP = 0x000080,
    MIX_AUXA_R = 0x000100,
    MIX_AUXA_R_RAMP = 0x000200,
    MIX_AUXA_S = 0x000400,
    MIX_AUXA_S_RAMP = 0x000800,
    MIX_AUXB_L = 0x001000,
    MIX_AUXB_L_RAMP = 0x002000,
    MIX_AUXB_R = 0x004000,
    MIX_AUXB_R_RAMP = 0x008000,
    MIX_AUXB_S = 0x010000,
    MIX_AUXB_S_RAMP = 0x020000,
    MIX_ALL_RAMPS = 0xAAAAAA,
  };

  // Access for tests: mixing buffers of the last frame (L, R, S of the main output).
  const int32_t* main_left() const { return m_main_l; }
  const int32_t* main_right() const { return m_main_r; }

private:
  enum CmdType : uint16_t {
    CMD_SETUP = 0x00,
    CMD_DL_AND_VOL_MIX = 0x01,
    CMD_PB_ADDR = 0x02,
    CMD_PROCESS = 0x03,
    CMD_MIX_AUXA = 0x04,
    CMD_MIX_AUXB = 0x05,
    CMD_UPLOAD_LRS = 0x06,
    CMD_SET_LR = 0x07,
    CMD_UNK_08 = 0x08,
    CMD_MIX_AUXB_NOWRITE = 0x09,
    CMD_UNK_0A = 0x0A,
    CMD_UNK_0B = 0x0B,
    CMD_UNK_0C = 0x0C,
    CMD_MORE = 0x0D,
    CMD_OUTPUT = 0x0E,
    CMD_END = 0x0F,
    CMD_MIX_AUXB_LR = 0x10,
    CMD_SET_OPPOSITE_LR = 0x11,
    CMD_COMPRESSOR = 0x12,
    CMD_SEND_AUX_AND_MIX = 0x13,
  };
  enum class MailState { WaitingForCmdListSize, WaitingForCmdListAddress, WaitingForNextTask };

  bool has_lpf() const { return m_crc != 0x4e8a8b21; }
  uint32_t convert_mixer_control(uint32_t mixer_control) const;
  void signal_work_end();
  void copy_cmd_list(uint32_t addr, uint16_t size);
  void handle_command_list();

  void setup_processing(uint32_t init_addr);
  void download_and_mix_with_volume(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa, uint16_t vol_auxb);
  void process_pb_list(uint32_t pb_addr);
  void mix_aux_samples(int aux_id, uint32_t write_addr, uint32_t read_addr);
  void upload_lrs(uint32_t dst_addr);
  void set_main_lr(uint32_t src_addr);
  void run_compressor(uint16_t threshold, uint16_t release_frames, uint32_t table_addr, uint32_t millis);
  void output_samples(uint32_t lr_addr, uint32_t surround_addr);
  void mix_auxb_lr(uint32_t ul_addr, uint32_t dl_addr);
  void set_opposite_lr(uint32_t src_addr);
  void send_aux_and_mix(uint32_t auxa_lrs_up, uint32_t auxb_s_up, uint32_t main_l_dl, uint32_t main_r_dl,
                        uint32_t auxb_l_dl, uint32_t auxb_r_dl);

  void read_pb(uint32_t addr, ax::AXPB& pb) const;
  void write_pb(uint32_t addr, const ax::AXPB& pb) const;
  void process_voice(ax::AXPB& pb, int32_t* const* buffers, uint32_t count, uint32_t mctrl);

  int32_t m_main_l[kFrame]{};
  int32_t m_main_r[kFrame]{};
  int32_t m_main_s[kFrame]{};
  int32_t m_auxa_l[kFrame]{};
  int32_t m_auxa_r[kFrame]{};
  int32_t m_auxa_s[kFrame]{};
  int32_t m_auxb_l[kFrame]{};
  int32_t m_auxb_r[kFrame]{};
  int32_t m_auxb_s[kFrame]{};

  uint16_t m_cmdlist[512]{};
  uint32_t m_cmdlist_size = 0;
  uint16_t m_compressor_pos = 0;
  MailState m_mail_state = MailState::WaitingForCmdListSize;
  AxHleAccelerator m_accelerator;
};

} // namespace gcrt::audio
