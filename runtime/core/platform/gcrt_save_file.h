/*
 * Save file layer (milestone 7): PC file operations used by the native memory card
 * (hle/card_model.c). Generic: it knows neither the game nor the file format.
 *
 * Safe write (gcrt_sf_write_atomic): never an in-place write. The full contents are written to a temporary file
 * in the same folder ("<name>.tmp"), flushed to disk (FlushFileBuffers / fsync), then renamed
 * over the old one in a single operation (Windows: MoveFileExW with MOVEFILE_REPLACE_EXISTING |
 * MOVEFILE_WRITE_THROUGH; elsewhere: rename). A hard kill of the process before the rename leaves the old
 * file intact and an orphan ".tmp", deleted at the next mount (gcrt_sf_clean_orphans). Sources and choices:
 * notes/decisions.md (2026-10-11, runtime agent, milestone 7).
 *
 * Paths in UTF-8 (converted to UTF-16 on Windows: a user name with accented letters works).
 *
 * Diagnostics reserved for tests (GCRT_SAVE_FAULT): hard kill of the process during the N-th safe write,
 * "mid-write:N" (half of the temporary file written) or "before-rename:N" (temporary file complete and flushed
 * to disk, rename not done yet). Used to prove that a forced kill leaves the previous save intact.
 */
#ifndef GCRT_SAVE_FILE_H
#define GCRT_SAVE_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Suffix of the temporary files of gcrt_sf_write_atomic. */
#define GCRT_SF_TMP_SUFFIX ".tmp"

/* Creates the folder and its parents (no error if it already exists). */
bool gcrt_sf_mkdirs(const char* dir);
/* Does the path designate a regular file? */
bool gcrt_sf_is_file(const char* path);
/* Reads the whole file (at most `max_size` bytes, otherwise failure) into a buffer allocated by malloc (to be freed
 * with free). Returns false if the file is missing, too large or unreadable. */
bool gcrt_sf_read_all(const char* path, uint8_t** data, size_t* size, size_t max_size);
/* Safe write (see the top of the file). Returns false on failure: the old file is then intact and the temporary
 * file deleted. */
bool gcrt_sf_write_atomic(const char* path, const void* data, size_t size);
/* Deletes a regular file (true if it no longer exists). */
bool gcrt_sf_remove(const char* path);
/* Moves (renames) a file within the same volume, without replacing an existing destination. */
bool gcrt_sf_move(const char* from, const char* to);

/* Enumerates the regular files of `dir` whose name ends with `suffix` (case-insensitive comparison; "" = all),
 * in alphabetical order of the names (UTF-8 bytes): `visit(name, size, user)`. Returns the number of files seen,
 * -1 if the folder is unreadable. */
typedef void (*GcrtSfVisit)(const char* name, uint64_t size, void* user);
int gcrt_sf_list(const char* dir, const char* suffix, GcrtSfVisit visit, void* user);

/* Deletes the orphan temporary files "<x><suffix>.tmp" of `dir` (interrupted writes); touches no other
 * file. Returns the number of files deleted. */
int gcrt_sf_clean_orphans(const char* dir, const char* suffix);

/* Number of successful and failed safe writes since startup (reports). */
void gcrt_sf_stats(uint64_t* ok, uint64_t* failed);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_SAVE_FILE_H */
