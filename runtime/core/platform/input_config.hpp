// Configurable keyboard / SDL gamepad -> GameCube controller mappings (generic GameCube).
//
// "Pure" part: model, default values, reading and writing of the text file (controls.ini),
// translation of an input state (SDL keys, buttons and axes) into a GameCube controller state. No call to
// SDL here (only its constants): this module is tested without a window or a device
// (tests/input/). Device sampling and the user folder are in input_host.cpp.
//
// File format, default values and choices: notes/runtime.md, section 8.3; commented default text:
// default_file_text().
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gcrt::input {

// GameCube controller controls: 12 buttons (PAD_BUTTON_* bits of the SDK) then 10 half-axes.
enum Control : int {
  kA, kB, kX, kY, kZ, kL, kR, kStart, kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kStickUp, kStickDown, kStickLeft, kStickRight,
  kCStickUp, kCStickDown, kCStickLeft, kCStickRight,
  kTriggerL, kTriggerR,
  kControlCount
};
constexpr int kButtonControls = 12; // kA .. kDpadRight

// Number of buttons and axes of the SDL gamepads (SDL_GAMEPAD_BUTTON_COUNT, SDL_GAMEPAD_AXIS_COUNT of SDL 3.4;
// checked by static_assert in input_config.cpp).
constexpr int kGamepadButtons = 26;
constexpr int kGamepadAxes = 6;

// One physical input: key (SDL scancode = physical position), button or half-axis of an SDL gamepad.
struct Physical {
  enum class Kind : uint8_t { Key, Button, Axis };
  Kind kind = Kind::Key;
  int16_t code = 0; // SDL_Scancode, SDL_GamepadButton or SDL_GamepadAxis
  int8_t sign = 1;  // half-axis: +1 (positive SDL direction: right, down, trigger pressed) or -1
  bool operator==(const Physical&) const = default;
};
using Bindings = std::vector<Physical>;

// Settings of a gamepad family (SDL units: 0 to 32767). Default values = those of Aurora
// (lib/gamepad.hpp: dead zones 8000, trigger activation 31150 -> threshold (31150 + 1)).
struct Thresholds {
  int stickDeadZone = 8000;    // main stick: |value| <= zone -> 0
  int cstickDeadZone = 8000;   // C-stick
  int triggerDeadZone = 0;     // analog triggers (Aurora: none)
  int triggerButton = 31151;   // SDL trigger assigned to a GameCube button: pressed if value >= threshold
  int stickButton = 16384;     // stick half-axis assigned to a GameCube button (Aurora threshold 0.5)
  bool operator==(const Thresholds&) const = default;
};

// Settings of a file section (indices of Section::setting).
enum Setting : int { kStickDeadZone, kCStickDeadZone, kTriggerDeadZone, kTriggerButton, kStickButton, kSettingCount };

// One section of the file: only the entries present (and valid) replace the inherited values.
struct Section {
  std::array<Bindings, kControlCount> bindings{};
  std::array<bool, kControlCount> present{};
  std::array<int, kSettingCount> setting{};
  std::array<bool, kSettingCount> settingPresent{};
};

// SDL gamepad families, for the [gamepad.<family>] sections (complete [gamepad]).
enum class Family : int { Xbox, PlayStation, Switch, GameCube, Other, Count };
constexpr int kFamilyCount = static_cast<int>(Family::Count);

struct Config {
  int keyboardPort = 0; // 0-3; -1: keyboard disabled
  Section keyboard;     // [keyboard]
  Section gamepad;      // [gamepad]
  std::array<Section, kFamilyCount> family{};
};

// Effective mappings of a device (default values + file sections).
struct Resolved {
  std::array<Bindings, kControlCount> bindings{};
  Thresholds thresholds{};
  bool operator==(const Resolved&) const = default;
};

// Identity of an SDL gamepad: type (SDL_GamepadType), USB vendor and product.
struct GamepadIdentity {
  int sdlType = 0;
  uint16_t vendor = 0;
  uint16_t product = 0;
};

// Sampled input states (supplied by input_host.cpp or by a test).
struct KeyboardState {
  const bool* keys = nullptr; // indexed by SDL_Scancode (SDL_GetKeyboardState)
  int count = 0;
};
struct GamepadState {
  std::array<bool, kGamepadButtons> buttons{};
  std::array<int16_t, kGamepadAxes> axes{}; // raw values of SDL_GetGamepadAxis
};

// Produced GameCube state (PADStatus fields; analog A/B always 0, analog mode 3).
struct PadOutput {
  uint16_t buttons = 0;
  int8_t stickX = 0, stickY = 0, substickX = 0, substickY = 0;
  uint8_t triggerL = 0, triggerR = 0;
  bool operator==(const PadOutput&) const = default;
};

// A problem found in the file (line = 0: the whole file).
struct Diagnostic {
  int line = 0;
  std::string message;
};

struct ParseResult {
  Config config;
  std::vector<Diagnostic> problems;
};

enum class LoadStatus {
  Read,         // file read (possibly with problems: see problems)
  Created,      // file absent: created with the default text
  DefaultsOnly, // file absent or unreadable and impossible to create, or too big: default values
};

struct LoadResult {
  Config config;
  LoadStatus status = LoadStatus::DefaultsOnly;
  std::vector<Diagnostic> problems;
  std::string note;     // explanation (I/O error...); empty if there is nothing to say
  std::string fileName; // name of the file read or created (kFileName)
};

// File name in the user folder.
constexpr const char* kFileName = "controls.ini";
// Maximum size read (beyond: file ignored, default values).
constexpr uintmax_t kMaxFileSize = 1u << 20;

// Default configuration (identical to the one obtained by reading default_file_text()).
Config default_config();
// Default text of the file, with comments.
std::string default_file_text();
// Reads a configuration text. Throws no exception for invalid content: the faulty entries
// keep their default value and are described in `problems`.
ParseResult parse(std::string_view text);
// Reads `file`; if it does not exist, creates it with default_file_text() (write to a temporary file
// then rename). Never an exception, never a rewrite of an existing file.
LoadResult load_or_create(const std::filesystem::path& file);
// Controls file of the user folder `dir`: load_or_create(dir / kFileName).
LoadResult load_user_file(const std::filesystem::path& dir);

Family family_of(const GamepadIdentity& id);
const char* family_name(Family f); // "xbox", "playstation"...
Resolved keyboard_defaults();
Resolved gamepad_defaults(const GamepadIdentity& id);
Resolved resolve_keyboard(const Config& config);
Resolved resolve_gamepad(const Config& config, const GamepadIdentity& id);

// Translates the input states of a port (keyboard and/or gamepad, each optional) into a GameCube state,
// with the Aurora semantics (lib/dolphin/pad/adapter.cpp, lib/input/binding.cpp): buttons = at least one
// active input; axes = sum of the contributions clamped to [-1, 1]; stick = lround(v * 127);
// trigger = lround(v * 255); L or R pressed -> trigger at 180.
PadOutput translate(const Resolved* keyboard, const KeyboardState* keys, const Resolved* gamepad,
                    const GamepadState* pad);

// Names (those of the file).
const char* control_name(int control);
std::string physical_name(const Physical& p);
// Readable summary of the mappings (one line).
std::string describe(const Resolved& r, bool withThresholds);
// Key name -> scancode (-1 if unknown). Also used by GCRT_INPUT_KEYS.
int scancode_from_name(std::string_view name);

} // namespace gcrt::input
