/* Start-up errors shown to the player (generic). See gcrt_fatal.h. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "platform/gcrt_fatal.h"
#include "platform/gcrt_focus.h"
#include "platform/gcrt_path.h"
#include "platform/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static bool env_set(const char* name) {
    const char* v = getenv(name);
    return v != NULL && v[0] != '\0';
}

bool gcrt_launched_by_tool(void) {
    static const char* const k_vars[] = {"GCRT_WINDOW",      "GCRT_USER_DIR", "GCRT_INPUT",       "GCRT_INPUT_FRAMES",
                                         "GCRT_STOP_AT",     "GCRT_STOP_FRAME", "GCRT_CAPTURE_DIR", "GCRT_RTC"};
    for (size_t i = 0; i < sizeof(k_vars) / sizeof(k_vars[0]); i++) {
        /* Set at all, even empty: the setup menu's rule before this function existed (std::getenv != nullptr). */
        if (getenv(k_vars[i]) != NULL)
            return true;
    }
    return false;
}

typedef enum { MODE_AUTO, MODE_LOG, MODE_OFF, MODE_SHOW } DialogMode;

static DialogMode dialog_mode(void) {
    const char* v = getenv("GCRT_FATAL_DIALOG");
    if (!v || !v[0] || !strcmp(v, "auto"))
        return MODE_AUTO;
    if (!strcmp(v, "log"))
        return MODE_LOG;
    if (!strcmp(v, "off") || !strcmp(v, "0"))
        return MODE_OFF;
    if (!strcmp(v, "show") || !strcmp(v, "1"))
        return MODE_SHOW;
    fprintf(stderr, "gcrt: GCRT_FATAL_DIALOG=%s ignored (auto, log, off or show)\n", v);
    return MODE_AUTO;
}

static bool player_launch(void) {
    return !gcrt_launched_by_tool() && !gcrt_no_focus() && !env_set("GCRT_SETUP_MENU_AUTO");
}

bool gcrt_fatal_dialog_wanted(void) {
    DialogMode m = dialog_mode();
    return m == MODE_SHOW || (m == MODE_AUTO && player_launch());
}

/* `text` on one line: each run of newlines becomes a single space. */
static void one_line(const char* text, char* out, size_t size) {
    size_t used = 0;
    bool gap = false;
    for (const char* p = text; *p && used + 2 < size; p++) {
        if (*p == '\n' || *p == '\r') {
            gap = true;
            continue;
        }
        if (gap && used > 0)
            out[used++] = ' ';
        gap = false;
        out[used++] = *p;
    }
    out[used] = '\0';
}

#if defined(_WIN32)
static void show_dialog(const char* title, const char* text) {
    wchar_t* wtitle = gcrt_utf8_to_wide(title);
    wchar_t* wtext = gcrt_utf8_to_wide(text);
    /* No owner window (none exists yet); MB_SETFOREGROUND: the box comes to the front on a player's launch, which has
     * nothing else on screen. Returns when the player presses OK or closes the box. */
    const UINT flags = MB_OK | MB_ICONERROR | MB_SETFOREGROUND;
    if (wtitle && wtext)
        MessageBoxW(NULL, wtext, wtitle, flags);
    else
        MessageBoxA(NULL, text, title, flags); /* not valid UTF-8 (not expected): bytes in the ANSI code page */
    free(wtitle);
    free(wtext);
}
#endif

void gcrt_fatal_error(const char* title, const char* text) {
    if (!title)
        title = "Error";
    if (!text)
        text = "";
    char line[4096];
    one_line(text, line, sizeof(line));
    fprintf(stderr, "gcrt: fatal error: %s: %s\n", title, line);
    /* The trace too, when it goes elsewhere than stderr (GCRT_TRACE_FILE): the line above would not be in it. */
    if (gcrt_trace_stream() != stderr)
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "fatal error: %s: %s", title, line);

    const DialogMode mode = dialog_mode();
    const bool player = player_launch();
    const bool dialog = mode == MODE_SHOW || (mode == MODE_AUTO && player);
    if (mode == MODE_LOG) {
        /* What the dialog would show, without a window (tests): the decision of the automatic mode, the title, the
         * text (newlines as \n) and its length once converted to UTF-16 (the conversion the dialog uses). */
        long units = -1;
#if defined(_WIN32)
        wchar_t* w = gcrt_utf8_to_wide(text);
        if (w) {
            units = (long)wcslen(w);
            free(w);
        }
#endif
        fprintf(stderr, "gcrt: fatal-error dialog (GCRT_FATAL_DIALOG=log, not shown; automatic mode would %s it): "
                        "title \"%s\", %ld UTF-16 units, text \"",
                player ? "show" : "not show", title, units);
        for (const char* p = text; *p; p++) {
            if (*p == '\n')
                fputs("\\n", stderr);
            else
                fputc(*p, stderr);
        }
        fputs("\"\n", stderr);
    }
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "fatal error: dialog %s (GCRT_FATAL_DIALOG %s, %s launch); exit code 1",
                      dialog ? "shown" : "not shown",
                      mode == MODE_AUTO ? "auto" : mode == MODE_LOG ? "log" : mode == MODE_OFF ? "off" : "show",
                      player ? "player" : "automated");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32)
    if (dialog)
        show_dialog(title, text);
#endif
    exit(1);
}
