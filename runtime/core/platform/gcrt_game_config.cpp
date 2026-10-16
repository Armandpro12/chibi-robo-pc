#include "gcrt_game_config.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <imgui.h>
#include <SDL3/SDL_keyboard.h>

#include <cstdlib>
#include <fstream>
#include <string>
#include "gcrt_user_dir.h"

namespace {
static const struct { const char* label; int w, h; } kSizes[] = {
    {"960x720 (4:3)", 960, 720},
    {"1280x960 (4:3)", 1280, 960},
    {"1600x1200 (4:3)", 1600, 1200},
    {"1920x1080 (16:9)", 1920, 1080},
};
GcrtGameConfig g_cfg = {1280, 960, false, 1.0f, false};

std::string path_for(const char* dir) { return std::string(dir) + "game_config.ini"; }

void save(const char* dir) {
  if (dir[0] == '\0') return; 
  std::ofstream f(path_for(dir));
  f << "width=" << g_cfg.windowWidth << "\nheight=" << g_cfg.windowHeight
    << "\nfullscreen=" << (g_cfg.fullscreen ? 1 : 0) << "\nrenderScale=" << g_cfg.renderScale
    << "\nskipMenu=" << (g_cfg.skipMenu ? 1 : 0) << "\n";
}
} // namespace

extern "C" {

const GcrtGameConfig* gcrt_game_config(void) { return &g_cfg; }

void gcrt_game_config_load() {

  const char* dir = gcrt_user_dir();
  if (dir[0] == '\0') return; // no user folder: keep the defaults, write nothing
  std::ifstream f(path_for(dir));
  std::string line;
  while (std::getline(f, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if      (k == "width")       g_cfg.windowWidth  = std::atoi(v.c_str());
    else if (k == "height")      g_cfg.windowHeight = std::atoi(v.c_str());
    else if (k == "fullscreen")  g_cfg.fullscreen   = (v == "1");
    else if (k == "renderScale") g_cfg.renderScale  = static_cast<float>(std::atof(v.c_str()));
  }
  bool knownSize = false;
  for (const auto& s : kSizes) knownSize |= s.w == g_cfg.windowWidth && s.h == g_cfg.windowHeight;
  if (!knownSize) { g_cfg.windowWidth = 1280; g_cfg.windowHeight = 960; }
  if (!(g_cfg.renderScale >= 0.5f && g_cfg.renderScale <= 4.0f)) g_cfg.renderScale = 1.0f; // also rejects NaN
}

bool gcrt_game_config_run_menu(const char* title) {
  
  int sel = 1;
  for (int i = 0; i < 4; ++i)
    if (kSizes[i].w == g_cfg.windowWidth && kSizes[i].h == g_cfg.windowHeight) sel = i;

  // Hold Shift at launch to force the menu even if "skip" is set.
  const bool forceMenu = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
  if (g_cfg.skipMenu && !forceMenu) return true;

  bool start = false;
  while (!start) {
    const AuroraEvent* ev = aurora_update();
    while (ev != nullptr && ev->type != AURORA_NONE) {
      if (ev->type == AURORA_EXIT) return false;
      ++ev;
    }
    if (!aurora_begin_frame()) continue;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420, 0));
    ImGui::Begin(title, nullptr,
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::BeginCombo("Window size", kSizes[sel].label)) {
      for (int i = 0; i < 4; ++i)
        if (ImGui::Selectable(kSizes[i].label, sel == i)) sel = i;
      ImGui::EndCombo();
    }
    ImGui::Checkbox("Fullscreen", &g_cfg.fullscreen);
    ImGui::SliderFloat("Render scale", &g_cfg.renderScale, 0.5f, 4.0f, "%.2fx");
    ImGui::Separator();
    if (ImGui::Button("Start game", ImVec2(-1, 0))) start = true;
    ImGui::End();
    aurora_end_frame();
  }
  g_cfg.windowWidth  = kSizes[sel].w;
  g_cfg.windowHeight = kSizes[sel].h;
  save(gcrt_user_dir());
  return true;
}

} // extern "C"
