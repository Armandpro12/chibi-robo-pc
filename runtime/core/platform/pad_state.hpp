// Controller state read by the platform (keyboard, SDL gamepads, scripted inputs), in the SDK format
// (PADStatus): PAD_BUTTON_* buttons (A = 0x0100, B = 0x0200, X = 0x0400, Y = 0x0800, START = 0x1000,
// Z = 0x0010, R = 0x0020, L = 0x0040, D-pad: left 0x1, right 0x2, down 0x4, up 0x8), signed sticks
// centred on 0. Consumed by the native controllers (hle/native_pad.c, through gcrt_native_set_pad_source).
// Generic GameCube.
#pragma once

#include <cstdint>

namespace gcrt::platform {

struct PadState {
  uint16_t buttons = 0;
  int8_t stickX = 0;
  int8_t stickY = 0;
  int8_t substickX = 0;
  int8_t substickY = 0;
  uint8_t triggerL = 0;
  uint8_t triggerR = 0;
  uint8_t analogA = 0;
  uint8_t analogB = 0;
};

} // namespace gcrt::platform
