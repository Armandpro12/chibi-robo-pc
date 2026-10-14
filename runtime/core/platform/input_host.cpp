// PC inputs -> GameCube controllers (see input_host.hpp).
#include "input_host.hpp"

#include "gcrt_user_dir.h"

#include <dolphin/pad.h> // PADGetIndexForPort, PADGetSDLGamepadForIndex: gamepad assignment by Aurora

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "trace.h"
}

namespace gcrt::platform {
namespace {
namespace fs = std::filesystem;
using namespace gcrt::input;

constexpr int kPorts = 4;
constexpr size_t kMaxReportedProblems = 40;

struct PortCache {
  SDL_JoystickID id = 0; // 0: no gamepad
  Resolved resolved;
  PadOutput last;
  bool lastConnected = false;
  bool traced = false;
};

struct KeyInjection {
  uint64_t first = 0;
  uint64_t count = 10;
  std::vector<int> scancodes;
  bool applied = false;
};

Config g_config;
Resolved g_keyboard;
bool g_initialized = false;
PortCache g_ports[kPorts];
std::vector<KeyInjection> g_inject;
std::unique_ptr<bool[]> g_keysBuffer;
int g_keysBufferSize = 0;
uint64_t g_reads = 0;

fs::path from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  return out;
}

// Keyboard, with the letter that each key bears on the active layout (AZERTY, QWERTY...).
std::string describe_keyboard(const Resolved& r) {
  std::string out;
  for (int c = 0; c < kControlCount; ++c) {
    if (!out.empty()) {
      out += "; ";
    }
    out += control_name(c);
    out += " = ";
    if (r.bindings[c].empty()) {
      out += "NONE";
    }
    bool first = true;
    for (const Physical& p : r.bindings[c]) {
      if (!first) {
        out += ", ";
      }
      first = false;
      out += physical_name(p);
      const SDL_Keycode k = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(p.code), SDL_KMOD_NONE, false);
      const char* label = SDL_GetKeyName(k);
      out += " [";
      out += label != nullptr && *label != '\0' ? label : "?";
      out += "]";
    }
  }
  return out;
}

void parse_injection() {
  const char* spec = std::getenv("GCRT_INPUT_KEYS");
  if (spec == nullptr || *spec == '\0') {
    return;
  }
  for (const std::string& item : split(spec, ';')) {
    if (item.empty()) {
      continue;
    }
    const auto parts = split(item, ':');
    KeyInjection inj;
    char* end = nullptr;
    inj.first = std::strtoull(parts[0].c_str(), &end, 0);
    bool ok = parts.size() >= 2 && end != parts[0].c_str() && *end == '\0' && inj.first > 0;
    if (ok && parts.size() >= 3) {
      inj.count = std::strtoull(parts[2].c_str(), &end, 0);
      ok = end != parts[2].c_str() && *end == '\0' && inj.count > 0;
    }
    if (ok) {
      for (const std::string& name : split(parts[1], '+')) {
        const int sc = scancode_from_name(name);
        if (sc < 0) {
          ok = false;
          break;
        }
        inj.scancodes.push_back(sc);
      }
    }
    if (!ok) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "GCRT_INPUT_KEYS: \"%s\" invalid (READ:KEY+KEY[:COUNT]): ignored",
                        item.c_str());
      continue;
    }
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "GCRT_INPUT_KEYS: keys %s held at reads %llu to %llu",
                      parts[1].c_str(), static_cast<unsigned long long>(inj.first),
                      static_cast<unsigned long long>(inj.first + inj.count - 1));
    g_inject.push_back(std::move(inj));
  }
}

const char* status_text(LoadStatus s) {
  switch (s) {
  case LoadStatus::Read:
    return "read";
  case LoadStatus::Created:
    return "absent: created with the default values";
  case LoadStatus::DefaultsOnly:
  default:
    return "not read: default values";
  }
}
} // namespace

void input_init() {
  g_initialized = true;
  const std::string dir = gcrt_user_dir();
  LoadResult loaded;
  if (dir.empty()) {
    loaded.config = default_config();
    loaded.note = "no user folder";
    loaded.fileName = kFileName;
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "input: no user folder: default mappings");
  } else {
    loaded = load_user_file(from_utf8(dir));
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "input: %s%s: %s%s%s", dir.c_str(), loaded.fileName.c_str(),
                      status_text(loaded.status), loaded.note.empty() ? "" : "; ", loaded.note.c_str());
  }
  const char* fileName = loaded.fileName.c_str();
  size_t shown = 0;
  for (const Diagnostic& d : loaded.problems) {
    if (shown++ == kMaxReportedProblems) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "%s: ... and %zu more problems", fileName,
                        loaded.problems.size() - kMaxReportedProblems);
      break;
    }
    if (d.line > 0) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "%s, line %d: %s", fileName, d.line, d.message.c_str());
    } else {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "%s: %s", fileName, d.message.c_str());
    }
  }
  if (!loaded.problems.empty()) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                      "%s: %zu problem(s); the faulty entries keep their default value", fileName,
                      loaded.problems.size());
  }
  g_config = loaded.config;
  g_keyboard = resolve_keyboard(g_config);
  if (g_config.keyboardPort < 0) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "input: keyboard disabled (port = 0)");
  } else {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                      "input: keyboard on port %d (QWERTY positions; in brackets, the key on this "
                      "keyboard): %s",
                      g_config.keyboardPort + 1, describe_keyboard(g_keyboard).c_str());
  }
  const Resolved standard = resolve_gamepad(g_config, {SDL_GAMEPAD_TYPE_STANDARD, 0, 0});
  gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                    "input: SDL gamepads assigned by Aurora; mappings of a standard gamepad: %s",
                    describe(standard, true).c_str());
  for (auto& p : g_ports) {
    p = PortCache{};
  }
  parse_injection();
}

void input_read_pads(PadOutput out[4], bool connected[4]) {
  if (!g_initialized) {
    input_init();
  }
  ++g_reads;
  int count = 0;
  const bool* sdlKeys = SDL_GetKeyboardState(&count);
  KeyboardState keys{sdlKeys, count};
  if (!g_inject.empty()) {
    const int size = std::max<int>(count, SDL_SCANCODE_COUNT);
    if (g_keysBufferSize != size) {
      g_keysBuffer = std::make_unique<bool[]>(static_cast<size_t>(size));
      g_keysBufferSize = size;
    }
    for (int i = 0; i < size; ++i) {
      g_keysBuffer[i] = sdlKeys != nullptr && i < count && sdlKeys[i];
    }
    for (size_t n = 0; n < g_inject.size(); ++n) {
      auto& inj = g_inject[n];
      if (g_reads >= inj.first && g_reads < inj.first + inj.count) {
        for (int sc : inj.scancodes) {
          if (sc >= 0 && sc < size) {
            g_keysBuffer[sc] = true;
          }
        }
        if (!inj.applied) {
          inj.applied = true;
          gcrt_trace_printf(GCRT_TRACE_PLATFORM, "GCRT_INPUT_KEYS: input #%zu applied (read %llu)", n + 1,
                            static_cast<unsigned long long>(g_reads));
        }
      }
    }
    keys = {g_keysBuffer.get(), size};
  }

  for (int port = 0; port < kPorts; ++port) {
    PortCache& cache = g_ports[port];
    SDL_Gamepad* pad = nullptr;
    const s32 index = PADGetIndexForPort(static_cast<u32>(port));
    if (index >= 0) {
      pad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
    }
    const SDL_JoystickID id = pad != nullptr ? SDL_GetGamepadID(pad) : 0;
    if (id != cache.id) {
      cache.id = id;
      if (pad != nullptr) {
        const GamepadIdentity ident{static_cast<int>(SDL_GetGamepadType(pad)), SDL_GetGamepadVendor(pad),
                                    SDL_GetGamepadProduct(pad)};
        cache.resolved = resolve_gamepad(g_config, ident);
        const char* name = SDL_GetGamepadName(pad);
        gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                          "input: port %d: gamepad \"%s\" (SDL type %d, %04X:%04X, section [gamepad.%s]): %s",
                          port + 1, name != nullptr ? name : "?", ident.sdlType, ident.vendor, ident.product,
                          family_name(family_of(ident)), describe(cache.resolved, true).c_str());
      } else {
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "input: port %d: gamepad removed", port + 1);
      }
    }
    GamepadState state;
    if (pad != nullptr) {
      for (int b = 0; b < kGamepadButtons; ++b) {
        state.buttons[b] = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
      }
      for (int a = 0; a < kGamepadAxes; ++a) {
        state.axes[a] = SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a));
      }
    }
    const bool keyboard = g_config.keyboardPort == port;
    out[port] = translate(keyboard ? &g_keyboard : nullptr, keyboard ? &keys : nullptr,
                          pad != nullptr ? &cache.resolved : nullptr, pad != nullptr ? &state : nullptr);
    connected[port] = keyboard || pad != nullptr;
    if (GCRT_TRACE_ON(GCRT_TRACE_PAD) && (!cache.traced || !(out[port] == cache.last) ||
                                          connected[port] != cache.lastConnected)) {
      const PadOutput& o = out[port];
      gcrt_trace_printf(GCRT_TRACE_PAD,
                        "input: port %d (read %llu): %s buttons %04X stick %d,%d C-stick %d,%d triggers %u,%u",
                        port + 1, static_cast<unsigned long long>(g_reads), connected[port] ? "connected" : "empty",
                        o.buttons, o.stickX, o.stickY, o.substickX, o.substickY, o.triggerL, o.triggerR);
    }
    cache.traced = true;
    cache.last = out[port];
    cache.lastConnected = connected[port];
  }
}

} // namespace gcrt::platform
