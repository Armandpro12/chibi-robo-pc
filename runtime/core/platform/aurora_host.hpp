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
  // Setup menu before the game (gcrt_game_config.h): only on a player's direct launch (GCRT_SETUP_MENU,
  // platform/gcrt_platform.h); the settings file is applied only then too.
  bool setupMenu = false;
  uint32_t width = 640;  // initial window size
  uint32_t height = 480;
};

// false: the window was closed from the setup menu (HostConfig::setupMenu); the boot is aborted.
bool host_init(const HostConfig& config);
void host_shutdown();
// Opens an Aurora frame (false if the window cannot be drawn: minimized, etc.).
bool host_begin_frame();
// Ends and presents the open frame.
void host_end_frame();
// Processes the window events. Returns false if the user closes the window.
// Only between two frames (no Aurora frame open), like Aurora's own loop (aurora_update before aurora_begin_frame):
// a window resize recreates the frame buffer and clears Aurora's bind group cache, which the draws already recorded
// in an open frame still refer to (game hang on resize, issue 21).
bool host_poll_events();
// While a frame is open: lets the system deliver the window messages (window responsive, keyboard state) but leaves
// the events queued for the next host_poll_events. Returns false if a close request is waiting.
bool host_pump_events();
// Waits until Aurora's pipeline compilation queue is empty (otherwise the draws whose
// pipeline is not ready are skipped by Aurora). Returns the time waited in seconds. If given, queuedAtStart receives
// the queue length when the wait began and compiled the number of pipeline objects Aurora created during the wait
// (diagnostic: compilation throughput).
double host_wait_pipelines(double timeoutSeconds, uint32_t* queuedAtStart = nullptr, uint32_t* compiled = nullptr);
// Pipelines requested from Aurora and not compiled yet (same statistic, without waiting).
uint32_t host_pending_pipelines();
// Test of the crash handler (GCRT_CRASH_TEST=render): queues an access violation on Aurora's render worker thread.
void host_crash_in_render_thread();
// Logical size of the game's EFB: width x height is the rectangle of the copy to the XFB (the displayed region,
// Aurora adjusts its rendering to it); efbLines, if larger than height, is the EFB's real height: the lines
// below the displayed region are rendered and can be copied to textures, but are not presented
// (VISetEfbSize, Aurora patch in tools/patches/aurora). 0 = the displayed height (former behaviour).
void host_configure_efb(uint32_t width, uint32_t height, uint32_t efbLines);

// Reads the 4 controllers: keyboard on its port (1 by default), SDL gamepads assigned by Aurora, translated
// by the mappings of controls.ini (input_host.hpp).
void host_read_pads(PadState out[4], bool connected[4]);
void host_rumble(int port, int command);

// Reminder of the default mappings and of the file location, for the trace (UTF-8 text).
const char* host_keyboard_help();

} // namespace gcrt::platform
