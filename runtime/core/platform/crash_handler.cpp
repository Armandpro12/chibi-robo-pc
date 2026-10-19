// Crash handler and hang watchdog (see gcrt_crash.h). Generic: no reference to any game.
//
// Robustness rules (a crash report must be written whatever state the process is in):
//  - the work is done by a reporting thread created at installation (1 MiB stack): the faulting thread may have
//    almost no stack left (stack overflow) and only signals it, then waits (bounded);
//  - the header (cause, exception, registers, code bytes, raw stack, guest state of a faulting game thread) is composed
//    and written before anything that may use the heap: the faulting thread may hold the heap lock (a fault inside
//    the heap code, heap corruption). The .txt is created with NtCreateFile relative to a handle on the crash folder
//    opened in advance (CreateFileW converts the path on the process heap);
//  - phase 0 (may allocate: module and thread snapshots) comes after that header; phase 1 uses no heap allocation
//    and no loader lock: preallocated buffers; machine information, the environment, the command line, the
//    executable name and the processor count are read at installation;
//  - the other threads are suspended only while their contexts, a copy of their stacks and the guest state are
//    taken, with nothing that allocates or takes a lock (a suspended thread may hold the heap lock); everything else
//    (unwinding the copies, names, text) comes after they are resumed. A suspension guard thread, never suspended,
//    resumes them if they stay suspended more than GCRT_CRASH_SUSPEND_LIMIT seconds (default 5): a report must never
//    freeze the game, which a hang report (watchdog) could otherwise do to a healthy game;
//  - if the reporting thread does not finish phase 1 within 60 s (blocked), the faulting thread writes what is left
//    to stderr, marks the .txt as incomplete and lets the process end as usual;
//  - the text report is written first, section by section (whatever was composed is on disk if a later step hangs or
//    faults), then the minidump, then the guest memory; the risky steps of phase 2 are guarded by SEH;
//  - if the folder cannot be written, the temporary folder is tried, then the report goes to stderr only; if the
//    reporting thread does not answer, the faulting thread writes what was composed to stderr itself.
#include "gcrt_crash.h"
#include "gcrt_diag.h"

extern "C" {
#include "../hle/gcrt_hle.h"
#include "trace.h"
}

#include <algorithm>
#include <string>
#include <vector>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <dbghelp.h>
#include <intrin.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <csignal>
#include <exception>

namespace {

GcrtCpu* g_cpu = nullptr;
DWORD g_game_thread = 0;
GcrtGuestSymbolizer g_symbolizer = nullptr;
GcrtCrashContextFn g_context_fn = nullptr;
void* g_context_user = nullptr;
volatile LONG g_reporting = 0; // 0 idle, 1 crash report, 2 hang report
volatile LONG g_crash_finished = 0; // the crash report is complete (the process is about to end)
bool g_installed = false;
wchar_t g_dir[MAX_PATH * 2] = L"";      // final folder, with a trailing separator
wchar_t g_temp_dir[MAX_PATH * 2] = L""; // fallback folder (%TEMP%\gcrt-crashes\)
bool g_dir_from_env = false;
// Handle on g_dir opened in advance, so that the report can be created without the heap (open_report_file).
HANDLE g_dir_handle = INVALID_HANDLE_VALUE;
// NtCreateFile (winternl.h, ntdll), resolved at installation.
using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER,
                                        ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
NtCreateFileFn g_nt_create_file = nullptr;
constexpr NTSTATUS kStatusNameCollision = static_cast<NTSTATUS>(0xC0000035L); // STATUS_OBJECT_NAME_COLLISION

// --- Text buffer -------------------------------------------------------------------------------------------------
// Report text, assembled in a fixed buffer (no allocation once the crash has happened). Flushed to the open .txt
// section by section (flush_text), scrubbed of the user-profile folder on the way.
constexpr size_t kReportSize = 512 * 1024;
char g_report[kReportSize];
size_t g_report_len = 0;
size_t g_report_flushed = 0;
HANDLE g_txt = INVALID_HANDLE_VALUE;
volatile LONG g_phase1_done = 0;

// Output target of out(): the report buffer, or another fixed buffer while the full event list is formatted.
char* g_out_buf = g_report;
size_t g_out_cap = kReportSize;
size_t* g_out_len = &g_report_len;

void out(const char* fmt, ...) {
  if (*g_out_len >= g_out_cap - 1) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(g_out_buf + *g_out_len, g_out_cap - *g_out_len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    *g_out_len += static_cast<size_t>(n);
    if (*g_out_len > g_out_cap - 1) {
      *g_out_len = g_out_cap - 1;
    }
  }
}

void append_text(void* /*sink*/, const char* text) { out("%s", text); }

bool env_set(const wchar_t* name, wchar_t* buf, DWORD size) {
  const DWORD n = GetEnvironmentVariableW(name, buf, size);
  return n > 0 && n < size;
}

// --- Privacy of the text report --------------------------------------------------------------------------------------
// The .txt is the file players share: it should not reveal the Windows user name nor local folders. The report writes
// file names only (executable, host modules). In the rest of the text (command line, environment, log lines):
//  - every absolute path ("X:\..." or "\\server\...") loses its folders: "<path>\" + its last component. A path may
//    contain spaces, so the folders end at the last separator before the end of the line, ", " or a character that
//    cannot be in a path (quote, tab, | < > * ?): text between two paths of the same line may be lost with them;
//  - then, as a second barrier, the user-profile folder (USERPROFILE, long and 8.3 short forms) is replaced by the
//    literal "%USERPROFILE%", and the bare user name (USERNAME) by "%USERNAME%" when it is a path component.
// Prepared at installation.
constexpr const char kProfileToken[] = "%USERPROFILE%";
constexpr const char kUserToken[] = "%USERNAME%";
constexpr const char kPathToken[] = "<path>";
char g_profile[2][MAX_PATH * 4] = {}; // UTF-8, without a trailing separator; empty if unknown
char g_user[256] = "";                 // UTF-8 user name; empty if unknown or too short to replace safely
char g_clean[kReportSize];             // scrubbed copy of a chunk of g_report

void prepare_profile_prefixes() {
  wchar_t buf[MAX_PATH * 2];
  wchar_t short_buf[MAX_PATH * 2];
  if (env_set(L"USERNAME", buf, 256) && wcslen(buf) >= 3) {
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, g_user, sizeof(g_user), nullptr, nullptr);
  }
  if (!env_set(L"USERPROFILE", buf, MAX_PATH * 2)) {
    return;
  }
  size_t n = wcslen(buf);
  while (n > 0 && (buf[n - 1] == L'\\' || buf[n - 1] == L'/')) {
    buf[--n] = 0;
  }
  if (n < 4) {
    return; // a bare drive ("C:"): replacing it would mangle every path without hiding anything
  }
  if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, g_profile[0], sizeof(g_profile[0]), nullptr, nullptr) == 0) {
    g_profile[0][0] = 0;
  }
  const DWORD s = GetShortPathNameW(buf, short_buf, MAX_PATH * 2);
  if (s > 0 && s < MAX_PATH * 2 && _wcsicmp(short_buf, buf) != 0 &&
      WideCharToMultiByte(CP_UTF8, 0, short_buf, -1, g_profile[1], sizeof(g_profile[1]), nullptr, nullptr) == 0) {
    g_profile[1][0] = 0;
  }
}

// Path characters compared as Windows does for ASCII: case-insensitive, '\' and '/' equivalent.
bool same_path_char(char a, char b) {
  if ((a == '\\' || a == '/') && (b == '\\' || b == '/')) {
    return true;
  }
  if (a >= 'A' && a <= 'Z') {
    a = static_cast<char>(a - 'A' + 'a');
  }
  if (b >= 'A' && b <= 'Z') {
    b = static_cast<char>(b - 'A' + 'a');
  }
  return a == b;
}

bool name_char(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u >= 0x80 || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' ||
         c == '-' || c == '.' || c == '~';
}

// Length of `prefix` if `text` starts with it and the match ends at a path boundary (so that "C:\Users\ann" does not
// match inside "C:\Users\anne"), 0 otherwise.
size_t profile_match(const char* text, size_t avail, const char* prefix) {
  size_t i = 0;
  for (; prefix[i] != 0; i++) {
    if (i >= avail || !same_path_char(text[i], prefix[i])) {
      return 0;
    }
  }
  if (i == 0) {
    return 0;
  }
  if (i < avail && name_char(text[i])) {
    return 0;
  }
  return i;
}

bool path_end_char(char c) {
  return c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '|' || c == '<' || c == '>' || c == '*' || c == '?' ||
         c == 0;
}

// If an absolute path starts at text[0] ("X:\" or "\\name"), length of its folder part (up to and including the last
// separator before the end of the path span), 0 otherwise. `prev`: the character before, 0 at the start.
size_t path_folders(const char* text, size_t avail, char prev) {
  if (name_char(prev) || prev == '\\' || prev == '/' || prev == ':') {
    return 0;
  }
  const bool drive = avail >= 4 && ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z')) &&
                     text[1] == ':' && (text[2] == '\\' || text[2] == '/');
  const bool unc = avail >= 3 && text[0] == '\\' && text[1] == '\\' && name_char(text[2]);
  if (!drive && !unc) {
    return 0;
  }
  size_t last_sep = 0;
  // ", " also ends the span (log lines such as "image X:\...\disc.iso, rate 3145728 B/s"): a folder name containing
  // ", " would then show its end, which is accepted.
  for (size_t k = 2; k < avail && !path_end_char(text[k]) && !(text[k] == ',' && k + 1 < avail && text[k + 1] == ' ');
       k++) {
    if (text[k] == '\\' || text[k] == '/') {
      last_sep = k;
    }
  }
  return last_sep + 1;
}

// Scrubs g_report[from, to) into g_clean; returns the scrubbed length.
size_t scrub_range(size_t from, size_t to) {
  size_t n = 0;
  size_t i = from;
  while (i < to && n < kReportSize - 32) {
    const size_t folders = path_folders(g_report + i, to - i, i > 0 ? g_report[i - 1] : 0);
    if (folders != 0) {
      for (const char* t = kPathToken; *t != 0; t++) {
        g_clean[n++] = *t;
      }
      g_clean[n++] = g_report[i + folders - 1]; // the separator, as written
      i += folders;
      continue;
    }
    size_t matched = 0;
    const char* token = kProfileToken;
    for (const char* prefix : g_profile) {
      matched = profile_match(g_report + i, to - i, prefix);
      if (matched != 0) {
        break;
      }
    }
    // Bare user name as a path component ("\name\" or "/name/"), e.g. a profile on another drive.
    if (matched == 0 && g_user[0] != 0 && i > from && (g_report[i - 1] == '\\' || g_report[i - 1] == '/')) {
      matched = profile_match(g_report + i, to - i, g_user);
      token = kUserToken;
    }
    if (matched != 0) {
      for (const char* t = token; *t != 0; t++) {
        g_clean[n++] = *t;
      }
      i += matched;
    } else {
      g_clean[n++] = g_report[i++];
    }
  }
  return n;
}

// Writes the text composed since the last flush to the .txt (and stderr), scrubbed. Safe without the heap.
void flush_text() {
  if (g_report_flushed >= g_report_len) {
    return;
  }
  const size_t n = scrub_range(g_report_flushed, g_report_len);
  g_report_flushed = g_report_len;
  DWORD written = 0;
  if (g_txt != INVALID_HANDLE_VALUE) {
    WriteFile(g_txt, g_clean, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(g_txt);
  }
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if (err != nullptr && err != INVALID_HANDLE_VALUE) {
    WriteFile(err, g_clean, static_cast<DWORD>(n), &written, nullptr);
  }
}

void set_dir(const wchar_t* dir, wchar_t* target = g_dir) {
  wcsncpy_s(target, MAX_PATH * 2, dir, _TRUNCATE);
  const size_t n = wcslen(target);
  if (n > 0 && n + 1 < MAX_PATH * 2 && target[n - 1] != L'\\' && target[n - 1] != L'/') {
    target[n] = L'\\';
    target[n + 1] = 0;
  }
}

// Opens the handle on g_dir used to create reports without the heap. `create`: create the folder first (the user's
// crash folder); otherwise (the temporary folder, before the user folder is known) only an existing folder is opened,
// and a report there goes through the path-based fallback of open_report_file.
void open_dir_handle(bool create) {
  if (g_dir_handle != INVALID_HANDLE_VALUE) {
    CloseHandle(g_dir_handle);
    g_dir_handle = INVALID_HANDLE_VALUE;
  }
  if (g_dir[0] == 0) {
    return;
  }
  if (create) {
    CreateDirectoryW(g_dir, nullptr);
  }
  // FILE_FLAG_BACKUP_SEMANTICS is required to open a folder (CreateFileW, Microsoft Learn); every sharing mode so
  // that the folder can still be renamed or deleted by the user (deletion completes when the game ends).
  g_dir_handle = CreateFileW(g_dir, FILE_LIST_DIRECTORY | FILE_TRAVERSE | SYNCHRONIZE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

// Number of reports of each kind (crash-, hang-) kept in the crash folder. Each crash report holds the guest memory
// (about 25 MiB) and a minidump, so older ones are deleted. Called at startup when the folder is chosen, never from
// the handler.
constexpr size_t kMaxReports = 5;

void prune_old_reports(const wchar_t* prefix) {
  std::vector<std::wstring> stems;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((std::wstring(g_dir) + prefix + L"*.txt").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  const size_t prefix_len = wcslen(prefix);
  do {
    std::wstring name = fd.cFileName;
    // Reports only (<prefix><digit>...): not crash-signatures.txt, which shares the prefix.
    if (name.size() > prefix_len && name[prefix_len] >= L'0' && name[prefix_len] <= L'9') {
      stems.push_back(name.substr(0, name.size() - 4)); // without ".txt"
    }
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  if (stems.size() <= kMaxReports) {
    return;
  }
  // Names are <prefix>YYYYMMDD-HHMMSS-pid[-n]: lexicographic order is chronological.
  std::sort(stems.begin(), stems.end());
  for (size_t i = 0; i + kMaxReports < stems.size(); ++i) {
    const std::wstring base = std::wstring(g_dir) + stems[i];
    DeleteFileW((base + L".txt").c_str());
    DeleteFileW((base + L".dmp").c_str());
    DeleteFileW((base + L"-guest-ram.bin").c_str());
  }
}

// --- Recurrence ---------------------------------------------------------------------------------------------------
// Signature of a crash: cause, faulting module + offset, guest pc (when the game thread faulted). Kept, one line per
// crash ("signature<TAB>date"), in crash-signatures.txt of the crash folder; read when the folder is chosen, so that
// the report can say "same signature as the crash of <date>" without any file access.
constexpr size_t kSigHistory = 16 * 1024;
char g_sig_history[kSigHistory];
size_t g_sig_history_len = 0;
char g_signature[256] = "";

void load_signatures() {
  g_sig_history_len = 0;
  const std::wstring path = std::wstring(g_dir) + L"crash-signatures.txt";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                         nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  LARGE_INTEGER size{};
  GetFileSizeEx(f, &size);
  if (size.QuadPart > static_cast<LONGLONG>(kSigHistory - 1)) {
    LARGE_INTEGER pos;
    pos.QuadPart = size.QuadPart - static_cast<LONGLONG>(kSigHistory - 1);
    SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
  }
  DWORD n = 0;
  if (ReadFile(f, g_sig_history, kSigHistory - 1, &n, nullptr)) {
    g_sig_history_len = n;
  }
  g_sig_history[g_sig_history_len] = 0;
  CloseHandle(f);
}

// Earlier crashes with the same signature: count and date of the most recent one.
int find_recurrence(const char* sig, char* date, size_t date_size) {
  int count = 0;
  date[0] = 0;
  const size_t sig_len = std::strlen(sig);
  const char* p = g_sig_history;
  const char* end = g_sig_history + g_sig_history_len;
  while (p < end) {
    const char* eol = static_cast<const char*>(std::memchr(p, '\n', static_cast<size_t>(end - p)));
    if (eol == nullptr) {
      eol = end;
    }
    const char* tab = static_cast<const char*>(std::memchr(p, '\t', static_cast<size_t>(eol - p)));
    if (tab != nullptr && static_cast<size_t>(tab - p) == sig_len && std::memcmp(p, sig, sig_len) == 0) {
      count++;
      size_t dl = static_cast<size_t>(eol - tab - 1);
      if (dl > 0 && tab[dl] == '\r') {
        dl--;
      }
      dl = std::min(dl, date_size - 1);
      std::memcpy(date, tab + 1, dl);
      date[dl] = 0;
    }
    p = eol + 1;
  }
  return count;
}

void append_signature(const SYSTEMTIME& st) {
  const std::wstring path = std::wstring(g_dir) + L"crash-signatures.txt";
  HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  char line[400];
  const int n = std::snprintf(line, sizeof(line), "%s\t%04u-%02u-%02u %02u:%02u:%02u\n", g_signature, st.wYear,
                              st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  DWORD written = 0;
  if (n > 0) {
    WriteFile(f, line, static_cast<DWORD>(n), &written, nullptr);
  }
  CloseHandle(f);
}

// --- Machine, environment, GPU: read at installation ----------------------------------------------------------------
char g_machine[2048] = "";  // lines: windows, cpu microcode, bios
char g_env_text[8192] = ""; // GCRT_* variables, one per line
char g_cmdline[4096] = "";
char g_gpu[1024] = "";
char g_exe_sha1[16] = "pending";
char g_rearm_note[160] = "";

void append_machine(const char* fmt, ...) {
  const size_t len = std::strlen(g_machine);
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_machine + len, sizeof(g_machine) - len, fmt, ap);
  va_end(ap);
}

// A registry string (REG_SZ) as UTF-8, empty if absent.
void reg_string(HKEY root, const wchar_t* key, const wchar_t* value, char* buf, size_t size) {
  wchar_t w[512];
  DWORD bytes = sizeof(w);
  buf[0] = 0;
  if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, w, &bytes) == ERROR_SUCCESS) {
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, static_cast<int>(size), nullptr, nullptr);
  }
}

void read_machine_info() {
  // Windows version (registry, SOFTWARE\Microsoft\Windows NT\CurrentVersion): ProductName, DisplayVersion,
  // CurrentBuild, UBR. Values printed as found; absent ones are left out.
  const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  char product[128], display[64], build[32];
  reg_string(HKEY_LOCAL_MACHINE, nt, L"ProductName", product, sizeof(product));
  reg_string(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion", display, sizeof(display));
  reg_string(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild", build, sizeof(build));
  DWORD ubr = 0, bytes = sizeof(ubr);
  const bool have_ubr = RegGetValueW(HKEY_LOCAL_MACHINE, nt, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &bytes) ==
                        ERROR_SUCCESS;
  append_machine("windows %s %s, build %s.%lu\n", product[0] ? product : "?", display, build[0] ? build : "?",
                 have_ubr ? static_cast<unsigned long>(ubr) : 0ul);
  // Processor microcode (HARDWARE\DESCRIPTION\System\CentralProcessor\0): "Update Revision" is the revision in use,
  // "Previous Update Revision" the one loaded by the firmware (REG_BINARY, 4 or 8 bytes). [hypothesis] With 8 bytes
  // the revision is the high 32 bits: this layout is not documented by Microsoft (it is the one read by Mozilla's
  // "Updating processor microcode" help page and by OpenJDK's os_windows_x86.cpp), so the report says "layout
  // assumed" and prints the raw bytes as well.
  const wchar_t* cpu0 = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
  const wchar_t* names[2] = {L"Update Revision", L"Previous Update Revision"};
  const char* labels[2] = {"microcode revision", "previous (firmware)"};
  for (int i = 0; i < 2; i++) {
    unsigned char raw[16] = {};
    DWORD size = sizeof(raw);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, cpu0, names[i], RRF_RT_REG_BINARY, nullptr, raw, &size) == ERROR_SUCCESS &&
        (size == 4 || size == 8)) {
      unsigned rev = 0;
      std::memcpy(&rev, raw + (size == 8 ? 4 : 0), 4);
      append_machine("%s%s 0x%X (raw", i == 0 ? "cpu " : ", ", labels[i], rev);
      for (DWORD b = 0; b < size; b++) {
        append_machine(" %02X", raw[b]);
      }
      append_machine(size == 8 ? ", layout assumed: high 32 bits)" : ")");
    } else {
      append_machine("%s%s ?", i == 0 ? "cpu " : ", ", labels[i]);
    }
  }
  char mhz_text[32] = "";
  DWORD mhz = 0;
  bytes = sizeof(mhz);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, cpu0, L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &bytes) == ERROR_SUCCESS) {
    std::snprintf(mhz_text, sizeof(mhz_text), ", nominal %lu MHz", static_cast<unsigned long>(mhz));
  }
  append_machine("%s\n", mhz_text);
  // Firmware and board (HARDWARE\DESCRIPTION\System\BIOS).
  const wchar_t* bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
  const wchar_t* bnames[6] = {L"BIOSVendor",         L"BIOSVersion",       L"BIOSReleaseDate",
                              L"SystemManufacturer", L"SystemProductName", L"BaseBoardProduct"};
  const char* blabels[6] = {"vendor", "version", "date", "system", "product", "board"};
  append_machine("bios");
  bool any = false;
  for (int i = 0; i < 6; i++) {
    char v[160];
    reg_string(HKEY_LOCAL_MACHINE, bios, bnames[i], v, sizeof(v));
    if (v[0] != 0) {
      append_machine("%s %s \"%s\"", any ? "," : "", blabels[i], v);
      any = true;
    }
  }
  append_machine("%s\n", any ? "" : " ?");
}

void read_environment() {
  wchar_t* block = GetEnvironmentStringsW();
  size_t len = 0;
  if (block != nullptr) {
    for (const wchar_t* e = block; *e != 0; e += wcslen(e) + 1) {
      if (_wcsnicmp(e, L"GCRT_", 5) != 0) {
        continue;
      }
      char line[1024];
      if (WideCharToMultiByte(CP_UTF8, 0, e, -1, line, sizeof(line), nullptr, nullptr) > 0) {
        len += static_cast<size_t>(
            std::snprintf(g_env_text + len, len < sizeof(g_env_text) ? sizeof(g_env_text) - len : 0, "  %s\n", line));
        if (len >= sizeof(g_env_text)) {
          len = sizeof(g_env_text) - 1;
          break;
        }
      }
    }
    FreeEnvironmentStringsW(block);
  }
  if (g_env_text[0] == 0) {
    std::snprintf(g_env_text, sizeof(g_env_text), "  (none)\n");
  }
  WideCharToMultiByte(CP_UTF8, 0, GetCommandLineW(), -1, g_cmdline, sizeof(g_cmdline), nullptr, nullptr);
}

const char* exception_name(DWORD code) {
  switch (code) {
  case EXCEPTION_ACCESS_VIOLATION: return "access violation";
  case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
  case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
  case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
  case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
  case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
  case EXCEPTION_INT_OVERFLOW: return "integer overflow";
  case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
  case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
  case EXCEPTION_BREAKPOINT: return "breakpoint";
  case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "non-continuable exception";
  case 0xC0000409: return "stack buffer overrun / fast fail";
  case 0xC0000374: return "heap corruption";
  case 0xE06D7363: return "C++ exception";
  case 0: return "no exception";
  default: return "?";
  }
}

// --- Modules (prepared before threads are suspended) ------------------------------------------------------------------
struct ModuleInfo {
  DWORD64 base;
  DWORD size;
  DWORD timestamp;
  char name[64];
};
constexpr int kMaxModules = 512;
ModuleInfo g_modules[kMaxModules];
int g_module_count = 0;

void snapshot_modules() {
  static HMODULE handles[kMaxModules];
  DWORD needed = 0;
  g_module_count = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed)) {
    return;
  }
  const int count = std::min<int>(kMaxModules, static_cast<int>(needed / sizeof(HMODULE)));
  for (int i = 0; i < count; i++) {
    MODULEINFO mi{};
    if (!K32GetModuleInformation(GetCurrentProcess(), handles[i], &mi, sizeof(mi))) {
      continue;
    }
    ModuleInfo& m = g_modules[g_module_count++];
    m.base = reinterpret_cast<DWORD64>(mi.lpBaseOfDll);
    m.size = mi.SizeOfImage;
    wchar_t wname[MAX_PATH];
    if (K32GetModuleBaseNameW(GetCurrentProcess(), handles[i], wname, MAX_PATH) == 0 ||
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, m.name, sizeof(m.name), nullptr, nullptr) == 0) {
      std::snprintf(m.name, sizeof(m.name), "?");
    }
    const auto* image = reinterpret_cast<const unsigned char*>(mi.lpBaseOfDll);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    m.timestamp = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew)->FileHeader.TimeDateStamp;
  }
}

// "module+0xOFF" from the module snapshot (no loader lock).
void describe_host_address(DWORD64 address, char* buf, size_t size) {
  for (int i = 0; i < g_module_count; i++) {
    const ModuleInfo& m = g_modules[i];
    if (address >= m.base && address < m.base + m.size) {
      std::snprintf(buf, size, "%s+0x%llX", m.name, static_cast<unsigned long long>(address - m.base));
      return;
    }
  }
  std::snprintf(buf, size, "(no module)");
}

void host_symbol(HANDLE process, DWORD64 address, char* buf, size_t size) {
  alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(storage);
  std::memset(storage, 0, sizeof(storage));
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 255;
  DWORD64 displacement = 0;
  char where[300];
  describe_host_address(address, where, sizeof(where));
  if (SymFromAddr(process, address, &displacement, sym)) {
    std::snprintf(buf, size, "%s (%s+0x%llX)", where, sym->Name, static_cast<unsigned long long>(displacement));
  } else {
    std::snprintf(buf, size, "%s", where);
  }
}

// --- Build and machine identification ----------------------------------------------------------------------------
extern "C" const char gcrt_build_commit[];

// First 12 hex digits of the SHA-1 of a file (BCrypt). Computed once by the reporting thread at startup.
bool file_sha1_short(const wchar_t* path, char* hex, size_t hex_size) {
  static unsigned char buf[64 * 1024];
  HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  while (ok) {
    DWORD n = 0;
    if (!ReadFile(f, buf, sizeof(buf), &n, nullptr)) {
      ok = false;
    } else if (n == 0) {
      break;
    } else {
      ok = BCryptHashData(hash, buf, n, 0) >= 0;
    }
  }
  unsigned char digest[20];
  ok = ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
  if (hash != nullptr) {
    BCryptDestroyHash(hash);
  }
  if (alg != nullptr) {
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  CloseHandle(f);
  if (!ok || hex_size < 13) {
    return false;
  }
  for (size_t i = 0; i < 6; i++) {
    std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  }
  return true;
}

// Read at installation: GetModuleFileNameW may take the loader lock, and GetActiveProcessorCount allocates on the
// process heap (seen blocked in RtlAllocateHeap from it while another thread held the heap lock).
char g_exe_name[MAX_PATH * 4] = "?";
unsigned long g_logical_cpus = 0;

void read_exe_and_cpus() {
  wchar_t wpath[MAX_PATH * 2] = L"";
  const DWORD len = GetModuleFileNameW(nullptr, wpath, MAX_PATH * 2);
  if (len > 0 && len < MAX_PATH * 2) {
    const wchar_t* file = wpath;
    for (const wchar_t* c = wpath; *c != 0; c++) {
      if (*c == L'\\' || *c == L'/') {
        file = c + 1;
      }
    }
    if (WideCharToMultiByte(CP_UTF8, 0, file, -1, g_exe_name, sizeof(g_exe_name), nullptr, nullptr) == 0) {
      std::snprintf(g_exe_name, sizeof(g_exe_name), "?");
    }
  }
  g_logical_cpus = static_cast<unsigned long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
}

void report_build_and_machine() {
  out("executable %s\n", g_exe_name);
  const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
  out("executable sha1 %s, PE time stamp 0x%08lX, image size 0x%lX, build commit %s\n", g_exe_sha1,
      static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
      static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage), gcrt_build_commit);
  int regs[4] = {};
  char brand[49] = "";
  __cpuid(regs, static_cast<int>(0x80000000u));
  if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
    for (int i = 0; i < 3; i++) {
      __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(i)));
      std::memcpy(brand + 16 * i, regs, sizeof(regs));
    }
  }
  brand[48] = 0;
  const char* name = brand;
  while (*name == ' ') {
    name++;
  }
  __cpuid(regs, 1);
  out("cpu %s, signature 0x%08X, %lu logical processors\n", *name ? name : "?", static_cast<unsigned>(regs[0]),
      g_logical_cpus);
  out("%s", g_machine);
  out("gpu %s\n", g_gpu[0] ? g_gpu : "? (not reported by the renderer yet)");
}

void report_code_bytes(DWORD64 rip) {
  constexpr int kBefore = 32, kAfter = 32, kPiece = 8;
  out("code bytes around rip (rip-%d .. rip+%d, | = rip, ?? = unreadable):\n ", kBefore, kAfter);
  for (int off = -kBefore; off < kAfter; off += kPiece) {
    unsigned char piece[kPiece];
    SIZE_T got = 0;
    const bool ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(rip + off), piece, kPiece, &got) &&
                    got == kPiece;
    out("%s", off == 0 ? " | " : " ");
    for (int i = 0; i < kPiece; i++) {
      if (ok) {
        out("%02X", piece[i]);
      } else {
        out("??");
      }
    }
  }
  out("\n");
}

// --- Memory ---------------------------------------------------------------------------------------------------------
void report_memory() {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    DWORD handles = 0;
    GetProcessHandleCount(GetCurrentProcess(), &handles);
    out("process memory: working set %.1f MiB (peak %.1f), private/commit %.1f MiB (peak %.1f), page faults %lu, "
        "handles %lu\n",
        static_cast<double>(pmc.WorkingSetSize) / 1048576.0, static_cast<double>(pmc.PeakWorkingSetSize) / 1048576.0,
        static_cast<double>(pmc.PrivateUsage) / 1048576.0, static_cast<double>(pmc.PeakPagefileUsage) / 1048576.0,
        static_cast<unsigned long>(pmc.PageFaultCount), static_cast<unsigned long>(handles));
  }
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) {
    out("system memory: %u %% used, %.0f of %.0f MiB physical available, %.0f of %.0f MiB commit available\n",
        ms.dwMemoryLoad, static_cast<double>(ms.ullAvailPhys) / 1048576.0,
        static_cast<double>(ms.ullTotalPhys) / 1048576.0, static_cast<double>(ms.ullAvailPageFile) / 1048576.0,
        static_cast<double>(ms.ullTotalPageFile) / 1048576.0);
  }
}

// Address space by region type (VirtualQuery, no allocation), plus the largest committed private regions: where the
// private bytes are. The full region list is in the minidump (MemoryInfoListStream).
void report_regions() {
  struct Big {
    DWORD64 base, size;
    DWORD protect;
  };
  constexpr int kBig = 12;
  Big big[kBig] = {};
  // [type][state]: type 0 private, 1 mapped, 2 image; state 0 committed, 1 reserved.
  DWORD64 totals[3][2] = {};
  DWORD64 counts[3][2] = {};
  DWORD64 guard_bytes = 0, exec_private = 0;
  MEMORY_BASIC_INFORMATION mbi;
  DWORD64 address = 0;
  int regions = 0;
  while (VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)) == sizeof(mbi) && regions < 1000000) {
    regions++;
    const DWORD64 base = reinterpret_cast<DWORD64>(mbi.BaseAddress);
    const DWORD64 size = mbi.RegionSize;
    if (mbi.State != MEM_FREE) {
      const int t = mbi.Type == MEM_PRIVATE ? 0 : mbi.Type == MEM_MAPPED ? 1 : 2;
      const int s = mbi.State == MEM_COMMIT ? 0 : 1;
      totals[t][s] += size;
      counts[t][s]++;
      if (mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_GUARD) != 0) {
        guard_bytes += size;
      }
      if (t == 0 && s == 0 &&
          (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0) {
        exec_private += size;
      }
      if (t == 0 && s == 0) {
        int slot = -1;
        for (int i = 0; i < kBig; i++) {
          if (slot < 0 || big[i].size < big[slot].size) {
            slot = i;
          }
        }
        if (size > big[slot].size) {
          big[slot] = Big{base, size, mbi.Protect};
        }
      }
    }
    if (base + size <= address) {
      break;
    }
    address = base + size;
  }
  static const char* const types[3] = {"private", "mapped", "image"};
  out("address space (%d regions):", regions);
  for (int t = 0; t < 3; t++) {
    out(" %s %.1f MiB committed in %llu, %.1f MiB reserved in %llu;", types[t],
        static_cast<double>(totals[t][0]) / 1048576.0, static_cast<unsigned long long>(counts[t][0]),
        static_cast<double>(totals[t][1]) / 1048576.0, static_cast<unsigned long long>(counts[t][1]));
  }
  out(" guard pages %.0f KiB; executable private %.1f MiB\n", static_cast<double>(guard_bytes) / 1024.0,
      static_cast<double>(exec_private) / 1048576.0);
  std::sort(big, big + kBig, [](const Big& a, const Big& b) { return a.size > b.size; });
  out("largest committed private regions:");
  for (int i = 0; i < kBig && big[i].size != 0; i++) {
    out("%s %016llX %.1f MiB prot 0x%lX", i % 3 == 0 ? "\n " : ";", static_cast<unsigned long long>(big[i].base),
        static_cast<double>(big[i].size) / 1048576.0, static_cast<unsigned long>(big[i].protect));
  }
  out("\n");
}

// --- Guest memory and names ---------------------------------------------------------------------------------------
bool guest_read32(u32 address, u32* value) {
  if (g_cpu == nullptr || g_cpu->state.ram == nullptr) {
    return false;
  }
  u32 offset = address;
  if (address >= 0xC0000000u) {
    offset = address - 0xC0000000u;
  } else if (address >= 0x80000000u) {
    offset = address - 0x80000000u;
  }
  if ((offset & 3u) != 0 || offset > g_cpu->state.ram_size - 4u) {
    return false;
  }
  const u8* p = g_cpu->state.ram + offset;
  *value = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
  return true;
}

void guest_name(u32 address, char* buf, size_t size) {
  u32 start = 0;
  const char* name = g_symbolizer ? g_symbolizer(address, &start) : nullptr;
  if (name == nullptr) {
    name = gcrt_hle_name_at(address, &start);
  }
  const char* module = "outside any module";
  if (g_cpu != nullptr && g_cpu->modules != nullptr) {
    for (unsigned i = 0; i < g_cpu->modules->count; i++) {
      const GcrtModule* m = g_cpu->modules->modules[i];
      if (m->find(address) != nullptr) {
        module = m->name;
        break;
      }
    }
  }
  if (name != nullptr) {
    std::snprintf(buf, size, "%08X %s+0x%X [%s]", address, name, address - start, module);
  } else {
    std::snprintf(buf, size, "%08X [%s]", address, module);
  }
}

// Guest state copied for the report: from the live state when the game thread is the faulting thread (stopped in the
// handler), otherwise while the other threads are suspended (capture_threads), so that the game thread, resumed right
// after, cannot change it while the report is composed. Plain memory copies: no heap, no lock. Names are looked up
// later (the symbolizer may allocate).
constexpr int kGuestFrames = 48;
constexpr u32 kGuestRingCopy = 64 * 1024; // newest history entries copied (more would not fit in the .txt anyway)
struct GuestSnap {
  bool valid;
  CPUState state;
  u64 ticks, blocks, deliveries;
  u32 ring_size;  // entries in the history (GCRT_GUEST_HISTORY)
  u32 ring_count; // entries filled
  u32 ring_copied;
  u32 ring[kGuestRingCopy]; // oldest to newest: the last ring_copied entries
  int frames;
  u32 frame_back[kGuestFrames];
  u32 frame_saved[kGuestFrames];
};
GuestSnap g_guest;

void snapshot_guest() {
  g_guest.valid = false;
  if (g_cpu == nullptr) {
    return;
  }
  std::memcpy(&g_guest.state, &g_cpu->state, sizeof(CPUState));
  g_guest.ticks = g_cpu->ticks;
  g_guest.blocks = g_cpu->blocks;
  g_guest.deliveries = g_cpu->deliveries;
  const u32 mask = g_cpu->ring_mask;
  const u32 pos = g_cpu->ring_pos;
  const u32 size = mask + 1u;
  g_guest.ring_size = size;
  g_guest.ring_count = pos < size ? pos : size;
  g_guest.ring_copied = g_guest.ring_count < kGuestRingCopy ? g_guest.ring_count : kGuestRingCopy;
  for (u32 i = 0; i < g_guest.ring_copied; i++) {
    g_guest.ring[i] = g_cpu->ring[(pos - g_guest.ring_copied + i) & mask];
  }
  // Guest call stack: back chain of the stack frames (raw values, named later).
  g_guest.frames = 0;
  u32 sp = g_guest.state.gpr[1];
  for (int depth = 1; depth < kGuestFrames; depth++) {
    u32 back = 0, saved = 0;
    if (!guest_read32(sp, &back) || back == 0 || back <= sp || back - sp > 0x100000u) {
      break;
    }
    if (!guest_read32(back + 4u, &saved)) {
      break;
    }
    g_guest.frame_back[g_guest.frames] = back;
    g_guest.frame_saved[g_guest.frames] = saved;
    g_guest.frames++;
    sp = back;
  }
  g_guest.valid = true;
}

// Last guest blocks: the 64 most recent with names, the older part of the history as raw addresses.
void report_guest_history() {
  const GuestSnap& g = g_guest;
  const u32 count = g.ring_copied;
  const u32 named = count < 64u ? count : 64u;
  char name[200];
  if (count > named) {
    out("older blocks (oldest to newest, %u of a %u-entry history, GCRT_GUEST_HISTORY%s):", count - named, g.ring_size,
        g.ring_copied < g.ring_count ? "; only the newest entries were copied" : "");
    for (u32 i = 0; i < count - named; i++) {
      out("%s%08X", (i % 10) ? " " : "\n  ", g.ring[i]);
    }
    out("\n");
  }
  out("last blocks run (oldest to newest):\n");
  for (u32 i = 0; i < named; i++) {
    guest_name(g.ring[count - named + i], name, sizeof(name));
    out("  %s\n", name);
  }
}

// Guest section, from g_guest (snapshot_guest must have run).
void report_guest(bool stopped) {
  if (g_cpu == nullptr || !g_guest.valid) {
    out("guest: no state\n");
    return;
  }
  const CPUState* c = &g_guest.state;
  char name[200];
  out("\n== Guest state (start of the current block: the generated code updates pc at block boundaries)%s\n",
      stopped ? "" : "\n   (the game thread was not the faulting thread: values copied while it was suspended)");
  guest_name(c->pc, name, sizeof(name));
  out("pc  %s\n", name);
  guest_name(c->lr, name, sizeof(name));
  out("lr  %s\n", name);
  out("ctr %08X cr %08X xer %08X msr %08X srr0 %08X srr1 %08X\n", c->ctr, c->cr, c->xer, c->msr, c->srr0, c->srr1);
  for (int i = 0; i < 32; i += 8) {
    out("r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, c->gpr[i], c->gpr[i + 1], c->gpr[i + 2],
        c->gpr[i + 3], c->gpr[i + 4], c->gpr[i + 5], c->gpr[i + 6], c->gpr[i + 7]);
  }
  out("guest time %.3f s (ticks %llu), blocks %llu, delivered interrupts %llu\n",
      static_cast<double>(g_guest.ticks) / static_cast<double>(GCRT_TB_HZ),
      static_cast<unsigned long long>(g_guest.ticks), static_cast<unsigned long long>(g_guest.blocks),
      static_cast<unsigned long long>(g_guest.deliveries));
  out("guest call stack (lr, then the return addresses saved in the stack frames):\n");
  guest_name(c->lr, name, sizeof(name));
  out("  #0 %s\n", name);
  for (int i = 0; i < g_guest.frames; i++) {
    guest_name(g_guest.frame_saved[i], name, sizeof(name));
    out("  #%d %s (frame %08X)\n", i + 1, name, g_guest.frame_back[i]);
  }
  report_guest_history();
}

// --- Native events ------------------------------------------------------------------------------------------------
constexpr uint32_t kTextEvents = 256; // events in the .txt; the minidump comment stream holds the whole ring

// Events recorded up to `upto` (the count read while the other threads were suspended; UINT64_MAX: all so far).
void format_events(uint32_t max_events, LARGE_INTEGER now, uint64_t upto = UINT64_MAX) {
  uint32_t capacity = 0;
  uint64_t recorded = 0;
  const GcrtDiagSlot* slots = gcrt_diag_slots(&capacity, &recorded);
  if (slots == nullptr || capacity == 0) {
    out("(event ring disabled: GCRT_DIAG_EVENTS=0)\n");
    return;
  }
  recorded = std::min<uint64_t>(recorded, upto);
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  uint64_t n = std::min<uint64_t>(recorded, capacity);
  n = std::min<uint64_t>(n, max_events);
  out("%llu events recorded, ring of %u, last %llu shown (seconds before the report, thread, event, values):\n",
      static_cast<unsigned long long>(recorded), capacity, static_cast<unsigned long long>(n));
  for (uint64_t i = recorded - n; i < recorded; i++) {
    const GcrtDiagSlot& s = slots[i & (capacity - 1u)];
    if (s.seq != i + 1u) {
      out("  (slot %llu being written or overwritten)\n", static_cast<unsigned long long>(i));
      continue;
    }
    const double ago = static_cast<double>(now.QuadPart - static_cast<LONGLONG>(s.qpc)) / static_cast<double>(freq.QuadPart);
    out("  -%9.4f %5u %-18s 0x%llX 0x%llX\n", ago, s.thread, gcrt_diag_event_name(s.kind),
        static_cast<unsigned long long>(s.a), static_cast<unsigned long long>(s.b));
  }
}

// --- Threads ------------------------------------------------------------------------------------------------------
// While the other threads are suspended (capture_threads), the reporting thread only reads their contexts and copies
// their stacks and the guest state into preallocated buffers: no heap, no lock of the C runtime, the loader or
// dbghelp. A suspended thread may hold any of those locks: a report that needed one would wait for ever for a thread
// it suspended itself (seen: the game thread suspended while holding the process heap lock, the reporting thread
// blocked in RtlAllocateHeap, the whole process frozen). Unwinding (RtlLookupFunctionEntry may take locks), naming
// and formatting come after every thread is resumed, on the copies.
// Second barrier: the suspension guard, a thread created at installation and never suspended, resumes every thread
// still suspended by the report after GCRT_CRASH_SUSPEND_LIMIT seconds (default 5); the report then says so. The
// reporting thread, the guard and the watchdog are never suspended.
constexpr int kMaxThreads = 256;
constexpr int kMaxFrames = 32;
constexpr size_t kStackCopyMax = 128 * 1024;    // per thread, from rsp towards the stack base
constexpr size_t kStackPool = 2 * 1024 * 1024;  // all threads
struct ThreadInfo {
  DWORD tid;
  HANDLE handle;
  LONG held;                 // 1 while suspended by the report; taken with InterlockedExchange (reporter and guard)
  bool have_context;
  const char* left_running;  // why the thread is never suspended (null: it is)
  int frames;
  DWORD64 frame[kMaxFrames];
  DWORD64 rsp;
  DWORD64 stack_lo;          // original address of the first copied byte (rsp)
  size_t stack_len;
  unsigned char* stack_copy;
  char name[96];
  CONTEXT ctx;               // 16-byte aligned (DECLSPEC_ALIGN in winnt.h)
};
ThreadInfo g_threads[kMaxThreads];
int g_thread_count = 0;
alignas(16) unsigned char g_stack_pool[kStackPool];

// Suspension guard.
HANDLE g_suspend_begin = nullptr;    // auto-reset: the reporting thread starts suspending threads
HANDLE g_suspend_end = nullptr;      // manual-reset: every thread it suspended is resumed
volatile LONG g_suspend_abort = 0;   // 1: the guard resumed the threads itself
DWORD g_suspend_limit_ms = 5000;     // GCRT_CRASH_SUSPEND_LIMIT (seconds)
DWORD g_guard_tid = 0;
DWORD g_watchdog_tid = 0;
// Test (GCRT_CRASH_TEST_SUSPENDED): what the reporting thread does while the other threads are suspended. 0 nothing,
// > 0 sleep that many ms, -1 a 1 MiB allocation on the process heap (the defect the guard protects against).
long g_test_suspended = 0;
// Result of the last capture.
int g_suspended_count = 0;
double g_suspended_ms = 0.0;
bool g_suspend_interrupted = false;
uint64_t g_events_upto = UINT64_MAX;

// GetThreadDescription (Windows 10 1607+), resolved at installation (no loader lock after a crash).
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
GetThreadDescriptionFn g_get_thread_description = nullptr;

void thread_name(DWORD tid, char* buf, size_t size) {
  buf[0] = 0;
  const GetThreadDescriptionFn fn = g_get_thread_description;
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (fn != nullptr && h != nullptr) {
    PWSTR desc = nullptr;
    if (SUCCEEDED(fn(h, &desc)) && desc != nullptr) {
      WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, static_cast<int>(size), nullptr, nullptr);
      LocalFree(desc);
    }
  }
  if (h != nullptr) {
    CloseHandle(h);
  }
}

// Unwinds a context with the x64 unwind tables (RtlLookupFunctionEntry / RtlVirtualUnwind): no allocation, guarded
// against a corrupt stack. Returns the number of frames (the first one is rip). With hi != 0, the stack is a copy
// spanning [lo, hi): the walk stops when rsp leaves it.
int unwind_stack(const CONTEXT* start, DWORD64* frames, int max, DWORD64 lo = 0, DWORD64 hi = 0) {
  static CONTEXT c; // only the reporting thread unwinds
  c = *start;
  int n = 0;
  __try {
    while (n < max && c.Rip != 0) {
      frames[n++] = c.Rip;
      if (hi != 0 && (c.Rsp < lo || c.Rsp + 8 > hi)) {
        break;
      }
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
      if (fn == nullptr) {
        // Leaf function without unwind data: return address at [rsp].
        c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
        c.Rsp += 8;
      } else {
        PVOID handler_data = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fn, &c, &handler_data, &establisher, nullptr);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

// Lists the threads of the process and their names (before any suspension: names need the heap). The reporting thread
// (`self`) is left out; the guard and the watchdog are listed but never suspended.
void snapshot_threads(DWORD self) {
  g_thread_count = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) {
    return;
  }
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  const DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(snap, &te); ok && g_thread_count < kMaxThreads; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
      continue;
    }
    ThreadInfo& t = g_threads[g_thread_count];
    std::memset(&t, 0, sizeof(t));
    t.tid = te.th32ThreadID;
    t.left_running = t.tid == g_guard_tid      ? "suspension guard"
                     : t.tid == g_watchdog_tid ? "watchdog"
                                               : nullptr;
    t.handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, t.tid);
    thread_name(t.tid, t.name, sizeof(t.name));
    g_thread_count++; // after the entry is complete: the guard reads the list
  }
  CloseHandle(snap);
}

bool guarded_copy(void* dst, const void* src, size_t n) {
  __try {
    std::memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Copies the used part of a suspended thread's stack (rsp up to the top of its committed region, at most
// kStackCopyMax) into the pool. VirtualQuery and a copy: no heap, no lock.
void copy_stack(ThreadInfo& t, size_t* pool_used) {
  t.stack_len = 0;
  MEMORY_BASIC_INFORMATION mbi;
  const DWORD64 rsp = t.ctx.Rsp;
  if (VirtualQuery(reinterpret_cast<LPCVOID>(rsp), &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return;
  }
  const DWORD64 top = reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize;
  size_t len = static_cast<size_t>(std::min<DWORD64>(top - rsp, kStackCopyMax));
  len = std::min(len, kStackPool - *pool_used) & ~static_cast<size_t>(7);
  if (len == 0) {
    return;
  }
  unsigned char* dst = g_stack_pool + *pool_used;
  if (!guarded_copy(dst, reinterpret_cast<const void*>(rsp), len)) {
    return;
  }
  t.stack_lo = rsp;
  t.stack_len = len;
  t.stack_copy = dst;
  *pool_used += (len + 15) & ~static_cast<size_t>(15);
}

// Resumes every thread still suspended by the report (also called by the guard: each thread is resumed once).
void resume_threads() {
  const int count = g_thread_count;
  for (int i = 0; i < count; i++) {
    ThreadInfo& t = g_threads[i];
    if (InterlockedExchange(&t.held, 0) != 0) {
      ResumeThread(t.handle);
    }
  }
}

DWORD WINAPI guard_main(LPVOID) {
  for (;;) {
    if (WaitForSingleObject(g_suspend_begin, INFINITE) != WAIT_OBJECT_0) {
      return 0;
    }
    if (WaitForSingleObject(g_suspend_end, g_suspend_limit_ms) == WAIT_OBJECT_0) {
      continue;
    }
    // The reporting thread is stuck while threads are suspended: give them back. Nothing here allocates.
    InterlockedExchange(&g_suspend_abort, 1);
    resume_threads();
  }
}

// Suspends every listed thread but `faulting` (0: none) and the ones left running, copies each context and stack, and
// the guest state when `copy_guest` (the game thread is not stopped in the handler), then resumes them all.
void capture_threads(DWORD faulting, bool copy_guest) {
  LARGE_INTEGER t0, t1, freq;
  QueryPerformanceCounter(&t0);
  size_t pool_used = 0;
  int suspended = 0;
  InterlockedExchange(&g_suspend_abort, 0);
  if (g_suspend_end != nullptr) {
    ResetEvent(g_suspend_end);
  }
  if (g_suspend_begin != nullptr) {
    SetEvent(g_suspend_begin);
  }
  for (int i = 0; i < g_thread_count; i++) {
    ThreadInfo& t = g_threads[i];
    if (t.handle == nullptr || t.tid == faulting || t.left_running != nullptr) {
      continue;
    }
    if (InterlockedCompareExchange(&g_suspend_abort, 0, 0) != 0) {
      break;
    }
    if (SuspendThread(t.handle) == static_cast<DWORD>(-1)) {
      continue;
    }
    InterlockedExchange(&t.held, 1);
    if (InterlockedCompareExchange(&g_suspend_abort, 0, 0) != 0) {
      if (InterlockedExchange(&t.held, 0) != 0) {
        ResumeThread(t.handle);
      }
      break;
    }
    suspended++;
    t.ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(t.handle, &t.ctx)) { // also waits until the suspension is effective
      t.have_context = true;
      t.rsp = t.ctx.Rsp;
      copy_stack(t, &pool_used);
    }
  }
  if (copy_guest) {
    snapshot_guest();
  }
  {
    uint32_t capacity = 0;
    uint64_t recorded = 0;
    gcrt_diag_slots(&capacity, &recorded);
    g_events_upto = recorded;
  }
  if (g_test_suspended > 0) {
    Sleep(static_cast<DWORD>(g_test_suspended));
  } else if (g_test_suspended < 0) {
    HeapFree(GetProcessHeap(), 0, HeapAlloc(GetProcessHeap(), 0, 1u << 20));
  }
  resume_threads();
  // Read before the guard is told: a guard waking up after this point finds nothing left to resume.
  g_suspend_interrupted = InterlockedCompareExchange(&g_suspend_abort, 0, 0) != 0;
  if (g_suspend_end != nullptr) {
    SetEvent(g_suspend_end);
  }
  QueryPerformanceCounter(&t1);
  QueryPerformanceFrequency(&freq);
  g_suspended_count = suspended;
  g_suspended_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// Unwinds the copied stack of a captured thread (after the threads are resumed). Values of the copy and registers that
// point into the original stack range are moved into the copy first, so that frame pointers and saved rsp follow it.
void unwind_copy(ThreadInfo& t) {
  t.frames = 0;
  if (!t.have_context) {
    return;
  }
  if (t.stack_len == 0) {
    t.frame[0] = t.ctx.Rip;
    t.frames = 1;
    return;
  }
  const DWORD64 lo = t.stack_lo;
  const DWORD64 hi = lo + t.stack_len;
  const DWORD64 delta = reinterpret_cast<DWORD64>(t.stack_copy) - lo;
  auto* words = reinterpret_cast<DWORD64*>(t.stack_copy);
  for (size_t i = 0; i < t.stack_len / 8; i++) {
    if (words[i] >= lo && words[i] < hi) {
      words[i] += delta;
    }
  }
  static CONTEXT c;
  c = t.ctx;
  DWORD64* regs[] = {&c.Rax, &c.Rcx, &c.Rdx, &c.Rbx, &c.Rsp, &c.Rbp, &c.Rsi, &c.Rdi,
                     &c.R8,  &c.R9,  &c.R10, &c.R11, &c.R12, &c.R13, &c.R14, &c.R15};
  for (DWORD64* r : regs) {
    if (*r >= lo && *r < hi) {
      *r += delta;
    }
  }
  t.frames = unwind_stack(&c, t.frame, kMaxFrames, reinterpret_cast<DWORD64>(t.stack_copy),
                          reinterpret_cast<DWORD64>(t.stack_copy) + t.stack_len);
}

void close_threads() {
  for (int i = 0; i < g_thread_count; i++) {
    if (g_threads[i].handle != nullptr) {
      CloseHandle(g_threads[i].handle);
      g_threads[i].handle = nullptr;
    }
  }
}

void report_frames(const DWORD64* frames, int n) {
  char where[300];
  for (int i = 0; i < n; i++) {
    describe_host_address(frames[i], where, sizeof(where));
    out("    #%-2d %016llX %s\n", i, static_cast<unsigned long long>(frames[i]), where);
  }
}

void report_threads(DWORD faulting) {
  out("\n== Threads at the time of the report (%d besides the reporting thread; raw unwind of a copy of each stack, "
      "module + offset)\n",
      g_thread_count);
  out("%d threads suspended for %.2f ms while their contexts and stacks were copied\n", g_suspended_count,
      g_suspended_ms);
  if (g_suspend_interrupted) {
    out("note: the suspension was interrupted: the threads stayed suspended more than %lu ms "
        "(GCRT_CRASH_SUSPEND_LIMIT) and the suspension guard resumed them; the states below may be incomplete or "
        "inconsistent\n",
        static_cast<unsigned long>(g_suspend_limit_ms));
  }
  for (int i = 0; i < g_thread_count; i++) {
    const ThreadInfo& t = g_threads[i];
    out("thread %lu \"%s\"%s%s\n", static_cast<unsigned long>(t.tid), t.name,
        t.tid == g_game_thread ? " (game thread)" : "", t.tid == faulting ? " (faulting thread, see above)" : "");
    if (t.tid == faulting) {
      continue;
    }
    if (t.left_running != nullptr) {
      out("    (not suspended: %s)\n", t.left_running);
      continue;
    }
    if (!t.have_context) {
      out("    (context unavailable)\n");
      continue;
    }
    out("    rsp %016llX%s\n", static_cast<unsigned long long>(t.rsp),
        t.stack_len == 0 ? " (stack not copied: first frame only)" : "");
    report_frames(t.frame, t.frames);
  }
}

// --- Report -------------------------------------------------------------------------------------------------------
enum class Cause { Exception, Abort, Terminate, PureCall, InvalidParameter, Hang };

const char* cause_name(Cause c) {
  switch (c) {
  case Cause::Exception: return "unhandled exception";
  case Cause::Abort: return "abort()";
  case Cause::Terminate: return "std::terminate";
  case Cause::PureCall: return "pure virtual function call";
  case Cause::InvalidParameter: return "invalid parameter passed to a C runtime function";
  case Cause::Hang: return "hang (watchdog)";
  }
  return "?";
}

struct CrashInput {
  EXCEPTION_POINTERS* info; // null for a hang
  DWORD thread;              // faulting thread (hang: game thread)
  Cause cause;
  unsigned hang_seconds;
  wchar_t stem[MAX_PATH * 2];
};
CrashInput g_in;
bool g_hang_in_startup = false; // set by the watchdog before a hang report: the start-up stopped, not the game loop
HANDLE g_request = nullptr; // auto-reset: a report is requested
HANDLE g_done = nullptr;    // auto-reset: the report is finished
DWORD g_reporter_tid = 0;
alignas(16) CONTEXT g_capture_ctx; // context captured by abort/terminate/purecall/invalid parameter
EXCEPTION_RECORD g_capture_rec;
EXCEPTION_POINTERS g_capture_ptrs;

void write_file(const wchar_t* path, const void* data, size_t size) {
  HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return;
  }
  const char* p = static_cast<const char*>(data);
  while (size > 0) {
    DWORD chunk = size > (1u << 30) ? (1u << 30) : static_cast<DWORD>(size);
    DWORD written = 0;
    if (!WriteFile(f, p, chunk, &written, nullptr) || written == 0) {
      break;
    }
    p += written;
    size -= written;
  }
  CloseHandle(f);
}

// Opens <dir><prefix><date>-<pid>[-n].txt without overwriting. First without the heap: NtCreateFile relative to the
// handle on the crash folder opened in advance (FILE_CREATE: fails on an existing name). Then by path (CreateFileW,
// which may allocate): the crash folder, then the temporary folder.
bool open_report_file(const wchar_t* prefix, const SYSTEMTIME& st) {
  if (g_dir_handle != INVALID_HANDLE_VALUE && g_nt_create_file != nullptr) {
    for (int n = 1; n <= 9; n++) {
      wchar_t suffix[8] = L"";
      if (n > 1) {
        swprintf_s(suffix, L"-%d", n);
      }
      static wchar_t name[96];
      swprintf_s(name, L"%s%04u%02u%02u-%02u%02u%02u-%lu%s.txt", prefix, st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond, GetCurrentProcessId(), suffix);
      UNICODE_STRING us;
      us.Buffer = name;
      us.Length = static_cast<USHORT>(wcslen(name) * sizeof(wchar_t));
      us.MaximumLength = static_cast<USHORT>(sizeof(name));
      OBJECT_ATTRIBUTES oa{};
      oa.Length = sizeof(oa);
      oa.RootDirectory = g_dir_handle;
      oa.ObjectName = &us;
      oa.Attributes = OBJ_CASE_INSENSITIVE;
      IO_STATUS_BLOCK io{};
      HANDLE h = nullptr;
      const NTSTATUS status = g_nt_create_file(&h, FILE_GENERIC_WRITE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                               FILE_SHARE_READ, FILE_CREATE,
                                               FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
      if (status >= 0) {
        g_txt = h;
        name[wcslen(name) - 4] = 0; // without ".txt"
        swprintf_s(g_in.stem, L"%s%s", g_dir, name);
        return true;
      }
      if (status != kStatusNameCollision) {
        break;
      }
    }
  }
  const wchar_t* dirs[2] = {g_dir, g_temp_dir};
  for (const wchar_t* dir : dirs) {
    if (dir[0] == 0) {
      continue;
    }
    CreateDirectoryW(dir, nullptr);
    for (int n = 1; n <= 9; n++) {
      wchar_t suffix[8] = L"";
      if (n > 1) {
        swprintf_s(suffix, L"-%d", n);
      }
      swprintf_s(g_in.stem, L"%s%s%04u%02u%02u-%02u%02u%02u-%lu%s", dir, prefix, st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId(), suffix);
      wchar_t path[MAX_PATH * 2 + 32];
      swprintf_s(path, L"%s.txt", g_in.stem);
      g_txt = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (g_txt != INVALID_HANDLE_VALUE) {
        return true;
      }
      if (GetLastError() != ERROR_FILE_EXISTS) {
        break;
      }
    }
  }
  g_in.stem[0] = 0;
  return false;
}

// Phase 2 helpers guarded by SEH: a fault there (corrupt heap, dbghelp) must not lose what is already written.
bool guarded_minidump(HANDLE process, HANDLE dump, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* mei,
                      MINIDUMP_USER_STREAM_INFORMATION* user) {
  __try {
    return MiniDumpWriteDump(process, GetCurrentProcessId(), dump, type, mei, user, nullptr) != FALSE;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void symbolized_stack(HANDLE process, const CONTEXT* ctx, DWORD tid) {
  CONTEXT walk = *ctx;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = walk.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrStack.Offset = walk.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = walk.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
  char where[400];
  for (int i = 0; i < 64; i++) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread ? thread : GetCurrentThread(), &frame, &walk, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
        frame.AddrPC.Offset == 0) {
      break;
    }
    host_symbol(process, frame.AddrPC.Offset, where, sizeof(where));
    out("  #%d %016llX %s\n", i, static_cast<unsigned long long>(frame.AddrPC.Offset), where);
  }
  if (thread != nullptr) {
    CloseHandle(thread);
  }
}

bool guarded_symbolized_stack(HANDLE process, const CONTEXT* ctx, DWORD tid) {
  __try {
    symbolized_stack(process, ctx, tid);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Full event list for the minidump comment stream (formatted in phase 2).
char g_events_full[1024 * 1024];
size_t g_events_full_len = 0;

void do_report() {
  const bool hang = g_in.cause == Cause::Hang;
  const HANDLE process = GetCurrentProcess();
  const EXCEPTION_RECORD* rec = g_in.info != nullptr ? g_in.info->ExceptionRecord : nullptr;
  const CONTEXT* ctx = g_in.info != nullptr ? g_in.info->ContextRecord : nullptr;
  SYSTEMTIME st;
  GetLocalTime(&st);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  g_report_len = 0;
  g_report_flushed = 0;
  g_phase1_done = 0;

  // Header, without the heap (the faulting thread may hold its lock): file created relative to the folder handle,
  // text in the fixed buffer, host addresses described with the module list of the startup (refreshed below).
  open_report_file(hang ? L"hang-" : L"crash-", st);
  out("== gcrt %s report\n", hang ? "hang" : "crash");
  out("time %04u-%02u-%02u %02u:%02u:%02u, process %lu, cause: %s\n", st.wYear, st.wMonth, st.wDay, st.wHour,
      st.wMinute, st.wSecond, GetCurrentProcessId(), cause_name(g_in.cause));
  char where[400] = "";
  DWORD64 fault_address = 0;
  if (rec != nullptr) {
    fault_address = reinterpret_cast<DWORD64>(rec->ExceptionAddress);
    out("exception 0x%08lX (%s), flags 0x%lX\n", rec->ExceptionCode, exception_name(rec->ExceptionCode),
        rec->ExceptionFlags);
    describe_host_address(fault_address, where, sizeof(where));
    out("at %p = %s\n", rec->ExceptionAddress, where);
    if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        rec->NumberParameters >= 2) {
      const ULONG_PTR kind = rec->ExceptionInformation[0];
      out("%s of address 0x%016llX%s\n", kind == 0 ? "read" : kind == 1 ? "write" : kind == 8 ? "execution" : "access",
          static_cast<unsigned long long>(rec->ExceptionInformation[1]),
          rec->ExceptionInformation[1] == ~static_cast<ULONG_PTR>(0)
              ? " (Windows gives -1 for a general protection fault: non-canonical pointer or misaligned SSE access;"
                " decode the instruction with tools/crash_inspect.py)"
              : "");
    }
  } else {
    out("no exception: the %s made no progress for %u s (GCRT_WATCHDOG); the game keeps running\n",
        g_hang_in_startup ? "start-up (window creation, setup menu, before the game loop)" : "game loop",
        g_in.hang_seconds);
  }
  const bool game_faulted = !hang && g_in.thread == g_game_thread;
  out("%s thread id %lu%s\n", hang ? "watched" : "faulting", static_cast<unsigned long>(g_in.thread),
      g_in.thread == g_game_thread ? " (game thread)" : "");
  if (ctx != nullptr) {
    out("rip %016llX rsp %016llX rbp %016llX\n", ctx->Rip, ctx->Rsp, ctx->Rbp);
    out("rax %016llX rbx %016llX rcx %016llX rdx %016llX\n", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    out("rsi %016llX rdi %016llX r8  %016llX r9  %016llX\n", ctx->Rsi, ctx->Rdi, ctx->R8, ctx->R9);
    out("r10 %016llX r11 %016llX r12 %016llX r13 %016llX\n", ctx->R10, ctx->R11, ctx->R12, ctx->R13);
    out("r14 %016llX r15 %016llX eflags %08lX mxcsr %08lX\n", ctx->R14, ctx->R15,
        static_cast<unsigned long>(ctx->EFlags), static_cast<unsigned long>(ctx->MxCsr));
    report_code_bytes(ctx->Rip);
    static DWORD64 frames[64];
    const int n = unwind_stack(ctx, frames, 64);
    out("host call stack (raw unwind, module + offset):\n");
    report_frames(frames, n);
  }
  if (game_faulted) {
    snapshot_guest(); // the game thread is the faulting thread: stopped in the handler
    report_guest(true);
  }
  flush_text();

  // Phase 0 (may allocate): module and thread snapshots. The header above is already on disk if this blocks.
  snapshot_modules();
  snapshot_threads(GetCurrentThreadId());
  // Phase 1: the other threads suspended only while their contexts, stacks and the guest state are copied (no heap,
  // no lock; bounded by the suspension guard), then resumed.
  capture_threads(hang ? 0 : g_in.thread, !game_faulted);
  for (int i = 0; i < g_thread_count; i++) {
    unwind_copy(g_threads[i]);
  }
  // Phase 1 continued (threads running again; still no allocation: after a crash the faulting thread may hold the
  // heap lock): the rest of the text report.
  out("\n== Process and machine\n");
  if (rec != nullptr) {
    describe_host_address(fault_address, where, sizeof(where)); // with the refreshed module list
  }
  // Recurrence.
  if (!hang) {
    const bool guest_known = g_cpu != nullptr && g_in.thread == g_game_thread;
    std::snprintf(g_signature, sizeof(g_signature), "%s|%08lX|%s|%08X", cause_name(g_in.cause),
                  rec != nullptr ? static_cast<unsigned long>(rec->ExceptionCode) : 0ul, where,
                  guest_known ? g_cpu->state.pc : 0u);
    char date[64];
    const int previous = find_recurrence(g_signature, date, sizeof(date));
    if (previous > 0) {
      out("recurrence: same signature as the crash of %s (%d earlier crash%s with this signature in this folder)\n",
          date, previous, previous > 1 ? "es" : "");
    } else {
      out("recurrence: first crash with this signature in this folder\n");
    }
    out("signature %s\n", g_signature);
  }
  report_build_and_machine();
  char tname[96] = "";
  for (int i = 0; i < g_thread_count; i++) {
    if (g_threads[i].tid == g_in.thread) {
      std::snprintf(tname, sizeof(tname), "%s", g_threads[i].name);
    }
  }
  out("thread %lu \"%s\"%s\n", g_in.thread, tname, g_in.thread == g_game_thread ? " (game thread)" : "");
  report_memory();
  report_regions();
  if (g_rearm_note[0] != 0) {
    out("%s\n", g_rearm_note);
  }
  flush_text();
  if (!game_faulted) {
    report_guest(g_in.thread == g_game_thread);
    flush_text();
  }
  report_threads(hang ? 0 : g_in.thread);
  flush_text();
  out("\n== Recent native events\n");
  format_events(kTextEvents, now, g_events_upto);
  flush_text();
  out("\n== Recent log lines (trace output, oldest first)\n");
  {
    static char tail[16 * 1024];
    gcrt_trace_tail(tail, sizeof(tail));
    out("%s", tail);
  }
  out("\n== Environment\ncommand line: %s\nGCRT_* variables:\n%s", g_cmdline, g_env_text);
  {
    // The folder this report was actually written to (the temporary folder when the crash folder failed), from the
    // file stem; its parent folders are reduced to "<path>" by the scrubbing, like every absolute path.
    static wchar_t used[MAX_PATH * 2];
    wcsncpy_s(used, g_in.stem, _TRUNCATE);
    wchar_t* sep = wcsrchr(used, L'\\');
    wchar_t* slash = wcsrchr(used, L'/');
    if (sep == nullptr || (slash != nullptr && slash > sep)) {
      sep = slash;
    }
    if (sep != nullptr) {
      *sep = 0;
    }
    char dir_utf8[MAX_PATH * 4] = "";
    WideCharToMultiByte(CP_UTF8, 0, used, -1, dir_utf8, sizeof(dir_utf8), nullptr, nullptr);
    const bool temp = used[0] != 0 && g_temp_dir[0] != 0 && wcsncmp(g_in.stem, g_temp_dir, wcslen(g_temp_dir)) == 0;
    out("report folder: %s%s\n", used[0] != 0 ? dir_utf8 : "(none: standard error only)",
        temp ? " (temporary folder)" : "");
  }
  if (g_context_fn != nullptr) {
    out("\n== Platform\n");
    g_context_fn(g_context_user, append_text, nullptr);
  }
  flush_text();
  g_phase1_done = 1;
  if (hang) {
    // Activable trace (GCRT_TRACE platform), once the text is on disk (it may allocate). Hang reports only: after a
    // crash, the faulting thread may hold a lock of the trace output.
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "hang report: %d threads suspended for %.2f ms%s", g_suspended_count,
                      g_suspended_ms, g_suspend_interrupted ? ", interrupted by the suspension guard" : "");
  }

  // Phase 2 (may allocate; guarded): symbolized stack, modules, minidump, guest memory, signature history.
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitialize(process, nullptr, TRUE); // may fail if already initialized (host profiler): symbols still usable
  if (ctx != nullptr) {
    out("\n== Host call stack of the faulting thread (dbghelp, function names when a PDB is present)\n");
    if (!guarded_symbolized_stack(process, ctx, g_in.thread)) {
      out("  (stack walk faulted)\n");
    }
    flush_text();
  }
  out("\n== Modules (name, base, size, PE time stamp)\n");
  for (int i = 0; i < g_module_count; i++) {
    const ModuleInfo& m = g_modules[i];
    out("  %-28s %016llX %8lX %08lX\n", m.name, static_cast<unsigned long long>(m.base),
        static_cast<unsigned long>(m.size), static_cast<unsigned long>(m.timestamp));
  }
  flush_text();
  close_threads();

  if (g_in.stem[0] != 0) {
    // Comment stream of the minidump: the whole event ring (the .txt holds the last kTextEvents).
    g_events_full_len = 0;
    g_out_buf = g_events_full;
    g_out_cap = sizeof(g_events_full);
    g_out_len = &g_events_full_len;
    format_events(0xFFFFFFFFu, now);
    g_out_buf = g_report;
    g_out_cap = kReportSize;
    g_out_len = &g_report_len;
    const size_t full_len = g_events_full_len;

    wchar_t path[MAX_PATH * 2 + 32];
    swprintf_s(path, L"%s.dmp", g_in.stem);
    HANDLE dump = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumped = false;
    DWORD dump_error = 0;
    LARGE_INTEGER dump_size{};
    if (dump != INVALID_HANDLE_VALUE) {
      MINIDUMP_EXCEPTION_INFORMATION mei{};
      mei.ThreadId = g_in.thread;
      mei.ExceptionPointers = g_in.info;
      mei.ClientPointers = FALSE;
      MINIDUMP_USER_STREAM stream{};
      stream.Type = CommentStreamA;
      stream.BufferSize = static_cast<ULONG>(full_len + 1);
      stream.Buffer = g_events_full;
      MINIDUMP_USER_STREAM_INFORMATION user{1, &stream};
      // MINIDUMP_TYPE (Microsoft Learn, minidumpapiset.h): FullMemoryInfo = region list
      // (MINIDUMP_MEMORY_INFO_LIST), ThreadInfo = thread state (MINIDUMP_THREAD_INFO_LIST).
      const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithFullMemoryInfo |
                                                   MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules |
                                                   MiniDumpWithHandleData | MiniDumpWithProcessThreadData);
      dumped = guarded_minidump(process, dump, type, g_in.info != nullptr ? &mei : nullptr, &user);
      if (!dumped) {
        dump_error = GetLastError();
      }
      GetFileSizeEx(dump, &dump_size);
      CloseHandle(dump);
    } else {
      dump_error = GetLastError();
    }
    out("\nminidump: %s", dumped ? "written" : "FAILED");
    if (dumped) {
      out(" (%.2f MiB, with the region list, thread information and the whole event ring)\n",
          static_cast<double>(dump_size.QuadPart) / 1048576.0);
    } else {
      out(" (error %lu)\n", static_cast<unsigned long>(dump_error));
    }
    flush_text();
    if (!hang && g_cpu != nullptr && g_cpu->state.ram != nullptr) {
      swprintf_s(path, L"%s-guest-ram.bin", g_in.stem);
      write_file(path, g_cpu->state.ram, g_cpu->state.ram_size);
      out("guest memory: %u bytes from 0x80000000\n", g_cpu->state.ram_size);
    }
    if (!hang && g_dir[0] != 0) {
      append_signature(st);
    }
  } else {
    out("\nreport folder not writable: text report on stderr only, no minidump\n");
  }
  out("== end of report\n");
  flush_text();
  if (g_txt != INVALID_HANDLE_VALUE) {
    CloseHandle(g_txt);
    g_txt = INVALID_HANDLE_VALUE;
  }
  char where_files[MAX_PATH * 2];
  WideCharToMultiByte(CP_UTF8, 0, g_in.stem, -1, where_files, sizeof(where_files), nullptr, nullptr);
  std::fprintf(stderr, "gcrt: %s report written: %s.txt (.dmp%s)\n", hang ? "hang" : "crash", where_files,
               hang ? "" : ", -guest-ram.bin");
  std::fflush(stderr);
}

DWORD WINAPI reporter_main(LPVOID) {
  // Executable fingerprint, once, at low priority (reads the file).
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
  wchar_t wpath[MAX_PATH * 2] = L"";
  const DWORD len = GetModuleFileNameW(nullptr, wpath, MAX_PATH * 2);
  char sha[16];
  if (len > 0 && len < MAX_PATH * 2 && file_sha1_short(wpath, sha, sizeof(sha))) {
    std::memcpy(g_exe_sha1, sha, sizeof(sha));
  } else {
    std::snprintf(g_exe_sha1, sizeof(g_exe_sha1), "?");
  }
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  for (;;) {
    if (WaitForSingleObject(g_request, INFINITE) != WAIT_OBJECT_0) {
      return 0;
    }
    do_report();
    const bool hang = g_in.cause == Cause::Hang;
    SetEvent(g_done); // before g_reporting is released: the next report resets g_done once it owns g_reporting
    if (hang) {
      InterlockedExchange(&g_reporting, 0); // the game keeps running: later hangs or crashes can be reported
    }
  }
}

constexpr int kReportWaitMs = 60000; // bound of every wait on the reporting thread

// Called by the faulting thread (or the watchdog): hands the work to the reporting thread and waits (bounded).
void run_report(EXCEPTION_POINTERS* info, Cause cause, DWORD thread, unsigned hang_seconds) {
  const LONG want = cause == Cause::Hang ? 2 : 1;
  if (GetCurrentThreadId() == g_reporter_tid) {
    return; // a fault inside the reporting thread itself: what was written stays
  }
  LONG prev = InterlockedCompareExchange(&g_reporting, want, 0);
  if (prev == 2 && want == 1) {
    // A hang report is being written: wait for it (bounded), then report the crash.
    for (int i = 0; i < kReportWaitMs / 100 && (prev = InterlockedCompareExchange(&g_reporting, want, 0)) != 0; i++) {
      Sleep(100);
    }
    if (prev == 2) {
      // The reporting thread is still blocked in the hang report: this crash cannot be reported.
      static const char kMsg[] = "\ngcrt: crash not reported: the reporting thread is blocked in a hang report\n";
      DWORD written = 0;
      HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
      if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        WriteFile(err, kMsg, sizeof(kMsg) - 1, &written, nullptr);
      }
      return;
    }
  }
  if (prev != 0) {
    // Another crash is being reported: wait for it to finish so that the process does not end under it.
    if (want == 1) {
      for (int i = 0; i < kReportWaitMs / 100 && g_reporting != 0 && !g_crash_finished; i++) {
        Sleep(100);
      }
    }
    return;
  }
  g_in.info = info;
  g_in.thread = thread;
  g_in.cause = cause;
  g_in.hang_seconds = hang_seconds;
  if (g_request != nullptr && g_done != nullptr) {
    ResetEvent(g_done); // a late end of an earlier report (after its wait expired) must not end this wait
    SetEvent(g_request);
    if (WaitForSingleObject(g_done, kReportWaitMs) != WAIT_OBJECT_0 && !g_phase1_done) {
      // The reporting thread did not finish phase 1 (blocked, e.g. on a lock held by the faulting thread): what is
      // not written yet goes to stderr (unscrubbed), and the .txt says that it stops there. No heap used here.
      static const char kMsg[] = "\ngcrt: report incomplete: the reporting thread was blocked here for 60 s; any text "
                                 "below was written after that\n";
      DWORD written = 0;
      HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
      if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        const size_t flushed = g_report_flushed;
        const size_t len = g_report_len;
        if (len > flushed) {
          WriteFile(err, g_report + flushed, static_cast<DWORD>(len - flushed), &written, nullptr);
        }
        WriteFile(err, kMsg, sizeof(kMsg) - 1, &written, nullptr);
      }
      if (g_txt != INVALID_HANDLE_VALUE) {
        WriteFile(g_txt, kMsg, sizeof(kMsg) - 1, &written, nullptr);
        FlushFileBuffers(g_txt);
      }
    }
  } else {
    do_report(); // no reporting thread (creation failed at installation): best effort on this stack
    if (cause == Cause::Hang) {
      InterlockedExchange(&g_reporting, 0);
    }
  }
  if (cause != Cause::Hang) {
    InterlockedExchange(&g_crash_finished, 1);
  }
  // A hang report releases g_reporting in the reporting thread once really finished (reporter_main): if that thread
  // is still blocked after the wait above, no other report starts over it.
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
  run_report(info, Cause::Exception, GetCurrentThreadId(), 0);
  return EXCEPTION_CONTINUE_SEARCH; // default handling: the process ends with the exception code
}

// Non-exception causes: the context of the calling thread is captured here (one at a time: g_reporting guards it).
__declspec(noinline) void report_here(Cause cause) {
  if (g_reporting != 0) {
    run_report(nullptr, cause, GetCurrentThreadId(), 0); // waits for the report in progress
    return;
  }
  RtlCaptureContext(&g_capture_ctx);
  std::memset(&g_capture_rec, 0, sizeof(g_capture_rec));
  g_capture_rec.ExceptionCode = 0;
  g_capture_rec.ExceptionAddress = reinterpret_cast<void*>(g_capture_ctx.Rip);
  g_capture_ptrs.ExceptionRecord = &g_capture_rec;
  g_capture_ptrs.ContextRecord = &g_capture_ctx;
  run_report(&g_capture_ptrs, cause, GetCurrentThreadId(), 0);
}

void on_abort(int) { report_here(Cause::Abort); }

[[noreturn]] void on_terminate() {
  report_here(Cause::Terminate);
  std::abort(); // default behavior of std::terminate (the abort handler sees a report in progress and returns)
}

void __cdecl on_purecall() {
  report_here(Cause::PureCall);
  // Returning lets the C runtime end the process as it does without a handler.
}

void __cdecl on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  report_here(Cause::InvalidParameter);
  // Default behavior of the release C runtime: the process ends (fast fail, invalid argument).
  __fastfail(FAST_FAIL_INVALID_ARG);
}

// --- Hang watchdog ------------------------------------------------------------------------------------------------
unsigned g_watchdog_seconds = 0;
unsigned g_startup_watchdog_seconds = 0; // before the game loop: GCRT_WATCHDOG_STARTUP, else max(GCRT_WATCHDOG, 60)
constexpr int kMaxHangReports = 3;

DWORD WINAPI watchdog_main(LPVOID) {
  uint64_t last = 0;
  bool lastStartup = true;
  unsigned still = 0;
  bool reported = false;
  int reports = 0;
  for (;;) {
    // Counting sleeps, not clock time: after a system sleep or hibernation only one more tick has passed.
    Sleep(1000);
    // Before the game loop beats: the start-up beat (window creation, setup menu passes, gcrt_diag.h), armed when
    // the platform opens the window; before that (boot), nothing is watched.
    const uint64_t loop = g_gcrt_heartbeat;
    const bool startup = loop == 0;
    const uint64_t beat = startup ? g_gcrt_startup_beat : loop;
    if (beat == 0) {
      continue;
    }
    if (beat != last || startup != lastStartup) {
      if (reported) {
        gcrt_trace_printf(GCRT_TRACE_PLATFORM, "watchdog: the %s progresses again after %u s",
                          lastStartup ? "start-up" : "game loop", still);
      }
      last = beat;
      lastStartup = startup;
      still = 0;
      reported = false;
      continue;
    }
    still++;
    if (still == (startup ? g_startup_watchdog_seconds : g_watchdog_seconds) && !reported) {
      reported = true;
      if (IsDebuggerPresent()) {
        continue; // a breakpoint is not a hang
      }
      gcrt_diag_event(GCRT_EV_WATCHDOG, still, startup ? 1u : 0u);
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "watchdog: no progress of the %s for %u s%s",
                        startup ? "start-up (window creation, setup menu)" : "game loop", still,
                        reports < kMaxHangReports ? ", writing a hang report (the game keeps running)" : "");
      if (reports < kMaxHangReports) {
        reports++;
        g_hang_in_startup = startup;
        run_report(nullptr, Cause::Hang, g_game_thread, still);
      }
    }
  }
}

} // namespace

extern "C" void gcrt_crash_handler_install(GcrtCpu* g) {
  g_cpu = g;
  g_game_thread = GetCurrentThreadId();
  gcrt_diag_init();
  wchar_t buf[MAX_PATH * 2];
  if (env_set(L"GCRT_CRASH_HANDLER", buf, MAX_PATH) && buf[0] == L'0') {
    return;
  }
  g_installed = true;
  prepare_profile_prefixes();
  {
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH) {
      wcsncat_s(buf, L"gcrt-crashes", _TRUNCATE);
      set_dir(buf, g_temp_dir);
    }
  }
  if (env_set(L"GCRT_CRASH_DIR", buf, MAX_PATH * 2) || env_set(L"GCRT_CAPTURE_DIR", buf, MAX_PATH * 2)) {
    set_dir(buf);
    g_dir_from_env = true;
    prune_old_reports(L"crash-");
    prune_old_reports(L"hang-");
    open_dir_handle(true);
  } else {
    // Until the platform gives the user folder: the system temporary folder, never the current folder (which may
    // be a source tree, where a report holding guest memory must not end up).
    set_dir(g_temp_dir);
    open_dir_handle(false);
  }
  g_nt_create_file = reinterpret_cast<NtCreateFileFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile")));
  snapshot_modules(); // used by the header of a report (written before the list is refreshed); refreshed at rearm
  load_signatures();
  g_get_thread_description = reinterpret_cast<GetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
  read_machine_info();
  read_exe_and_cpus();
  read_environment();
  // Stack kept for the handler when the game thread overflows its stack (SetThreadStackGuarantee).
  ULONG guarantee = 64 * 1024;
  SetThreadStackGuarantee(&guarantee);
  g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE t = CreateThread(nullptr, 1 << 20, reporter_main, nullptr, 0, &g_reporter_tid);
  if (t != nullptr) {
    SetThreadDescription(t, L"gcrt crash reporter");
    CloseHandle(t);
  } else {
    CloseHandle(g_request);
    CloseHandle(g_done);
    g_request = g_done = nullptr;
  }
  // Suspension guard (see "Threads"): GCRT_CRASH_SUSPEND_LIMIT seconds, default 5, at least 1.
  if (env_set(L"GCRT_CRASH_SUSPEND_LIMIT", buf, 32)) {
    const unsigned long s = wcstoul(buf, nullptr, 10);
    g_suspend_limit_ms = static_cast<DWORD>(std::min<unsigned long>(std::max<unsigned long>(s, 1ul), 3600ul) * 1000ul);
  }
  if (env_set(L"GCRT_CRASH_TEST_SUSPENDED", buf, 32)) {
    g_test_suspended = _wcsicmp(buf, L"heap") == 0 ? -1 : static_cast<long>(wcstoul(buf, nullptr, 10));
  }
  g_suspend_begin = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_suspend_end = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE guard = g_suspend_begin != nullptr && g_suspend_end != nullptr
                     ? CreateThread(nullptr, 64 * 1024, guard_main, nullptr, 0, &g_guard_tid)
                     : nullptr;
  if (guard != nullptr) {
    SetThreadPriority(guard, THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(guard, L"gcrt suspension guard");
    CloseHandle(guard);
  } else {
    g_guard_tid = 0;
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "crash handler: the suspension guard could not be started");
  }
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "crash handler: suspension guard %lu ms%s",
                    static_cast<unsigned long>(g_suspend_limit_ms),
                    g_test_suspended != 0 ? " (GCRT_CRASH_TEST_SUSPENDED set)" : "");
  SetUnhandledExceptionFilter(unhandled_filter);
  std::signal(SIGABRT, on_abort);
  std::set_terminate(on_terminate);
  _set_purecall_handler(on_purecall);
  _set_invalid_parameter_handler(on_invalid_parameter);
  // Hang watchdog (GCRT_WATCHDOG seconds, default 60, 0 = off).
  g_watchdog_seconds = 60;
  if (env_set(L"GCRT_WATCHDOG", buf, 32)) {
    g_watchdog_seconds = static_cast<unsigned>(wcstoul(buf, nullptr, 10));
  }
  // Start-up (window creation, setup menu): at least 60 s, so that the short GCRT_WATCHDOG of the tests of the game
  // loop hang (a few seconds) does not also report a slow window creation on a busy computer; GCRT_WATCHDOG_STARTUP
  // sets it apart (test of this watch). Off with GCRT_WATCHDOG=0.
  g_startup_watchdog_seconds = g_watchdog_seconds > 60 ? g_watchdog_seconds : 60;
  if (env_set(L"GCRT_WATCHDOG_STARTUP", buf, 32)) {
    g_startup_watchdog_seconds = static_cast<unsigned>(wcstoul(buf, nullptr, 10));
  }
  if (g_watchdog_seconds > 0) {
    HANDLE w = CreateThread(nullptr, 64 * 1024, watchdog_main, nullptr, 0, &g_watchdog_tid);
    if (w != nullptr) {
      SetThreadDescription(w, L"gcrt watchdog");
      CloseHandle(w);
    }
  }
}

extern "C" void gcrt_crash_handler_rearm(void) {
  if (!g_installed) {
    return;
  }
  // A library initialized after the handler (window, renderer, audio) may have replaced the unhandled exception
  // filter: put ours back, and say so in the trace and in later reports.
  const LPTOP_LEVEL_EXCEPTION_FILTER previous = SetUnhandledExceptionFilter(unhandled_filter);
  snapshot_modules(); // the libraries of the window and the renderer are loaded now (header of a report)
  if (previous != unhandled_filter) {
    char where[300];
    describe_host_address(reinterpret_cast<DWORD64>(previous), where, sizeof(where));
    std::snprintf(g_rearm_note, sizeof(g_rearm_note),
                  "note: the unhandled exception filter had been replaced by %s, restored at startup", where);
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "crash handler: %s", g_rearm_note);
  }
}

extern "C" void gcrt_crash_handler_set_default_dir(const char* dir_utf8) {
  if (!g_installed || g_dir_from_env || dir_utf8 == nullptr || dir_utf8[0] == 0) {
    return;
  }
  wchar_t buf[MAX_PATH * 2];
  if (MultiByteToWideChar(CP_UTF8, 0, dir_utf8, -1, buf, MAX_PATH * 2) > 0) {
    set_dir(buf);
    prune_old_reports(L"crash-");
    prune_old_reports(L"hang-");
    open_dir_handle(true);
    load_signatures();
  }
}

extern "C" void gcrt_crash_handler_set_guest_symbolizer(GcrtGuestSymbolizer fn) { g_symbolizer = fn; }

extern "C" void gcrt_crash_handler_set_context(GcrtCrashContextFn fn, void* user) {
  g_context_fn = fn;
  g_context_user = user;
}

extern "C" void gcrt_crash_handler_set_gpu_info(const char* text) {
  if (text == nullptr) {
    return;
  }
  size_t n = 0;
  for (const char* c = text; *c != 0 && n < sizeof(g_gpu) - 1; c++) {
    if (*c == '\n') {
      if (n > 0 && g_gpu[n - 1] != ' ') {
        g_gpu[n++] = ';';
      }
      continue;
    }
    if (*c == ' ' && (n == 0 || g_gpu[n - 1] == ' ' || g_gpu[n - 1] == ';')) {
      if (n > 0 && g_gpu[n - 1] == ';') {
        g_gpu[n++] = ' ';
      }
      continue;
    }
    g_gpu[n++] = *c;
  }
  g_gpu[n] = 0;
}

extern "C" bool gcrt_process_memory(u64* working_set, u64* private_bytes, u32* handles) {
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  pmc.cb = sizeof(pmc);
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    return false;
  }
  DWORD count = 0;
  GetProcessHandleCount(GetCurrentProcess(), &count);
  *working_set = pmc.WorkingSetSize;
  *private_bytes = pmc.PrivateUsage;
  *handles = count;
  return true;
}

#else // not Windows

extern "C" void gcrt_crash_handler_install(GcrtCpu*) { gcrt_diag_init(); }
extern "C" void gcrt_crash_handler_rearm(void) {}
extern "C" void gcrt_crash_handler_set_default_dir(const char*) {}
extern "C" void gcrt_crash_handler_set_guest_symbolizer(GcrtGuestSymbolizer) {}
extern "C" void gcrt_crash_handler_set_context(GcrtCrashContextFn, void*) {}
extern "C" void gcrt_crash_handler_set_gpu_info(const char*) {}
extern "C" bool gcrt_process_memory(u64*, u64*, u32*) { return false; }

#endif
