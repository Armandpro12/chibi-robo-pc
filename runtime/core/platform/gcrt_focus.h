#pragma once
/* "No focus" test mode: GCRT_NO_FOCUS=1 (platform/gcrt_platform.h). Automated launches on a computer someone is using:
 * no window opened by the runtime ever takes the foreground or comes to the front.
 *   - game window: shown without activation (SDL hints SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN / _RAISED set to "0"), or
 *     minimized with GCRT_WINDOW=minimized (without activation, off the screen); as soon as it is shown, moved to the
 *     bottom of the Z order (it never covers the foreground window) and made not focusable (WS_EX_NOACTIVATE: a
 *     click, Alt+Tab or the closing of the foreground window does not activate it, and it has no taskbar button;
 *     aurora_host.cpp, keep_window_behind); the setup menu's fullscreen setting is not applied;
 *   - console window (gcrt_console.h): created minimized without activation, and the foreground is never given back
 *     to the game window (nothing took it);
 *   - setup menu: holding Shift (read from the physical keyboard) no longer shows a skipped menu.
 * Any value other than empty or "0" turns it on. Header only: read where it is needed, no state. */
#include <stdbool.h>
#include <stdlib.h>

static inline bool gcrt_no_focus(void) {
    const char* v = getenv("GCRT_NO_FOCUS");
    return v != NULL && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}
