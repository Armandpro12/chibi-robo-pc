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
} GcrtGameConfig;

/* Read-only access to the current values. Valid at any time; holds defaults
 * until the menu has run or the saved file has been loaded. */
const GcrtGameConfig* gcrt_game_config(void);

/* Loads game_config.ini into the config (missing file = defaults). */
void gcrt_game_config_load(void);

/* Shows the ImGui setup menu (blocks until "Start game"). Returns false if the
 * window was closed. Must be called after aurora_initialize. */
bool gcrt_game_config_run_menu(const char* title);

#ifdef __cplusplus
}
#endif