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
    int   backend; /* graphics backend saved in the file (an AuroraBackend value: BACKEND_AUTO, BACKEND_D3D12 or
                      BACKEND_VULKAN); read before the window opens, so a change in the menu applies
                      at the next launch. --backend / GCRT_BACKEND override it for one launch and are not saved */
} GcrtGameConfig;

/* Graphics backend names, as written in game_config.ini, given to --backend and to GCRT_BACKEND: "auto", "d3d12",
 * "vulkan" (case is ignored). Returns the AuroraBackend value, or -1 for any other name. Direct3D 11 is not offered: it
 * is only Aurora's last resort and is less tested (its US picture was wrong until Aurora patch 0008, issue 38; since
 * then the A/B title-boot captures are identical on both versions, 2026-10-20). */
int gcrt_backend_from_name(const char* name);
/* Same, for --backend and GCRT_BACKEND: with the test switch GCRT_TEST_ALL_BACKENDS=1 (off by default, for tests
 * only), the names that are not offered ("d3d11", "null"...) are accepted too, and gcrt_backend_available() accepts
 * their values. The menu and game_config.ini never offer them. */
int gcrt_backend_from_test_name(const char* name);
/* Short name ("auto", "d3d12"...) and label for people ("Auto", "Direct3D 12"...) of an AuroraBackend value; "?" for
 * a value without a name here. */
const char* gcrt_backend_name(int backend);
const char* gcrt_backend_label(int backend);
/* true if the backend is one of the names above and this build of Aurora can try it (aurora_get_available_backends:
 * the backends compiled in, not a test of the computer). BACKEND_AUTO is always available. */
bool gcrt_backend_available(int backend);

/* --backend NAME (boot.c): request for this launch only, checked when the window opens (unknown or unavailable: auto,
 * with a message). NULL = no request (default). */
void gcrt_backend_set_request(const char* name);
const char* gcrt_backend_request(void);

/* Backend in use, set by the host once the window is open (BACKEND_AUTO before): shown by the setup menu and in the
 * hang and crash reports. */
void gcrt_backend_set_active(int backend);
int gcrt_backend_active(void);

/* Read-only access to the current values. Valid at any time; holds defaults
 * until the menu has run or the saved file has been loaded. */
const GcrtGameConfig* gcrt_game_config(void);

/* Loads game_config.ini from the user folder (gcrt_user_dir.h) into the config. A missing file or an unavailable
 * folder keeps the defaults; a size outside the menu's list or a render scale outside 0.5-4 falls back to the
 * default value; an unknown or unavailable backend falls back to auto (trace line and message on stderr). A
 * --console / --no-console request (gcrt_console.h) then sets `console` for this launch only. Called before
 * aurora_initialize (the backend is chosen there). */
void gcrt_game_config_load(void);

/* Shows the ImGui setup menu (blocks until "Start game"), then saves the settings. Skipped when skipMenu is set,
 * unless Shift is held. Returns false if the window was closed. Must be called after aurora_initialize. The saved
 * `console` value changes only if the player toggled the box (a command-line request is not saved). */
bool gcrt_game_config_run_menu(const char* title);

#ifdef __cplusplus
}
#endif
