// Aurora host (see aurora_host.hpp).
#include "aurora_host.hpp"
#include "input_host.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/gx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

// Aurora's internal API (external/aurora/lib), only for the crash-handler test GCRT_CRASH_TEST=render (fault on Aurora's
// worker thread). Behind the CMake option GCRT_RENDER_CRASH_TEST (platform/sources.cmake): if an Aurora update changes
// or removes this header, turning the option off keeps the build working; only that test is lost.
#if defined(GCRT_RENDER_CRASH_TEST) && __has_include("gfx/render_worker.hpp")
#include "gfx/render_worker.hpp"
#define GCRT_HAVE_RENDER_CRASH_TEST 1
#else
#define GCRT_HAVE_RENDER_CRASH_TEST 0
#endif

extern "C" {
#include "trace.h"
#include "gcrt_crash.h"
#include "gcrt_diag.h"
#include "gcrt_focus.h"
#include "gcrt_game_config.h"
}

namespace gcrt::platform {
namespace {
std::string g_title;
std::string g_dataDir;
bool g_initialized = false;
uint32_t g_efbWidth = 0;
uint32_t g_efbHeight = 0;
uint32_t g_efbLines = 0;
GXRenderModeObj g_renderMode{};

void log_callback(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
  // Graphics adapter and driver chosen by Aurora (lib/webgpu/gpu.cpp logs them at the info level): kept for crash
  // reports.
  if (level == LOG_INFO && std::strncmp(message, "Graphics adapter information", 28) == 0) {
    gcrt_crash_handler_set_gpu_info(message + 28);
  }
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

// "No focus" test mode (gcrt_focus.h). SDL shows the window without activation (hint
// SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN = "0": SetWindowPos with SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER, SDL 3.4
// src/video/windows/SDL_windowswindow.c, WIN_ShowWindow), but a new window is at the top of the Z order: it is drawn
// over the foreground window of the person using the computer, and a click of theirs there activates it (issue 30).
// Hence, as soon as SDL reports it shown (SDL_EVENT_WINDOW_SHOWN, sent from WM_SHOWWINDOW inside that SetWindowPos,
// SDL 3.4 src/video/windows/SDL_windowsevents.c; seen by an event watch, called when the event is queued), or else
// right after aurora_initialize (about 0.35 s later: GPU and ImGui set-up):
//   - at the bottom of the Z order (SetWindowPos HWND_BOTTOM with SWP_NOACTIVATE, Microsoft Learn "SetWindowPos");
//   - not focusable (SDL_SetWindowFocusable: WS_EX_NOACTIVATE, Microsoft Learn "Extended Window Styles": does not
//     become the foreground window when clicked, nor when the foreground window is minimized or closed; no taskbar
//     button, hence not offered by Alt+Tab either), in every window mode. With GCRT_WINDOW=minimized it used to be
//     kept focusable, for its taskbar button: then a person's Alt+Tab or taskbar click, or the closing of the
//     foreground window, could activate it (issue 39). A minimized window without a taskbar button is drawn as a
//     small bar above the taskbar (seen at the bottom left of the screen): minimize_without_activation gives it a
//     minimized position off the screen instead.
// Trace (GCRT_TRACE_PLATFORM): "no focus: ..." with the moment it was done.
bool g_keptBehind = false;

void keep_window_behind(SDL_Window* window, const char* when) {
  g_keptBehind = true;
  const bool unfocusable = SDL_SetWindowFocusable(window, false);
  bool bottom = false;
#ifdef _WIN32
  const auto hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  bottom = hwnd != nullptr &&
           SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
#endif
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "no focus: window %s, %s (%s)",
                    unfocusable ? "not focusable" : "focusable (failed)",
                    bottom ? "moved to the bottom of the Z order" : "Z order unchanged (failed)", when);
}

// "No focus" mode with GCRT_WINDOW=minimized: SDL_MinimizeWindow is ShowWindow(SW_MINIMIZE) (SDL 3.4
// src/video/windows/SDL_windowswindow.c, WIN_MinimizeWindow), which "activates the next top-level window in the Z
// order" (Microsoft Learn, "ShowWindow"). Instead: SetWindowPlacement with SW_SHOWMINNOACTIVE ("displays the window
// as a minimized window ... the window is not activated") and WPF_SETMINPOSITION (Microsoft Learn, "WINDOWPLACEMENT")
// with the minimized position at (-32000, -32000), off the screen, where the minimized window with a taskbar button
// was measured before (tests of issue 30). SDL sees the minimization from the window messages, as before. Returns
// false if not done (the caller then uses SDL_MinimizeWindow).
bool minimize_without_activation(SDL_Window* window) {
#ifdef _WIN32
  const auto hwnd = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  WINDOWPLACEMENT wp{};
  wp.length = sizeof(wp);
  if (hwnd == nullptr || !GetWindowPlacement(hwnd, &wp)) {
    return false;
  }
  wp.flags = WPF_SETMINPOSITION;
  wp.showCmd = SW_SHOWMINNOACTIVE;
  wp.ptMinPosition.x = -32000;
  wp.ptMinPosition.y = -32000;
  if (!SetWindowPlacement(hwnd, &wp)) {
    return false;
  }
  RECT r{};
  GetWindowRect(hwnd, &r);
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "no focus: window minimized without activation, at (%ld, %ld)%s",
                    static_cast<long>(r.left), static_cast<long>(r.top), IsIconic(hwnd) ? "" : " (not minimized!)");
  return true;
#else
  (void)window;
  return false;
#endif
}

// Event watch (SDL_AddEventWatch), only during aurora_initialize: runs on the thread that shows the window.
bool SDLCALL keep_behind_on_shown(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_WINDOW_SHOWN && !g_keptBehind) {
    if (SDL_Window* window = SDL_GetWindowFromEvent(event)) {
      keep_window_behind(window, "when shown");
    }
  }
  return true;
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

// Host seconds since `start` (start-up times in the trace: its timestamps are emulated time, 0 before the game).
static double seconds_since(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

bool host_init(const HostConfig& config) {
  // Start-up progress, watched by the hang watchdog until the game loop beats (gcrt_diag.h).
  gcrt_diag_startup_beat();
  const auto start = std::chrono::steady_clock::now();
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
  const bool noFocus = gcrt_no_focus();
  if (noFocus) {
    SDL_AddEventWatch(keep_behind_on_shown, nullptr);
  }
  // Graphics backend (gcrt_game_config.h): --backend, else GCRT_BACKEND, else the settings file on a player's launch
  // (read now, before the window opens: the setup menu itself runs on Aurora's window), else auto. An unknown name or
  // a backend this build cannot try gives auto, said in the trace and on stderr. If the backend asked for fails to
  // start, Aurora itself goes on with its preferred order (lib/aurora.cpp, aurora::initialize): reported below.
  if (config.setupMenu) {
    gcrt_game_config_load();
  }
  std::string requestedName = "auto";
  const char* requestSource = "default";
  if (const char* cli = gcrt_backend_request()) {
    requestedName = cli;
    requestSource = "--backend";
  } else if (const char* env = std::getenv("GCRT_BACKEND"); env != nullptr && env[0] != '\0') {
    requestedName = env;
    requestSource = "GCRT_BACKEND";
  } else if (config.setupMenu) {
    requestedName = gcrt_backend_name(gcrt_game_config()->backend);
    requestSource = "game_config.ini";
  }
  int requested = gcrt_backend_from_name(requestedName.c_str());
  // What the trace line below says was asked for: the name as given when it was refused.
  std::string requestDesc;
  if (requested < 0 || !gcrt_backend_available(requested)) {
    const char* why = requested < 0 ? "unknown name (auto|d3d12|vulkan)" : "not available in this build";
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "graphics backend: %s \"%s\" ignored, %s: auto used", requestSource,
                      requestedName.c_str(), why);
    std::fprintf(stderr, "gcrt: graphics backend \"%s\" (%s) ignored, %s; using auto\n", requestedName.c_str(),
                 requestSource, why);
    requested = BACKEND_AUTO;
    requestDesc = "'" + requestedName + "' (" + requestSource + "), using auto";
  } else {
    requestDesc = std::string(gcrt_backend_name(requested)) + " (" + requestSource + ")";
  }
  ac.desiredBackend = static_cast<AuroraBackend>(requested);
  char* argv0 = const_cast<char*>(g_title.c_str());
  const AuroraInfo info = aurora_initialize(1, &argv0, &ac);
  gcrt_backend_set_active(info.backend);
  {
    std::string offered;
    size_t count = 0;
    const AuroraBackend* list = aurora_get_available_backends(&count);
    for (size_t i = 0; list != nullptr && i < count; ++i) {
      if (gcrt_backend_available(list[i])) {
        offered += offered.empty() ? "" : ", ";
        offered += gcrt_backend_name(list[i]);
      }
    }
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "graphics backend: %s in use (Aurora %d), requested %s; this build: %s",
                      gcrt_backend_label(info.backend), static_cast<int>(info.backend), requestDesc.c_str(),
                      offered.c_str());
    if (requested != BACKEND_AUTO && info.backend != requested) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "graphics backend: %s could not be started, Aurora fell back to %s",
                        gcrt_backend_label(requested), gcrt_backend_label(info.backend));
      std::fprintf(stderr, "gcrt: graphics backend %s could not be started; using %s instead\n",
                   gcrt_backend_label(requested), gcrt_backend_label(info.backend));
    } else if (requested == BACKEND_AUTO && (info.backend == BACKEND_D3D11 || info.backend == BACKEND_NULL)) {
      // Last resort of Aurora's own order: neither Direct3D 12 nor Vulkan could be started. Not a player's choice, and
      // the picture may be wrong (measured on one of the two supported versions with Direct3D 11).
      gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                        "graphics backend: neither Direct3D 12 nor Vulkan could be started, Aurora fell back to %s",
                        gcrt_backend_label(info.backend));
      std::fprintf(stderr, "gcrt: neither Direct3D 12 nor Vulkan could be started; using %s, the picture may be wrong\n",
                   gcrt_backend_label(info.backend));
    }
  }
  gcrt_diag_startup_beat();
  const double windowSeconds = seconds_since(start);
  if (noFocus) {
    SDL_RemoveEventWatch(keep_behind_on_shown, nullptr);
    if (!g_keptBehind && info.window != nullptr) {
      keep_window_behind(info.window, "after aurora_initialize");
    }
  }
  if (config.windowMode == 2 && info.window != nullptr) {
    if (!(noFocus && minimize_without_activation(info.window))) {
      SDL_MinimizeWindow(info.window);
    }
  }
  static const char* const kWindowModes[] = {"normal", "no activation", "minimized"};
  gcrt_diag_event(GCRT_EV_WINDOW, GCRT_EVW_OPENED,
                  (static_cast<uint64_t>(info.windowSize.fb_width) << 32) | info.windowSize.fb_height);
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "Aurora window opened: backend %d (%s), %ux%u, %s, in %.2f s (host)",
                    static_cast<int>(info.backend), gcrt_backend_name(info.backend), info.windowSize.fb_width,
                    info.windowSize.fb_height,
                    kWindowModes[config.windowMode >= 0 && config.windowMode <= 2 ? config.windowMode : 0],
                    windowSeconds);
  // Rendering at the EFB resolution (exact logical height, width stretched to the window's 4:3, like
  // a console's picture on a TV set): AURORA_VIEWPORT_STRETCH + scale 1.
  float renderScale = config.renderScale;
  if (config.setupMenu) { // a player's direct launch only: automated runs keep their settings
    // Setup menu (gcrt_game_config.h): window size, fullscreen, render scale, console and graphics backend, saved in
    // the user folder (file already loaded above, before the window opened).
    if (!gcrt_game_config_run_menu(g_title.c_str())) {
      aurora_shutdown(); // window closed from the menu: abort the boot
      return false;
    }
    const GcrtGameConfig* gc = gcrt_game_config();
    renderScale = gc->renderScale;
    if (info.window != nullptr) {
      // "No focus" test mode (gcrt_focus.h): a fullscreen window would cover the screen of the person using the
      // computer, so the window keeps the menu's window size instead.
      const bool fullscreen = gc->fullscreen && !gcrt_no_focus();
      if (fullscreen != gc->fullscreen) {
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "fullscreen not applied (GCRT_NO_FOCUS)");
      }
      SDL_SetWindowFullscreen(info.window, fullscreen);
      if (!fullscreen) {
        SDL_SetWindowSize(info.window, gc->windowWidth, gc->windowHeight);
      }
    }
  }
  if (renderScale > 0.0f) {
    AuroraSetViewportPolicy(config.viewportFit ? AURORA_VIEWPORT_FIT : AURORA_VIEWPORT_STRETCH);
    VISetFrameBufferScale(renderScale);
  }
  const double menuEnd = seconds_since(start);
  gcrt_diag_startup_beat();
  // Aurora's PAD: opening of SDL gamepads, port assignment, rumble. The mappings
  // (keyboard, buttons, axes, dead zones) come from controls.ini (input_host.cpp), not from Aurora.
  PADInit();
  input_init();
  gcrt_diag_startup_beat();
  // Host time of each start-up step: a slow start (busy computer) is told apart from a stop (hang report).
  gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                    "start-up: window and input ready in %.2f s (host): window %.2f s, setup menu %s%.2f s, input "
                    "%.2f s",
                    seconds_since(start), windowSeconds, config.setupMenu ? "" : "none, ", menuEnd - windowSeconds,
                    seconds_since(start) - menuEnd);
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
      gcrt_diag_event(GCRT_EV_WINDOW, GCRT_EVW_CLOSE_REQUESTED, 0);
      keepRunning = false;
    } else if (ev->type == AURORA_WINDOW_RESIZED || ev->type == AURORA_DISPLAY_SCALE_CHANGED) {
      const AuroraWindowSize& s = ev->windowSize;
      gcrt_diag_event(GCRT_EV_WINDOW, ev->type == AURORA_WINDOW_RESIZED ? GCRT_EVW_RESIZED : GCRT_EVW_SCALE_CHANGED,
                      (static_cast<uint64_t>(s.width) << 32) | s.height);
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "window %s: %ux%u, frame buffer %ux%u, surface %ux%u, scale %.2f",
                        ev->type == AURORA_WINDOW_RESIZED ? "resized" : "display scale changed", s.width, s.height,
                        s.fb_width, s.fb_height, s.native_fb_width, s.native_fb_height, static_cast<double>(s.scale));
    }
    ++ev;
  }
  return keepRunning;
}

bool host_pump_events() {
  // Window messages only (the window keeps answering Windows, keyboard state up to date); the SDL events stay queued
  // for the next host_poll_events. A close request is seen without being consumed.
  SDL_PumpEvents();
  return !SDL_HasEvent(SDL_EVENT_QUIT) && !SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED);
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

void host_crash_in_render_thread() {
#if GCRT_HAVE_RENDER_CRASH_TEST
  aurora::gfx::render_worker::enqueue_work([] {
    volatile int* bad = nullptr;
    *bad = 1;
  });
#else
  gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                    "GCRT_CRASH_TEST=render: not available in this build (CMake option GCRT_RENDER_CRASH_TEST off)");
#endif
}

uint32_t host_pending_pipelines() {
  auto& queued = const_cast<AuroraStats*>(aurora_get_stats())->queuedPipelines;
  return std::atomic_ref<uint32_t>(queued).load(std::memory_order_acquire);
}

void host_configure_efb(uint32_t width, uint32_t height, uint32_t efbLines) {
  const uint32_t lines = efbLines > height ? efbLines : height;
  if (width == g_efbWidth && height == g_efbHeight && lines == g_efbLines) {
    return;
  }
  g_efbWidth = width;
  g_efbHeight = height;
  g_efbLines = lines;
  g_renderMode = GXNtsc480IntDf;
  g_renderMode.fbWidth = static_cast<u16>(width);
  g_renderMode.efbHeight = static_cast<u16>(height);
  g_renderMode.xfbHeight = static_cast<u16>(height);
  // Taller EFB first: Aurora resizes its frame buffer once for both changes (at the next window event poll).
  VISetEfbSize(lines > height ? width : 0, lines > height ? lines : 0);
  VIConfigure(&g_renderMode);
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "game EFB: %ux%u displayed, %ux%u rendered", width, height, width, lines);
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
