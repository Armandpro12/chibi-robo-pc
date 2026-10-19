// SHA-1 (FIPS 180-4), self-contained, used only to recognise disc images (not for security).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace gcx {

class Sha1 {
 public:
  Sha1() { reset(); }

  void reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    length_ = 0;
    used_ = 0;
  }

  void update(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    length_ += static_cast<uint64_t>(size);
    if (used_) {
      size_t take = 64 - used_ < size ? 64 - used_ : size;
      std::memcpy(block_ + used_, p, take);
      used_ += take, p += take, size -= take;
      if (used_ < 64)
        return;
      compress(block_);
      used_ = 0;
    }
    for (; size >= 64; p += 64, size -= 64)
      compress(p);
    if (size)
      std::memcpy(block_, p, size), used_ = size;
  }

  // Lowercase hexadecimal digest; the object must be reset() before reuse.
  std::string hex() {
    const uint64_t bits = length_ * 8u;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (used_ != 56)
      update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; i++)
      tail[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(tail, 8);
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (uint32_t w : h_)
      for (int i = 7; i >= 0; i--)
        out += digits[(w >> (4 * i)) & 0xF];
    return out;
  }

 private:
  static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

  void compress(const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++)
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20)
        f = (b & c) | (~b & d), k = 0x5A827999u;
      else if (i < 40)
        f = b ^ c ^ d, k = 0x6ED9EBA1u;
      else if (i < 60)
        f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
      else
        f = b ^ c ^ d, k = 0xCA62C1D6u;
      uint32_t t = rol(a, 5) + f + e + k + w[i];
      e = d, d = c, c = rol(b, 30), b = a, a = t;
    }
    h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e;
  }

  uint32_t h_[5];
  uint64_t length_;
  uint8_t block_[64];
  size_t used_;
};

}  // namespace gcx
