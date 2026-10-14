/* Sampling host profiler (diagnostic tool, generic).
 *
 * A sampling thread suspends the game thread every ~1 ms, reads its instruction pointer (RIP) and
 * resumes it. At the end, the addresses are attributed to host functions (DbgHelp + the executable's PDB file:
 * build in RelWithDebInfo): generated blocks (func_XXXXXXXX, loop_XXXXXXXX), DolRecomp helpers
 * (mem_read32, ppc_fadds...), chassis, hardware, Aurora/Dawn, kernel (waits). The CPU time of each thread of the
 * process is also shown (rendering on another thread?).
 * Windows only; elsewhere, the functions do nothing. */
#include "cpu/gcrt_cpu.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#define TABLE_BITS 20u
#define TABLE_SIZE (1u << TABLE_BITS)

typedef struct {
    DWORD64 rip;
    u32 count;
} Sample;

static Sample* s_table;
static HANDLE s_target;
static HANDLE s_thread;
static volatile LONG s_running;
static u64 s_samples, s_dropped;

static void record(DWORD64 rip) {
    u32 h = (u32)((rip * 0x9E3779B97F4A7C15ull) >> (64u - TABLE_BITS));
    for (u32 i = 0; i < TABLE_SIZE; i++) {
        Sample* s = &s_table[(h + i) & (TABLE_SIZE - 1u)];
        if (s->rip == rip) {
            s->count++;
            return;
        }
        if (s->rip == 0) {
            s->rip = rip;
            s->count = 1;
            return;
        }
    }
    s_dropped++;
}

static DWORD WINAPI sampler(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    while (s_running) {
        Sleep(1);
        if (SuspendThread(s_target) == (DWORD)-1)
            continue;
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(s_target, &ctx)) {
            record(ctx.Rip);
            s_samples++;
        }
        ResumeThread(s_target);
    }
    timeEndPeriod(1);
    return 0;
}

void gcrt_host_profiler_start(void) {
    if (s_thread)
        return;
    s_table = (Sample*)calloc(TABLE_SIZE, sizeof(Sample));
    if (!s_table)
        return;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    s_running = 1;
    s_thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (s_thread)
        SetThreadPriority(s_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

typedef struct {
    char name[160];
    u64 count;
} Bucket;

static int by_count(const void* a, const void* b) {
    const Bucket* x = (const Bucket*)a;
    const Bucket* y = (const Bucket*)b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

/* Name of the main module (the executable, without extension), as reported by DbgHelp. */
static char s_main_module[64];

/* Groups the functions by family, for a readable report. */
static const char* family(const char* module, const char* fn) {
    if (_stricmp(module, s_main_module) != 0) {
        if (!_stricmp(module, "ntdll") || !_stricmp(module, "KERNELBASE") || !_stricmp(module, "kernel32") ||
            !_stricmp(module, "win32u"))
            return "system (waits, kernel calls)";
        return "other DLLs (graphics driver, Dawn, SDL...)";
    }
    if (!strncmp(fn, "func_", 5) || !strncmp(fn, "loop_", 5))
        return "generated code (blocks)";
    if (!strncmp(fn, "mem_read", 8) || !strncmp(fn, "mem_write", 9) || !strcmp(fn, "resolve_addr") ||
        !strcmp(fn, "bus_read") || !strcmp(fn, "bus_write") || !strcmp(fn, "clear_matching_reservation"))
        return "memory accesses (mem_read/mem_write, bus)";
    if (!strncmp(fn, "ppc_", 4) || !strncmp(fn, "dolrecomp_", 10) || !strncmp(fn, "extend_f32", 10) ||
        !strncmp(fn, "set_fp", 6) || !strncmp(fn, "classify_", 9) || !strncmp(fn, "is_snan", 7) ||
        !strncmp(fn, "f32_", 4) || !strncmp(fn, "f64_", 4) || !strncmp(fn, "psq_", 4) || !strncmp(fn, "fe", 2) ||
        !strcmp(fn, "set_fprf") || !strncmp(fn, "round", 5))
        return "DolRecomp CPU helpers (FP, psq, SPR...)";
    if (!strncmp(fn, "gcrt_cpu_", 9) || !strcmp(fn, "dispatch") || !strcmp(fn, "deliver_interrupts") ||
        !strcmp(fn, "host_now") || !strcmp(fn, "instruction_fallback") || !strncmp(fn, "gcrt_hle", 8) ||
        !strcmp(fn, "repl_slot") || !strcmp(fn, "step"))
        return "chassis, clock, HLE";
    if (strstr(fn, "gcrt::gx") || strstr(fn, "gcrt::platform") ||
        strstr(fn, "gcrt_platform") || strstr(fn, "gcrt_dsp") || strstr(fn, "gcrt::audio") || strstr(fn, "gcrt_audio"))
        return "GX bridge, platform, audio";
    if (strstr(fn, "aurora") || strstr(fn, "webgpu") || strstr(fn, "wgpu") || strstr(fn, "dawn") || strstr(fn, "SDL"))
        return "Aurora / Dawn / SDL";
    return "other functions of the executable";
}

void gcrt_host_profiler_report(FILE* out) {
    if (!s_thread)
        return;
    s_running = 0;
    WaitForSingleObject(s_thread, 2000);
    CloseHandle(s_thread);
    s_thread = NULL;

    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, NULL, TRUE);
    {
        IMAGEHLP_MODULE64 main_mod;
        memset(&main_mod, 0, sizeof(main_mod));
        main_mod.SizeOfStruct = sizeof(main_mod);
        if (SymGetModuleInfo64(process, (DWORD64)(uintptr_t)GetModuleHandleW(NULL), &main_mod))
            snprintf(s_main_module, sizeof(s_main_module), "%s", main_mod.ModuleName);
    }

    Bucket* fns = (Bucket*)calloc(TABLE_SIZE, sizeof(Bucket));
    u32 fn_count = 0;
    enum { FAMILIES = 16 };
    Bucket fam[FAMILIES];
    u32 fam_count = 0;
    memset(fam, 0, sizeof(fam));
    char symbuf[sizeof(SYMBOL_INFO) + 256];
    for (u32 i = 0; i < TABLE_SIZE && fns; i++) {
        if (!s_table[i].rip)
            continue;
        char name[160] = "?";
        char module[64] = "?";
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
        memset(symbuf, 0, sizeof(symbuf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_MODULE64 mod;
        memset(&mod, 0, sizeof(mod));
        mod.SizeOfStruct = sizeof(mod);
        if (SymGetModuleInfo64(process, s_table[i].rip, &mod))
            snprintf(module, sizeof(module), "%s", mod.ModuleName);
        if (SymFromAddr(process, s_table[i].rip, &displacement, sym))
            snprintf(name, sizeof(name), "%s", sym->Name);
        char key[160];
        snprintf(key, sizeof(key), "%s!%s", module, name);
        u32 k = 0;
        for (; k < fn_count; k++)
            if (!strcmp(fns[k].name, key))
                break;
        if (k == fn_count) {
            if (fn_count >= TABLE_SIZE)
                continue;
            snprintf(fns[fn_count++].name, sizeof(fns[0].name), "%s", key);
        }
        fns[k].count += s_table[i].count;
        const char* f = family(module, name);
        u32 j = 0;
        for (; j < fam_count; j++)
            if (!strcmp(fam[j].name, f))
                break;
        if (j == fam_count && fam_count < FAMILIES)
            snprintf(fam[fam_count++].name, sizeof(fam[0].name), "%s", f);
        if (j < FAMILIES)
            fam[j].count += s_table[i].count;
    }
    SymCleanup(process);
    qsort(fam, fam_count, sizeof(Bucket), by_count);
    qsort(fns, fn_count, sizeof(Bucket), by_count);
    fprintf(out, "host profile by sampling of the game thread: %llu samples (~1 ms)%s\n",
            (unsigned long long)s_samples, s_dropped ? " (table full, samples lost)" : "");
    for (u32 j = 0; j < fam_count; j++)
        fprintf(out, "  %-45s %6.2f %%\n", fam[j].name, s_samples ? 100.0 * (double)fam[j].count / (double)s_samples : 0.0);
    fprintf(out, "  busiest host functions:\n");
    for (u32 k = 0; k < fn_count && k < 40; k++)
        fprintf(out, "    %6.2f %%  %s\n", 100.0 * (double)fns[k].count / (double)s_samples, fns[k].name);
    free(fns);
    free(s_table);
    s_table = NULL;

    /* CPU time per thread of the process. */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
        fprintf(out, "  CPU time per thread (user + kernel):\n");
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid)
                continue;
            HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            FILETIME c, e, k, u;
            if (GetThreadTimes(th, &c, &e, &k, &u)) {
                double user = (double)(((u64)u.dwHighDateTime << 32) | u.dwLowDateTime) / 1e7;
                double kern = (double)(((u64)k.dwHighDateTime << 32) | k.dwLowDateTime) / 1e7;
                if (user + kern > 0.5)
                    fprintf(out, "    thread %6lu%s: %7.1f s (%5.1f s kernel)\n", te.th32ThreadID,
                            te.th32ThreadID == self ? " (game thread)" : "", user + kern, kern);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
    }
}
#else
void gcrt_host_profiler_start(void) {}
void gcrt_host_profiler_report(FILE* out) { (void)out; }
#endif
