// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// HLE of the GameCube AX microcode. Adapted from Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08:
// Source/Core/Core/HW/DSPHLE/UCodes/AX.cpp and AXVoice.h (see ax_gc.hpp for the changes).
#include "ax_gc.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "audio_trace.hpp"

namespace gcrt::audio {

using namespace ax;

namespace {

int16_t clamp_s16(int64_t sample) { return static_cast<int16_t>(std::clamp<int64_t>(sample, -0x8000, 0x7FFF)); }

// Produces `count` output samples, reading input samples through `input()`. With conversion (polyphase or linear),
// the position moves by `ratio` per output sample and one input sample is read each time it crosses an integer: about
// `count * ratio / 0x10000` input samples. Without conversion (SRCTYPE_NEAREST), exactly `count` are read and the
// position does not move. `curr_pos` and `ratio` are 16.16 fixed-point numbers. Returns the final position.
// Same logic as Dolphin (AXVoice.h, ResampleAudio).
template <typename Input>
uint32_t resample_audio(Input&& input, int16_t* output, uint32_t count, int16_t* last_samples, uint32_t curr_pos,
                        uint32_t ratio, uint16_t srctype, const int16_t* coeffs) {
  if (coeffs != nullptr && srctype == SRCTYPE_POLYPHASE) {
    int16_t temp[4];
    uint32_t idx = 0;
    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];
    for (uint32_t i = 0; i < count; ++i) {
      curr_pos += ratio;
      while (curr_pos >= 0x10000) {
        temp[idx++ & 3] = input();
        curr_pos -= 0x10000;
      }
      const uint16_t curr_pos_frac = static_cast<uint16_t>(((curr_pos & 0xFFFF) >> 9) << 2);
      const int16_t* c = &coeffs[curr_pos_frac];
      const int64_t t0 = temp[idx++ & 3];
      const int64_t t1 = temp[idx++ & 3];
      const int64_t t2 = temp[idx++ & 3];
      const int64_t t3 = temp[idx++ & 3];
      const int64_t samp = (t0 * c[0] + t1 * c[1] + t2 * c[2] + t3 * c[3]) >> 15;
      output[i] = clamp_s16(samp);
    }
    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  } else if (srctype == SRCTYPE_LINEAR || srctype == SRCTYPE_POLYPHASE) {
    // Circular buffer of the last 4 samples, initialised from the PB and written back at the end.
    int16_t temp[4];
    uint32_t idx = 0;
    temp[idx++ & 3] = last_samples[0];
    temp[idx++ & 3] = last_samples[1];
    temp[idx++ & 3] = last_samples[2];
    temp[idx++ & 3] = last_samples[3];
    for (uint32_t i = 0; i < count; ++i) {
      curr_pos += ratio;
      while (curr_pos >= 0x10000) {
        temp[idx++ & 3] = input();
        curr_pos -= 0x10000;
      }
      const uint16_t curr_frac = curr_pos & 0xFFFF;
      const uint16_t inv_curr_frac = static_cast<uint16_t>(-curr_frac);
      int16_t sample;
      if (curr_frac) {
        const int32_t s0 = temp[idx++ & 3];
        const int32_t s1 = temp[idx++ & 3];
        sample = static_cast<int16_t>(((s0 * inv_curr_frac) + (s1 * curr_frac)) >> 16);
        idx += 2;
      } else {
        sample = temp[idx++ & 3];
        idx += 3;
      }
      output[i] = sample;
    }
    last_samples[3] = temp[--idx & 3];
    last_samples[2] = temp[--idx & 3];
    last_samples[1] = temp[--idx & 3];
    last_samples[0] = temp[--idx & 3];
  } else { // SRCTYPE_NEAREST: no conversion, direct read
    for (uint32_t i = 0; i < count; ++i) {
      output[i] = input();
    }
    std::memcpy(last_samples, output + count - 4, 4 * sizeof(int16_t));
  }
  return curr_pos;
}

// Adds samples to an output buffer, with an optional volume ramp.
void mix_add(int32_t* out, const int16_t* input, uint32_t count, VolumeData* vd, int16_t* dpop, bool ramp) {
  uint16_t& volume = vd->volume;
  const uint16_t volume_delta = ramp ? vd->volume_delta : 0;
  for (uint32_t i = 0; i < count; ++i) {
    int64_t sample = input[i];
    sample *= volume;
    sample >>= 15;
    const int16_t sample16 = clamp_s16(static_cast<int32_t>(sample));
    out[i] += sample16;
    volume = static_cast<uint16_t>(volume + volume_delta);
    *dpop = sample16;
  }
}

// One-pole low-pass filter.
void low_pass_filter(int16_t* samples, uint32_t count, PBLowPassFilter& f) {
  for (uint32_t i = 0; i < count; ++i) {
    f.yn1 = samples[i] = clamp_s16((f.a0 * static_cast<int32_t>(samples[i]) + f.b0 * static_cast<int32_t>(f.yn1)) >> 15);
  }
}

void apply_updates_for_ms(int curr_ms, AXPB& pb, const PBUpdateData& updates) {
  uint32_t start_idx = 0;
  for (int i = 0; i < curr_ms; ++i) {
    start_idx += pb.updates.num_updates[i];
  }
  if (start_idx >= updates.size()) {
    return;
  }
  const uint16_t count = pb.updates.num_updates[curr_ms];
  if (count > updates.size() - start_idx) {
    return;
  }
  uint16_t words[sizeof(AXPB) / 2];
  std::memcpy(words, &pb, sizeof(AXPB));
  for (uint32_t i = start_idx; i < start_idx + count; ++i) {
    const uint16_t off = updates[i].pb_offset;
    if (off < sizeof(AXPB) / 2) {
      words[off] = updates[i].new_value;
    }
  }
  std::memcpy(&pb, words, sizeof(AXPB));
}

} // namespace

void AxHleAccelerator::on_sample_read_end() {
  if (pb->audio_addr.looping) {
    // Resume ADPCM decoding at the loop start.
    set_pred_scale(pb->adpcm_loop_info.pred_scale);
    if (pb->is_stream != 1) {
      set_yn1(static_cast<int16_t>(pb->adpcm_loop_info.yn1));
      set_yn2(static_cast<int16_t>(pb->adpcm_loop_info.yn2));
    } else {
      // Stream: the history is kept; writing YN1/YN2 again restarts the accelerator's reads.
      set_yn1(yn1());
      set_yn2(yn2());
      pb->loop_counter++; // GameCube: stream loop counter
    }
  } else {
    // Non-looping voice that reached its end: stop.
    pb->running = 0;
  }
}

AxUcodeGc::AxUcodeGc(UcodeHost& host, uint32_t crc) : Ucode(host, crc), m_accelerator(host.memory()) {}

void AxUcodeGc::initialize() { push_mail(DSP_INIT, true); }

void AxUcodeGc::signal_work_end() {
  // End of processing. Dolphin: 2500 CPU cycle delay (minimal empty list); some games crash if the
  // interrupt arrives too soon after the list was sent (see bugs.dolphin-emu.org/issues/10265).
  constexpr uint32_t kAxEmptyCommandListCycles = 2500;
  push_mail(DSP_YIELD, true, kAxEmptyCommandListCycles);
}

uint32_t AxUcodeGc::convert_mixer_control(uint32_t mixer_control) const {
  uint32_t ret = 0;
  if (m_crc == 0x4e8a8b21) {
    // Oldest AX version.
    if (mixer_control & 0x0010) {
      // DPL2 mixing
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if ((mixer_control & 0x0006) == 0) {
        ret |= MIX_AUXB_L | MIX_AUXB_R;
      }
      if ((mixer_control & 0x0007) == 1) {
        ret |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_S;
      }
    } else {
      ret |= MIX_MAIN_L | MIX_MAIN_R;
      if (mixer_control & 0x0001) {
        ret |= MIX_AUXA_L | MIX_AUXA_R;
      }
      if (mixer_control & 0x0002) {
        ret |= MIX_AUXB_L | MIX_AUXB_R;
      }
      if (mixer_control & 0x0004) {
        ret |= MIX_MAIN_S;
        if (ret & MIX_AUXA_L) {
          ret |= MIX_AUXA_S;
        }
        if (ret & MIX_AUXB_L) {
          ret |= MIX_AUXB_S;
        }
      }
    }
    if (mixer_control & 0x0008) {
      ret |= MIX_ALL_RAMPS;
    }
  } else {
    // More recent GameCube versions.
    if (mixer_control & 0x0001) ret |= MIX_MAIN_L;
    if (mixer_control & 0x0002) ret |= MIX_MAIN_R;
    if (mixer_control & 0x0004) ret |= MIX_MAIN_S;
    if (mixer_control & 0x0008) ret |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;
    if (mixer_control & 0x0010) ret |= MIX_AUXA_L;
    if (mixer_control & 0x0020) ret |= MIX_AUXA_R;
    if (mixer_control & 0x0040) ret |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
    if (mixer_control & 0x0080) ret |= MIX_AUXA_S;
    if (mixer_control & 0x0100) ret |= MIX_AUXA_S_RAMP;
    if (mixer_control & 0x0200) ret |= MIX_AUXB_L;
    if (mixer_control & 0x0400) ret |= MIX_AUXB_R;
    if (mixer_control & 0x0800) ret |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
    if (mixer_control & 0x1000) ret |= MIX_AUXB_S;
    if (mixer_control & 0x2000) ret |= MIX_AUXB_S_RAMP;
    // 0x4000: Dolby Pro Logic II (surround input of the AUXB channels); not handled, like Dolphin.
  }
  return ret;
}

void AxUcodeGc::copy_cmd_list(uint32_t addr, uint16_t size) {
  if (size >= std::size(m_cmdlist)) {
    GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: command list too large (0x%08X, %u words)", addr, size);
    return;
  }
  m_host.memory().read16_array(addr, m_cmdlist, size);
}

void AxUcodeGc::handle_command_list() {
  uint32_t pb_addr = 0;
  uint32_t curr_idx = 0;
  uint32_t more_count = 0;
  bool end = false;
  m_host.stats().ax_command_lists++;
  auto next = [&]() -> uint16_t {
    if (curr_idx >= std::size(m_cmdlist)) {
      end = true;
      return 0;
    }
    return m_cmdlist[curr_idx++];
  };
  auto next32 = [&]() -> uint32_t {
    const uint16_t hi = next();
    const uint16_t lo = next();
    return hilo(hi, lo);
  };

  while (!end) {
    if (curr_idx >= std::size(m_cmdlist)) {
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: command list without CMD_END");
      break;
    }
    const uint16_t cmd = m_cmdlist[curr_idx++];
    switch (cmd) {
    case CMD_SETUP: {
      const uint32_t a = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "SETUP 0x%08X", a);
      setup_processing(a);
      break;
    }
    case CMD_DL_AND_VOL_MIX: {
      const uint32_t a = next32();
      const uint16_t vol_main = next();
      const uint16_t vol_auxa = next();
      const uint16_t vol_auxb = next();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "DL_AND_VOL_MIX 0x%08X %04X %04X %04X", a, vol_main, vol_auxa, vol_auxb);
      download_and_mix_with_volume(a, vol_main, vol_auxa, vol_auxb);
      break;
    }
    case CMD_PB_ADDR:
      pb_addr = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "PB_ADDR 0x%08X", pb_addr);
      break;
    case CMD_PROCESS:
      GCRT_AUDIO_TRACE(TraceCat::Ax, "PROCESS");
      process_pb_list(pb_addr);
      break;
    case CMD_MIX_AUXA:
    case CMD_MIX_AUXB: {
      // Handled almost the same way by the microcode.
      const uint32_t a = next32();
      const uint32_t b = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "MIX_AUX%c 0x%08X 0x%08X", cmd == CMD_MIX_AUXA ? 'A' : 'B', a, b);
      mix_aux_samples(cmd - CMD_MIX_AUXA, a, b);
      break;
    }
    case CMD_UPLOAD_LRS: {
      const uint32_t a = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "UPLOAD_LRS 0x%08X", a);
      upload_lrs(a);
      break;
    }
    case CMD_SET_LR: {
      const uint32_t a = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "SET_LR 0x%08X", a);
      set_main_lr(a);
      break;
    }
    case CMD_UNK_08:
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: command 0x08 not implemented (ignored, like Dolphin)");
      curr_idx += 10;
      break;
    case CMD_MIX_AUXB_NOWRITE: {
      const uint32_t a = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "MIX_AUXB_NOWRITE 0x%08X", a);
      mix_aux_samples(1, 0, a);
      break;
    }
    case CMD_UNK_0A:
    case CMD_UNK_0B:
    case CMD_UNK_0C:
      // No effect in the known GameCube AX microcodes.
      break;
    case CMD_MORE: {
      const uint32_t a = next32();
      const uint16_t size = next();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "MORE 0x%08X (%u words)", a, size);
      if (++more_count > 64) {
        GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: too many chained CMD_MORE, list abandoned");
        end = true;
        break;
      }
      copy_cmd_list(a, size);
      curr_idx = 0;
      break;
    }
    case CMD_OUTPUT: {
      const uint32_t surround = next32();
      const uint32_t lr = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "OUTPUT LR 0x%08X S 0x%08X", lr, surround);
      output_samples(lr, surround);
      break;
    }
    case CMD_END:
      GCRT_AUDIO_TRACE(TraceCat::Ax, "END");
      end = true;
      break;
    case CMD_MIX_AUXB_LR: {
      const uint32_t a = next32();
      const uint32_t b = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "MIX_AUXB_LR 0x%08X 0x%08X", a, b);
      mix_auxb_lr(a, b);
      break;
    }
    case CMD_SET_OPPOSITE_LR: {
      const uint32_t a = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "SET_OPPOSITE_LR 0x%08X", a);
      set_opposite_lr(a);
      break;
    }
    case CMD_COMPRESSOR: {
      // The oldest version (0x4e8a8b21) does not have this command.
      if (m_crc == 0x4e8a8b21) {
        GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: CMD_COMPRESSOR received by a version that does not have it");
      }
      const uint16_t threshold = next();
      const uint16_t frames = next();
      const uint32_t a = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "COMPRESSOR threshold %u, %u frames, table 0x%08X", threshold, frames, a);
      run_compressor(threshold, frames, a, 5);
      break;
    }
    case CMD_SEND_AUX_AND_MIX: {
      const uint32_t auxa_lrs_up = next32();
      const uint32_t auxb_s_up = next32();
      const uint32_t main_l_dl = next32();
      const uint32_t main_r_dl = next32();
      const uint32_t auxb_l_dl = next32();
      const uint32_t auxb_r_dl = next32();
      GCRT_AUDIO_TRACE(TraceCat::Ax, "SEND_AUX_AND_MIX");
      send_aux_and_mix(auxa_lrs_up, auxb_s_up, main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl);
      break;
    }
    default:
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: unknown command 0x%04X in the list", cmd);
      end = true;
      break;
    }
  }
}

void AxUcodeGc::setup_processing(uint32_t init_addr) {
  int32_t* const buffers[9] = {m_main_l, m_main_r, m_main_s, m_auxa_l, m_auxa_r, m_auxa_s, m_auxb_l, m_auxb_r, m_auxb_s};
  uint16_t init[3 * 9];
  m_host.memory().read16_array(init_addr, init, 3 * 9);
  for (uint32_t i = 0; i < 9; ++i) {
    const int32_t value = static_cast<int32_t>((uint32_t(init[3 * i]) << 16) | init[3 * i + 1]);
    const int16_t delta = static_cast<int16_t>(init[3 * i + 2]);
    if (value == 0) {
      std::memset(buffers[i], 0, kFrame * sizeof(int32_t));
    } else {
      for (uint32_t j = 0; j < kFrame; ++j) {
        buffers[i][j] = value + static_cast<int32_t>(j) * delta;
      }
    }
  }
}

void AxUcodeGc::download_and_mix_with_volume(uint32_t addr, uint16_t vol_main, uint16_t vol_auxa,
                                             uint16_t vol_auxb) {
  int32_t* const buffers[3][3] = {
      {m_main_l, m_main_r, m_main_s}, {m_auxa_l, m_auxa_r, m_auxa_s}, {m_auxb_l, m_auxb_r, m_auxb_s}};
  const uint16_t volumes[3] = {vol_main, vol_auxa, vol_auxb};
  static thread_local int32_t data[3 * kFrame];
  for (uint32_t i = 0; i < 3; ++i) {
    // Like Dolphin: reading restarts from `addr` for each of the three groups (main, AUXA, AUXB).
    m_host.memory().read32s_array(addr, data, 3 * kFrame);
    const int32_t* ptr = data;
    for (uint32_t j = 0; j < 3; ++j) {
      int32_t* buffer = buffers[i][j];
      for (uint32_t k = 0; k < kFrame; ++k) {
        int64_t sample = *ptr++;
        sample *= volumes[i];
        buffer[k] += static_cast<int32_t>(sample >> 15);
      }
    }
  }
}

void AxUcodeGc::read_pb(uint32_t addr, AXPB& pb) const {
  uint16_t words[sizeof(AXPB) / 2];
  if (has_lpf()) {
    m_host.memory().read16_array(addr, words, sizeof(AXPB) / 2);
  } else {
    // Version without low-pass filter: the lpf field does not exist in RAM.
    constexpr uint32_t lpf_w = offsetof(AXPB, lpf) / 2;
    constexpr uint32_t lc_w = offsetof(AXPB, loop_counter) / 2;
    m_host.memory().read16_array(addr, words, lpf_w);
    std::fill(words + lpf_w, words + lc_w, uint16_t(0));
    m_host.memory().read16_array(addr + lpf_w * 2, words + lc_w, sizeof(AXPB) / 2 - lc_w);
  }
  std::memcpy(&pb, words, sizeof(AXPB));
}

void AxUcodeGc::write_pb(uint32_t addr, const AXPB& pb) const {
  uint16_t words[sizeof(AXPB) / 2];
  std::memcpy(words, &pb, sizeof(AXPB));
  if (has_lpf()) {
    m_host.memory().write16_array(addr, words, sizeof(AXPB) / 2);
  } else {
    constexpr uint32_t lpf_w = offsetof(AXPB, lpf) / 2;
    constexpr uint32_t lc_w = offsetof(AXPB, loop_counter) / 2;
    m_host.memory().write16_array(addr, words, lpf_w);
    m_host.memory().write16_array(addr + lpf_w * 2, words + lc_w, sizeof(AXPB) / 2 - lc_w);
  }
}

void AxUcodeGc::process_voice(AXPB& pb, int32_t* const* buf, uint32_t count, uint32_t mctrl) {
  if (pb.running != 1) {
    return;
  }

  // Read the input samples (accelerator) with sample rate conversion.
  int16_t samples[kSpms];
  AxHleAccelerator& acc = m_accelerator;
  acc.pb = &pb;
  acc.set_start_address(hilo(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo));
  acc.set_end_address(hilo(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo));
  acc.set_current_address(hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo));
  acc.set_sample_format(pb.audio_addr.sample_format);
  acc.set_yn1(pb.adpcm.yn1);
  acc.set_yn2(pb.adpcm.yn2);
  acc.set_gain(static_cast<int16_t>(pb.adpcm.gain));
  acc.set_pred_scale(pb.adpcm.pred_scale);

  const int16_t* coeffs = m_host.polyphase_coefs();
  if (coeffs != nullptr) {
    if (pb.coef_select < 4) {
      coeffs += pb.coef_select * 0x200;
    } else {
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: coef_select %u outside the table, linear interpolation", pb.coef_select);
      coeffs = nullptr;
    }
  }
  const int16_t* adpcm_coefs = pb.adpcm.coefs;
  const uint32_t curr_pos = resample_audio([&acc, adpcm_coefs]() { return static_cast<int16_t>(acc.read_sample(adpcm_coefs)); },
                                           samples, count, pb.src.last_samples, pb.src.cur_addr_frac,
                                           hilo(pb.src.ratio_hi, pb.src.ratio_lo), pb.src_type, coeffs);
  pb.src.cur_addr_frac = static_cast<uint16_t>(curr_pos & 0xFFFF);
  pb.audio_addr.cur_addr_hi = static_cast<uint16_t>(acc.current_address() >> 16);
  pb.audio_addr.cur_addr_lo = static_cast<uint16_t>(acc.current_address());
  pb.adpcm.yn1 = acc.yn1();
  pb.adpcm.yn2 = acc.yn2();
  pb.adpcm.pred_scale = acc.pred_scale();

  // Global volume envelope (signed on GameCube).
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t volume = pb.vol_env.cur_volume;
    const int32_t sample = (static_cast<int32_t>(samples[i]) * volume) >> 15;
    samples[i] = clamp_s16(sample);
    pb.vol_env.cur_volume = static_cast<int16_t>(pb.vol_env.cur_volume + pb.vol_env.cur_volume_delta);
  }

  if (pb.lpf.on != 0) {
    low_pass_filter(samples, count, pb.lpf);
  }

  const auto on = [mctrl](uint32_t bit) { return (mctrl & bit) != 0; };
  if (on(MIX_MAIN_L)) mix_add(buf[0], samples, count, &pb.mixer.main_left, &pb.dpop.main_left, on(MIX_MAIN_L_RAMP));
  if (on(MIX_MAIN_R)) mix_add(buf[1], samples, count, &pb.mixer.main_right, &pb.dpop.main_right, on(MIX_MAIN_R_RAMP));
  if (on(MIX_MAIN_S))
    mix_add(buf[2], samples, count, &pb.mixer.main_surround, &pb.dpop.main_surround, on(MIX_MAIN_S_RAMP));
  if (on(MIX_AUXA_L)) mix_add(buf[3], samples, count, &pb.mixer.auxA_left, &pb.dpop.auxA_left, on(MIX_AUXA_L_RAMP));
  if (on(MIX_AUXA_R)) mix_add(buf[4], samples, count, &pb.mixer.auxA_right, &pb.dpop.auxA_right, on(MIX_AUXA_R_RAMP));
  if (on(MIX_AUXA_S))
    mix_add(buf[5], samples, count, &pb.mixer.auxA_surround, &pb.dpop.auxA_surround, on(MIX_AUXA_S_RAMP));
  if (on(MIX_AUXB_L)) mix_add(buf[6], samples, count, &pb.mixer.auxB_left, &pb.dpop.auxB_left, on(MIX_AUXB_L_RAMP));
  if (on(MIX_AUXB_R)) mix_add(buf[7], samples, count, &pb.mixer.auxB_right, &pb.dpop.auxB_right, on(MIX_AUXB_R_RAMP));
  if (on(MIX_AUXB_S))
    mix_add(buf[8], samples, count, &pb.mixer.auxB_surround, &pb.dpop.auxB_surround, on(MIX_AUXB_S_RAMP));

  // Initial time delay (ITD, 3D sound): not emulated, like Dolphin.
}

void AxUcodeGc::process_pb_list(uint32_t pb_addr) {
  AXPB pb;
  uint32_t guard = 0;
  while (pb_addr) {
    if (++guard > 4096) {
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: voice list too long or looped, stopping");
      break;
    }
    int32_t* buffers[9] = {m_main_l, m_main_r, m_main_s, m_auxa_l, m_auxa_r, m_auxa_s, m_auxb_l, m_auxb_r, m_auxb_s};
    read_pb(pb_addr, pb);

    PBUpdateData updates{};
    uint32_t total_updates = 0;
    for (uint16_t n : pb.updates.num_updates) {
      total_updates += n;
    }
    if (total_updates != 0) {
      uint16_t raw[64];
      m_host.memory().read16_array(hilo(pb.updates.data_hi, pb.updates.data_lo), raw, 64);
      for (uint32_t i = 0; i < 32; ++i) {
        updates[i].pb_offset = raw[2 * i];
        updates[i].new_value = raw[2 * i + 1];
      }
    }

    if (trace_on(TraceCat::Voice) && pb.running == 1) {
      GCRT_AUDIO_TRACE(TraceCat::Voice,
                       "PB 0x%08X: fmt 0x%X src %u ratio 0x%08X cur 0x%08X loop 0x%08X end 0x%08X vol %d mix 0x%04X "
                       "L %04X R %04X stream %u",
                       pb_addr, pb.audio_addr.sample_format, pb.src_type, hilo(pb.src.ratio_hi, pb.src.ratio_lo),
                       hilo(pb.audio_addr.cur_addr_hi, pb.audio_addr.cur_addr_lo),
                       hilo(pb.audio_addr.loop_addr_hi, pb.audio_addr.loop_addr_lo),
                       hilo(pb.audio_addr.end_addr_hi, pb.audio_addr.end_addr_lo), pb.vol_env.cur_volume,
                       pb.mixer_control, pb.mixer.main_left.volume, pb.mixer.main_right.volume, pb.is_stream);
    }
    if (pb.running == 1) {
      m_host.stats().ax_voices_processed++;
    }

    for (int curr_ms = 0; curr_ms < 5; ++curr_ms) {
      if (total_updates != 0) {
        apply_updates_for_ms(curr_ms, pb, updates);
      }
      process_voice(pb, buffers, kSpms, convert_mixer_control(pb.mixer_control));
      for (auto& ptr : buffers) {
        ptr += kSpms;
      }
    }

    write_pb(pb_addr, pb);
    pb_addr = hilo(pb.next_pb_hi, pb.next_pb_lo);
  }
}

void AxUcodeGc::mix_aux_samples(int aux_id, uint32_t write_addr, uint32_t read_addr) {
  int32_t* buffers[3];
  if (aux_id == 0) {
    buffers[0] = m_auxa_l;
    buffers[1] = m_auxa_r;
    buffers[2] = m_auxa_s;
  } else {
    buffers[0] = m_auxb_l;
    buffers[1] = m_auxb_r;
    buffers[2] = m_auxb_s;
  }
  // Send the AUX buffers to the CPU (effects), then get the processed data back, added to the main output.
  if (write_addr) {
    for (int32_t* b : buffers) {
      m_host.memory().write32s_array(write_addr, b, kFrame);
      write_addr += kFrame * sizeof(int32_t);
    }
  }
  int32_t data[3 * kFrame];
  m_host.memory().read32s_array(read_addr, data, 3 * kFrame);
  for (uint32_t i = 0; i < kFrame; ++i) {
    m_main_l[i] += data[i];
  }
  for (uint32_t i = 0; i < kFrame; ++i) {
    m_main_r[i] += data[kFrame + i];
  }
  for (uint32_t i = 0; i < kFrame; ++i) {
    m_main_s[i] += data[2 * kFrame + i];
  }
}

void AxUcodeGc::upload_lrs(uint32_t dst_addr) {
  for (const int32_t* b : {m_main_l, m_main_r, m_main_s}) {
    m_host.memory().write32s_array(dst_addr, b, kFrame);
    dst_addr += kFrame * sizeof(int32_t);
  }
}

void AxUcodeGc::set_main_lr(uint32_t src_addr) {
  int32_t data[kFrame];
  m_host.memory().read32s_array(src_addr, data, kFrame);
  for (uint32_t i = 0; i < kFrame; ++i) {
    m_main_l[i] = data[i];
    m_main_r[i] = data[i];
    m_main_s[i] = 0;
  }
}

void AxUcodeGc::run_compressor(uint16_t threshold, uint16_t release_frames, uint32_t table_addr, uint32_t millis) {
  bool triggered = false;
  for (uint32_t i = 0; i < 32 * millis; ++i) {
    if (std::abs(m_main_l[i]) > int32_t(threshold) || std::abs(m_main_r[i]) > int32_t(threshold)) {
      triggered = true;
      break;
    }
  }
  const uint32_t frame_byte_size = 32 * millis * sizeof(int16_t);
  uint32_t table_offset = 0;
  if (triggered) {
    // one attack frame, based on the previous frame; the next one starts the release
    table_offset = m_compressor_pos * frame_byte_size;
    m_compressor_pos = release_frames;
  } else if (m_compressor_pos) {
    --m_compressor_pos;
    constexpr uint32_t ATTACK_ENTRY_COUNT = 11; // the release ramps follow the attack ramps
    table_offset = (ATTACK_ENTRY_COUNT + m_compressor_pos) * frame_byte_size;
  } else {
    return;
  }
  uint16_t ramp[32 * 5];
  const uint32_t n = std::min<uint32_t>(32 * millis, 32 * 5);
  m_host.memory().read16_array(table_addr + table_offset, ramp, n);
  for (uint32_t i = 0; i < n; ++i) {
    const uint16_t coef = ramp[i];
    m_main_l[i] = static_cast<int32_t>((int64_t(m_main_l[i]) * coef) >> 15);
    m_main_r[i] = static_cast<int32_t>((int64_t(m_main_r[i]) * coef) >> 15);
  }
}

void AxUcodeGc::output_samples(uint32_t lr_addr, uint32_t surround_addr) {
  m_host.memory().write32s_array(surround_addr, m_main_s, kFrame);
  // 32 samples per ms, 5 ms, 2 channels, saturated to 16 bits, interleaved R L R L... (big-endian).
  uint16_t buffer[kFrame * 2];
  for (uint32_t i = 0; i < kFrame; ++i) {
    buffer[2 * i + 0] = static_cast<uint16_t>(clamp_s16(m_main_r[i]));
    buffer[2 * i + 1] = static_cast<uint16_t>(clamp_s16(m_main_l[i]));
  }
  m_host.memory().write16_array(lr_addr, buffer, kFrame * 2);
}

void AxUcodeGc::mix_auxb_lr(uint32_t ul_addr, uint32_t dl_addr) {
  m_host.memory().write32s_array(ul_addr, m_auxb_l, kFrame);
  m_host.memory().write32s_array(ul_addr + kFrame * sizeof(int32_t), m_auxb_r, kFrame);
  int32_t data[2 * kFrame];
  m_host.memory().read32s_array(dl_addr, data, 2 * kFrame);
  for (uint32_t i = 0; i < kFrame; ++i) {
    m_auxb_l[i] = data[i];
    m_main_l[i] += data[i];
  }
  for (uint32_t i = 0; i < kFrame; ++i) {
    m_auxb_r[i] = data[kFrame + i];
    m_main_r[i] += data[kFrame + i];
  }
}

void AxUcodeGc::set_opposite_lr(uint32_t src_addr) {
  int32_t data[kFrame];
  m_host.memory().read32s_array(src_addr, data, kFrame);
  for (uint32_t i = 0; i < kFrame; ++i) {
    m_main_l[i] = -data[i];
    m_main_r[i] = data[i];
    m_main_s[i] = 0;
  }
}

void AxUcodeGc::send_aux_and_mix(uint32_t auxa_lrs_up, uint32_t auxb_s_up, uint32_t main_l_dl, uint32_t main_r_dl,
                                 uint32_t auxb_l_dl, uint32_t auxb_r_dl) {
  const GuestMemory& mem = m_host.memory();
  mem.write32s_array(auxa_lrs_up, m_auxa_l, kFrame);
  mem.write32s_array(auxa_lrs_up + kFrame * 4, m_auxa_r, kFrame);
  mem.write32s_array(auxa_lrs_up + 2 * kFrame * 4, m_auxa_s, kFrame);
  mem.write32s_array(auxb_s_up, m_auxb_s, kFrame);
  int32_t* const dl_buffers[4] = {m_main_l, m_main_r, m_auxb_l, m_auxb_r};
  const uint32_t dl_addrs[4] = {main_l_dl, main_r_dl, auxb_l_dl, auxb_r_dl};
  int32_t data[kFrame];
  for (size_t i = 0; i < 4; ++i) {
    mem.read32s_array(dl_addrs[i], data, kFrame);
    for (uint32_t j = 0; j < kFrame; ++j) {
      dl_buffers[i][j] += data[j];
    }
  }
}

void AxUcodeGc::handle_mail(uint32_t m) {
  if (m_upload_setup_in_progress) {
    prepare_boot_ucode(m);
    return;
  }
  switch (m_mail_state) {
  case MailState::WaitingForCmdListSize:
    if ((m & MAIL_CMDLIST_MASK) == MAIL_CMDLIST) {
      m_cmdlist_size = static_cast<uint16_t>(m & ~MAIL_CMDLIST_MASK);
      m_mail_state = MailState::WaitingForCmdListAddress;
    } else {
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: unexpected mail 0x%08X (list size expected)", m);
    }
    break;
  case MailState::WaitingForCmdListAddress:
    copy_cmd_list(m, static_cast<uint16_t>(m_cmdlist_size));
    handle_command_list();
    m_cmdlist_size = 0;
    signal_work_end();
    m_mail_state = MailState::WaitingForNextTask;
    break;
  case MailState::WaitingForNextTask:
    if ((m & TASK_MAIL_MASK) != TASK_MAIL_TO_DSP) {
      // The real microcode does not check the CDD1 prefix.
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: task mail without the CDD1 prefix: 0x%08X", m);
      m = TASK_MAIL_TO_DSP | (m & ~TASK_MAIL_MASK);
    }
    switch (m) {
    case MAIL_RESUME:
      push_mail(DSP_RESUME, true);
      m_mail_state = MailState::WaitingForCmdListSize;
      break;
    case MAIL_NEW_UCODE:
      m_upload_setup_in_progress = true;
      m_mail_state = MailState::WaitingForCmdListSize;
      break;
    case MAIL_RESET:
      m_host.request_set_ucode(UCODE_ROM);
      break;
    case MAIL_CONTINUE:
      // The CPU does not wait for an acknowledgement and sends the next list right after.
      m_mail_state = MailState::WaitingForCmdListSize;
      break;
    default:
      GCRT_AUDIO_TRACE(TraceCat::Warn, "AX: unknown task mail 0x%08X", m);
      break;
    }
    break;
  }
}

void AxUcodeGc::update() {
  // After a microcode (task) switch followed by a return to AX.
  if (needs_resume_mail()) {
    push_mail(DSP_RESUME, true);
  }
}

} // namespace gcrt::audio
