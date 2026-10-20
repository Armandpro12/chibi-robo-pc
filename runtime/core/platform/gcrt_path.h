/*
 * Paths given by the user (generic): UTF-8 everywhere inside the runtime, converted to UTF-16 at the Windows API.
 *
 * Why: on Windows, fopen(), getenv() and main()'s argv use the ANSI code page. A folder or a disc image whose name
 * has characters outside that code page (e.g. a Japanese name on a French Windows) cannot be opened that way. The
 * runtime keeps the paths of the command line, the disc, the manifest and the data folders in UTF-8 and opens them
 * with the wide-character functions (_wfopen, _wgetenv, GetCommandLineW, GetModuleFileNameW). The output files of the
 * developer and test variables (trace file, screenshot folder, WAV recording, audio capture, timing log, timeline, GX
 * stream log) go through these helpers too. Elsewhere (not compiled to date) the plain C functions are used: paths are
 * already bytes there.
 *
 * Also: the installed-copy conventions (default disc root next to the executable) and the writable-folder test used
 * to place the renderer caches (notes/decisions.md, 2026-10-19, installer mission 2a).
 */
#ifndef GCRT_PATH_H
#define GCRT_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fopen() of a UTF-8 path (_wfopen on Windows). NULL if the path is not valid UTF-8 or cannot be opened. */
FILE* gcrt_fopen_utf8(const char* path, const char* mode);

#if defined(_WIN32)
/* UTF-8 -> UTF-16 copy (MultiByteToWideChar, CP_UTF8), to be released with free(). NULL if `s` is NULL or not valid
 * UTF-8. For the Windows calls that take text rather than a path (e.g. MessageBoxW, platform/gcrt_fatal.h). */
wchar_t* gcrt_utf8_to_wide(const char* s);
#endif

/* Environment variable as UTF-8 (_wgetenv on Windows). False if it is unset, empty, or does not fit in `size`. */
bool gcrt_getenv_utf8(const char* name, char* out, size_t size);

/* Windows: replaces *argv with UTF-8 copies of the command-line arguments (GetCommandLineW + CommandLineToArgvW),
 * kept for the whole process, when the argument counts agree; otherwise argv is left as is. Elsewhere: no change. */
void gcrt_utf8_args(int* argc, char*** argv);

/* Folder of the running executable in UTF-8, without a trailing separator. False if unknown. */
bool gcrt_exe_dir_utf8(char* out, size_t size);

/* `path` as shown to a player: on Windows the full path with ".." resolved and backslashes (GetFullPathNameW; the
 * file need not exist); elsewhere, or if that fails, a copy. Always writes a NUL-terminated string (truncated). */
void gcrt_path_display(const char* path, char* out, size_t size);

/* True if `path` is absolute ("C:\...", "\\server\...", "/..."). */
bool gcrt_path_is_absolute(const char* path);

/* True if a file can be created in `dir` (UTF-8), which is created first if needed: a temporary file is created and
 * deleted. Used to choose where the caches go when the executable's folder is read-only (e.g. Program Files). */
bool gcrt_dir_writable(const char* dir);

/* Disc root used when no --disc option is given: "<executable folder>/disc" if "<executable folder>/disc/sys/main.dol"
 * exists (an installed copy), otherwise `compiled_in` (the extracted disc of the build machine). Writes the root to
 * `out` and returns a short reason for the trace ("next to the executable" or "build default"). */
const char* gcrt_default_disc_root(const char* compiled_in, char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_PATH_H */
