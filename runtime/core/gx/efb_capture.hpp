// Capture of the EFB rendered by Aurora into a PNG file (generic).
//
// Used for automatic screenshots (comparison with Dolphin, milestone 5) and for
// prototypes. Uses only Aurora's public API (include/aurora/gfx.hpp):
// resolve_pass() freezes the current EFB into a texture, an encoder task copies it
// into a readable buffer, then the buffer is read after the frame is submitted.
#pragma once

#include <cstdint>

namespace gcrt::gx {

// Requests a capture of the EFB as it is at this point of the current frame.
// To be called between aurora_begin_frame() and aurora_end_frame(), after the wanted draws.
// Returns false if no render pass is active.
bool request_efb_capture(const char* pngPath);

// To be called after aurora_end_frame(): writes the captures whose GPU readback has
// finished. Returns the number of files written.
int poll_efb_captures();

// Number of captures requested and not yet written.
int efb_captures_pending();

// Last capture read (8-bit RGBA), for automatic tests without reading the PNG back.
struct CapturedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  const uint8_t* rgba = nullptr; // valid until the next capture
};
CapturedImage last_efb_capture();

} // namespace gcrt::gx
