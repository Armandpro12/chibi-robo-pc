// Setup menu shown on a player's direct launch, and its settings file (see gcrt_game_config.h).
#include "gcrt_game_config.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "gcrt_console.h"
#include "gcrt_user_dir.h"

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
GcrtGameConfig g_cfg = {1280, 960, false, 1.0f, false, true};
bool g_savedConsole = true; // value of the file: what is written back unless the player toggles the box

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
      << "\nskipMenu=" << (g_cfg.skipMenu ? 1 : 0) << "\nconsole=" << (g_savedConsole ? 1 : 0) << "\n";
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
#if defined(_WIN32)
  return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
  SDL_PumpEvents();
  return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
}
} // namespace

extern "C" {

const GcrtGameConfig* gcrt_game_config(void) { return &g_cfg; }

void gcrt_game_config_load(void) {
  const char* dir = gcrt_user_dir();
  std::ifstream f;
  if (dir[0] != '\0') { // no user folder: the defaults
    f.open(config_path(dir));
  }
  std::string line;
  while (std::getline(f, line)) {
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
    return true;
  }
  int sel = 1;
  for (int i = 0; i < 4; ++i) {
    if (kSizes[i].w == g_cfg.windowWidth && kSizes[i].h == g_cfg.windowHeight) {
      sel = i;
    }
  }
  const bool consoleAtOpen = g_cfg.console;
  bool start = false;
  while (!start) {
    const AuroraEvent* ev = aurora_update();
    while (ev != nullptr && ev->type != AURORA_NONE) {
      if (ev->type == AURORA_EXIT) {
        return false;
      }
      ++ev;
    }
    if (!aurora_begin_frame()) {
      SDL_Delay(16); // window minimized: nothing to draw
      continue;
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
  return true;
}

} // extern "C"
