// The game's user folder (see gcrt_user_dir.h).
#include "gcrt_user_dir.h"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

extern "C" {
#include "trace.h"
}

namespace {
namespace fs = std::filesystem;

std::string g_org;
std::string g_app;
std::string g_dir;
bool g_resolved = false;
std::string g_save_dir;
bool g_save_resolved = false;

// UTF-8 path <-> std::filesystem::path (on Windows, a std::string would be read in the ANSI code page:
// a user name with accented letters in %APPDATA% would be converted wrongly).
fs::path from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// GCRT_USER_DIR, read as UTF-16 on Windows for the same reason.
bool env_user_dir(fs::path& out) {
#if defined(_WIN32)
  const wchar_t* v = _wgetenv(L"GCRT_USER_DIR");
  if (v == nullptr || *v == L'\0') {
    return false;
  }
  out = fs::path(v);
#else
  const char* v = std::getenv("GCRT_USER_DIR");
  if (v == nullptr || *v == '\0') {
    return false;
  }
  out = fs::path(v);
#endif
  return true;
}

void resolve() {
  g_resolved = true;
  fs::path override;
  if (env_user_dir(override)) {
    std::error_code ec;
    fs::create_directories(override, ec);
    const fs::path absolute = fs::absolute(override, ec);
    g_dir = to_utf8(ec ? override : absolute);
    if (!g_dir.empty() && g_dir.back() != '/' && g_dir.back() != '\\') {
      g_dir += static_cast<char>(fs::path::preferred_separator);
    }
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "user folder: %s (GCRT_USER_DIR)", g_dir.c_str());
    return;
  }
  if (g_app.empty()) {
    g_org = "gcrecomp";
    g_app = "gcrt";
    gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                      "WARNING: the game did not give a user folder name (gcrt_set_user_identity): "
                      "'%s/%s' used",
                      g_org.c_str(), g_app.c_str());
  }
  char* pref = SDL_GetPrefPath(g_org.c_str(), g_app.c_str());
  if (pref == nullptr) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                      "WARNING: user folder unavailable (SDL_GetPrefPath: %s): default settings",
                      SDL_GetError());
    return;
  }
  g_dir = pref;
  SDL_free(pref);
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "user folder: %s", g_dir.c_str());
}

std::string with_separator(std::string s) {
  if (!s.empty() && s.back() != '/' && s.back() != static_cast<char>(0x5C)) { // 0x5C: backslash
    s += static_cast<char>(fs::path::preferred_separator);
  }
  return s;
}

void resolve_save_dir() {
  g_save_resolved = true;
  fs::path override;
  if (env_user_dir(override)) {
    std::error_code ec;
    const fs::path absolute = fs::absolute(override, ec);
    g_save_dir = with_separator(to_utf8((ec ? override : absolute) / "saves"));
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "save folder: %s (GCRT_USER_DIR)", g_save_dir.c_str());
    return;
  }
  if (g_app.empty()) {
    gcrt_user_dir(); // sets the default identity and the warning
  }
  // FOLDERID_SavedGames on Windows (SHGetKnownFolderPath with KF_FLAG_CREATE, according to the SDL 3.4 sources);
  // NULL elsewhere. String kept by SDL: do not free it.
  const char* saved_games = SDL_GetUserFolder(SDL_FOLDER_SAVEDGAMES);
  if (saved_games != nullptr && saved_games[0] != 0) {
    g_save_dir = with_separator(to_utf8(from_utf8(saved_games) / from_utf8(g_org) / from_utf8(g_app)));
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "save folder: %s", g_save_dir.c_str());
    return;
  }
  const std::string base = gcrt_user_dir();
  if (base.empty()) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "WARNING: no folder for saves (SDL_GetUserFolder: %s)",
                      SDL_GetError());
    return;
  }
  g_save_dir = with_separator(to_utf8(from_utf8(base) / "saves"));
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "save folder: %s (no \"Saved Games\" folder)",
                    g_save_dir.c_str());
}
} // namespace

extern "C" void gcrt_set_user_identity(const char* org, const char* app) {
  g_org = org != nullptr ? org : "";
  g_app = app != nullptr ? app : "";
}

extern "C" const char* gcrt_user_dir(void) {
  if (!g_resolved) {
    resolve();
  }
  return g_dir.c_str();
}

extern "C" const char* gcrt_save_dir(void) {
  if (!g_save_resolved) {
    resolve_save_dir();
  }
  return g_save_dir.c_str();
}
