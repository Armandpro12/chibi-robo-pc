/*
 * Crash handler and hang watchdog of the runtime (generic GameCube, diagnostic).
 *
 * Causes covered: unhandled host exception in any thread (access violation, illegal instruction, stack overflow...,
 * game thread, renderer, audio or any other thread), abort(), std::terminate (game thread; elsewhere it ends in
 * abort()), pure virtual call, invalid parameter given to a C runtime function. Not covered: fast-fail exits
 * (__fastfail, /GS failures, heap corruption detected by Windows), which bypass every in-process handler.
 *
 * Files written into the crash folder, never overwriting (a -2, -3... suffix if needed):
 *   crash-<date>-<pid>.txt           text report, written first and section by section (see notes below);
 *   crash-<date>-<pid>.dmp           minidump (MiniDumpWriteDump: threads and thread information, memory region list,
 *                                    indirectly referenced memory, unloaded modules, handles; comment stream = the
 *                                    whole native event ring);
 *   crash-<date>-<pid>-guest-ram.bin guest main memory (MEM1 and the area of the REL modules), raw, big-endian;
 *   hang-<date>-<pid>.txt / .dmp     hang report of the watchdog (no guest memory); the game keeps running;
 *   crash-signatures.txt             one line per crash (signature, date): recurrence detection.
 * Text report: cause, exception, faulting address (module + offset), recurrence ("same signature as the crash of
 * <date>"), executable (name, SHA-1, PE time stamp, build commit), processor, microcode revision and firmware (read
 * from the registry), Windows version, graphics adapter and driver (as reported by the renderer), process memory
 * (working set, private bytes and their peaks, handles), address space by region type and the largest committed
 * private regions, registers, code bytes around rip, host call stack (raw unwind, then dbghelp names when a PDB is
 * present), guest state (pc, lr, registers, guest call stack, last blocks: GCRT_GUEST_HISTORY), every thread with
 * its call stack at the time of the report (threads suspended while it is captured), the last native events
 * (platform/gcrt_diag.h, GCRT_DIAG_EVENTS), the last log lines, command line, GCRT_* variables, platform lines
 * (frames, settings), loaded modules. The same text goes to stderr. The exception is then passed on (process exit
 * code = exception code).
 *
 * Folder: GCRT_CRASH_DIR, otherwise GCRT_CAPTURE_DIR (automated runs: run folder), otherwise the default given by the
 * platform (<user folder>/crashes/), otherwise the system temporary folder (%TEMP%\gcrt-crashes), never the current
 * folder; if the folder cannot be written, the temporary folder, then stderr only. The 5 most recent reports of each
 * kind are kept (older ones are deleted at startup when the folder is chosen).
 * Only the .txt may be shared: it names the executable and the host modules by their file name only, and any
 * user-profile folder (or the user name as a path component) left in it is replaced by %USERPROFILE% / %USERNAME%.
 * The guest memory file holds game data and the minidump holds local paths: they stay on the user's machine and must
 * not be attached to public issues.
 *
 * Variables: GCRT_CRASH_HANDLER=0 disables the handler and the watchdog (to let a debugger or Windows Error Reporting
 * see the exception first); GCRT_WATCHDOG=<seconds> without progress of the game loop before a hang report (default
 * 60, 0 = off; one report per hang, at most 3 per session; never when a debugger is attached); before the game loop
 * starts, the start-up (window creation, setup menu, game boot up to its first frame, gcrt_diag.h) is watched the
 * same way, with GCRT_WATCHDOG_STARTUP=<seconds> (default: GCRT_WATCHDOG, at least 60; 0 = off; GCRT_WATCHDOG=0 turns
 * both off);
 * GCRT_CRASH_SUSPEND_LIMIT=<seconds> the other threads may stay suspended while a report copies their state (default
 * 5, at least 1): past it, a guard thread resumes them and the report says it was interrupted.
 * A start-up hang report is text only: an in-process minidump suspends every other thread for the whole dump and can
 * block for ever on a lock one of them holds, so it would turn a slow start-up into a freeze. A game-loop hang report
 * writes a minidump last, only if the game loop is still stopped then; that step is not bounded (same risk).
 * Windows only; elsewhere the functions do nothing.
 */
#ifndef GCRT_CRASH_H
#define GCRT_CRASH_H

#include "cpu/gcrt_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the handler (once, from the game thread, before the game runs). `g`: guest state to report. */
void gcrt_crash_handler_install(GcrtCpu* g);
/* Installs our unhandled exception filter again if a library initialized later replaced it (noted in reports). */
void gcrt_crash_handler_rearm(void);
/* Graphics adapter and driver as reported by the renderer (free text, newlines allowed), printed in reports. */
void gcrt_crash_handler_set_gpu_info(const char* text);
/* Default crash folder (UTF-8), used when neither GCRT_CRASH_DIR nor GCRT_CAPTURE_DIR is set. */
void gcrt_crash_handler_set_default_dir(const char* dir_utf8);
/* Name of the guest function containing `address` (game layer: symbol map), NULL if unknown; `start` receives its
   first address. Optional: without it, only the SDK functions recognized by signature are named. */
typedef const char* (*GcrtGuestSymbolizer)(u32 address, u32* start);
void gcrt_crash_handler_set_guest_symbolizer(GcrtGuestSymbolizer fn);
/* Extra lines written into the report by the platform (frames presented, guest time...): `write` appends text with
   the provided function. Called from the reporting thread while the game thread is stopped. */
typedef void (*GcrtCrashAppend)(void* sink, const char* text);
typedef void (*GcrtCrashContextFn)(void* user, GcrtCrashAppend append, void* sink);
void gcrt_crash_handler_set_context(GcrtCrashContextFn fn, void* user);
/* Process memory (working set, private bytes in bytes, handles); false if unavailable. */
bool gcrt_process_memory(u64* working_set, u64* private_bytes, u32* handles);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_CRASH_H */
