// View of guest memory as the DSP sees it (generic GameCube).
//
// - Main RAM (MEM1, 24 MiB), big-endian bytes: the DSP accesses it by DMA with physical
//   addresses (the SDK converts with OSCachedToPhysical). Virtual addresses 0x8xxxxxxx / 0xCxxxxxxx
//   are accepted too, by clearing the two most significant bits, like a physical address.
// - ARAM (16 MiB): read by the DSP accelerator (voice samples); address masked to the size
//   (Dolphin DSP.cpp, ReadARAM: `address & ARAM_MASK`, commit e6f3ae17).
//
// Any access outside RAM is ignored (read = 0) and reported in the "audiowarn" trace: an
// HLE microcode must never crash the runtime on a wrong address.
#pragma once

#include <cstdint>
#include <cstring>

#include "audio_trace.hpp"

namespace gcrt::audio {

struct GuestMemory {
  uint8_t* ram = nullptr;
  uint32_t ram_size = 0;
  uint8_t* aram = nullptr;
  uint32_t aram_size = 0; // power of two (16 MiB on GameCube)

  // Converts a DSP/physical address into an offset in RAM; false if [addr, addr+len) overflows.
  bool ram_offset(uint32_t addr, uint32_t len, uint32_t& off) const {
    const uint32_t phys = addr & 0x3FFFFFFFu;
    if (ram == nullptr || phys >= ram_size || len > ram_size - phys) {
      GCRT_AUDIO_TRACE(TraceCat::Warn, "DSP access outside RAM: address 0x%08X, %u bytes", addr, len);
      return false;
    }
    off = phys;
    return true;
  }

  uint16_t read16(uint32_t addr) const {
    uint32_t o;
    if (!ram_offset(addr, 2, o)) {
      return 0;
    }
    return static_cast<uint16_t>((ram[o] << 8) | ram[o + 1]);
  }
  uint32_t read32(uint32_t addr) const {
    uint32_t o;
    if (!ram_offset(addr, 4, o)) {
      return 0;
    }
    return (uint32_t(ram[o]) << 24) | (uint32_t(ram[o + 1]) << 16) | (uint32_t(ram[o + 2]) << 8) | ram[o + 3];
  }
  void write16(uint32_t addr, uint16_t v) const {
    uint32_t o;
    if (!ram_offset(addr, 2, o)) {
      return;
    }
    ram[o] = static_cast<uint8_t>(v >> 8);
    ram[o + 1] = static_cast<uint8_t>(v);
  }
  void write32(uint32_t addr, uint32_t v) const {
    uint32_t o;
    if (!ram_offset(addr, 4, o)) {
      return;
    }
    ram[o] = static_cast<uint8_t>(v >> 24);
    ram[o + 1] = static_cast<uint8_t>(v >> 16);
    ram[o + 2] = static_cast<uint8_t>(v >> 8);
    ram[o + 3] = static_cast<uint8_t>(v);
  }

  // Copies `count` big-endian 16-bit words to the host (zeros if outside RAM).
  void read16_array(uint32_t addr, uint16_t* dst, uint32_t count) const {
    uint32_t o;
    if (!ram_offset(addr, count * 2, o)) {
      std::memset(dst, 0, count * sizeof(uint16_t));
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      dst[i] = static_cast<uint16_t>((ram[o + 2 * i] << 8) | ram[o + 2 * i + 1]);
    }
  }
  void write16_array(uint32_t addr, const uint16_t* src, uint32_t count) const {
    uint32_t o;
    if (!ram_offset(addr, count * 2, o)) {
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      ram[o + 2 * i] = static_cast<uint8_t>(src[i] >> 8);
      ram[o + 2 * i + 1] = static_cast<uint8_t>(src[i]);
    }
  }
  // Big-endian signed 32-bit integers (AX mixing buffers).
  void read32s_array(uint32_t addr, int32_t* dst, uint32_t count) const {
    uint32_t o;
    if (!ram_offset(addr, count * 4, o)) {
      std::memset(dst, 0, count * sizeof(int32_t));
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* p = ram + o + 4 * i;
      dst[i] = static_cast<int32_t>((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]);
    }
  }
  void write32s_array(uint32_t addr, const int32_t* src, uint32_t count) const {
    uint32_t o;
    if (!ram_offset(addr, count * 4, o)) {
      return;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t v = static_cast<uint32_t>(src[i]);
      uint8_t* p = ram + o + 4 * i;
      p[0] = static_cast<uint8_t>(v >> 24);
      p[1] = static_cast<uint8_t>(v >> 16);
      p[2] = static_cast<uint8_t>(v >> 8);
      p[3] = static_cast<uint8_t>(v);
    }
  }
  // Host pointer to a RAM range (nullptr if outside RAM). Used to hash the microcode.
  const uint8_t* ram_ptr(uint32_t addr, uint32_t len) const {
    uint32_t o;
    return ram_offset(addr, len, o) ? ram + o : nullptr;
  }

  uint8_t aram_read8(uint32_t addr) const {
    if (aram == nullptr || aram_size == 0) {
      return 0;
    }
    return aram[addr & (aram_size - 1)];
  }
  void aram_write8(uint32_t addr, uint8_t v) const {
    if (aram == nullptr || aram_size == 0) {
      return;
    }
    aram[addr & (aram_size - 1)] = v;
  }
};

} // namespace gcrt::audio
