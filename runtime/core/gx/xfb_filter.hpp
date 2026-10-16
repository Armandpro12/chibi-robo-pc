// Display copy filter (EFB -> XFB): vertical filter ("deflicker", 7 coefficients from the
// BP registers 0x53/0x54) and gamma correction (BP 0x52 bits 7-8), applied to the presented frame.
//
// Generic GameCube. Aurora presents its EFB as is (empty GXCopyDisp); the console, however, filters
// every copy to the XFB. Behaviour taken from Dolphin (VideoCommon/TextureConverterShaderGen.cpp
// and TextureCacheBase.cpp GetRAMCopyFilterCoefficients, commit e6f3ae17): coefficients 0+1 for the
// line above, 2+3+4 for the current line, 5+6 for the line below, sum / 64, clamped
// to 255, alpha unchanged, then gamma (1.0 / 1.7 / 2.2) as a power of 1/gamma.
//
// Two modes: Full (complete filter, like the console) and Unblended (like Dolphin by default,
// setting "Disable Copy Filter" = true in Core/Config/GraphicsSettings.cpp: the coefficients
// are all moved onto the current line, TextureCacheBase::GetVRAMCopyFilterCoefficients; the
// brightness intended by the game is kept, without blending the neighbouring lines).
#pragma once

#include <cstdint>

namespace gcrt::gx {

enum class CopyFilterMode { Full, Unblended };

struct XfbFilterParams {
  uint8_t coefficients[7]{};
  uint8_t gamma = 0;           // 0 = 1.0; 1 = 1.7; 2 = 2.2
  uint32_t efbHeight = 0;      // height of the display copy's source (console lines), at the top of the EFB
  bool clampBottom = true;     // BP 0x52 bit 1: the line below the source's last one is that line itself
};

// Decodes the BP registers 0x53 and 0x54 (6 bits per coefficient).
XfbFilterParams xfb_filter_params(uint32_t bp53, uint32_t bp54, uint8_t gamma, uint32_t efbHeight,
                                  bool clampBottom = true);

// Applies the mode: Unblended moves coefficients 0, 1, 5 and 6 onto the current line.
XfbFilterParams with_mode(XfbFilterParams params, CopyFilterMode mode);

// To be called during an open Aurora frame, at the time of the display copy: replaces the displayed region of
// the EFB with its filtered version (this is what will be presented and captured). Without clampBottom, the last
// line reads the EFB line below the source, like Dolphin (TextureCacheBase.cpp, clamp_bottom). Returns false if
// the filter is neutral (nothing to do) or impossible (multisampling, no active pass).
bool apply_xfb_filter(const XfbFilterParams& params);

} // namespace gcrt::gx
