// Console window of the executable (gcrt_console.h).
//
// Windows-subsystem program: Windows gives it no console, and its C streams stdout and stderr have no file descriptor
// ("-2" in a Windows application without a console window: Microsoft's _fileno documentation). Until the game starts,
// both streams are reopened on the NUL device (which gives them a descriptor), then that descriptor is pointed at the
// write end of an anonymous pipe (_dup2); a thread reads the pipe and keeps the first kEarlyCap bytes. When the game
// starts, the streams are reopened on the console (CONOUT$) or on NUL, which closes the pipe's write ends: the thread
// sees the end of the pipe, and the kept bytes are written to the console if it opened.
#include "gcrt_console.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "trace.h"

namespace {
constexpr size_t kEarlyCap = size_t{1} << 20; // early output kept for the console: 1 MiB, the rest is counted

int g_request = -1;
bool g_initialized = false;
bool g_left_to_parent = false; // redirected by the parent, or a console already there: nothing is ever touched
bool g_started = false;        // gcrt_console_start has run
HANDLE g_pipe_read = nullptr;
HANDLE g_reader = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
std::string g_early;  // guarded by g_lock
size_t g_dropped = 0; // guarded by g_lock

// A standard handle set by the parent to something other than a console: file, pipe, NUL device...
bool redirected(DWORD which) {
  const HANDLE h = GetStdHandle(which);
  if (h == nullptr || h == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD mode = 0;
  if (GetConsoleMode(h, &mode)) {
    return false; // a console handle
  }
  SetLastError(NO_ERROR);
  return GetFileType(h) != FILE_TYPE_UNKNOWN || GetLastError() == NO_ERROR; // UNKNOWN + error: not a valid handle
}

DWORD WINAPI read_early_output(void*) {
  char chunk[4096];
  DWORD n = 0;
  while (ReadFile(g_pipe_read, chunk, sizeof(chunk), &n, nullptr) && n > 0) {
    AcquireSRWLockExclusive(&g_lock);
    const size_t keep = std::min<size_t>(n, kEarlyCap - g_early.size());
    g_early.append(chunk, keep);
    g_dropped += n - keep;
    ReleaseSRWLockExclusive(&g_lock);
  }
  return 0; // ERROR_BROKEN_PIPE: every write end is closed
}

// Gives the stream a descriptor (reopened on NUL), then points that descriptor at the pipe.
bool capture(FILE* stream, int pipe_fd) {
  FILE* f = nullptr;
  if (freopen_s(&f, "NUL", "w", stream) != 0) {
    return false;
  }
  // Unbuffered while captured: stdout and stderr lines keep the order they were written in (a pipe would make stdout
  // fully buffered). Reopening the stream when the game starts gives it its usual buffering again.
  setvbuf(stream, nullptr, _IONBF, 0);
  const int fd = _fileno(stream);
  return fd >= 0 && _dup2(pipe_fd, fd) == 0;
}

// Reopens the stream on a device (CONOUT$, CONIN$, NUL) and makes the matching Win32 standard handle follow it, for
// the code that writes through GetStdHandle.
void reopen(FILE* stream, const char* device, const char* mode, DWORD std_handle) {
  FILE* f = nullptr;
  if (freopen_s(&f, device, mode, stream) == 0) {
    SetStdHandle(std_handle, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream))));
  }
}

// Called once the streams no longer write into the pipe: waits for the reader to see the end of the pipe, then takes
// the kept output.
std::string stop_capture(size_t* dropped) {
  if (g_reader != nullptr) {
    if (WaitForSingleObject(g_reader, 2000) != WAIT_OBJECT_0) {
      CancelSynchronousIo(g_reader); // a write end still open elsewhere: stop reading anyway
      WaitForSingleObject(g_reader, 1000);
    }
    DWORD code = 0;
    if (GetExitCodeThread(g_reader, &code) && code != STILL_ACTIVE) {
      CloseHandle(g_pipe_read); // the thread is done with it
      g_pipe_read = nullptr;
    }
    CloseHandle(g_reader);
    g_reader = nullptr;
  }
  AcquireSRWLockExclusive(&g_lock);
  std::string out;
  out.swap(g_early);
  *dropped = g_dropped;
  g_dropped = 0;
  ReleaseSRWLockExclusive(&g_lock);
  return out;
}

// The process ends before the game started (--help, option or boot error, window closed from the setup menu): a launch
// from a terminal still prints there, unless --no-console.
void at_exit() {
  if (!g_started) {
    gcrt_console_start(g_request == 0 ? GCRT_CONSOLE_NONE : GCRT_CONSOLE_PARENT);
  }
}
} // namespace

extern "C" {

void gcrt_console_early_init(void) {
  if (g_initialized) {
    return;
  }
  g_initialized = true;
  if (redirected(STD_OUTPUT_HANDLE) || redirected(STD_ERROR_HANDLE) || GetConsoleWindow() != nullptr) {
    g_left_to_parent = true;
    return;
  }
  std::atexit(at_exit);
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, nullptr, 0)) {
    return; // early output lost (the streams keep writing nowhere)
  }
  const int pipe_fd = _open_osfhandle(reinterpret_cast<intptr_t>(wr), _O_WRONLY | _O_BINARY);
  if (pipe_fd < 0) {
    CloseHandle(rd);
    CloseHandle(wr);
    return;
  }
  const bool out = capture(stdout, pipe_fd);
  const bool err = capture(stderr, pipe_fd);
  _close(pipe_fd); // also closes wr: the streams' descriptors hold their own duplicates
  g_pipe_read = rd;
  g_reader = (out || err) ? CreateThread(nullptr, 64 * 1024, read_early_output, nullptr, 0, nullptr) : nullptr;
  if (g_reader == nullptr) {
    CloseHandle(rd); // no reader: writes into the pipe fail instead of blocking
    g_pipe_read = nullptr;
  }
  SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))));
  SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stderr))));
}

void gcrt_console_set_request(int request) { g_request = request; }

int gcrt_console_request(void) { return g_request; }

void gcrt_console_start(GcrtConsoleChoice choice) {
  if (!g_initialized || g_left_to_parent || g_started) {
    return;
  }
  g_started = true;
  std::fflush(stdout);
  std::fflush(stderr);
  const HWND foreground = GetForegroundWindow();
  bool attached = false, created = false;
  if (choice != GCRT_CONSOLE_NONE) {
    attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0; // started from a terminal: print there
    if (!attached && choice == GCRT_CONSOLE_OPEN) {
      created = AllocConsole() != 0;
    }
  }
  const bool open = attached || created;
  // Reopening closes the pipe's write ends held by the two streams.
  reopen(stdout, open ? "CONOUT$" : "NUL", "w", STD_OUTPUT_HANDLE);
  reopen(stderr, open ? "CONOUT$" : "NUL", "w", STD_ERROR_HANDLE);
  if (open) {
    reopen(stdin, "CONIN$", "r", STD_INPUT_HANDLE);
  }
  size_t dropped = 0;
  const std::string early = stop_capture(&dropped);
  if (open) {
    std::fwrite(early.data(), 1, early.size(), stdout);
    if (dropped != 0) {
      std::fprintf(stdout, "[console] %zu bytes of earlier output dropped (only the first %zu are kept)\n", dropped,
                   kEarlyCap);
    }
    std::fflush(stdout);
  }
  // A new console window takes the foreground: give it back to the game window, which had it (the key press that
  // started the game came from there).
  if (created && foreground != nullptr && GetForegroundWindow() != foreground) {
    SetForegroundWindow(foreground);
  }
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "console: %s; %zu bytes of earlier output %s",
                    created ? "new console opened" : attached ? "attached to the parent's console" : "none",
                    early.size() + dropped, open ? "written to it" : "dropped");
}

} // extern "C"

#else // !_WIN32: the console is the terminal the program was started from

static int g_request = -1;
extern "C" {
void gcrt_console_early_init(void) {}
void gcrt_console_set_request(int request) { g_request = request; }
int gcrt_console_request(void) { return g_request; }
void gcrt_console_start(GcrtConsoleChoice) {}
}

#endif
