// PC inputs -> GameCube controllers: sampling of the keyboard and of the SDL gamepads (opened and
// assigned to ports by Aurora), translation by the mappings of controls.ini (input_config.hpp).
//
// Traces: "platform" (on by default): file read or created, problems in the file, effective mappings,
// connected gamepads; "pad": each change of the translated state of a port.
// GCRT_INPUT_KEYS (diagnostic, tests): simulated physical keys, "READ:KEY+KEY[:COUNT]"
// separated by ";": keys (names from the file) held during COUNT reads (default 10) starting at
// read no. READ (1 = first read of the controllers by the game), before translation.
#pragma once

#include "input_config.hpp"

namespace gcrt::platform {

// Loads controls.ini from the user folder (creates it if missing); to be called once after
// Aurora (SDL) initialization. Never fails: default values if there is a problem.
void input_init();
// Reads the 4 ports: keyboard on its port, SDL gamepad assigned by Aurora on each port.
void input_read_pads(input::PadOutput out[4], bool connected[4]);

} // namespace gcrt::platform
