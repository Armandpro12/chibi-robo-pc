// Setup menu shown on a player's direct launch, and its settings file (see gcrt_game_config.h).
#include "gcrt_game_config.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "gcrt_console.h"
#include "gcrt_diag.h"
#include "gcrt_focus.h"
#include "gcrt_user_dir.h"
#include "trace.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {
const struct {
  const char* label;
  int w, h;
} kSizes[] = {
    {"960x720 (4:3)", 960, 720},
    {"1280x960 (4:3)", 1280, 960},
    {"1600x1200 (4:3)", 1600, 1200},
    {"1920x1080 (16:9)", 1920, 1080},
};
// Console checked by default: the console window that testers had before (notes/decisions.md, 2026-10-17).
GcrtGameConfig g_cfg = {1280, 960, false, 1.0f, false, true, BACKEND_AUTO};
bool g_savedConsole = true; // value of the file: what is written back unless the player toggles the box

// Graphics backends (gcrt_game_config.h). Only the "offered" ones can be chosen; the others are named for the trace
// (Aurora can fall back to them by itself): Metal, OpenGL and WebGPU are not built on Windows here, null draws
// nothing, and Direct3D 11 gives a different picture (see the header).
const struct {
  AuroraBackend value;
  const char* name;  // in game_config.ini, --backend, GCRT_BACKEND
  const char* label; // in the menu
  bool offered;
} kBackends[] = {
    {BACKEND_AUTO, "auto", "Auto", true},
    {BACKEND_D3D12, "d3d12", "Direct3D 12", true},
    {BACKEND_VULKAN, "vulkan", "Vulkan", true},
    {BACKEND_D3D11, "d3d11", "Direct3D 11", false},
    {BACKEND_METAL, "metal", "Metal", false},
    {BACKEND_OPENGL, "opengl", "OpenGL", false},
    {BACKEND_OPENGLES, "opengles", "OpenGL ES", false},
    {BACKEND_WEBGPU, "webgpu", "WebGPU", false},
    {BACKEND_NULL, "null", "null (no picture)", false},
};
// Test switch (OFF by default, never set for players): GCRT_TEST_ALL_BACKENDS=1 lets --backend and GCRT_BACKEND name
// the backends that are not offered (d3d11, null...), so that tests can compare them with the offered ones. The menu
// and game_config.ini keep the offered list only.
bool test_all_backends() {
  static const bool on = [] {
    const char* v = std::getenv("GCRT_TEST_ALL_BACKENDS");
    return v != nullptr && v[0] == '1';
  }();
  return on;
}
std::string g_backendRequest;  // --backend, for this launch
bool g_haveBackendRequest = false;
int g_activeBackend = BACKEND_AUTO;

// Value of the file: unknown or not built in -> auto, said in the trace and on stderr (a hand-edited file).
void sanitize_backend(const std::string& raw) {
  const int b = gcrt_backend_from_name(raw.c_str());
  if (b >= 0 && gcrt_backend_available(b)) {
    g_cfg.backend = b;
    return;
  }
  g_cfg.backend = BACKEND_AUTO;
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "graphics backend: game_config.ini has backend=%s, %s: auto used",
                    raw.c_str(), b < 0 ? "unknown name (auto|d3d12|vulkan)" : "not available in this build");
  std::fprintf(stderr, "gcrt: game_config.ini: graphics backend \"%s\" %s, using auto\n", raw.c_str(),
               b < 0 ? "is unknown (auto, d3d12, vulkan)" : "is not available in this build");
}

// game_config.ini in the user folder (UTF-8, ending with a separator: gcrt_user_dir.h). The path is built from UTF-8
// explicitly: a std::string path would be read in the ANSI code page on Windows (user names with accents).
fs::path config_path(const char* dir) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(dir), std::strlen(dir))) / "game_config.ini";
}

// Values outside what the menu offers fall back to the defaults (a garbled file, or a value typed into the slider
// with Ctrl+click).
void sanitize() {
  bool knownSize = false;
  for (const auto& s : kSizes) {
    knownSize |= s.w == g_cfg.windowWidth && s.h == g_cfg.windowHeight;
  }
  if (!knownSize) {
    g_cfg.windowWidth = 1280;
    g_cfg.windowHeight = 960;
  }
  if (!(g_cfg.renderScale >= 0.5f && g_cfg.renderScale <= 4.0f)) { // also rejects NaN
    g_cfg.renderScale = 1.0f;
  }
}

// Written to a temporary file, then renamed: an interrupted write never leaves a truncated file.
void save(const char* dir) {
  if (dir[0] == '\0') {
    return; // no user folder: nothing written
  }
  const fs::path file = config_path(dir);
  fs::path tmp = file;
  tmp += ".tmp";
  std::error_code ec;
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << "width=" << g_cfg.windowWidth << "\nheight=" << g_cfg.windowHeight
      << "\nfullscreen=" << (g_cfg.fullscreen ? 1 : 0) << "\nrenderScale=" << g_cfg.renderScale
      << "\nskipMenu=" << (g_cfg.skipMenu ? 1 : 0) << "\nconsole=" << (g_savedConsole ? 1 : 0)
      << "\nbackend=" << gcrt_backend_name(g_cfg.backend) << "\n";
    f.close();
    if (!f) {
      fs::remove(tmp, ec);
      return;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
  }
}

// Shift held right now. SDL's modifier state comes from the keyboard events it has processed, and none has been
// delivered yet when the menu is checked at startup, so on Windows the physical key state is read instead
// (GetAsyncKeyState: most significant bit set = key down).
bool shift_held() {
  if (gcrt_no_focus()) {
    return false; // the keyboard belongs to the person using the computer (gcrt_focus.h)
  }
#if defined(_WIN32)
  return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  SDL_PumpEvents();
  return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
}

// Diagnostic hook GCRT_SETUP_MENU_AUTO=start|close[:N] (platform/gcrt_platform.h): after N passes of the menu loop
// (default 10; a pass draws one menu frame, or waits 16 ms while the window is minimized), act as if "Start game" were
// pressed (current box values) or the window were closed. Tests need neither the keyboard nor the foreground.
enum class MenuAuto { None, Start, Close };
struct MenuAutoSetting {
  MenuAuto action = MenuAuto::None;
  int passes = 10;
};

MenuAutoSetting menu_auto() {
  MenuAutoSetting m;
  const char* v = std::getenv("GCRT_SETUP_MENU_AUTO");
  if (v == nullptr || v[0] == '\0') {
    return m;
  }
  const std::string s(v);
  const std::string verb = s.substr(0, s.find(':'));
  if (verb == "start") {
    m.action = MenuAuto::Start;
  } else if (verb == "close") {
    m.action = MenuAuto::Close;
  } else {
    gcrt_trace_printf(GCRT_TRACE_UNHANDLED, "GCRT_SETUP_MENU_AUTO=%s ignored (start|close[:N])", v);
    return m;
  }
  if (const auto colon = s.find(':'); colon != std::string::npos) {
    const int n = std::atoi(s.c_str() + colon + 1);
    m.passes = n > 0 ? n : 0;
  }
  return m;
}

// Backdrop behind the menu panel: before the game starts nothing else is rendered, so without it the window around the
// panel stays plain black (issue 25). A vertical gradient over the whole window, on ImGui's background draw list (drawn
// before every ImGui window). Only the menu frames have it; the window itself (size, position, focus) is not touched.
void draw_menu_backdrop() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const ImVec2 topLeft = vp->Pos;
  const ImVec2 bottomRight(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y); // not min/max: macros of windows.h
  const ImU32 top = IM_COL32(46, 62, 98, 255);
  const ImU32 bottom = IM_COL32(12, 14, 24, 255);
  ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(topLeft, bottomRight, top, top, bottom, bottom);
}
} // namespace

extern "C" {

const GcrtGameConfig* gcrt_game_config(void) { return &g_cfg; }

int gcrt_backend_from_name(const char* name) {
  if (name == nullptr) {
    return -1;
  }
  std::string lower(name);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const auto& b : kBackends) {
    if (b.offered && lower == b.name) {
      return b.value;
    }
  }
  return -1;
}

int gcrt_backend_from_test_name(const char* name) {
  const int offered = gcrt_backend_from_name(name);
  if (offered >= 0 || name == nullptr || !test_all_backends()) {
    return offered;
  }
  std::string lower(name);
  for (char& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  for (const auto& b : kBackends) {
    if (lower == b.name) {
      return b.value;
    }
  }
  return -1;
}

const char* gcrt_backend_name(int backend) {
  for (const auto& b : kBackends) {
    if (b.value == backend) {
      return b.name;
    }
  }
  return "?";
}

const char* gcrt_backend_label(int backend) {
  for (const auto& b : kBackends) {
    if (b.value == backend) {
      return b.label;
    }
  }
  return "?";
}

bool gcrt_backend_available(int backend) {
  if (backend == BACKEND_AUTO) {
    return true;
  }
  bool offered = false;
  for (const auto& b : kBackends) {
    offered |= (b.offered || test_all_backends()) && b.value == backend;
  }
  if (!offered) {
    return false; // null, Direct3D 11...: never offered
  }
  size_t count = 0;
  const AuroraBackend* list = aurora_get_available_backends(&count);
  for (size_t i = 0; list != nullptr && i < count; ++i) {
    if (list[i] == backend) {
      return true;
    }
  }
  return false;
}

void gcrt_backend_set_request(const char* name) {
  g_haveBackendRequest = name != nullptr;
  g_backendRequest = name != nullptr ? name : "";
}

const char* gcrt_backend_request(void) { return g_haveBackendRequest ? g_backendRequest.c_str() : nullptr; }

void gcrt_backend_set_active(int backend) { g_activeBackend = backend; }

int gcrt_backend_active(void) { return g_activeBackend; }

void gcrt_game_config_load(void) {
  const char* dir = gcrt_user_dir();
  std::ifstream f;
  if (dir[0] != '\0') { // no user folder: the defaults
    f.open(config_path(dir));
  }
  std::string line;
  while (std::getline(f, line)) {
    // A file edited by hand on Windows has CRLF line ends: "console=0\r" must still read as "0".
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos) {
      continue;
    }
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k == "width") {
      g_cfg.windowWidth = std::atoi(v.c_str());
    } else if (k == "height") {
      g_cfg.windowHeight = std::atoi(v.c_str());
    } else if (k == "fullscreen") {
      g_cfg.fullscreen = v == "1";
    } else if (k == "renderScale") {
      g_cfg.renderScale = static_cast<float>(std::atof(v.c_str()));
    } else if (k == "skipMenu") {
      g_cfg.skipMenu = v == "1";
    } else if (k == "console") {
      g_savedConsole = v != "0"; // anything but 0 keeps the default (checked)
    } else if (k == "backend") {
      sanitize_backend(v);
    }
  }
  sanitize();
  // --console / --no-console decide for this launch: the box starts that way, the file keeps the player's choice.
  const int request = gcrt_console_request();
  g_cfg.console = request >= 0 ? request == 1 : g_savedConsole;
}

bool gcrt_game_config_run_menu(const char* title) {
  // Hold Shift at launch to show the menu even if it is set to be skipped.
  if (g_cfg.skipMenu && !shift_held()) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "setup menu: skipped (skipMenu=1)");
    return true;
  }
  const MenuAutoSetting autoAct = menu_auto();
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "setup menu: shown%s",
                    autoAct.action == MenuAuto::Start   ? " (GCRT_SETUP_MENU_AUTO: start)"
                    : autoAct.action == MenuAuto::Close ? " (GCRT_SETUP_MENU_AUTO: close)"
                                                        : "");
  int passes = 0;
  int sel = 1;
  for (int i = 0; i < 4; ++i) {
    if (kSizes[i].w == g_cfg.windowWidth && kSizes[i].h == g_cfg.windowHeight) {
      sel = i;
    }
  }
  const bool consoleAtOpen = g_cfg.console;
  bool start = false;
  bool backdropTraced = false;
  // Host time of the menu (trace timestamps are emulated time, still 0 here): the first aurora_update also opens the
  // gamepads (Aurora's gamepad::initialize, SDL joystick subsystems), and each pass presents a frame. On a busy
  // computer both can take ten times longer or more: the times below tell a slow start from a stop (issue 31). With
  // GCRT_SETUP_MENU_AUTO, a progress line every 10 s while the menu is open.
  using Clock = std::chrono::steady_clock;
  const auto menuStart = Clock::now();
  auto passStart = menuStart;
  auto nextProgress = menuStart + std::chrono::seconds(10);
  double firstUpdate = -1.0;
  double slowest = 0.0;
  int total = 0;
  const auto elapsed = [](Clock::time_point from) {
    return std::chrono::duration<double>(Clock::now() - from).count();
  };
  while (!start) {
    gcrt_diag_startup_beat(); // one per pass: watched by the hang watchdog (gcrt_diag.h)
    const auto now = Clock::now();
    if (total > 1) { // the first pass (gamepads opened) is timed apart
      slowest = (std::max)(slowest, std::chrono::duration<double>(now - passStart).count());
    }
    passStart = now;
    ++total;
    if (autoAct.action != MenuAuto::None && now >= nextProgress) {
      nextProgress = now + std::chrono::seconds(10);
      gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                        "setup menu: open for %.1f s (host), %d passes, slowest pass after the first %.0f ms",
                        elapsed(menuStart), total - 1, slowest * 1000.0);
    }
    const AuroraEvent* ev = aurora_update();
    if (firstUpdate < 0.0) {
      firstUpdate = elapsed(menuStart);
    }
    while (ev != nullptr && ev->type != AURORA_NONE) {
      if (ev->type == AURORA_EXIT) {
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "setup menu: window closed");
        return false;
      }
      ++ev;
    }
    if (autoAct.action != MenuAuto::None && passes++ >= autoAct.passes) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "setup menu: GCRT_SETUP_MENU_AUTO %s after %d passes, %.1f s (host)",
                        autoAct.action == MenuAuto::Start ? "start" : "close", autoAct.passes, elapsed(menuStart));
      if (autoAct.action == MenuAuto::Close) {
        return false;
      }
      break; // "Start game"
    }
    if (!aurora_begin_frame()) {
      SDL_Delay(16); // window minimized: nothing to draw
      continue;
    }
    draw_menu_backdrop();
    if (!backdropTraced) {
      const ImVec2 size = ImGui::GetMainViewport()->Size;
      gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                        "setup menu: first frame, %.0fx%.0f, backdrop drawn, %.2f s (host) after the menu opened "
                        "(first event poll, which opens the gamepads: %.2f s)",
                        size.x, size.y, elapsed(menuStart), firstUpdate);
      backdropTraced = true;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 0));
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginCombo("Window size", kSizes[sel].label)) {
      for (int i = 0; i < 4; ++i) {
        if (ImGui::Selectable(kSizes[i].label, sel == i)) {
          sel = i;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::Checkbox("Fullscreen", &g_cfg.fullscreen);
    ImGui::SliderFloat("Render scale", &g_cfg.renderScale, 0.5f, 4.0f, "%.2fx");
    // Only the backends this build can try (gcrt_backend_available). The window is already open with the backend in
    // use, so a change applies the next time the game starts.
    if (ImGui::BeginCombo("Graphics backend", gcrt_backend_label(g_cfg.backend))) {
      for (const auto& b : kBackends) {
        if (b.offered && gcrt_backend_available(b.value) && ImGui::Selectable(b.label, g_cfg.backend == b.value)) {
          g_cfg.backend = b.value;
        }
      }
      ImGui::EndCombo();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    // Measured: Vulkan and Direct3D 12 differ by 1-2 levels on a few pixels of some 3D scenes, hence "up to".
    ImGui::TextWrapped("In use: %s. A change applies the next time the game starts. Auto tries Direct3D 12, then "
                       "Vulkan; both give the same picture, up to tiny rounding differences.",
                       gcrt_backend_label(g_activeBackend));
    ImGui::PopStyleColor();
#if defined(_WIN32)
    // Applied when the game starts (gcrt_console.h): nothing opens or closes while the menu is up.
    ImGui::Checkbox("Show console", &g_cfg.console);
    ImGui::SameLine();
    ImGui::TextDisabled("(game log, opens with the game)");
#endif
    ImGui::Checkbox("Skip this menu next time", &g_cfg.skipMenu);
    ImGui::TextDisabled("Hold Shift at launch to show it again.");
    ImGui::Separator();
    if (ImGui::Button("Start game (Enter)", ImVec2(-1, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
      start = true;
    }
    ImGui::End();
    aurora_end_frame();
    SDL_Delay(10); // no vsync (pacing is the game's): keep the menu from using a whole CPU core
  }
  g_cfg.windowWidth = kSizes[sel].w;
  g_cfg.windowHeight = kSizes[sel].h;
  if (g_cfg.console != consoleAtOpen) {
    g_savedConsole = g_cfg.console; // toggled by the player: remembered (a command-line request alone is not)
  }
  sanitize();
  save(gcrt_user_dir());
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "setup menu: start (console box %s, graphics backend saved: %s) after %.1f s "
                    "(host), %d passes, slowest pass after the first %.0f ms",
                    g_cfg.console ? "checked" : "unchecked", gcrt_backend_name(g_cfg.backend), elapsed(menuStart),
                    total, slowest * 1000.0);
  return true;
}

} // extern "C"
