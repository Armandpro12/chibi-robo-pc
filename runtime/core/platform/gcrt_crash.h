/*
 * Crash handler of the runtime (generic GameCube, diagnostic).
 *
 * On a fatal host exception (access violation for a read or a write, illegal instruction, stack overflow...) or on
 * abort(), writes into the crash folder:
 *   crash-<date>-<pid>.txt           exception code and name, faulting address (module + offset), accessed address,
 *                                    thread (name, game thread or not), host registers and call stack (module + offset,
 *                                    function if a PDB is present), guest state (pc, lr, registers, call stack read from
 *                                    the guest stack frames, last blocks, function names known to the runtime), process
 *                                    memory (working set, private bytes, handles), lines added by the platform;
 *   crash-<date>-<pid>.dmp           minidump (MiniDumpWriteDump: threads, indirectly referenced memory, handles);
 *   crash-<date>-<pid>-guest-ram.bin guest main memory (MEM1 and the area of the REL modules), raw, big-endian.
 * The same summary goes to stderr. The exception is then passed on (process exit code = exception code).
 *
 * Folder: GCRT_CRASH_DIR, otherwise GCRT_CAPTURE_DIR (automated runs: run folder), otherwise the default given by the
 * platform (<user folder>/crashes/), otherwise the system temporary folder (%TEMP%\gcrt-crashes), never the current
 * folder. Only the 5 most recent reports are kept (older ones are deleted at startup when the folder is chosen).
 * The guest memory file holds game data and the minidump holds local paths: they stay on the user's machine and must
 * not be attached to public issues. GCRT_CRASH_HANDLER=0 disables the handler
 * (to let a debugger or Windows Error Reporting see the exception first). Windows only; elsewhere the functions do
 * nothing.
 */
#ifndef GCRT_CRASH_H
#define GCRT_CRASH_H

#include "cpu/gcrt_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the handler (once, from the game thread, before the game runs). `g`: guest state to report. */
void gcrt_crash_handler_install(GcrtCpu* g);
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
