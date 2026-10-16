// Aurora host (see aurora_host.hpp).
#include "aurora_host.hpp"
#include "input_host.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

extern "C" {
#include "trace.h"
#include "gcrt_game_config.h"
}

namespace gcrt::platform {
namespace {
std::string g_title;
std::string g_dataDir;
bool g_initialized = false;
uint32_t g_efbWidth = 0;
uint32_t g_efbHeight = 0;
GXRenderModeObj g_renderMode{};

void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
  if (level >= LOG_WARNING) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "Aurora %s (%s): %s", level == LOG_WARNING ? "warning" : "error",
                      module, message);
  } else if (level == LOG_INFO) {
    GCRT_TRACE(GCRT_TRACE_GX, "Aurora (%s): %s", module, message);
  }
  if (level == LOG_FATAL) {
    std::fflush(gcrt_trace_stream());
    std::fflush(stdout);
    std::abort();
  }
}

// Keyboard and gamepad mappings: controls.ini file in the user folder
// (input_config.cpp, input_host.cpp). The keyboard table that used to be here is its default value.
} // namespace

const char* host_keyboard_help() {
  return "input: keyboard and SDL gamepads, mappings read from controls.ini (user folder; "
         "default, QWERTY positions: stick = arrows, C-stick = I/J/K/L, A = X, B = Z, X = C, Y = S, Z = D, "
         "START = Enter, L = Q, R = W, analog triggers = A and E, D-pad = T/F/G/H); path and effective table "
         "right after the window opens";
}

bool host_init(const HostConfig& config) {
  g_title = config.title;
  g_dataDir = config.dataDir;
  AuroraConfig ac{};
  ac.appName = g_title.c_str();
  ac.userPath = g_dataDir.c_str();
  ac.cachePath = g_dataDir.c_str();
  ac.logCallback = log_callback;
  ac.logLevel = LOG_INFO;
  ac.windowWidth = config.width;
  ac.windowHeight = config.height;
  ac.vsync = false; // pacing comes from the emulated VI (real time), not from vertical sync
  ac.mem1Size = 0;  // guest memory belongs to the runtime
  ac.mem2Size = 0;
  if (config.windowMode != 0) {
    // Automatic launches: the window does not steal the foreground (SDL 3.2+ hints, to be set
    // before Aurora shows the window in aurora_initialize).
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
  }
  char* argv0 = const_cast<char*>(g_title.c_str());
  const AuroraInfo info = aurora_initialize(1, &argv0, &ac);
  if (config.windowMode == 2 && info.window != nullptr) {
    SDL_MinimizeWindow(info.window);
  }
  static const char* const kWindowModes[] = {"normal", "no activation", "minimized"};
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "Aurora window opened: backend %d, %ux%u, %s", static_cast<int>(info.backend),
                    info.windowSize.fb_width, info.windowSize.fb_height,
                    kWindowModes[config.windowMode >= 0 && config.windowMode <= 2 ? config.windowMode : 0]);
  // Rendering at the EFB resolution (exact logical height, width stretched to the window's 4:3, like
  // a console's picture on a TV set): AURORA_VIEWPORT_STRETCH + scale 1.
  float renderScale = config.renderScale;
  if (config.windowMode == 0) { // direct launch only: automated runs keep their settings
    // Setup menu (gcrt_game_config.h): window size, fullscreen and render scale, saved in the user folder.
    gcrt_game_config_load();
    if (!gcrt_game_config_run_menu(g_title.c_str())) {
      aurora_shutdown(); // window closed from the menu: abort the boot
      return false;
    }
    const GcrtGameConfig* gc = gcrt_game_config();
    renderScale = gc->renderScale;
    if (info.window != nullptr) {
      SDL_SetWindowFullscreen(info.window, gc->fullscreen);
      if (!gc->fullscreen) {
        SDL_SetWindowSize(info.window, gc->windowWidth, gc->windowHeight);
      }
    }
  }
  if (renderScale > 0.0f) {
    AuroraSetViewportPolicy(config.viewportFit ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
    VISetFrameBufferScale(renderScale);
  }
  // Aurora's PAD: opening of SDL gamepads, port assignment, rumble. The mappings
  // (keyboard, buttons, axes, dead zones) come from controls.ini (input_host.cpp), not from Aurora.
  PADInit();
  input_init();
  g_initialized = true;
  return true;
}

void host_shutdown() {
  if (g_initialized) {
    aurora_shutdown();
    g_initialized = false;
  }
}

bool host_begin_frame() { return aurora_begin_frame(); }

void host_end_frame() { aurora_end_frame(); }

bool host_poll_events() {
  const AuroraEvent* ev = aurora_update();
  bool keepRunning = true;
  while (ev != nullptr && ev->type != AURORA_NONE) {
    if (ev->type == AURORA_EXIT) {
      keepRunning = false;
    }
    ++ev;
  }
  return keepRunning;
}

double host_wait_pipelines(double timeoutSeconds) {
  // Aurora's public statistic (include/aurora/gfx.h): pipelines waiting for compilation,
  // incremented when queued and decremented by the compilation thread (pipeline_cache.cpp).
  auto& queued = const_cast<AuroraStats*>(aurora_get_stats())->queuedPipelines;
  const auto start = std::chrono::steady_clock::now();
  double waited = 0.0;
  while (std::atomic_ref<uint32_t>(queued).load(std::memory_order_acquire) != 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (waited >= timeoutSeconds) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "pipelines: wait abandoned after %.1f s (%u queued)", waited,
                        std::atomic_ref<uint32_t>(queued).load());
      break;
    }
  }
  return waited;
}

uint32_t host_pending_pipelines() {
  auto& queued = const_cast<AuroraStats*>(aurora_get_stats())->queuedPipelines;
  return std::atomic_ref<uint32_t>(queued).load(std::memory_order_acquire);
}

void host_configure_efb(uint32_t width, uint32_t height) {
  if (width == g_efbWidth && height == g_efbHeight) {
    return;
  }
  g_efbWidth = width;
  g_efbHeight = height;
  g_renderMode = GXNtsc480IntDf;
  g_renderMode.fbWidth = static_cast<u16>(width);
  g_renderMode.efbHeight = static_cast<u16>(height);
  g_renderMode.xfbHeight = static_cast<u16>(height);
  VIConfigure(&g_renderMode);
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "game EFB: %ux%u", width, height);
}

void host_read_pads(PadState out[4], bool connected[4]) {
  input::PadOutput pads[4];
  input_read_pads(pads, connected);
  for (int i = 0; i < 4; ++i) {
    PadState& s = out[i];
    s.buttons = pads[i].buttons;
    s.stickX = pads[i].stickX;
    s.stickY = pads[i].stickY;
    s.substickX = pads[i].substickX;
    s.substickY = pads[i].substickY;
    s.triggerL = pads[i].triggerL;
    s.triggerR = pads[i].triggerR;
    s.analogA = 0; // analog mode 3: no analog A/B buttons (like Aurora)
    s.analogB = 0;
  }
}

void host_rumble(int port, int command) {
  if (port >= 0 && port < PAD_CHANMAX) {
    PADControlMotor(static_cast<u32>(port), static_cast<u32>(command));
  }
}

} // namespace gcrt::platform
