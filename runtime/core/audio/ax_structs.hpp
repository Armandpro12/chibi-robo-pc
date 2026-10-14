// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// In-RAM structures of the GameCube AX microcode (voice parameter block, "PB").
// Adapted for gcrt from Dolphin, commit e6f3ae17627e4344da95b13424af5baf4c892b08:
//   Source/Core/Core/HW/DSPHLE/UCodes/AXStructs.h (GameCube part only).
// Changes (2026): comments translated, Wii structures removed, size assertions added.
//
// In RAM, the PB is a sequence of big-endian 16-bit words; it is copied word by word into this
// structure (16-bit members only, hence no padding). 32-bit addresses are split into two
// _hi/_lo halves.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace gcrt::audio::ax {

struct VolumeData {
  uint16_t volume;
  uint16_t volume_delta;
};

struct PBMixer {
  VolumeData main_left, main_right;
  VolumeData auxA_left, auxA_right;
  VolumeData auxB_left, auxB_right;
  // Order of the surround channels that lets the microcode use the ROM function mix_two_add().
  VolumeData auxB_surround;
  VolumeData main_surround;
  VolumeData auxA_surround;
};

struct PBInitialTimeDelay {
  uint16_t on;
  uint16_t addrMemHigh;
  uint16_t addrMemLow;
  uint16_t offsetLeft;
  uint16_t offsetRight;
  uint16_t targetLeft;
  uint16_t targetRight;
};

struct PBUpdate {
  uint16_t pb_offset;
  uint16_t new_value;
};
using PBUpdateData = std::array<PBUpdate, 32>;

// Updates per 1 ms sub-frame (one AX frame = 5 ms): allow more precise events.
struct PBUpdates {
  uint16_t num_updates[5];
  uint16_t data_hi; // address in main RAM
  uint16_t data_lo;
};

// Last value of each voice, for attenuating ("depop") voices that are cut off.
struct PBDpop {
  int16_t main_left;
  int16_t auxA_left;
  int16_t auxB_left;
  int16_t main_right;
  int16_t auxA_right;
  int16_t auxB_right;
  int16_t main_surround;
  int16_t auxA_surround;
  int16_t auxB_surround;
};

struct PBVolumeEnvelope {
  int16_t cur_volume;       // volume at the start of the frame
  int16_t cur_volume_delta; // signed change per sample
};

struct PBUnknown2 {
  uint16_t unknown_reserved[3];
};

struct PBAudioAddr {
  uint16_t looping;
  uint16_t sample_format;
  uint16_t loop_addr_hi; // loop start (points to a shared "zero" buffer in non-looping mode)
  uint16_t loop_addr_lo;
  uint16_t end_addr_hi; // end of the sample (and of the loop), inclusive
  uint16_t end_addr_lo;
  uint16_t cur_addr_hi;
  uint16_t cur_addr_lo;
};

struct PBADPCMInfo {
  int16_t coefs[16];
  uint16_t gain;
  uint16_t pred_scale;
  int16_t yn1;
  int16_t yn2;
};

struct PBSampleRateConverter {
  // ratio = (float)ratio * 0x10000; valid range 1/512 to 4.0
  uint16_t ratio_hi; // integer part
  uint16_t ratio_lo; // fractional part
  uint16_t cur_addr_frac;
  int16_t last_samples[4];
};

struct PBADPCMLoopInfo {
  uint16_t pred_scale;
  uint16_t yn1;
  uint16_t yn2;
};

struct PBLowPassFilter {
  uint16_t on;
  int16_t yn1;
  uint16_t a0;
  int16_t b0;
};

struct AXPB {
  uint16_t next_pb_hi;
  uint16_t next_pb_lo;
  uint16_t this_pb_hi;
  uint16_t this_pb_lo;

  uint16_t src_type; // sample rate converter type (polyphase, linear, none)
  uint16_t coef_select;
  uint16_t mixer_control;

  uint16_t running;   // 1 = playing, other = stopped
  uint16_t is_stream; // 1 = stream, other = one-shot sound

  PBMixer mixer;
  PBInitialTimeDelay initial_time_delay;
  PBUpdates updates;
  PBDpop dpop;
  PBVolumeEnvelope vol_env;
  PBUnknown2 unknown3;
  PBAudioAddr audio_addr;
  PBADPCMInfo adpcm;
  PBSampleRateConverter src;
  PBADPCMLoopInfo adpcm_loop_info;
  PBLowPassFilter lpf; // missing from the in-RAM PB for the oldest AX versions (see AxUcode)
  uint16_t loop_counter;

  uint16_t padding[24];
};

static_assert(sizeof(AXPB) == 122 * 2, "AXPB: 122 16-bit words expected (Dolphin AXStructs.h)");
static_assert(offsetof(AXPB, mixer) == 9 * 2, "AXPB.mixer");
static_assert(offsetof(AXPB, audio_addr) == 55 * 2, "AXPB.audio_addr");
static_assert(offsetof(AXPB, adpcm) == 63 * 2, "AXPB.adpcm");
static_assert(offsetof(AXPB, src) == 83 * 2, "AXPB.src");
static_assert(offsetof(AXPB, lpf) == 93 * 2, "AXPB.lpf");
static_assert(offsetof(AXPB, loop_counter) == 97 * 2, "AXPB.loop_counter");

// Sample formats (raw value of the accelerator FORMAT register).
enum : uint16_t {
  AUDIOFORMAT_ADPCM = 0,
  AUDIOFORMAT_PCM8 = 0x19,
  AUDIOFORMAT_PCM16 = 0xA,
};

enum : uint16_t {
  SRCTYPE_POLYPHASE = 0,
  SRCTYPE_LINEAR = 1,
  SRCTYPE_NEAREST = 2,
};

constexpr uint32_t hilo(uint16_t hi, uint16_t lo) { return (uint32_t(hi) << 16) | lo; }

} // namespace gcrt::audio::ax
