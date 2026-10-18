/* Save file layer (see gcrt_save_file.h). Generic, with no dependency on the rest of the runtime except the
 * traces.
 *
 * Windows (checked in the Microsoft Learn documentation, 2026-10-11; details in notes/decisions.md):
 *   - CreateFileW(CREATE_ALWAYS) + WriteFile + FlushFileBuffers (writes all the buffered data of the file to the
 *     device, GENERIC_WRITE handle required);
 *   - MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH): replaces the existing destination, and only
 *     returns once the move has been done on disk. Same folder, hence same volume: a plain rename.
 *     ReplaceFileW (recommended by the page "Alternatives to using Transactional NTFS") was not chosen: its
 *     documentation describes it as several steps (rename the original, rename the new one, delete
 *     the original) with failure states where the original no longer exists under its name, and
 *     REPLACEFILE_WRITE_THROUGH is documented as not supported; it also requires the destination to exist.
 *   - A rename refused because of sharing (antivirus, indexer, another program reading the file) is retried
 *     a few times before failing.
 * Elsewhere (not compiled to date, [hypothesis]): open + write + fsync + rename + fsync of the folder (POSIX). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "platform/gcrt_save_file.h"
#include "platform/trace.h"
#include "platform/gcrt_diag.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static uint64_t s_ok, s_failed, s_writes;

/* ---------------------------------------------------------------------------------------------
 * Simulated hard kill (GCRT_SAVE_FAULT, tests only)
 * ------------------------------------------------------------------------------------------- */

enum { FAULT_NONE = 0, FAULT_MID_WRITE, FAULT_BEFORE_RENAME };
static int s_fault_kind = -1; /* -1: variable not read yet */
static uint64_t s_fault_at;

static void fault_read_env(void) {
    if (s_fault_kind >= 0)
        return;
    s_fault_kind = FAULT_NONE;
    const char* v = getenv("GCRT_SAVE_FAULT");
    if (!v || !v[0])
        return;
    const char* colon = strchr(v, ':');
    const size_t n = colon ? (size_t)(colon - v) : strlen(v);
    if (n == strlen("mid-write") && !strncmp(v, "mid-write", n))
        s_fault_kind = FAULT_MID_WRITE;
    else if (n == strlen("before-rename") && !strncmp(v, "before-rename", n))
        s_fault_kind = FAULT_BEFORE_RENAME;
    s_fault_at = colon ? strtoull(colon + 1, NULL, 10) : 1;
    if (s_fault_at == 0)
        s_fault_at = 1;
    if (s_fault_kind == FAULT_NONE)
        fprintf(stderr, "gcrt: GCRT_SAVE_FAULT=%s ignored (mid-write:N or before-rename:N)\n", v);
    else
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "WARNING: GCRT_SAVE_FAULT=%s: the process will be killed during safe "
                                               "write #%llu (test)", v,
                          (unsigned long long)s_fault_at);
}

/* Immediate stop, without cleanup (equivalent to a killed process): no destructor, no buffer flushed. */
static void fault_kill(const char* stage, const char* path) {
    fprintf(stderr, "gcrt: GCRT_SAVE_FAULT: hard kill %s (%s)\n", stage, path);
    fflush(stderr);
    FILE* t = gcrt_trace_stream();
    if (t) {
        fprintf(t, "[platform] GCRT_SAVE_FAULT: hard kill %s (%s)\n", stage, path);
        fflush(t);
    }
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), 86);
#endif
    _exit(86);
}

/* ---------------------------------------------------------------------------------------------
 * Windows
 * ------------------------------------------------------------------------------------------- */
#if defined(_WIN32)

static wchar_t* widen(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0)
        return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
        free(w);
        w = NULL;
    }
    return w;
}

static char* narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0)
        return NULL;
    char* s = (char*)malloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) {
        free(s);
        s = NULL;
    }
    return s;
}

bool gcrt_sf_mkdirs(const char* dir) {
    if (!dir || !dir[0])
        return false;
    wchar_t* w = widen(dir);
    if (!w)
        return false;
    size_t len = wcslen(w);
    while (len > 1 && (w[len - 1] == L'\\' || w[len - 1] == L'/'))
        w[--len] = L'\0';
    bool ok = true;
    for (size_t i = 1; i <= len && ok; i++) {
        if (w[i] != L'\\' && w[i] != L'/' && w[i] != L'\0')
            continue;
        if (i == 2 && w[1] == L':')
            continue; /* "C:" */
        const wchar_t saved = w[i];
        w[i] = L'\0';
        if (!CreateDirectoryW(w, NULL)) {
            const DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS && e != ERROR_ACCESS_DENIED)
                ok = false;
        }
        w[i] = saved;
    }
    const DWORD attr = GetFileAttributesW(w);
    ok = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    free(w);
    return ok;
}

bool gcrt_sf_is_file(const char* path) {
    wchar_t* w = widen(path);
    if (!w)
        return false;
    const DWORD attr = GetFileAttributesW(w);
    free(w);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool gcrt_sf_read_all(const char* path, uint8_t** data, size_t* size, size_t max_size) {
    *data = NULL;
    *size = 0;
    wchar_t* w = widen(path);
    if (!w)
        return false;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER li;
    bool ok = GetFileSizeEx(h, &li) && li.QuadPart >= 0 && (uint64_t)li.QuadPart <= max_size;
    uint8_t* buf = NULL;
    const size_t n = ok ? (size_t)li.QuadPart : 0;
    if (ok) {
        buf = (uint8_t*)malloc(n ? n : 1);
        ok = buf != NULL;
    }
    size_t done = 0;
    while (ok && done < n) {
        DWORD got = 0;
        const DWORD want = (DWORD)((n - done) > 0x40000000u ? 0x40000000u : (n - done));
        if (!ReadFile(h, buf + done, want, &got, NULL) || got == 0)
            ok = false;
        done += got;
    }
    CloseHandle(h);
    gcrt_diag_event(GCRT_EV_SAVE_READ, n, ok ? 1u : 0u);
    if (!ok) {
        free(buf);
        return false;
    }
    *data = buf;
    *size = n;
    return true;
}

static bool write_handle(HANDLE h, const uint8_t* p, size_t n) {
    size_t done = 0;
    while (done < n) {
        DWORD put = 0;
        const DWORD want = (DWORD)((n - done) > 0x40000000u ? 0x40000000u : (n - done));
        if (!WriteFile(h, p + done, want, &put, NULL) || put == 0)
            return false;
        done += put;
    }
    return true;
}

bool gcrt_sf_write_atomic(const char* path, const void* data, size_t size) {
    fault_read_env();
    const uint64_t index = ++s_writes;
    char tmp_path[2048];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s%s", path, GCRT_SF_TMP_SUFFIX) >= (int)sizeof(tmp_path)) {
        s_failed++;
        return false;
    }
    wchar_t* wfinal = widen(path);
    wchar_t* wtmp = widen(tmp_path);
    bool ok = wfinal && wtmp;
    const char* step = "path conversion";
    HANDLE h = INVALID_HANDLE_VALUE;
    if (ok) {
        step = "creating the temporary file";
        h = CreateFileW(wtmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = h != INVALID_HANDLE_VALUE;
    }
    const uint8_t* p = (const uint8_t*)data;
    if (ok && s_fault_kind == FAULT_MID_WRITE && index == s_fault_at) {
        write_handle(h, p, size / 2);
        FlushFileBuffers(h);
        fault_kill("while writing the temporary file", path);
    }
    if (ok) {
        step = "writing the temporary file";
        ok = write_handle(h, p, size);
    }
    if (ok) {
        step = "flushing the temporary file to disk";
        ok = FlushFileBuffers(h) != 0;
    }
    if (h != INVALID_HANDLE_VALUE && !CloseHandle(h) && ok) {
        step = "closing the temporary file";
        ok = false;
    }
    if (ok && s_fault_kind == FAULT_BEFORE_RENAME && index == s_fault_at)
        fault_kill("before the rename", path);
    DWORD err = 0;
    if (ok) {
        step = "rename";
        ok = false;
        for (int attempt = 0; attempt < 10 && !ok; attempt++) {
            if (MoveFileExW(wtmp, wfinal, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                ok = true;
                break;
            }
            err = GetLastError();
            if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION && err != ERROR_LOCK_VIOLATION)
                break;
            Sleep(20); /* file briefly opened by another program */
        }
    } else {
        err = GetLastError();
    }
    if (!ok) {
        if (wtmp)
            DeleteFileW(wtmp);
        s_failed++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "save: safe write of %s failed (%s, error %lu): "
                                               "previous file unchanged", path, step, (unsigned long)err);
    } else {
        s_ok++;
    }
    gcrt_diag_event(GCRT_EV_SAVE_WRITE, size, ok ? 1u : 0u);
    free(wfinal);
    free(wtmp);
    return ok;
}

bool gcrt_sf_remove(const char* path) {
    wchar_t* w = widen(path);
    if (!w)
        return false;
    bool ok = DeleteFileW(w) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND;
    free(w);
    gcrt_diag_event(GCRT_EV_SAVE_REMOVE, 0, ok ? 1u : 0u);
    return ok;
}

bool gcrt_sf_move(const char* from, const char* to) {
    wchar_t* a = widen(from);
    wchar_t* b = widen(to);
    bool ok = a && b && MoveFileExW(a, b, MOVEFILE_WRITE_THROUGH);
    free(a);
    free(b);
    gcrt_diag_event(GCRT_EV_SAVE_MOVE, 0, ok ? 1u : 0u);
    return ok;
}

typedef struct {
    char* name;
    uint64_t size;
} Entry;

static int entry_cmp(const void* a, const void* b) {
    return strcmp(((const Entry*)a)->name, ((const Entry*)b)->name);
}

static bool has_suffix(const char* name, const char* suffix) {
    const size_t n = strlen(name), s = strlen(suffix);
    if (s > n)
        return false;
    for (size_t i = 0; i < s; i++)
        if (tolower((unsigned char)name[n - s + i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

int gcrt_sf_list(const char* dir, const char* suffix, GcrtSfVisit visit, void* user) {
    char pattern[2048];
    const size_t dl = strlen(dir);
    const bool sep = dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/');
    snprintf(pattern, sizeof(pattern), "%s%s*", dir, sep ? "" : "\\");
    wchar_t* w = widen(pattern);
    if (!w)
        return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : -1; /* empty or missing folder */
    }
    Entry* list = NULL;
    size_t count = 0, cap = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char* name = narrow(fd.cFileName);
        if (!name)
            continue;
        if (!has_suffix(name, suffix ? suffix : "")) {
            free(name);
            continue;
        }
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            Entry* grown = (Entry*)realloc(list, cap * sizeof(Entry));
            if (!grown) {
                free(name);
                break;
            }
            list = grown;
        }
        list[count].name = name;
        list[count].size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        count++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (count)
        qsort(list, count, sizeof(Entry), entry_cmp);
    for (size_t i = 0; i < count; i++) {
        if (visit)
            visit(list[i].name, list[i].size, user);
        free(list[i].name);
    }
    free(list);
    return (int)count;
}

/* ---------------------------------------------------------------------------------------------
 * POSIX ([hypothesis]: not compiled to date)
 * ------------------------------------------------------------------------------------------- */
#else

bool gcrt_sf_mkdirs(const char* dir) {
    if (!dir || !dir[0])
        return false;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", dir);
    size_t len = strlen(buf);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = '\0';
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        const char saved = buf[i];
        buf[i] = '\0';
        mkdir(buf, 0755);
        buf[i] = saved;
    }
    struct stat st;
    return stat(buf, &st) == 0 && S_ISDIR(st.st_mode);
}

bool gcrt_sf_is_file(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool gcrt_sf_read_all(const char* path, uint8_t** data, size_t* size, size_t max_size) {
    *data = NULL;
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    struct stat st;
    bool ok = fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)st.st_size <= max_size;
    size_t n = ok ? (size_t)st.st_size : 0;
    uint8_t* buf = ok ? (uint8_t*)malloc(n ? n : 1) : NULL;
    ok = ok && buf && fread(buf, 1, n, f) == n;
    fclose(f);
    if (!ok) {
        free(buf);
        return false;
    }
    *data = buf;
    *size = n;
    return true;
}

bool gcrt_sf_write_atomic(const char* path, const void* data, size_t size) {
    fault_read_env();
    const uint64_t index = ++s_writes;
    char tmp_path[2048];
    snprintf(tmp_path, sizeof(tmp_path), "%s%s", path, GCRT_SF_TMP_SUFFIX);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    bool ok = fd >= 0;
    const uint8_t* p = (const uint8_t*)data;
    if (ok && s_fault_kind == FAULT_MID_WRITE && index == s_fault_at) {
        (void)!write(fd, p, size / 2);
        fsync(fd);
        fault_kill("while writing the temporary file", path);
    }
    size_t done = 0;
    while (ok && done < size) {
        ssize_t w = write(fd, p + done, size - done);
        if (w <= 0)
            ok = false;
        else
            done += (size_t)w;
    }
    ok = ok && fsync(fd) == 0;
    if (fd >= 0 && close(fd) != 0)
        ok = false;
    if (ok && s_fault_kind == FAULT_BEFORE_RENAME && index == s_fault_at)
        fault_kill("before the rename", path);
    ok = ok && rename(tmp_path, path) == 0;
    if (ok) {
        char dir[2048];
        snprintf(dir, sizeof(dir), "%s", path);
        char* slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            int dfd = open(dir, O_RDONLY);
            if (dfd >= 0) {
                fsync(dfd);
                close(dfd);
            }
        }
        s_ok++;
    } else {
        unlink(tmp_path);
        s_failed++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "save: safe write of %s failed (errno %d): previous file "
                                               "unchanged", path, errno);
    }
    return ok;
}

bool gcrt_sf_remove(const char* path) { return unlink(path) == 0 || errno == ENOENT; }

bool gcrt_sf_move(const char* from, const char* to) {
    if (access(to, F_OK) == 0)
        return false;
    return rename(from, to) == 0;
}

typedef struct {
    char* name;
    uint64_t size;
} Entry;

static int entry_cmp(const void* a, const void* b) {
    return strcmp(((const Entry*)a)->name, ((const Entry*)b)->name);
}

static bool has_suffix(const char* name, const char* suffix) {
    const size_t n = strlen(name), s = strlen(suffix);
    if (s > n)
        return false;
    for (size_t i = 0; i < s; i++)
        if (tolower((unsigned char)name[n - s + i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

int gcrt_sf_list(const char* dir, const char* suffix, GcrtSfVisit visit, void* user) {
    DIR* d = opendir(dir);
    if (!d)
        return errno == ENOENT ? 0 : -1;
    Entry* list = NULL;
    size_t count = 0, cap = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        char full[4096];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || !has_suffix(e->d_name, suffix ? suffix : ""))
            continue;
        if (count == cap) {
            cap = cap ? cap * 2 : 32;
            Entry* grown = (Entry*)realloc(list, cap * sizeof(Entry));
            if (!grown)
                break;
            list = grown;
        }
        list[count].name = strdup(e->d_name);
        list[count].size = (uint64_t)st.st_size;
        count++;
    }
    closedir(d);
    if (count)
        qsort(list, count, sizeof(Entry), entry_cmp);
    for (size_t i = 0; i < count; i++) {
        if (visit)
            visit(list[i].name, list[i].size, user);
        free(list[i].name);
    }
    free(list);
    return (int)count;
}

#endif

/* ---------------------------------------------------------------------------------------------
 * Common
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    const char* dir;
    const char* suffix;
    int removed;
} OrphanCtx;

static void orphan_visit(const char* name, uint64_t size, void* user) {
    (void)size;
    OrphanCtx* c = (OrphanCtx*)user;
    /* "<x><suffix>.tmp" only: the temporary file of gcrt_sf_write_atomic for a "<x><suffix>" file. */
    const size_t n = strlen(name), t = strlen(GCRT_SF_TMP_SUFFIX), s = strlen(c->suffix);
    if (n <= t + s)
        return;
    for (size_t i = 0; i < s; i++)
        if (tolower((unsigned char)name[n - t - s + i]) != tolower((unsigned char)c->suffix[i]))
            return;
    char path[2048];
    const size_t dl = strlen(c->dir);
    const bool sep = dl && (c->dir[dl - 1] == '\\' || c->dir[dl - 1] == '/');
    snprintf(path, sizeof(path), "%s%s%s", c->dir, sep ? "" : "/", name);
    if (gcrt_sf_remove(path)) {
        c->removed++;
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "save: orphan temporary file deleted (interrupted write): %s",
                          path);
    }
}

int gcrt_sf_clean_orphans(const char* dir, const char* suffix) {
    OrphanCtx c = {dir, suffix ? suffix : "", 0};
    gcrt_sf_list(dir, GCRT_SF_TMP_SUFFIX, orphan_visit, &c);
    return c.removed;
}

void gcrt_sf_stats(uint64_t* ok, uint64_t* failed) {
    if (ok)
        *ok = s_ok;
    if (failed)
        *failed = s_failed;
}
