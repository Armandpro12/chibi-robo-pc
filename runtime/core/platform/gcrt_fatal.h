/*
 * Start-up errors a player can fix (generic): bad installation, missing or wrong disc image, game files that do not
 * match the executable. Instead of a black window or a silent exit, the message is shown in a native message box on a
 * player's launch, and always written to stderr and to the trace; the process then exits with code 1 (no crash report:
 * these errors are expected, and they are reported before the crash handler is installed and before any window).
 *
 * Player launch: the same rule as the setup menu (platform/gcrt_platform.h, GCRT_SETUP_MENU=auto): none of the
 * variables a launcher sets (gcrt_launched_by_tool), and in addition neither GCRT_NO_FOCUS nor GCRT_SETUP_MENU_AUTO.
 * An automated run never gets a dialog: nobody would be there to close it.
 *
 * GCRT_FATAL_DIALOG overrides the choice:
 *   auto (default)  dialog on a player launch only;
 *   log             never a window: writes to stderr what the dialog would show (title, text, UTF-16 length), then
 *                   exits with code 1. For tests of the dialog path;
 *   off | 0         never a dialog;
 *   show | 1        always the dialog, even under automation: for a human, or for a test that runs the program on a
 *                   hidden desktop (tools/winbg.py) and closes the box itself. Never for an ordinary automated run.
 * Windows: MessageBoxW (Microsoft Learn, "MessageBoxW function (winuser.h)"), text converted from UTF-8 with
 * gcrt_utf8_to_wide (platform/gcrt_path.h). Elsewhere: stderr only.
 */
#ifndef GCRT_FATAL_H
#define GCRT_FATAL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when a launcher variable is set (GCRT_WINDOW, GCRT_USER_DIR, GCRT_INPUT, GCRT_INPUT_FRAMES, GCRT_STOP_AT,
 * GCRT_STOP_FRAME, GCRT_CAPTURE_DIR, GCRT_RTC): tools/run.py and the test benches set at least one. Shared by the
 * setup menu (platform.cpp) and the fatal-error dialog. */
bool gcrt_launched_by_tool(void);

/* True when a fatal error would be shown in a dialog (GCRT_FATAL_DIALOG and the player-launch rule above). */
bool gcrt_fatal_dialog_wanted(void);

/* Reports a start-up error and ends the process with exit code 1. `title`: short (window title); `text`: UTF-8, may
 * hold newlines (the dialog shows them; stderr and the trace get one line). Never returns. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#elif defined(_MSC_VER)
__declspec(noreturn)
#endif
void gcrt_fatal_error(const char* title, const char* text);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_FATAL_H */
