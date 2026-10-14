// Configurable keyboard / SDL gamepad -> GameCube controller mappings (see input_config.hpp).
#include "input_config.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <system_error>

namespace gcrt::input {
namespace {

static_assert(kGamepadButtons == SDL_GAMEPAD_BUTTON_COUNT, "SDL: number of gamepad buttons changed");
static_assert(kGamepadAxes == SDL_GAMEPAD_AXIS_COUNT, "SDL: number of gamepad axes changed");

namespace fs = std::filesystem;

// ------------------------------------------------------------------------------------------------
// Names
// ------------------------------------------------------------------------------------------------

// GameCube commands, in Control order.
constexpr const char* kControlNames[kControlCount] = {
    "A",        "B",          "X",          "Y",           "Z",          "L",
    "R",        "START",      "DPAD_UP",    "DPAD_DOWN",   "DPAD_LEFT",  "DPAD_RIGHT",
    "STICK_UP", "STICK_DOWN", "STICK_LEFT", "STICK_RIGHT", "CSTICK_UP",  "CSTICK_DOWN",
    "CSTICK_LEFT", "CSTICK_RIGHT", "TRIGGER_L", "TRIGGER_R",
};

// SDK PAD_BUTTON_* bits for the 12 buttons (Control order).
constexpr uint16_t kButtonBits[kButtonControls] = {0x0100, 0x0200, 0x0400, 0x0800, 0x0010, 0x0040,
                                                   0x0020, 0x1000, 0x0008, 0x0004, 0x0001, 0x0002};

// Settings of the gamepad sections (Setting order).
constexpr const char* kSettingNames[kSettingCount] = {"STICK_DEAD_ZONE", "CSTICK_DEAD_ZONE",
                                                      "TRIGGER_DEAD_ZONE", "TRIGGER_BUTTON_THRESHOLD",
                                                      "STICK_BUTTON_THRESHOLD"};

// Scancodes: names of the SDL 3.4 SDL_SCANCODE_* constants without the prefix (list extracted from
// SDL_scancode.h; each name is checked at compile time by the constant itself).
struct NamedCode {
  const char* name;
  int code;
};
#define K(n) {#n, SDL_SCANCODE_##n}
constexpr NamedCode kScancodes[] = {
    K(A), K(B), K(C), K(D), K(E), K(F), K(G), K(H), K(I), K(J), K(K), K(L), K(M), K(N), K(O), K(P), K(Q), K(R),
    K(S), K(T), K(U), K(V), K(W), K(X), K(Y), K(Z), K(1), K(2), K(3), K(4), K(5), K(6), K(7), K(8), K(9), K(0),
    K(RETURN), K(ESCAPE), K(BACKSPACE), K(TAB), K(SPACE), K(MINUS), K(EQUALS), K(LEFTBRACKET), K(RIGHTBRACKET),
    K(BACKSLASH), K(NONUSHASH), K(SEMICOLON), K(APOSTROPHE), K(GRAVE), K(COMMA), K(PERIOD), K(SLASH),
    K(CAPSLOCK), K(F1), K(F2), K(F3), K(F4), K(F5), K(F6), K(F7), K(F8), K(F9), K(F10), K(F11), K(F12),
    K(PRINTSCREEN), K(SCROLLLOCK), K(PAUSE), K(INSERT), K(HOME), K(PAGEUP), K(DELETE), K(END), K(PAGEDOWN),
    K(RIGHT), K(LEFT), K(DOWN), K(UP), K(NUMLOCKCLEAR), K(KP_DIVIDE), K(KP_MULTIPLY), K(KP_MINUS), K(KP_PLUS),
    K(KP_ENTER), K(KP_1), K(KP_2), K(KP_3), K(KP_4), K(KP_5), K(KP_6), K(KP_7), K(KP_8), K(KP_9), K(KP_0),
    K(KP_PERIOD), K(NONUSBACKSLASH), K(APPLICATION), K(POWER), K(KP_EQUALS), K(F13), K(F14), K(F15), K(F16),
    K(F17), K(F18), K(F19), K(F20), K(F21), K(F22), K(F23), K(F24), K(EXECUTE), K(HELP), K(MENU), K(SELECT),
    K(STOP), K(AGAIN), K(UNDO), K(CUT), K(COPY), K(PASTE), K(FIND), K(MUTE), K(VOLUMEUP), K(VOLUMEDOWN),
    K(KP_COMMA), K(KP_EQUALSAS400), K(INTERNATIONAL1), K(INTERNATIONAL2), K(INTERNATIONAL3), K(INTERNATIONAL4),
    K(INTERNATIONAL5), K(INTERNATIONAL6), K(INTERNATIONAL7), K(INTERNATIONAL8), K(INTERNATIONAL9), K(LANG1),
    K(LANG2), K(LANG3), K(LANG4), K(LANG5), K(LANG6), K(LANG7), K(LANG8), K(LANG9), K(ALTERASE), K(SYSREQ),
    K(CANCEL), K(CLEAR), K(PRIOR), K(RETURN2), K(SEPARATOR), K(OUT), K(OPER), K(CLEARAGAIN), K(CRSEL),
    K(EXSEL), K(KP_00), K(KP_000), K(THOUSANDSSEPARATOR), K(DECIMALSEPARATOR), K(CURRENCYUNIT),
    K(CURRENCYSUBUNIT), K(KP_LEFTPAREN), K(KP_RIGHTPAREN), K(KP_LEFTBRACE), K(KP_RIGHTBRACE), K(KP_TAB),
    K(KP_BACKSPACE), K(KP_A), K(KP_B), K(KP_C), K(KP_D), K(KP_E), K(KP_F), K(KP_XOR), K(KP_POWER),
    K(KP_PERCENT), K(KP_LESS), K(KP_GREATER), K(KP_AMPERSAND), K(KP_DBLAMPERSAND), K(KP_VERTICALBAR),
    K(KP_DBLVERTICALBAR), K(KP_COLON), K(KP_HASH), K(KP_SPACE), K(KP_AT), K(KP_EXCLAM), K(KP_MEMSTORE),
    K(KP_MEMRECALL), K(KP_MEMCLEAR), K(KP_MEMADD), K(KP_MEMSUBTRACT), K(KP_MEMMULTIPLY), K(KP_MEMDIVIDE),
    K(KP_PLUSMINUS), K(KP_CLEAR), K(KP_CLEARENTRY), K(KP_BINARY), K(KP_OCTAL), K(KP_DECIMAL),
    K(KP_HEXADECIMAL), K(LCTRL), K(LSHIFT), K(LALT), K(LGUI), K(RCTRL), K(RSHIFT), K(RALT), K(RGUI), K(MODE),
    K(SLEEP), K(WAKE), K(CHANNEL_INCREMENT), K(CHANNEL_DECREMENT), K(MEDIA_PLAY), K(MEDIA_PAUSE),
    K(MEDIA_RECORD), K(MEDIA_FAST_FORWARD), K(MEDIA_REWIND), K(MEDIA_NEXT_TRACK), K(MEDIA_PREVIOUS_TRACK),
    K(MEDIA_STOP), K(MEDIA_EJECT), K(MEDIA_PLAY_PAUSE), K(MEDIA_SELECT), K(AC_NEW), K(AC_OPEN), K(AC_CLOSE),
    K(AC_EXIT), K(AC_SAVE), K(AC_PRINT), K(AC_PROPERTIES), K(AC_SEARCH), K(AC_HOME), K(AC_BACK), K(AC_FORWARD),
    K(AC_STOP), K(AC_REFRESH), K(AC_BOOKMARKS), K(SOFTLEFT), K(SOFTRIGHT), K(CALL), K(ENDCALL),
};
#undef K

// Gamepad buttons: names of the SDL_GAMEPAD_BUTTON_* constants without the prefix.
#define B(n) {#n, SDL_GAMEPAD_BUTTON_##n}
constexpr NamedCode kGamepadButtonNames[] = {
    B(SOUTH),         B(EAST),          B(WEST),          B(NORTH),        B(BACK),       B(GUIDE),
    B(START),         B(LEFT_STICK),    B(RIGHT_STICK),   B(LEFT_SHOULDER), B(RIGHT_SHOULDER),
    B(DPAD_UP),       B(DPAD_DOWN),     B(DPAD_LEFT),     B(DPAD_RIGHT),   B(MISC1),      B(RIGHT_PADDLE1),
    B(LEFT_PADDLE1),  B(RIGHT_PADDLE2), B(LEFT_PADDLE2),  B(TOUCHPAD),     B(MISC2),      B(MISC3),
    B(MISC4),         B(MISC5),         B(MISC6),
};
#undef B

// Gamepad half-axes: names specific to the file (SDL axis, direction). SDL: positive y points down.
struct NamedAxis {
  const char* name;
  int axis;
  int sign;
};
constexpr NamedAxis kGamepadAxisNames[] = {
    {"LEFT_STICK_RIGHT", SDL_GAMEPAD_AXIS_LEFTX, 1},   {"LEFT_STICK_LEFT", SDL_GAMEPAD_AXIS_LEFTX, -1},
    {"LEFT_STICK_DOWN", SDL_GAMEPAD_AXIS_LEFTY, 1},    {"LEFT_STICK_UP", SDL_GAMEPAD_AXIS_LEFTY, -1},
    {"RIGHT_STICK_RIGHT", SDL_GAMEPAD_AXIS_RIGHTX, 1}, {"RIGHT_STICK_LEFT", SDL_GAMEPAD_AXIS_RIGHTX, -1},
    {"RIGHT_STICK_DOWN", SDL_GAMEPAD_AXIS_RIGHTY, 1},  {"RIGHT_STICK_UP", SDL_GAMEPAD_AXIS_RIGHTY, -1},
    {"LEFT_TRIGGER", SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1}, {"RIGHT_TRIGGER", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1},
};

constexpr const char* kFamilyNames[kFamilyCount] = {"xbox", "playstation", "switch", "gamecube", "other"};

std::string upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return out;
}

std::string normalize_name(std::string_view s) { return upper(s); }

std::string_view trim(std::string_view s) {
  const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f'; };
  while (!s.empty() && blank(s.front())) {
    s.remove_prefix(1);
  }
  while (!s.empty() && blank(s.back())) {
    s.remove_suffix(1);
  }
  return s;
}

int control_from_name(const std::string& name) {
  for (int i = 0; i < kControlCount; ++i) {
    if (name == kControlNames[i]) {
      return i;
    }
  }
  return -1;
}

int setting_from_name(const std::string& name) {
  for (int i = 0; i < kSettingCount; ++i) {
    if (name == kSettingNames[i]) {
      return i;
    }
  }
  return -1;
}

bool gamepad_from_name(const std::string& name, Physical& out) {
  for (const auto& b : kGamepadButtonNames) {
    if (name == b.name) {
      out = {Physical::Kind::Button, static_cast<int16_t>(b.code), 1};
      return true;
    }
  }
  for (const auto& a : kGamepadAxisNames) {
    if (name == a.name) {
      out = {Physical::Kind::Axis, static_cast<int16_t>(a.axis), static_cast<int8_t>(a.sign)};
      return true;
    }
  }
  return false;
}

Physical key(int scancode) { return {Physical::Kind::Key, static_cast<int16_t>(scancode), 1}; }
Physical button(int b) { return {Physical::Kind::Button, static_cast<int16_t>(b), 1}; }
Physical axis(int a, int sign) { return {Physical::Kind::Axis, static_cast<int16_t>(a), static_cast<int8_t>(sign)}; }

bool is_trigger_axis(int a) { return a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }

// Percentage of the travel -> SDL units (0..32767), rounded to the nearest.
int percent_to_raw(double pct) {
  return static_cast<int>(std::clamp<long>(std::lround(pct * 32767.0 / 100.0), 0L, 32767L));
}

// SDL units -> percentage written with the fewest decimals that give back exactly the same value.
std::string raw_to_percent(int raw) {
  char buf[32];
  for (int decimals = 0; decimals <= 4; ++decimals) {
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, raw * 100.0 / 32767.0);
    double back = 0.0;
    std::from_chars(buf, buf + std::char_traits<char>::length(buf), back);
    if (percent_to_raw(back) == raw) {
      break;
    }
  }
  return buf;
}

bool parse_percent(std::string_view text, double& out) {
  std::string s(trim(text));
  if (!s.empty() && s.back() == '%') {
    s.pop_back();
    s = std::string(trim(s));
  }
  std::replace(s.begin(), s.end(), ',', '.'); // decimal comma accepted
  if (s.empty()) {
    return false;
  }
  const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
  return res.ec == std::errc() && res.ptr == s.data() + s.size() && out >= 0.0 && out <= 100.0;
}

bool parse_int(std::string_view text, int& out) {
  const std::string_view s = trim(text);
  if (s.empty()) {
    return false;
  }
  const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
  return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

std::vector<std::string_view> split_list(std::string_view value) {
  std::vector<std::string_view> items;
  size_t start = 0;
  while (start <= value.size()) {
    size_t comma = value.find(',', start);
    if (comma == std::string_view::npos) {
      comma = value.size();
    }
    const std::string_view item = trim(value.substr(start, comma - start));
    if (!item.empty()) {
      items.push_back(item);
    }
    start = comma + 1;
  }
  return items;
}

std::string join_names(const Bindings& b) {
  if (b.empty()) {
    return "NONE";
  }
  std::string out;
  for (const auto& p : b) {
    if (!out.empty()) {
      out += ", ";
    }
    out += physical_name(p);
  }
  return out;
}

// Applies a file section to effective mappings.
void apply(Resolved& r, const Section& s) {
  for (int c = 0; c < kControlCount; ++c) {
    if (s.present[c]) {
      r.bindings[c] = s.bindings[c];
    }
  }
  int* fields[kSettingCount] = {&r.thresholds.stickDeadZone, &r.thresholds.cstickDeadZone,
                                &r.thresholds.triggerDeadZone, &r.thresholds.triggerButton,
                                &r.thresholds.stickButton};
  for (int i = 0; i < kSettingCount; ++i) {
    if (s.settingPresent[i]) {
      *fields[i] = s.setting[i];
    }
  }
}

// ------------------------------------------------------------------------------------------------
// Translation (Aurora semantics)
// ------------------------------------------------------------------------------------------------

// Normalized value of an SDL axis (Aurora lib/input/sdl_input.cpp: normalize_axis).
float axis_value(int a, int16_t raw) {
  const float v = static_cast<float>(raw) / 32767.f;
  return is_trigger_axis(a) ? std::clamp(v, 0.f, 1.f) : std::clamp(v, -1.f, 1.f);
}

// Value of a physical input in [0, 1] (half-axis: the part with the right sign, like GamepadAxis::select).
float source_value(const Physical& p, const KeyboardState* keys, const GamepadState* pad) {
  switch (p.kind) {
  case Physical::Kind::Key:
    return keys != nullptr && keys->keys != nullptr && p.code >= 0 && p.code < keys->count && keys->keys[p.code]
               ? 1.f
               : 0.f;
  case Physical::Kind::Button:
    return pad != nullptr && p.code >= 0 && p.code < kGamepadButtons && pad->buttons[p.code] ? 1.f : 0.f;
  case Physical::Kind::Axis: {
    if (pad == nullptr || p.code < 0 || p.code >= kGamepadAxes) {
      return 0.f;
    }
    const float v = axis_value(p.code, pad->axes[p.code]);
    return p.sign > 0 ? (v > 0.f ? v : 0.f) : (v < 0.f ? -v : 0.f);
  }
  }
  return 0.f;
}

int8_t to_stick(float v) { return static_cast<int8_t>(std::clamp(std::lround(v * 127.f), -127L, 127L)); }
uint8_t to_trigger(float v) { return static_cast<uint8_t>(std::clamp(std::lround(v * 255.f), 0L, 255L)); }

} // namespace

// ------------------------------------------------------------------------------------------------
// Public names
// ------------------------------------------------------------------------------------------------

const char* control_name(int control) {
  return control >= 0 && control < kControlCount ? kControlNames[control] : "?";
}

int scancode_from_name(std::string_view name) {
  const std::string n = upper(trim(name));
  for (const auto& k : kScancodes) {
    if (n == k.name) {
      return k.code;
    }
  }
  return -1;
}

std::string physical_name(const Physical& p) {
  switch (p.kind) {
  case Physical::Kind::Key:
    for (const auto& k : kScancodes) {
      if (k.code == p.code) {
        return k.name;
      }
    }
    break;
  case Physical::Kind::Button:
    for (const auto& b : kGamepadButtonNames) {
      if (b.code == p.code) {
        return b.name;
      }
    }
    break;
  case Physical::Kind::Axis:
    for (const auto& a : kGamepadAxisNames) {
      if (a.axis == p.code && a.sign == p.sign) {
        return a.name;
      }
    }
    break;
  }
  return "#" + std::to_string(p.code);
}

const char* family_name(Family f) {
  const int i = static_cast<int>(f);
  return i >= 0 && i < kFamilyCount ? kFamilyNames[i] : "?";
}

std::string describe(const Resolved& r, bool withThresholds) {
  std::string out;
  for (int c = 0; c < kControlCount; ++c) {
    if (!out.empty()) {
      out += "; ";
    }
    out += kControlNames[c];
    out += " = ";
    out += join_names(r.bindings[c]);
  }
  if (withThresholds) {
    const auto& t = r.thresholds;
    out += "; dead zones: stick " + raw_to_percent(t.stickDeadZone) + "%, C-stick " +
           raw_to_percent(t.cstickDeadZone) + "%, triggers " + raw_to_percent(t.triggerDeadZone) +
           "%; button thresholds: trigger " + raw_to_percent(t.triggerButton) + "%, stick " +
           raw_to_percent(t.stickButton) + "%";
  }
  return out;
}

// ------------------------------------------------------------------------------------------------
// Default values
// ------------------------------------------------------------------------------------------------

// Keyboard: runtime table from before the file (former kKeyboard table of aurora_host.cpp, notes/runtime.md
// section 8.3), QWERTY physical positions.
Resolved keyboard_defaults() {
  Resolved r;
  auto& b = r.bindings;
  b[kA] = {key(SDL_SCANCODE_X)};
  b[kB] = {key(SDL_SCANCODE_Z)};
  b[kX] = {key(SDL_SCANCODE_C)};
  b[kY] = {key(SDL_SCANCODE_S)};
  b[kStart] = {key(SDL_SCANCODE_RETURN)};
  b[kZ] = {key(SDL_SCANCODE_D)};
  b[kL] = {key(SDL_SCANCODE_Q)};
  b[kR] = {key(SDL_SCANCODE_W)};
  b[kDpadUp] = {key(SDL_SCANCODE_T)};
  b[kDpadDown] = {key(SDL_SCANCODE_G)};
  b[kDpadLeft] = {key(SDL_SCANCODE_F)};
  b[kDpadRight] = {key(SDL_SCANCODE_H)};
  b[kStickRight] = {key(SDL_SCANCODE_RIGHT)};
  b[kStickLeft] = {key(SDL_SCANCODE_LEFT)};
  b[kStickUp] = {key(SDL_SCANCODE_UP)};
  b[kStickDown] = {key(SDL_SCANCODE_DOWN)};
  b[kCStickRight] = {key(SDL_SCANCODE_L)};
  b[kCStickLeft] = {key(SDL_SCANCODE_J)};
  b[kCStickUp] = {key(SDL_SCANCODE_I)};
  b[kCStickDown] = {key(SDL_SCANCODE_K)};
  b[kTriggerL] = {key(SDL_SCANCODE_A)};
  b[kTriggerR] = {key(SDL_SCANCODE_E)};
  return r;
}

Family family_of(const GamepadIdentity& id) {
  const bool nso = id.sdlType == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && id.product == 0x2073;
  const bool adapter = id.vendor == 0x057E && id.product == 0x0337;
  if (nso || adapter || id.sdlType == SDL_GAMEPAD_TYPE_GAMECUBE) {
    return Family::GameCube;
  }
  switch (id.sdlType) {
  case SDL_GAMEPAD_TYPE_XBOX360:
  case SDL_GAMEPAD_TYPE_XBOXONE:
    return Family::Xbox;
  case SDL_GAMEPAD_TYPE_PS3:
  case SDL_GAMEPAD_TYPE_PS4:
  case SDL_GAMEPAD_TYPE_PS5:
    return Family::PlayStation;
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
  case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
    return Family::Switch;
  default:
    return Family::Other;
  }
}

// Gamepads: Aurora default values (lib/dolphin/pad/pad.cpp, g_defaultButtons* tables chosen by
// default_buttons() according to SDL_GetGamepadType; g_defaultAxes; lib/gamepad.cpp add_controller for
// emulateTriggers; lib/dolphin/pad/adapter.cpp for the emulation of L/R by the triggers).
Resolved gamepad_defaults(const GamepadIdentity& id) {
  Resolved r;
  auto& b = r.bindings;
  const bool nso = id.sdlType == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO && id.product == 0x2073;
  const bool adapter = id.vendor == 0x057E && id.product == 0x0337;
  // Xbox 360/One, GameCube and GameCube NSO: B on the left (WEST), X on the right (EAST), as on a GameCube;
  // the other Aurora tables (standard, PlayStation, Switch) do the opposite.
  const bool gcLayout = nso || id.sdlType == SDL_GAMEPAD_TYPE_XBOX360 || id.sdlType == SDL_GAMEPAD_TYPE_XBOXONE ||
                        id.sdlType == SDL_GAMEPAD_TYPE_GAMECUBE;
  b[kA] = {button(SDL_GAMEPAD_BUTTON_SOUTH)};
  b[kB] = {button(gcLayout ? SDL_GAMEPAD_BUTTON_WEST : SDL_GAMEPAD_BUTTON_EAST)};
  b[kX] = {button(gcLayout ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_WEST)};
  b[kY] = {button(SDL_GAMEPAD_BUTTON_NORTH)};
  b[kStart] = {button(SDL_GAMEPAD_BUTTON_START)};
  b[kZ] = {button(nso ? SDL_GAMEPAD_BUTTON_BACK : SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)};
  if (nso) {
    b[kL] = {button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)};
    b[kR] = {button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)};
  } else if (id.sdlType == SDL_GAMEPAD_TYPE_GAMECUBE) {
    b[kL] = {button(SDL_GAMEPAD_BUTTON_MISC3)};
    b[kR] = {button(SDL_GAMEPAD_BUTTON_MISC4)};
  } else if (!adapter) {
    // emulateTriggers: digital L and R = triggers pressed beyond the threshold (31150 + 1).
    b[kL] = {axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1)};
    b[kR] = {axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1)};
  }
  b[kDpadUp] = {button(SDL_GAMEPAD_BUTTON_DPAD_UP)};
  b[kDpadDown] = {button(SDL_GAMEPAD_BUTTON_DPAD_DOWN)};
  b[kDpadLeft] = {button(SDL_GAMEPAD_BUTTON_DPAD_LEFT)};
  b[kDpadRight] = {button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)};
  b[kStickRight] = {axis(SDL_GAMEPAD_AXIS_LEFTX, 1)};
  b[kStickLeft] = {axis(SDL_GAMEPAD_AXIS_LEFTX, -1)};
  b[kStickUp] = {axis(SDL_GAMEPAD_AXIS_LEFTY, -1)};
  b[kStickDown] = {axis(SDL_GAMEPAD_AXIS_LEFTY, 1)};
  b[kCStickRight] = {axis(SDL_GAMEPAD_AXIS_RIGHTX, 1)};
  b[kCStickLeft] = {axis(SDL_GAMEPAD_AXIS_RIGHTX, -1)};
  b[kCStickUp] = {axis(SDL_GAMEPAD_AXIS_RIGHTY, -1)};
  b[kCStickDown] = {axis(SDL_GAMEPAD_AXIS_RIGHTY, 1)};
  b[kTriggerL] = {axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1)};
  b[kTriggerR] = {axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1)};
  return r;
}

Config default_config() { return Config{}; }

Resolved resolve_keyboard(const Config& config) {
  Resolved r = keyboard_defaults();
  apply(r, config.keyboard);
  return r;
}

Resolved resolve_gamepad(const Config& config, const GamepadIdentity& id) {
  Resolved r = gamepad_defaults(id);
  apply(r, config.gamepad);
  apply(r, config.family[static_cast<int>(family_of(id))]);
  return r;
}

// ------------------------------------------------------------------------------------------------
// Translation
// ------------------------------------------------------------------------------------------------

PadOutput translate(const Resolved* keyboard, const KeyboardState* keys, const Resolved* gamepad,
                    const GamepadState* pad) {
  PadOutput out;
  float axes[kControlCount - kButtonControls] = {};
  // Aurora order: gamepad mappings, then keyboard mappings.
  const Resolved* sources[2] = {gamepad, keyboard};
  for (const Resolved* r : sources) {
    if (r == nullptr) {
      continue;
    }
    const auto& t = r->thresholds;
    for (int c = 0; c < kControlCount; ++c) {
      for (const Physical& p : r->bindings[c]) {
        float v = source_value(p, keys, pad);
        if (c < kButtonControls) {
          // Button: key or button (threshold 0.5); half-axis: adjustable threshold (trigger or stick).
          float threshold = 0.5f;
          if (p.kind == Physical::Kind::Axis) {
            const int raw = is_trigger_axis(p.code) ? t.triggerButton : t.stickButton;
            threshold = std::clamp(static_cast<float>(raw) / 32767.f, 1.f / 32767.f, 1.f);
          }
          if (v != 0.f && v >= threshold) {
            out.buttons |= kButtonBits[c];
          }
          continue;
        }
        // Axis: dead zone (gamepad axes only, like Aurora), then clamped contribution.
        if (p.kind == Physical::Kind::Axis) {
          const int dz = c >= kTriggerL ? t.triggerDeadZone : c >= kCStickUp ? t.cstickDeadZone : t.stickDeadZone;
          if (std::abs(v) <= static_cast<float>(dz) / 32767.f) {
            v = 0.f;
          }
        }
        axes[c - kButtonControls] += std::clamp(v, -1.f, 1.f);
      }
    }
  }
  const auto half = [&](int c) { return axes[c - kButtonControls]; };
  const auto total = [](float pos, float neg) { return std::clamp(pos - neg, -1.f, 1.f); };
  out.stickX = to_stick(total(half(kStickRight), half(kStickLeft)));
  out.stickY = to_stick(total(half(kStickUp), half(kStickDown)));
  out.substickX = to_stick(total(half(kCStickRight), half(kCStickLeft)));
  out.substickY = to_stick(total(half(kCStickUp), half(kCStickDown)));
  out.triggerL = to_trigger(std::clamp(half(kTriggerL), -1.f, 1.f));
  out.triggerR = to_trigger(std::clamp(half(kTriggerR), -1.f, 1.f));
  // Aurora (adapter.cpp, PortState::read): a digital L or R reads as a trigger at 180.
  if (out.buttons & kButtonBits[kL]) {
    out.triggerL = 180;
  }
  if (out.buttons & kButtonBits[kR]) {
    out.triggerR = 180;
  }
  return out;
}

// ------------------------------------------------------------------------------------------------
// Default text
// ------------------------------------------------------------------------------------------------

namespace {
void line(std::string& out, const char* name, const std::string& value, const char* comment = nullptr,
          int width = 14) {
  char buf[256];
  if (comment != nullptr) {
    std::snprintf(buf, sizeof(buf), "%-*s = %-8s # %s\n", width, name, value.c_str(), comment);
  } else {
    std::snprintf(buf, sizeof(buf), "%-*s = %s\n", width, name, value.c_str());
  }
  out += buf;
}
} // namespace

std::string default_file_text() {
  const Resolved kb = keyboard_defaults();
  const Resolved pad = gamepad_defaults({SDL_GAMEPAD_TYPE_STANDARD, 0, 0});
  std::string t;
  t += "# ==========================================================================================\n"
       "#  PC keyboard and gamepad mappings to GameCube controllers\n"
       "# ==========================================================================================\n"
       "# This file was created automatically with the default values. Edit it with a text editor\n"
       "# (Notepad...) while the game is closed: it is read at every launch. Save it as UTF-8.\n"
       "# To go back to the default values, delete this file: it is created again at the next launch.\n"
       "#\n"
       "# Syntax:\n"
       "#   COMMAND = INPUT1, INPUT2, ...      one or more inputs, separated by commas;\n"
       "#   COMMAND = NONE                     command not assigned;\n"
       "#   \"#\" or \";\" starts a comment; upper and lower case are equivalent.\n"
       "#   A missing line keeps its default value. So does an invalid line: the problem is\n"
       "#   reported in the game trace (console window, or trace.log of launches made by\n"
       "#   tools/run.py), with the line number.\n"
       "#\n"
       "# GameCube controller commands:\n"
       "#   A, B, X, Y, Z, START, L, R              (L and R: trigger clicks; a click also reads\n"
       "#                                            as a trigger pressed to 180 out of 255)\n"
       "#   DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT                (directional pad)\n"
       "#   STICK_UP, STICK_DOWN, STICK_LEFT, STICK_RIGHT            (main stick)\n"
       "#   CSTICK_UP, CSTICK_DOWN, CSTICK_LEFT, CSTICK_RIGHT        (C-stick, the small yellow stick)\n"
       "#   TRIGGER_L, TRIGGER_R                                     (analog triggers)\n"
       "\n"
       "version = 1\n"
       "\n"
       "# ------------------------------------------------------------------------------------------\n"
       "[keyboard]\n"
       "# Keys are named by their POSITION on the keyboard (SDL scancodes), with the name they have\n"
       "# on a US QWERTY keyboard: what counts is where the key is, not the letter printed on it.\n"
       "# On an AZERTY keyboard: Q = key A, W = key Z, Z = key W, A = key Q, SEMICOLON = key M,\n"
       "# M = key \",\". At launch, the game trace shows the letter that each assigned key has on\n"
       "# YOUR keyboard.\n"
       "# Names: A to Z, 0 to 9, F1 to F24, UP, DOWN, LEFT, RIGHT (arrows), RETURN (Enter), SPACE,\n"
       "# ESCAPE, TAB, BACKSPACE, LSHIFT, RSHIFT, LCTRL, RCTRL, LALT, RALT (Alt Gr), INSERT, DELETE,\n"
       "# HOME, END, PAGEUP, PAGEDOWN, KP_0 to KP_9, KP_ENTER, KP_PLUS, KP_MINUS (numeric keypad),\n"
       "# COMMA, PERIOD, SLASH, SEMICOLON, APOSTROPHE, MINUS, EQUALS, GRAVE, NONUSBACKSLASH (< > key)...\n"
       "# Full list: SDL 3 SDL_SCANCODE_* constants (SDL_scancode.h), without the prefix.\n"
       "\n"
       "# GameCube port of the keyboard: 1 to 4, or 0 to not use the keyboard.\n"
       "port = 1\n"
       "\n";
  for (int c = 0; c < kControlCount; ++c) {
    line(t, kControlNames[c], join_names(kb.bindings[c]));
  }
  t += "\n"
       "# ------------------------------------------------------------------------------------------\n"
       "[gamepad]\n"
       "# Every gamepad recognized by SDL (Xbox, PlayStation, Switch, GameCube adapter...).\n"
       "# They are assigned to ports automatically: the first one on port 1, with the keyboard.\n"
       "# Buttons (SDL names, by position): SOUTH (bottom: Xbox A, PlayStation cross), EAST (right),\n"
       "# WEST (left), NORTH (top), START, BACK (Select, Share, -), GUIDE, LEFT_SHOULDER and\n"
       "# RIGHT_SHOULDER (LB, RB), LEFT_STICK and RIGHT_STICK (stick clicks), DPAD_UP, DPAD_DOWN,\n"
       "# DPAD_LEFT, DPAD_RIGHT, MISC1 to MISC6, LEFT_PADDLE1, LEFT_PADDLE2, RIGHT_PADDLE1,\n"
       "# RIGHT_PADDLE2, TOUCHPAD.\n"
       "# Axes: LEFT_STICK_UP, LEFT_STICK_DOWN, LEFT_STICK_LEFT, LEFT_STICK_RIGHT, RIGHT_STICK_UP,\n"
       "# RIGHT_STICK_DOWN, RIGHT_STICK_LEFT, RIGHT_STICK_RIGHT, LEFT_TRIGGER, RIGHT_TRIGGER (LT, RT).\n"
       "# An axis assigned to a GameCube button presses it beyond the threshold set below; a button\n"
       "# assigned to a stick or a trigger pushes it all the way.\n"
       "\n";
  for (int c = 0; c < kControlCount; ++c) {
    if (c == kB || c == kX || c == kZ || c == kL || c == kR) {
      continue; // depend on the gamepad type: see the commented block below
    }
    line(t, kControlNames[c], join_names(pad.bindings[c]));
  }
  t += "\n"
       "# B, X, Z, L and R depend on the gamepad type (Aurora values, unchanged):\n"
       "#   Xbox: B = WEST, X = EAST; others (PlayStation, Switch...): B = EAST, X = WEST;\n"
       "#   Z = RIGHT_SHOULDER; L = LEFT_TRIGGER, R = RIGHT_TRIGGER (triggers pressed beyond the\n"
       "#   threshold); GameCube adapter: B = WEST, X = EAST, L = MISC3, R = MISC4; Nintendo Switch\n"
       "#   Online GameCube controller: B = WEST, X = EAST, Z = BACK, L = LEFT_SHOULDER, R = RIGHT_SHOULDER.\n"
       "# To force a value for every gamepad, remove the \"#\" at the start of the line:\n"
       "# B              = EAST\n"
       "# X              = WEST\n"
       "# Z              = RIGHT_SHOULDER\n"
       "# L              = LEFT_TRIGGER\n"
       "# R              = RIGHT_TRIGGER\n"
       "\n"
       "# Dead zones, in % of the travel (0 to 100): below them, the axis counts as zero.\n";
  const Thresholds th;
  line(t, "stick_dead_zone", raw_to_percent(th.stickDeadZone), "= 8000/32767, Aurora value", 24);
  line(t, "cstick_dead_zone", raw_to_percent(th.cstickDeadZone), "= 8000/32767", 24);
  line(t, "trigger_dead_zone", raw_to_percent(th.triggerDeadZone), nullptr, 24);
  t += "# Thresholds, in % of the travel, beyond which an axis assigned to a GameCube button presses it.\n";
  line(t, "trigger_button_threshold", raw_to_percent(th.triggerButton), "= 31151/32767, Aurora value", 24);
  line(t, "stick_button_threshold", raw_to_percent(th.stickButton), nullptr, 24);
  t += "\n"
       "# ------------------------------------------------------------------------------------------\n"
       "# Sections per gamepad type (optional): they complete [gamepad] for that type only, with\n"
       "# the same names and settings. Types: [gamepad.xbox], [gamepad.playstation],\n"
       "# [gamepad.switch], [gamepad.gamecube] (adapter and GameCube controllers), [gamepad.other].\n"
       "# The type of each connected gamepad is written in the game trace.\n"
       "# Example: swap B and X on PlayStation gamepads only:\n"
       "# [gamepad.playstation]\n"
       "# B = WEST\n"
       "# X = EAST\n";
  return t;
}

// ------------------------------------------------------------------------------------------------
// Reading
// ------------------------------------------------------------------------------------------------

ParseResult parse(std::string_view text) {
  ParseResult r;
  r.config = default_config();
  auto problem = [&](int lineNo, std::string message) { r.problems.push_back({lineNo, std::move(message)}); };

  if (text.size() >= 2 && ((text[0] == '\xFF' && text[1] == '\xFE') || (text[0] == '\xFE' && text[1] == '\xFF'))) {
    problem(0, "file saved as UTF-16: save it again as UTF-8; default values for the whole file");
    return r;
  }
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") {
    text.remove_prefix(3); // UTF-8 byte order mark (Notepad)
  }

  enum class Kind { Top, Keyboard, Gamepad, Ignored };
  Kind kind = Kind::Top;
  Section* section = nullptr;
  std::string sectionName;
  std::map<std::string, int> seen; // "section|key" -> line of the first definition

  size_t pos = 0;
  int lineNo = 0;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view ln = text.substr(pos, end - pos);
    pos = end + 1;
    ++lineNo;
    if (ln.size() > 4096) {
      problem(lineNo, "line too long (more than 4096 characters): ignored");
      continue;
    }
    if (const size_t hash = ln.find_first_of("#;"); hash != std::string_view::npos) {
      ln = ln.substr(0, hash);
    }
    ln = trim(ln);
    if (ln.empty()) {
      continue;
    }

    if (ln.front() == '[') {
      if (ln.back() != ']') {
        problem(lineNo, "section header \"" + std::string(ln) +
                            "\" not closed (missing \"]\"): lines ignored until the next section");
        kind = Kind::Ignored;
        continue;
      }
      std::string name = normalize_name(trim(ln.substr(1, ln.size() - 2)));
      for (char& ch : name) {
        ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
      }
      sectionName = name;
      if (name == "keyboard") {
        kind = Kind::Keyboard;
        section = &r.config.keyboard;
      } else if (name == "gamepad") {
        kind = Kind::Gamepad;
        section = &r.config.gamepad;
      } else {
        kind = Kind::Ignored;
        section = nullptr;
        if (name.rfind("gamepad.", 0) == 0) {
          const std::string fam = name.substr(8);
          for (int f = 0; f < kFamilyCount; ++f) {
            if (fam == kFamilyNames[f]) {
              kind = Kind::Gamepad;
              section = &r.config.family[f];
            }
          }
        }
        if (kind == Kind::Ignored) {
          problem(lineNo, "unknown section [" + name +
                              "]: its lines are ignored (known sections: [keyboard], [gamepad], "
                              "[gamepad.xbox], [gamepad.playstation], [gamepad.switch], [gamepad.gamecube], "
                              "[gamepad.other])");
        }
      }
      continue;
    }

    const size_t eq = ln.find('=');
    if (eq == std::string_view::npos) {
      if (kind != Kind::Ignored) {
        problem(lineNo, "\"" + std::string(ln) + "\": missing \"=\" (line ignored)");
      }
      continue;
    }
    const std::string keyName = normalize_name(trim(ln.substr(0, eq)));
    const std::string_view value = trim(ln.substr(eq + 1));
    if (kind == Kind::Ignored) {
      continue;
    }
    if (keyName.empty()) {
      problem(lineNo, "missing name before \"=\" (line ignored)");
      continue;
    }
    if (kind == Kind::Top) {
      int version = 0;
      if (keyName != "VERSION") {
        problem(lineNo, "\"" + keyName + "\" outside any section (before [keyboard]): ignored");
      } else if (!parse_int(value, version) || version != 1) {
        problem(lineNo, "unknown version \"" + std::string(value) + "\": the file is read as version 1");
      }
      continue;
    }

    // Duplicates: the last line wins, with a message.
    const std::string seenKey = sectionName + "|" + keyName;
    if (const auto it = seen.find(seenKey); it != seen.end()) {
      problem(lineNo, "\"" + keyName + "\" already defined in [" + sectionName + "] line " +
                          std::to_string(it->second) + ": this line wins");
    } else {
      seen.emplace(seenKey, lineNo);
    }

    const int control = control_from_name(keyName);
    const int setting = setting_from_name(keyName);
    if (kind == Kind::Keyboard && keyName == "PORT") {
      int port = 0;
      if (!parse_int(value, port) || port < 0 || port > 4) {
        problem(lineNo, "invalid keyboard port \"" + std::string(value) +
                            "\" (1 to 4, or 0 to disable the keyboard): default value kept (1)");
      } else {
        r.config.keyboardPort = port - 1;
      }
      continue;
    }
    if (setting >= 0) {
      if (kind == Kind::Keyboard) {
        problem(lineNo, "\"" + keyName + "\": setting without effect on the keyboard (put it in [gamepad]): ignored");
        continue;
      }
      double pct = 0.0;
      if (!parse_percent(value, pct)) {
        problem(lineNo, "\"" + keyName + "\": invalid value \"" + std::string(value) +
                            "\" (percentage from 0 to 100): default value kept");
        continue;
      }
      section->setting[setting] = percent_to_raw(pct);
      section->settingPresent[setting] = true;
      continue;
    }
    if (control < 0) {
      std::string hint = kind == Kind::Gamepad && keyName == "PORT"
                             ? " (the gamepad port cannot be set: automatic assignment)"
                             : " (commands: A, B, X, Y, Z, L, R, START, DPAD_*, STICK_*, CSTICK_*, TRIGGER_L, "
                               "TRIGGER_R" +
                                   std::string(kind == Kind::Gamepad ? "; settings: *_dead_zone, *_button_threshold"
                                                                     : "; setting: port") +
                                   ")";
      problem(lineNo, "\"" + keyName + "\" unknown in [" + sectionName + "]" + hint + ": ignored");
      continue;
    }

    // List of physical inputs.
    Bindings list;
    std::string bad;
    const auto items = split_list(value);
    const bool none = items.empty() || (items.size() == 1 && normalize_name(items[0]) == "NONE");
    if (!none) {
      for (const auto item : items) {
        const std::string n = normalize_name(item);
        Physical p;
        bool ok = false;
        if (kind == Kind::Keyboard) {
          const int sc = scancode_from_name(n);
          ok = sc >= 0;
          p = key(sc);
          if (!ok && gamepad_from_name(n, p)) {
            bad = "\"" + std::string(item) + "\" is a gamepad name (put it in [gamepad])";
            break;
          }
        } else {
          ok = gamepad_from_name(n, p);
          if (!ok && scancode_from_name(n) >= 0) {
            bad = "\"" + std::string(item) + "\" is a key name (put it in [keyboard])";
            break;
          }
        }
        if (!ok) {
          bad = std::string(kind == Kind::Keyboard ? "unknown key" : "unknown button or axis") + " \"" +
                std::string(item) + "\"";
          break;
        }
        if (std::find(list.begin(), list.end(), p) == list.end()) {
          list.push_back(p);
        }
      }
      if (bad.empty() && list.size() > 16) {
        bad = "more than 16 inputs";
      }
    }
    if (!bad.empty()) {
      std::string def;
      if (kind == Kind::Keyboard) {
        def = " (" + join_names(keyboard_defaults().bindings[control]) + ")";
      }
      problem(lineNo, std::string(kControlNames[control]) + ": " + bad + ": default value kept" + def);
      continue;
    }
    section->bindings[control] = std::move(list);
    section->present[control] = true;
  }
  return r;
}

namespace {

// Reads an existing file (no creation): status, type and size checks, then the whole content.
bool read_existing(const fs::path& file, LoadResult& res, std::string& text) {
  std::error_code ec;
  const auto st = fs::status(file, ec);
  if (st.type() == fs::file_type::none || ec) {
    res.note = "unreadable file status (" + ec.message() + "): default values";
    return false;
  }
  if (!fs::is_regular_file(st)) {
    res.note = "not a file: default values";
    return false;
  }
  const uintmax_t size = fs::file_size(file, ec);
  if (ec || size > kMaxFileSize) {
    res.note = ec ? "unreadable size: default values" : "file too large (more than 1 MiB): default values";
    return false;
  }
  std::ifstream in(file, std::ios::binary);
  text.assign(static_cast<size_t>(size), '\0');
  if (!in || !in.read(text.data(), static_cast<std::streamsize>(text.size()))) {
    res.note = "cannot read: default values";
    return false;
  }
  return true;
}

} // namespace

LoadResult load_or_create(const fs::path& file) {
  LoadResult res;
  res.config = default_config();
  res.fileName = file.filename().string();
  try {
    std::error_code ec;
    const auto st = fs::status(file, ec);
    if (st.type() == fs::file_type::not_found) {
      const std::string text = default_file_text();
      fs::path tmp = file;
      tmp += ".tmp";
      if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
      }
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
          fs::remove(tmp, ec);
          res.note = "cannot write the default file: default values";
          return res;
        }
      }
      fs::rename(tmp, file, ec);
      if (ec) {
        fs::remove(tmp, ec);
        res.note = "cannot create the default file (" + ec.message() + "): default values";
        return res;
      }
      ParseResult parsed = parse(text);
      res.config = parsed.config;
      res.problems = std::move(parsed.problems); // always empty (checked by tests/input)
      res.status = LoadStatus::Created;
      return res;
    }
    std::string text;
    if (!read_existing(file, res, text)) {
      return res;
    }
    ParseResult parsed = parse(text);
    res.config = parsed.config;
    res.problems = std::move(parsed.problems);
    res.status = LoadStatus::Read;
  } catch (const std::exception& e) {
    res = LoadResult{};
    res.config = default_config();
    res.note = std::string("unexpected error (") + e.what() + "): default values";
  } catch (...) {
    res = LoadResult{};
    res.config = default_config();
    res.note = "unexpected error: default values";
  }
  return res;
}

LoadResult load_user_file(const fs::path& dir) { return load_or_create(dir / kFileName); }

} // namespace gcrt::input
