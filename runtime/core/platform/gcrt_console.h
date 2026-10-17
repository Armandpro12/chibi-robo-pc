#pragma once
/* Console window of the executable (Windows; elsewhere every function here does nothing).
 *
 * The executable is linked as a Windows-subsystem program (WIN32_EXECUTABLE, cmake/GcrtRecomp.cmake): Windows does not
 * create a console before main(). The runtime opens one, or not, at a single point: when the game starts (after the
 * setup menu, gcrt_game_config.h). Until then the standard output and error go to an anonymous pipe whose content is
 * kept in memory (bounded), then written to the console if it opens; otherwise they are dropped and both streams are
 * pointed at the NUL device. Crash reports and trace files are files of their own and do not depend on this.
 *
 * When the parent redirected the standard output or error (pipe, file, NUL device: launchers, test benches), nothing is
 * ever opened and no handle is touched: those runs behave exactly as before. A process that already has a console at
 * startup is left alone as well. Notes: notes/runtime.md (console window). */
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GcrtConsoleChoice {
    GCRT_CONSOLE_NONE = 0,   /* no console: the early output is dropped */
    GCRT_CONSOLE_OPEN = 1,   /* the console of the parent process (launch from a terminal), otherwise a new one */
    GCRT_CONSOLE_PARENT = 2, /* the console of the parent process if it has one, otherwise none */
} GcrtConsoleChoice;

/* First call of the runtime's entry point, before anything is printed: decides whether the standard streams are left
 * to the parent (redirected, or a console already there) or kept in memory until gcrt_console_start. */
void gcrt_console_early_init(void);

/* Command-line request for this launch: -1 none (default), 0 --no-console, 1 --console. */
void gcrt_console_set_request(int request);
int gcrt_console_request(void);

/* Opens the console or not, once (later calls do nothing), and writes the early output to it. Does nothing when the
 * streams are left to the parent. If the process exits before any call, GCRT_CONSOLE_PARENT is applied at exit (unless
 * --no-console was given): --help or a boot error started from a terminal still prints there. */
void gcrt_console_start(GcrtConsoleChoice choice);

#ifdef __cplusplus
}
#endif
