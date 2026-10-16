// Crash handler (see gcrt_crash.h). Generic: no reference to any game.
//
// The work is done by a separate reporting thread: the faulting thread may have little stack left (stack overflow)
// and stays stopped in the filter until the report is written (bounded wait). Everything that can be prepared in
// advance (folder, file names) is prepared at installation or when the default folder is set.
#include "gcrt_crash.h"

extern "C" {
#include "../hle/gcrt_hle.h"
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
#include <windows.h>
#include <bcrypt.h>
#include <dbghelp.h>
#include <intrin.h>
#include <psapi.h>
#include <csignal>

namespace {

GcrtCpu* g_cpu = nullptr;
DWORD g_game_thread = 0;
GcrtGuestSymbolizer g_symbolizer = nullptr;
GcrtCrashContextFn g_context_fn = nullptr;
void* g_context_user = nullptr;
volatile LONG g_reporting = 0;
wchar_t g_dir[MAX_PATH * 2] = L"";  // final folder, with a trailing separator
bool g_dir_from_env = false;

// Report text, assembled in a fixed buffer (no allocation once the crash has happened).
constexpr size_t kReportSize = 256 * 1024;
char g_report[kReportSize];
size_t g_report_len = 0;

void out(const char* fmt, ...) {
  if (g_report_len >= kReportSize - 1) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(g_report + g_report_len, kReportSize - g_report_len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    g_report_len += static_cast<size_t>(n);
    if (g_report_len > kReportSize - 1) {
      g_report_len = kReportSize - 1;
    }
  }
}

void append_text(void* /*sink*/, const char* text) { out("%s", text); }

bool env_set(const wchar_t* name, wchar_t* buf, DWORD size) {
  const DWORD n = GetEnvironmentVariableW(name, buf, size);
  return n > 0 && n < size;
}

void set_dir(const wchar_t* dir) {
  wcsncpy_s(g_dir, dir, _TRUNCATE);
  const size_t n = wcslen(g_dir);
  if (n > 0 && n + 1 < sizeof(g_dir) / sizeof(g_dir[0]) && g_dir[n - 1] != L'\\' && g_dir[n - 1] != L'/') {
    g_dir[n] = L'\\';
    g_dir[n + 1] = 0;
  }
}

// Number of crash reports kept in the crash folder. Each report holds the guest memory (about 25 MiB) and a
// minidump, so older ones are deleted. Called at startup when the folder is chosen, never from the handler.
constexpr size_t kMaxReports = 5;

void prune_old_reports() {
  std::vector<std::wstring> stems;
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((std::wstring(g_dir) + L"crash-*.txt").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    std::wstring name = fd.cFileName;
    stems.push_back(name.substr(0, name.size() - 4)); // without ".txt"
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  if (stems.size() <= kMaxReports) {
    return;
  }
  // Names are crash-YYYYMMDD-HHMMSS-pid: lexicographic order is chronological.
  std::sort(stems.begin(), stems.end());
  for (size_t i = 0; i + kMaxReports < stems.size(); ++i) {
    const std::wstring base = std::wstring(g_dir) + stems[i];
    DeleteFileW((base + L".txt").c_str());
    DeleteFileW((base + L".dmp").c_str());
    DeleteFileW((base + L"-guest-ram.bin").c_str());
  }
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
  case 0xE06D7363: return "C++ exception";
  case 0: return "abort()";
  default: return "?";
  }
}

// Module (path) and offset of a host address.
void describe_host_address(DWORD64 address, char* buf, size_t size) {
  HMODULE module = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(address), &module) &&
      module != nullptr) {
    char path[MAX_PATH] = "";
    GetModuleFileNameA(module, path, MAX_PATH);
    const char* base = std::strrchr(path, '\\');
    std::snprintf(buf, size, "%s+0x%llX", base ? base + 1 : path,
                  static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
  } else {
    std::snprintf(buf, size, "(no module)");
  }
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
// Commit of the source tree, written at every build by cmake/GcrtBuildInfo.cmake ("unknown" without Git, "-dirty"
// suffix when tracked files have uncommitted changes).
extern "C" const char gcrt_build_commit[];

// First 12 hex digits of the SHA-1 of a file (BCrypt). Identifies the exact executable, hence the meaning of the host
// offsets of the report, even when several builds of the same commit exist.
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

// Executable (path, SHA-1, PE time stamp and image size, build commit) and processor: what is needed to find the exact
// build again, and to tell a software fault from an unstable machine.
void report_build_and_machine() {
  wchar_t wpath[MAX_PATH * 2] = L"";
  char path[MAX_PATH * 4] = "?";
  const DWORD len = GetModuleFileNameW(nullptr, wpath, MAX_PATH * 2);
  const bool have_path = len > 0 && len < MAX_PATH * 2;
  if (have_path) {
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, sizeof(path), nullptr, nullptr);
  }
  out("executable %s\n", path);
  char sha1[16] = "?";
  if (!have_path || !file_sha1_short(wpath, sha1, sizeof(sha1))) {
    std::snprintf(sha1, sizeof(sha1), "?");
  }
  const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
  out("executable sha1 %s, PE time stamp 0x%08lX, image size 0x%lX, build commit %s\n", sha1,
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
      static_cast<unsigned long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
}

// Bytes around the faulting instruction, read without risk of a second fault (ReadProcessMemory, by 8-byte pieces):
// the instruction can then be decoded from the report alone, whatever the executable. tools/crash_inspect.py decodes
// it from the minidump and checks whether it can fault with the registers of the report.
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

void report_guest() {
  if (g_cpu == nullptr) {
    out("guest: no state\n");
    return;
  }
  const CPUState* c = &g_cpu->state;
  char name[200];
  out("\n== Guest state (start of the current block: the generated code updates pc at block boundaries)\n");
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
      static_cast<double>(g_cpu->ticks) / static_cast<double>(GCRT_TB_HZ), static_cast<unsigned long long>(g_cpu->ticks),
      static_cast<unsigned long long>(g_cpu->blocks), static_cast<unsigned long long>(g_cpu->deliveries));
  // Call stack from the PowerPC EABI frames: back chain at 0(r1), saved LR at 4(caller frame).
  out("guest call stack (lr, then the return addresses saved in the stack frames):\n");
  guest_name(c->lr, name, sizeof(name));
  out("  #0 %s\n", name);
  u32 sp = c->gpr[1];
  for (int depth = 1; depth < 48; depth++) {
    u32 back = 0, saved = 0;
    if (!guest_read32(sp, &back) || back == 0 || back <= sp || back - sp > 0x100000u) {
      break;
    }
    if (!guest_read32(back + 4u, &saved)) {
      break;
    }
    guest_name(saved, name, sizeof(name));
    out("  #%d %s (frame %08X)\n", depth, name, back);
    sp = back;
  }
  const u32 count = g_cpu->ring_pos < GCRT_TRACE_RING ? g_cpu->ring_pos : GCRT_TRACE_RING;
  out("last blocks run (oldest to newest):\n");
  for (u32 i = 0; i < count; i++) {
    const u32 pc = g_cpu->ring[(g_cpu->ring_pos - count + i) % GCRT_TRACE_RING];
    guest_name(pc, name, sizeof(name));
    out("  %s\n", name);
  }
}

void report_memory() {
  u64 ws = 0, priv = 0;
  u32 handles = 0;
  if (gcrt_process_memory(&ws, &priv, &handles)) {
    out("process memory: working set %.1f MiB, private %.1f MiB, handles %u\n", static_cast<double>(ws) / 1048576.0,
        static_cast<double>(priv) / 1048576.0, handles);
  }
  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) {
    out("system memory: %u %% used, %.0f MiB physical available, %.0f MiB commit available\n", ms.dwMemoryLoad,
        static_cast<double>(ms.ullAvailPhys) / 1048576.0, static_cast<double>(ms.ullAvailPageFile) / 1048576.0);
  }
}

void thread_name(DWORD tid, char* buf, size_t size) {
  buf[0] = 0;
  using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  auto fn = reinterpret_cast<GetThreadDescriptionFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
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

struct CrashInput {
  EXCEPTION_POINTERS* info;
  DWORD thread;
  wchar_t stem[MAX_PATH * 2];
};

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

DWORD WINAPI report_thread(LPVOID param) {
  auto* in = static_cast<CrashInput*>(param);
  const HANDLE process = GetCurrentProcess();
  const EXCEPTION_RECORD* rec = in->info->ExceptionRecord;
  const CONTEXT* ctx = in->info->ContextRecord;
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
  SymInitialize(process, nullptr, TRUE); // may fail if already initialized (host profiler): symbols still usable

  char where[400];
  out("== gcrt crash report\n");
  SYSTEMTIME st;
  GetLocalTime(&st);
  out("time %04u-%02u-%02u %02u:%02u:%02u, process %lu\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
      st.wSecond, GetCurrentProcessId());
  out("exception 0x%08lX (%s), flags 0x%lX\n", rec->ExceptionCode, exception_name(rec->ExceptionCode),
      rec->ExceptionFlags);
  host_symbol(process, reinterpret_cast<DWORD64>(rec->ExceptionAddress), where, sizeof(where));
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
  report_build_and_machine();
  char tname[128];
  thread_name(in->thread, tname, sizeof(tname));
  out("thread %lu \"%s\"%s\n", in->thread, tname, in->thread == g_game_thread ? " (game thread)" : "");
  report_memory();
  if (ctx != nullptr) {
#if defined(_M_X64) || defined(__x86_64__)
    out("rip %016llX rsp %016llX rbp %016llX\n", ctx->Rip, ctx->Rsp, ctx->Rbp);
    out("rax %016llX rbx %016llX rcx %016llX rdx %016llX\n", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
    out("rsi %016llX rdi %016llX r8  %016llX r9  %016llX\n", ctx->Rsi, ctx->Rdi, ctx->R8, ctx->R9);
    out("r10 %016llX r11 %016llX r12 %016llX r13 %016llX\n", ctx->R10, ctx->R11, ctx->R12, ctx->R13);
    out("r14 %016llX r15 %016llX eflags %08lX\n", ctx->R14, ctx->R15, static_cast<unsigned long>(ctx->EFlags));
    report_code_bytes(ctx->Rip);
    // Host call stack of the faulting thread.
    out("host call stack:\n");
    CONTEXT walk = *ctx;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = walk.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = walk.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = walk.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, in->thread);
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
#endif
  }
  report_guest();
  if (g_context_fn != nullptr) {
    out("\n== Platform\n");
    g_context_fn(g_context_user, append_text, nullptr);
  }

  wchar_t path[MAX_PATH * 2 + 32];
  CreateDirectoryW(g_dir, nullptr);
  // Minidump.
  swprintf_s(path, L"%s.dmp", in->stem);
  HANDLE dump = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  bool dumped = false;
  if (dump != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId = in->thread;
    mei.ExceptionPointers = in->info;
    mei.ClientPointers = FALSE;
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithUnloadedModules | MiniDumpWithHandleData |
                                                 MiniDumpWithProcessThreadData);
    dumped = MiniDumpWriteDump(process, GetCurrentProcessId(), dump, type, &mei, nullptr, nullptr) != FALSE;
    CloseHandle(dump);
  }
  out("\nminidump: %s\n", dumped ? "written" : "FAILED");
  // Guest memory.
  if (g_cpu != nullptr && g_cpu->state.ram != nullptr) {
    swprintf_s(path, L"%s-guest-ram.bin", in->stem);
    write_file(path, g_cpu->state.ram, g_cpu->state.ram_size);
    out("guest memory: %u bytes from 0x80000000\n", g_cpu->state.ram_size);
  }
  swprintf_s(path, L"%s.txt", in->stem);
  write_file(path, g_report, g_report_len);
  std::fwrite(g_report, 1, g_report_len, stderr);
  char where_files[MAX_PATH * 2];
  WideCharToMultiByte(CP_UTF8, 0, in->stem, -1, where_files, sizeof(where_files), nullptr, nullptr);
  std::fprintf(stderr, "gcrt: crash report written: %s.txt (.dmp, -guest-ram.bin)\n", where_files);
  std::fflush(stderr);
  return 0;
}

void run_report(EXCEPTION_POINTERS* info) {
  if (InterlockedExchange(&g_reporting, 1) != 0) {
    return; // a second crash while reporting (or two threads): only the first one is written
  }
  static CrashInput in;
  in.info = info;
  in.thread = GetCurrentThreadId();
  SYSTEMTIME st;
  GetLocalTime(&st);
  swprintf_s(in.stem, L"%scrash-%04u%02u%02u-%02u%02u%02u-%lu", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, GetCurrentProcessId());
  HANDLE t = CreateThread(nullptr, 1 << 20, report_thread, &in, 0, nullptr);
  if (t != nullptr) {
    WaitForSingleObject(t, 60000); // bounded: a deadlock in the reporting thread must not hang the crash
    CloseHandle(t);
  }
}

LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
  run_report(info);
  return EXCEPTION_CONTINUE_SEARCH; // default handling: the process ends with the exception code
}

void on_abort(int) {
  CONTEXT ctx{};
  RtlCaptureContext(&ctx);
  EXCEPTION_RECORD rec{};
  rec.ExceptionCode = 0; // reported as "abort()"
  rec.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
  EXCEPTION_POINTERS info{&rec, &ctx};
  run_report(&info);
}

} // namespace

extern "C" void gcrt_crash_handler_install(GcrtCpu* g) {
  g_cpu = g;
  g_game_thread = GetCurrentThreadId();
  wchar_t buf[MAX_PATH * 2];
  if (env_set(L"GCRT_CRASH_HANDLER", buf, MAX_PATH) && buf[0] == L'0') {
    return;
  }
  if (env_set(L"GCRT_CRASH_DIR", buf, MAX_PATH * 2) || env_set(L"GCRT_CAPTURE_DIR", buf, MAX_PATH * 2)) {
    set_dir(buf);
    g_dir_from_env = true;
    prune_old_reports();
  } else {
    // Until the platform gives the user folder: the system temporary folder, never the current folder (which may
    // be a source tree, where a report holding guest memory must not end up).
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH) {
      wcsncat_s(buf, L"gcrt-crashes", _TRUNCATE);
      set_dir(buf);
    }
  }
  SetUnhandledExceptionFilter(unhandled_filter);
  std::signal(SIGABRT, on_abort);
}

extern "C" void gcrt_crash_handler_set_default_dir(const char* dir_utf8) {
  if (g_dir_from_env || dir_utf8 == nullptr || dir_utf8[0] == 0) {
    return;
  }
  wchar_t buf[MAX_PATH * 2];
  if (MultiByteToWideChar(CP_UTF8, 0, dir_utf8, -1, buf, MAX_PATH * 2) > 0) {
    set_dir(buf);
    prune_old_reports();
  }
}

extern "C" void gcrt_crash_handler_set_guest_symbolizer(GcrtGuestSymbolizer fn) { g_symbolizer = fn; }

extern "C" void gcrt_crash_handler_set_context(GcrtCrashContextFn fn, void* user) {
  g_context_fn = fn;
  g_context_user = user;
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

extern "C" void gcrt_crash_handler_install(GcrtCpu*) {}
extern "C" void gcrt_crash_handler_set_default_dir(const char*) {}
extern "C" void gcrt_crash_handler_set_guest_symbolizer(GcrtGuestSymbolizer) {}
extern "C" void gcrt_crash_handler_set_context(GcrtCrashContextFn, void*) {}
extern "C" bool gcrt_process_memory(u64*, u64*, u32*) { return false; }

#endif
