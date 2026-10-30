/* "Chassis" loop: runs the recompiled code block by block.
 *
 * DolRecomp's C backend splits the code into functions of 16 KiB of guest code; each one accepts
 * any instruction address of its range as an entry point (switch on ctx->pc) and
 * returns to the caller as soon as the flow leaves the range (call into another range, `blr` return,
 * indirect jump, `rfi`, `sc`, delegated instruction) or the cycle budget is exhausted.
 * Here we replace `dolrecomp_run_blocks` (generated.h), which only knows a single module, with a
 * loop that queries all the recompiled modules (DOL + REL).
 *
 * Milestone 10b: no more exception vectors or mini-interpreter. Synchronous exceptions (cpu.c,
 * ppc_take_exception): "FPU unavailable" and "system call" are handled by the host (exception_hook: native OS,
 * hle/native_os.c); the others are faults (stop). Interrupts: delivered by the host at block boundaries when
 * MSR[EE] = 1 (native interrupts, hle/native_irq.c; no interrupt line from emulated hardware at all
 * since step 10d), with SRR0 = pc (address of the next instruction), like an asynchronous exception. Code in RAM
 * outside the recompiled modules is no longer executed (stop "pc outside the recompiled code"). */
#include "cpu/gcrt_cpu.h"

#include <stdlib.h>
#include <time.h>

static double now_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char* gcrt_stop_reason_name(GcrtStopReason reason) {
    switch (reason) {
    case GCRT_STOP_NONE: return "none";
    case GCRT_STOP_BLOCK_LIMIT: return "block limit reached";
    case GCRT_STOP_TIME_LIMIT: return "time limit reached";
    case GCRT_STOP_UNKNOWN_PC: return "pc outside the recompiled code";
    case GCRT_STOP_NO_PROGRESS: return "block without progress (pc not recognized by its module)";
    case GCRT_STOP_EXCEPTION: return "PowerPC exception (stop requested)";
    case GCRT_STOP_UNHANDLED_IO: return "unhandled hardware access";
    case GCRT_STOP_REQUESTED: return "stop requested";
    case GCRT_STOP_GUEST_HALT: return "game halted (OSPanic or PPCHalt)";
    case GCRT_STOP_HELD_INTERRUPT: return "interrupt held too long by a nested call";
    case GCRT_STOP_DEFERRED_SWITCH: return "thread switch due during a call of decompiled code into guest code";
    }
    return "?";
}

static const char* vector_name(u32 vector) {
    switch (vector) {
    case 0x100: return "reset";
    case 0x200: return "machine check";
    case 0x300: return "DSI";
    case 0x400: return "ISI";
    case 0x500: return "external interrupt";
    case 0x600: return "alignment";
    case 0x700: return "program";
    case 0x800: return "FPU unavailable";
    case 0x900: return "decrementer";
    case 0xC00: return "system call";
    case 0xD00: return "trace";
    case 0xF00: return "performance monitor";
    case 0x1300: return "breakpoint";
    case 0x1700: return "thermal";
    default: return "?";
    }
}

void gcrt_cpu_mark_host_call(GcrtCpu* g, u32 address) {
    const u32 off = address - GC_RAM_BASE;
    if (address < GC_RAM_BASE || off >= g->state.ram_size)
        return; /* outside RAM: always passed to host_call */
    if (!g->host_call_filter) {
        const char* off_env = getenv("GCRT_HOST_CALL_FILTER"); /* =0: no filter (diagnostics, measurements) */
        if (off_env && off_env[0] == '0')
            return;
        g->host_call_filter = (u8*)calloc((size_t)(g->state.ram_size / 4u + 7u) / 8u, 1);
        if (!g->host_call_filter)
            return;
    }
    const u32 i = off >> 2;
    g->host_call_filter[i >> 3] |= (u8)(1u << (i & 7u));
}

/* Does host_call have anything to do for `pc`? (Cost: a subtraction and a bit test, instead of an indirect
 * call and a lookup in the replacement table, at every block.) */
static inline bool host_call_wanted(const GcrtCpu* g, u32 pc) {
    if (!g->host_call)
        return false;
    if (!g->host_call_filter)
        return true;
    const u32 off = pc - GC_RAM_BASE;
    if (pc < GC_RAM_BASE || off >= g->state.ram_size)
        return true;
    const u32 i = off >> 2;
    return (g->host_call_filter[i >> 3] >> (i & 7u)) & 1u;
}

GcrtGuestFn gcrt_cpu_lookup(GcrtCpu* g, u32 pc, const GcrtModule** module_out) {
    GcrtGuestFn fn;
    if (g->last_module && (fn = g->last_module->find(pc)) != NULL) {
        if (module_out)
            *module_out = g->last_module;
        return fn;
    }
    if (!g->modules)
        return NULL;
    for (unsigned i = 0; i < g->modules->count; i++) {
        const GcrtModule* m = g->modules->modules[i];
        if (m == g->last_module)
            continue;
        if ((fn = m->find(pc)) != NULL) {
            g->last_module = m;
            if (module_out)
                *module_out = m;
            return fn;
        }
    }
    return NULL;
}

void gcrt_cpu_request_stop(GcrtCpu* g) {
    if (g->stop == GCRT_STOP_NONE)
        g->stop = GCRT_STOP_REQUESTED;
}

/* Does pc designate RAM (physical address, cached area 0x8... or uncached area 0xC...)? */
static bool pc_in_ram(const CPUState* cpu, u32 pc) {
    u32 offset = pc;
    if (pc >= GC_RAM_UNCACHED)
        offset = pc - GC_RAM_UNCACHED;
    else if (pc >= GC_RAM_BASE)
        offset = pc - GC_RAM_BASE;
    return offset < cpu->ram_size;
}

static void count_vector(GcrtCpu* g, u32 vector) {
    u32 slot = (vector >> 8) & 0xFFu;
    if (slot < GCRT_VECTOR_SLOTS)
        g->vector_count[slot]++;
}

/* Synchronous exception raised by the block or instruction that has just run. Returns false if execution
 * must stop. Milestone 10b: "FPU unavailable" and "system call" are handled by the host (exception_hook,
 * hle/native_os.c), without a vector; the others (program, DSI, alignment...) are faults. */
static bool deliver_sync_exception(GcrtCpu* g) {
    CPUState* cpu = &g->state;
    const u32 faults = PPC_EXC_PROGRAM | PPC_EXC_DSI | PPC_EXC_ALIGNMENT | PPC_EXC_MACHINE_CHECK;
    u32 vector = cpu->pc & 0x0000FFFFu;
    bool fault = (cpu->exception & faults) != 0;
    g->last_exception = cpu->exception;
    g->last_exception_srr0 = cpu->srr0;
    count_vector(g, vector);
    if (fault && g->faults_logged < g->fault_log_limit) {
        g->faults_logged++;
        fprintf(stderr, "gcrt: exception %s (vector %04X) at srr0=%08X, srr1=%08X, program=%08X, dar=%08X\n",
                vector_name(vector), vector, cpu->srr0, cpu->srr1, cpu->program_exception, cpu->dar);
    }
    if (g->stop_on_exception || (fault && g->stop_on_fault)) {
        g->stop = GCRT_STOP_EXCEPTION;
        return false;
    }
    const u32 exception = cpu->exception;
    cpu->exception = 0;
    cpu->program_exception = 0;
    /* Cleared before the callback: it may run guest code (nested calls), which must not see it again. */
    if (!fault && g->exception_hook && g->exception_hook(g, exception))
        return true;
    /* No vector to run any more: a fault or an exception without native handling stops the game (on the console,
     * OSDefaultExceptionHandler would display the error and then halt the processor). */
    if (!fault && g->faults_logged < g->fault_log_limit) {
        g->faults_logged++;
        fprintf(stderr, "gcrt: exception %s (vector %04X) without native handling at srr0=%08X\n", vector_name(vector),
                vector, cpu->srr0);
    }
    g->stop = GCRT_STOP_EXCEPTION;
    return false;
}

/* Asynchronous interrupts (milestone 10: all native, hle/native_irq.c): external ones for the OS, then the native
 * OS alarms, which stand in for the decrementer (same order as the 750CL manual and Dolphin,
 * PowerPC::CheckExceptions). The native callback decides (OS masks) and, if it delivers, leaves MSR[EE] at 0 until
 * the return. */
static void deliver_interrupts(GcrtCpu* g) {
    CPUState* cpu = &g->state;
    if (!(cpu->msr & GCRT_MSR_EE))
        return;
    if (g->native_irq_hook && g->ticks >= g->native_irq_deadline) {
        g->native_irq_hook(g);
        if (!(cpu->msr & GCRT_MSR_EE))
            g->deliveries++; /* delivered: the rest will wait for the return from the interrupt */
    }
}

/* Diagnostic block window (measurement only, off by default): GCRT_BLOCK_WINDOW=A:B[,A:B...] (ticks) and
 * GCRT_BLOCK_WINDOW_FILE=path write one line "pc cycles ticks ee blocks" per chassis iteration whose ticks, after the
 * clock synchronization, fall in a window (before the interrupt delivery of that iteration). It only reads the CPU
 * state: the run is unchanged. One test per iteration when it is off. */
#define BLOCK_WINDOW_MAX 8
static int s_window_state; /* 0: not read yet, 1: off, 2: on */
static unsigned s_window_count;
static u64 s_window_from[BLOCK_WINDOW_MAX], s_window_to[BLOCK_WINDOW_MAX];
static FILE* s_window_file;

static void block_window_open(void) {
    s_window_state = 1;
    const char* spec = getenv("GCRT_BLOCK_WINDOW");
    const char* path = getenv("GCRT_BLOCK_WINDOW_FILE");
    if (!spec || !path || !spec[0])
        return;
    while (*spec && s_window_count < BLOCK_WINDOW_MAX) {
        char* end = NULL;
        const u64 from = strtoull(spec, &end, 0);
        if (!end || *end != ':')
            break;
        const u64 to = strtoull(end + 1, &end, 0);
        s_window_from[s_window_count] = from;
        s_window_to[s_window_count] = to;
        s_window_count++;
        if (!end || *end != ',')
            break;
        spec = end + 1;
    }
    s_window_file = s_window_count ? fopen(path, "w") : NULL;
    if (s_window_file) {
        setvbuf(s_window_file, NULL, _IOFBF, 1u << 20);
        s_window_state = 2;
    } else {
        fprintf(stderr, "gcrt: GCRT_BLOCK_WINDOW ignored (bad windows or file %s not writable)\n", path);
    }
}

static void block_window_line(const GcrtCpu* g) {
    for (unsigned i = 0; i < s_window_count; i++) {
        if (g->ticks >= s_window_from[i] && g->ticks <= s_window_to[i]) {
            fprintf(s_window_file, "%08X %llu %llu %d %llu\n", g->state.pc, (unsigned long long)g->cycles,
                    (unsigned long long)g->ticks, (g->state.msr & GCRT_MSR_EE) ? 1 : 0,
                    (unsigned long long)g->blocks);
            return;
        }
    }
}

static inline void block_window(const GcrtCpu* g) {
    if (s_window_state == 2)
        block_window_line(g);
    else if (s_window_state == 0)
        block_window_open();
}

/* One profile line for the elapsed window (guest time = ticks), as a % of the window's host time. */
static void profile_window(GcrtCpu* g, u64 now) {
    double rate = gcrt_prof_ticks_per_second();
    double window = (double)(now - g->prof_window_start) / rate;
    if (g->prof_window_start && window > 0.0) {
        double pct[GCRT_PROF_COUNT];
        for (int i = 0; i < GCRT_PROF_COUNT; i++)
            pct[i] = 100.0 * (double)(g->prof[i] - g->prof_last[i]) / rate / window;
        double bus_in_code = 100.0 * (double)(g->prof_bus_in_code - g->prof_last_bus_in_code) / rate / window;
        pct[GCRT_PROF_CODE] -= bus_in_code; /* the MMIO measured during generated code is subtracted from code */
        fprintf(stderr, "[profile] guest t=%.1f s: code %.1f %%, MMIO %.1f %%, timer %.1f %%, clock %.1f %%, "
                        "host_call %.1f %%; %llu blocks, %llu delegations\n",
                (double)g->ticks / (double)GCRT_TB_HZ, pct[GCRT_PROF_CODE], pct[GCRT_PROF_BUS], pct[GCRT_PROF_TIMER],
                pct[GCRT_PROF_SYNC], pct[GCRT_PROF_HLE],
                (unsigned long long)(g->blocks - g->prof_last_blocks),
                (unsigned long long)(g->fallback_count - g->prof_last_fallback));
    }
    for (int i = 0; i < GCRT_PROF_COUNT; i++)
        g->prof_last[i] = g->prof[i];
    g->prof_last_bus_in_code = g->prof_bus_in_code;
    g->prof_last_fallback = g->fallback_count;
    g->prof_last_blocks = g->blocks;
    g->prof_window_start = now;
    g->prof_next_window = now + (u64)(g->prof_period_s * rate);
}

GcrtStopReason gcrt_cpu_run(GcrtCpu* g, const GcrtRunLimits* limits) {
    CPUState* cpu = &g->state;
    const double start = now_seconds();
    g->stop = GCRT_STOP_NONE;

    const bool profile = g->profile;
    if (profile && !g->prof_samples) {
        g->prof_samples = (u32*)calloc(cpu->ram_size >> 8, sizeof(u32));
        g->prof_sample_period = (u64)(gcrt_prof_ticks_per_second() / 10000.0); /* 10,000 samples/s */
        g->prof_next_sample = gcrt_prof_now();
    }
    for (;;) {
        u32 pc;
        bool hle_done;
        if (profile) {
            u64 t0 = gcrt_prof_now(), timer0 = g->prof[GCRT_PROF_TIMER];
            gcrt_cpu_sync_time(g);
            u64 t1 = gcrt_prof_now();
            g->prof[GCRT_PROF_SYNC] += (t1 - t0) - (g->prof[GCRT_PROF_TIMER] - timer0);
            g->prof_calls[GCRT_PROF_SYNC]++;
            block_window(g);
            deliver_interrupts(g);
            pc = cpu->pc;
            u64 t2 = gcrt_prof_now();
            hle_done = host_call_wanted(g, pc) && g->host_call(cpu, pc);
            g->prof[GCRT_PROF_HLE] += gcrt_prof_now() - t2;
            g->prof_calls[GCRT_PROF_HLE] += hle_done;
        } else {
            gcrt_cpu_sync_time(g);
            block_window(g);
            deliver_interrupts(g);
            pc = cpu->pc;
            hle_done = host_call_wanted(g, pc) && g->host_call(cpu, pc);
        }

        if (hle_done) {
            /* Replacement by the host (HLE), takes priority over the recompiled code. */
            g->blocks++;
        } else {
            const GcrtModule* module = NULL;
            GcrtGuestFn fn = gcrt_cpu_lookup(g, pc, &module);
            /* Busy wait recognized in the DOL code: the host waits for the next event instead of spinning
             * (cpu/idle_skip.c, real-time pacing only). */
            if (fn && g->idle_skip && g->modules && g->modules->count && module == g->modules->modules[0] &&
                gcrt_cpu_idle_try_skip(g, pc)) {
                /* The skip counts as a block: the requested stop (window closed) and the limits (--max-blocks,
                 * --max-seconds) are still checked, even if the guest waits with interrupts masked. */
                g->blocks++;
                goto after_block;
            }
            if (!fn && pc < cpu->ram_size) {
                /* Physical address (MSR[IR] = 0 after an exception): alias in the 0x8xxxxxxx area. */
                u32 alias = pc | GC_RAM_BASE;
                fn = gcrt_cpu_lookup(g, alias, NULL);
                if (fn)
                    cpu->pc = pc = alias;
            }
            if (fn) {
                g->ring[g->ring_pos++ & g->ring_mask] = pc;
                if (g->trace && g->blocks < g->trace_limit)
                    fprintf(g->trace, "%08X %08X %08X\n", pc, cpu->lr, cpu->gpr[1]);
                cpu->downcount = 0;
                if (profile) {
                    u64 t0 = gcrt_prof_now();
                    g->prof_in_code = 1;
                    fn(cpu);
                    g->prof_in_code = 0;
                    u64 t1 = gcrt_prof_now();
                    g->prof[GCRT_PROF_CODE] += t1 - t0;
                    g->prof_calls[GCRT_PROF_CODE]++;
                    /* Sample: the block that contained the sampling instant (weighted by time). */
                    while (t1 >= g->prof_next_sample) {
                        if (g->prof_samples && pc >= GC_RAM_BASE && pc - GC_RAM_BASE < cpu->ram_size &&
                            g->ticks >= g->prof_sample_from_ticks)
                            g->prof_samples[(pc - GC_RAM_BASE) >> 8]++;
                        g->prof_next_sample = (t1 - g->prof_next_sample > 64u * g->prof_sample_period)
                                                  ? t1 + g->prof_sample_period
                                                  : g->prof_next_sample + g->prof_sample_period;
                    }
                } else {
                    fn(cpu);
                }
                s64 used = -cpu->downcount;
                if (used > 0)
                    gcrt_cpu_add_cycles(g, (u64)used);
                g->blocks++;
                if (!cpu->exception && cpu->pc == pc && used <= 0 && g->stop == GCRT_STOP_NONE) {
                    g->stop = GCRT_STOP_NO_PROGRESS;
                    break;
                }
            } else {
                /* Outside the recompiled modules (including code copied into RAM by the game): no more interpreter. */
                if (pc_in_ram(cpu, pc))
                    fprintf(stderr, "gcrt: pc %08X in RAM outside the recompiled modules (lr %08X): stop\n", pc, cpu->lr);
                g->stop = GCRT_STOP_UNKNOWN_PC;
                break;
            }
            if (cpu->exception && !deliver_sync_exception(g))
                break;
        }

    after_block:
        if (g->stop != GCRT_STOP_NONE)
            break;
        if (profile && g->prof_period_s > 0.0 && (g->blocks & 1023u) == 0) {
            u64 now = gcrt_prof_now();
            if (now >= g->prof_next_window)
                profile_window(g, now);
        }
        if (limits) {
            if (limits->max_blocks && g->blocks >= limits->max_blocks) {
                g->stop = GCRT_STOP_BLOCK_LIMIT;
                break;
            }
            if (limits->max_seconds > 0.0 && (g->blocks & 4095u) == 0 &&
                now_seconds() - start >= limits->max_seconds) {
                g->stop = GCRT_STOP_TIME_LIMIT;
                break;
            }
        }
    }
    if (g->trace)
        fflush(g->trace);
    if (s_window_file)
        fflush(s_window_file);
    return g->stop;
}

void gcrt_cpu_profile_report(GcrtCpu* g, FILE* out, double wall_seconds, u64 frames) {
    static const char* names[GCRT_PROF_COUNT] = {
        "generated code (excl. MMIO)", "HLE replacements / host_call", "non-RAM accesses (GX pipe)", "native timers",
        "clock synchronization"};
    double rate = gcrt_prof_ticks_per_second();
    double ms[GCRT_PROF_COUNT];
    for (int i = 0; i < GCRT_PROF_COUNT; i++)
        ms[i] = 1000.0 * (double)g->prof[i] / rate;
    /* The MMIO time measured inside the generated code is subtracted from its category. */
    ms[GCRT_PROF_CODE] -= 1000.0 * (double)g->prof_bus_in_code / rate;
    double total = 0.0;
    for (int i = 0; i < GCRT_PROF_COUNT; i++)
        total += ms[i];
    fprintf(out, "host profile: %.1f s sampled over %.1f s of run time%s\n", total / 1000.0, wall_seconds,
            frames ? "" : " (frame count unknown)");
    for (int i = 0; i < GCRT_PROF_COUNT; i++) {
        fprintf(out, "  %-34s %9.1f ms  %5.1f %%  %12llu calls", names[i], ms[i],
                wall_seconds > 0 ? ms[i] / (10.0 * wall_seconds) : 0.0, (unsigned long long)g->prof_calls[i]);
        if (frames)
            fprintf(out, "  %7.3f ms/frame", ms[i] / (double)frames);
        fprintf(out, "\n");
    }
    if (frames)
        fprintf(out, "  total per frame: %.2f ms (%llu frames; 20 ms = 50 Hz, 40 ms = 25 fps)\n",
                total / (double)frames, (unsigned long long)frames);
    static const char* fb[GCRT_FALLBACK_KINDS] = {"mfspr", "mtspr", "dcbst", "dcbf", "dcbi", "icbi", "other"};
    fprintf(out, "  delegated instructions: %llu (", (unsigned long long)g->fallback_count);
    for (int i = 0; i < GCRT_FALLBACK_KINDS; i++)
        fprintf(out, "%s%s %llu", i ? ", " : "", fb[i], (unsigned long long)g->fallback_kind[i]);
    fprintf(out, ")\n");
    if (!g->prof_samples)
        return;
    /* Busiest 256-byte buckets (time in generated code, excluding the MMIO counted separately). */
    u32 buckets = g->state.ram_size >> 8;
    u64 total_samples = 0;
    for (u32 i = 0; i < buckets; i++)
        total_samples += g->prof_samples[i];
    fprintf(out, "  generated code, samples per 256-byte area (%llu samples):\n",
            (unsigned long long)total_samples);
    for (int k = 0; k < 20 && total_samples; k++) {
        u32 best = 0;
        for (u32 i = 1; i < buckets; i++)
            if (g->prof_samples[i] > g->prof_samples[best])
                best = i;
        if (!g->prof_samples[best])
            break;
        fprintf(out, "    %08X-%08X %6.2f %%\n", GC_RAM_BASE + (best << 8), GC_RAM_BASE + (best << 8) + 0xFFu,
                100.0 * (double)g->prof_samples[best] / (double)total_samples);
        g->prof_samples[best] = 0;
    }
}

void gcrt_cpu_dump_memory(GcrtCpu* g, u32 address, u32 length, FILE* out) {
    CPUState* cpu = &g->state;
    /* Read without side effects: RAM only (physical or 0x8.../0xC...). */
    for (u32 offset = 0; offset < length; offset += 16u) {
        fprintf(out, "  %08X:", address + offset);
        for (u32 i = 0; i < 16u && offset + i < length; i += 4u) {
            u32 a = address + offset + i;
            if (pc_in_ram(cpu, a))
                fprintf(out, " %08X", mem_read32(cpu, a < cpu->ram_size ? (a | GC_RAM_BASE) : a));
            else
                fprintf(out, " --------");
        }
        fprintf(out, "\n");
    }
}

void gcrt_cpu_dump(GcrtCpu* g, FILE* out) {
    CPUState* cpu = &g->state;
    const GcrtModule* module = NULL;
    gcrt_cpu_lookup(g, cpu->pc, &module);
    fprintf(out, "pc=%08X (%s) lr=%08X ctr=%08X cr=%08X xer=%08X msr=%08X\n", cpu->pc,
            module ? module->name : "outside any module", cpu->lr, cpu->ctr, cpu->cr, cpu->xer, cpu->msr);
    fprintf(out, "srr0=%08X srr1=%08X dar=%08X dsisr=%08X hid2=%08X fpscr=%08X\n", cpu->srr0, cpu->srr1,
            cpu->dar, cpu->dsisr, cpu->hid2, cpu->fpscr);
    if (g->stop == GCRT_STOP_EXCEPTION)
        fprintf(out, "exception=%08X (program=%08X), srr0 at the time of the exception=%08X\n", g->last_exception,
                cpu->program_exception, g->last_exception_srr0);
    for (int i = 0; i < 32; i += 8)
        fprintf(out, "r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, cpu->gpr[i], cpu->gpr[i + 1],
                cpu->gpr[i + 2], cpu->gpr[i + 3], cpu->gpr[i + 4], cpu->gpr[i + 5], cpu->gpr[i + 6], cpu->gpr[i + 7]);
    fprintf(out, "blocks=%llu cycles=%llu ticks=%llu (%.3f guest s) timebase=%llu; host waits %llu "
                 "(%.3f guest s)\n",
            (unsigned long long)g->blocks, (unsigned long long)g->cycles, (unsigned long long)g->ticks,
            (double)g->ticks / (double)GCRT_TB_HZ, (unsigned long long)cpu->timebase, (unsigned long long)g->waits,
            (double)g->waited_ticks / (double)GCRT_TB_HZ);
    fprintf(out, "delegated instructions=%llu icbi=%llu delivered interrupts=%llu\n",
            (unsigned long long)g->fallback_count, (unsigned long long)g->icbi_count, (unsigned long long)g->deliveries);
    fprintf(out, "exceptions handled by the host:");
    bool any = false;
    for (u32 i = 0; i < GCRT_VECTOR_SLOTS; i++) {
        if (g->vector_count[i]) {
            fprintf(out, " %s(%04X)=%llu", vector_name(i << 8), i << 8, (unsigned long long)g->vector_count[i]);
            any = true;
        }
    }
    fprintf(out, "%s\n", any ? "" : " none");
    if (g->io_unhandled)
        fprintf(out, "unhandled hardware accesses=%llu, first: %s %08X at pc=%08X\n",
                (unsigned long long)g->io_unhandled, g->io_first_write ? "write" : "read",
                g->io_first_address, g->io_first_pc);
    gcrt_hw_count_report(g, out);
    /* The last 64 blocks only (the full history goes to crash reports). */
    u32 count = g->ring_pos < GCRT_TRACE_RING ? g->ring_pos : GCRT_TRACE_RING;
    if (count > g->ring_mask + 1u)
        count = g->ring_mask + 1u;
    fprintf(out, "last blocks (oldest to newest):");
    for (u32 i = 0; i < count; i++) {
        u32 index = (g->ring_pos - count + i) & g->ring_mask;
        fprintf(out, "%s%08X", (i % 8) ? " " : "\n  ", g->ring[index]);
    }
    fprintf(out, "\n");
}
