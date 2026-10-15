// Aurora host: window, frames, gamepads and keyboard (SDL3 through Aurora).
//
// Only this module includes Aurora's SDK-style headers (dolphin/*.h), so as not to mix
// their types (u8, BOOL...) with DolRecomp's in the same compilation unit.
#pragma once

#include "pad_state.hpp"

#include <cstdint>
#include <string>

namespace gcrt::platform {

struct HostConfig {
  std::string title;
  // Render resolution: multiple of the logical EFB size (1 = console resolution,
  // like Dolphin by default), width adjusted to the window's aspect ratio; 0 = window size.
  float renderScale = 1.0f;
  // Window: 0 normal (activated when opened); 1 "background" (shown without taking the
  // foreground or the keyboard); 2 minimized to the taskbar. For automatic launches.
  int windowMode = 0;
  // Diagnostics: true = AURORA_VIEWPORT_FIT (EFB rendered at its exact logical size, frame presented
  // at the EFB's aspect ratio and not at a TV set's 4:3).
  bool viewportFit = false;
  std::string dataDir;   // Aurora's userPath / cachePath (pipeline caches; the gamepad mappings
                         // are in the user folder, gcrt_user_dir.h)
  uint32_t width = 1920;  // initial window size
  uint32_t height = 1080; // initial window size
};

bool host_init(const HostConfig& config);
void host_shutdown();
// Opens an Aurora frame (false if the window cannot be drawn: minimized, etc.).
bool host_begin_frame();
// Ends and presents the open frame.
void host_end_frame();
// Processes the window events. Returns false if the user closes the window.
bool host_poll_events();
// Waits until Aurora's pipeline compilation queue is empty (otherwise the draws whose
// pipeline is not ready are skipped by Aurora). Returns the time waited in seconds.
double host_wait_pipelines(double timeoutSeconds);
// Logical size of the game's EFB (rectangle of the copy to the XFB): Aurora adjusts its rendering to it.
void host_configure_efb(uint32_t width, uint32_t height);

// Reads the 4 controllers: keyboard on its port (1 by default), SDL gamepads assigned by Aurora, translated
// by the mappings of controls.ini (input_host.hpp).
void host_read_pads(PadState out[4], bool connected[4]);
void host_rumble(int port, int command);

// Reminder of the default mappings and of the file location, for the trace (UTF-8 text).
const char* host_keyboard_help();

} // namespace gcrt::platform
