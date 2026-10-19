/* Paths given by the user, UTF-8 inside the runtime (see gcrt_path.h). Generic.
 *
 * Windows functions used (Microsoft Learn): MultiByteToWideChar / WideCharToMultiByte with CP_UTF8, _wfopen,
 * GetEnvironmentVariableW (reads the process environment block, which is UTF-16 whatever the ANSI code page),
 * GetCommandLineW + CommandLineToArgvW (shell32), GetModuleFileNameW, CreateFileW with FILE_FLAG_DELETE_ON_CLOSE. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "platform/gcrt_path.h"
#include "platform/gcrt_save_file.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#if defined(_WIN32)

static wchar_t* widen(const char* s) {
    if (!s)
        return NULL;
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

wchar_t* gcrt_utf8_to_wide(const char* s) { return widen(s); }

/* UTF-16 -> UTF-8 into a caller buffer; false if it does not fit. */
static bool narrow_into(const wchar_t* w, char* out, size_t size) {
    if (!size)
        return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)(size > 0x7FFFFFFF ? 0x7FFFFFFF : size), NULL, NULL);
    if (n <= 0) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static char* narrow_dup(const wchar_t* w) {
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

FILE* gcrt_fopen_utf8(const char* path, const char* mode) {
    wchar_t* wp = widen(path);
    wchar_t* wm = widen(mode);
    FILE* f = (wp && wm) ? _wfopen(wp, wm) : NULL;
    free(wp);
    free(wm);
    return f;
}

bool gcrt_getenv_utf8(const char* name, char* out, size_t size) {
    if (size)
        out[0] = '\0';
    wchar_t* wn = widen(name);
    if (!wn)
        return false;
    wchar_t buf[4096];
    DWORD n = GetEnvironmentVariableW(wn, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    free(wn);
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return false;
    return narrow_into(buf, out, size) && out[0] != '\0';
}

void gcrt_utf8_args(int* argc, char*** argv) {
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w)
        return;
    if (n == *argc) {
        char** copy = (char**)calloc((size_t)n + 1, sizeof(char*));
        bool ok = copy != NULL;
        for (int i = 0; ok && i < n; i++)
            ok = (copy[i] = narrow_dup(w[i])) != NULL;
        if (ok) {
            *argv = copy; /* kept for the whole process */
        } else if (copy) {
            for (int i = 0; i < n; i++)
                free(copy[i]);
            free(copy);
        }
    }
    LocalFree(w);
}

bool gcrt_exe_dir_utf8(char* out, size_t size) {
    static wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
        return false;
    wchar_t* slash = wcsrchr(buf, L'\\');
    wchar_t* other = wcsrchr(buf, L'/');
    if (other && (!slash || other > slash))
        slash = other;
    if (!slash)
        return false;
    *slash = L'\0';
    return narrow_into(buf, out, size);
}

bool gcrt_dir_writable(const char* dir) {
    if (!dir || !dir[0])
        return false;
    gcrt_sf_mkdirs(dir);
    char probe[2048];
    int len = snprintf(probe, sizeof(probe), "%s/.gcrt-write-test-%lu", dir, (unsigned long)GetCurrentProcessId());
    if (len <= 0 || (size_t)len >= sizeof(probe))
        return false;
    wchar_t* w = widen(probe);
    if (!w)
        return false;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                           NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h); /* deleted on close */
    return true;
}

void gcrt_path_display(const char* path, char* out, size_t size) {
    if (!size)
        return;
    snprintf(out, size, "%s", path ? path : "");
    wchar_t* w = widen(path);
    if (!w)
        return;
    static wchar_t full[32768];
    DWORD n = GetFullPathNameW(w, (DWORD)(sizeof(full) / sizeof(full[0])), full, NULL);
    free(w);
    if (n == 0 || n >= sizeof(full) / sizeof(full[0]))
        return;
    char tmp[4096];
    if (narrow_into(full, tmp, sizeof(tmp)))
        snprintf(out, size, "%s", tmp);
}

#else /* not Windows: paths are bytes, plain C functions */

void gcrt_path_display(const char* path, char* out, size_t size) {
    if (size)
        snprintf(out, size, "%s", path ? path : "");
}

FILE* gcrt_fopen_utf8(const char* path, const char* mode) { return fopen(path, mode); }

bool gcrt_getenv_utf8(const char* name, char* out, size_t size) {
    const char* v = getenv(name);
    if (!v || !v[0] || strlen(v) >= size)
        return false;
    memcpy(out, v, strlen(v) + 1);
    return true;
}

void gcrt_utf8_args(int* argc, char*** argv) {
    (void)argc;
    (void)argv;
}

bool gcrt_exe_dir_utf8(char* out, size_t size) {
#if defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, size - 1);
    if (n <= 0)
        return false;
    out[n] = '\0';
    char* slash = strrchr(out, '/');
    if (!slash)
        return false;
    *slash = '\0';
    return true;
#else
    (void)out;
    (void)size;
    return false; /* [hypothesis] other systems: not needed to date */
#endif
}

bool gcrt_dir_writable(const char* dir) {
    if (!dir || !dir[0])
        return false;
    gcrt_sf_mkdirs(dir);
    char probe[2048];
    int len = snprintf(probe, sizeof(probe), "%s/.gcrt-write-test-%ld", dir, (long)getpid());
    if (len <= 0 || (size_t)len >= sizeof(probe))
        return false;
    int fd = open(probe, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return false;
    close(fd);
    unlink(probe);
    return true;
}

#endif

bool gcrt_path_is_absolute(const char* p) {
    if (!p || !p[0])
        return false;
    if (p[0] == '/' || p[0] == '\\')
        return true; /* "/x", "\x", "\\server\share" */
    /* "C:\x", "C:/x"; "C:x" (relative to the drive's current folder) is not joinable either: treated as absolute. */
    return ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':';
}

const char* gcrt_default_disc_root(const char* compiled_in, char* out, size_t size) {
    char dir[2048];
    if (gcrt_exe_dir_utf8(dir, sizeof(dir))) {
        char dol[2100];
        snprintf(dol, sizeof(dol), "%s/disc/sys/main.dol", dir);
        if (gcrt_sf_is_file(dol)) {
            int n = snprintf(out, size, "%s/disc", dir);
            if (n > 0 && (size_t)n < size)
                return "next to the executable";
        }
    }
    snprintf(out, size, "%s", compiled_in ? compiled_in : "");
    return "build default";
}
