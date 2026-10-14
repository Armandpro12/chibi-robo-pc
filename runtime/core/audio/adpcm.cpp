// GameCube DSP ADPCM: decoding by address range (see adpcm.hpp).
#include "adpcm.hpp"

namespace gcrt::audio::adpcm {

size_t decode_range(const uint8_t* data, size_t data_size, uint32_t start_addr, uint32_t end_addr,
                    const int16_t coefs[16], Context& ctx, int16_t* out, size_t out_cap) {
  size_t count = 0;
  uint32_t address = start_addr;
  while (address <= end_addr && count < out_cap) {
    if ((address & 0xF) == 0) {
      // Start of frame: new predictor/scale byte, then its two nibbles are skipped.
      if (address / 2 >= data_size) {
        break;
      }
      ctx.pred_scale = data[address / 2];
      address += 2;
      continue;
    }
    if (address / 2 >= data_size) {
      break;
    }
    const uint8_t byte = data[address / 2];
    const uint32_t nibble = (address & 1) ? (byte & 0xF) : (byte >> 4);
    out[count++] = decode_nibble(nibble, ctx.pred_scale, coefs, ctx.yn1, ctx.yn2);
    ++address;
  }
  return count;
}

size_t address_to_sample(size_t address) {
  if ((address & (kNibblesPerFrame - 1)) < 2) {
    address = (address & ~size_t(kNibblesPerFrame - 1)) + 2; // a header address counts as the 1st sample
  }
  const size_t frames = address / kNibblesPerFrame + 1;
  return address - frames * 2;
}

size_t sample_to_address(size_t sample) {
  const size_t frames = sample / kSamplesPerFrame + 1;
  return sample + frames * 2;
}

} // namespace gcrt::audio::adpcm
