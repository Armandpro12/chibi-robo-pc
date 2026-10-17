#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcrtGameConfig {
    int   windowWidth;
    int   windowHeight;
    bool  fullscreen;
    float renderScale;
    bool  skipMenu;
    bool  console; /* console window when the game starts (gcrt_console.h), Windows; --console / --no-console decide
                      its value for this launch, the file keeps the player's own choice */
} GcrtGameConfig;

/* Read-only access to the current values. Valid at any time; holds defaults
 * until the menu has run or the saved file has been loaded. */
const GcrtGameConfig* gcrt_game_config(void);

/* Loads game_config.ini from the user folder (gcrt_user_dir.h) into the config. A missing file or an unavailable
 * folder keeps the defaults; a size outside the menu's list or a render scale outside 0.5-4 falls back to the
 * default value. A --console / --no-console request (gcrt_console.h) then sets `console` for this launch only. */
void gcrt_game_config_load(void);

/* Shows the ImGui setup menu (blocks until "Start game"), then saves the settings. Skipped when skipMenu is set,
 * unless Shift is held. Returns false if the window was closed. Must be called after aurora_initialize. The saved
 * `console` value changes only if the player toggled the box (a command-line request is not saved). */
bool gcrt_game_config_run_menu(const char* title);

#ifdef __cplusplus
}
#endif
