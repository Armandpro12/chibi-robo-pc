/* CPU layer of the generic runtime: initialization, special registers, instructions delegated to the host,
 * default bus. See gcrt_cpu.h. */
#include "cpu/gcrt_cpu.h"

#include <stdlib.h>
#include <string.h>

/* Numbers of the special registers used here (PowerPC 750CL / Gekko user manual). */
enum {
    SPR_DEC = 22,
    SPR_PVR = 287,
    SPR_WPAR = 921,
    SPR_DMAU = 922,
    SPR_DMAL = 923,
    SPR_USER_MIRROR_FIRST = 936, /* UMMCR0..UPMC4: read-only copies of MMCR0..PMC4 (952..958) */
    SPR_USER_MIRROR_LAST = 942,
};

/* ---------------------------------------------------------------------------------------------
 * Time: monotonic ticks (40.5 MHz), the game's time base, timers, host waits
 *
 * Milestone 10b: no more hardware decrementer. The DEC register remains a counter (read = written value minus the
 * elapsed ticks, as in the 750CL user manual) but no longer raises an exception: the alarms of the SDK's OS are
 * replaced by native timers (hle/native_os_alarm.c).
 * ------------------------------------------------------------------------------------------- */

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static u64 host_now(u64* freq) {
    static u64 frequency; /* constant since system boot (QueryPerformanceFrequency documentation) */
    LARGE_INTEGER c;
    if (!frequency) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        frequency = (u64)f.QuadPart;
    }
    QueryPerformanceCounter(&c);
    *freq = frequency;
    return (u64)c.QuadPart;
}
#else
#include <time.h>
static u64 host_now(u64* freq) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *freq = 1000000000ull;
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
#endif

/* Profiling: cycle counter of the host processor (rdtsc, a few nanoseconds per read), calibrated
 * against the monotonic clock on the first call to gcrt_prof_ticks_per_second. */
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64)
#include <intrin.h>
u64 gcrt_prof_now(void) {
    return __rdtsc();
}
#else
u64 gcrt_prof_now(void) {
    u64 f;
    return host_now(&f);
}
#endif

double gcrt_prof_ticks_per_second(void) {
    static double rate;
    if (rate == 0.0) {
        u64 f0, f1;
        u64 h0 = host_now(&f0), c0 = gcrt_prof_now();
        u64 h1;
        do {
            h1 = host_now(&f1);
        } while ((double)(h1 - h0) / (double)f1 < 0.05);
        u64 c1 = gcrt_prof_now();
        rate = (double)(c1 - c0) / ((double)(h1 - h0) / (double)f1);
    }
    return rate;
}

static u64 host_to_ticks(u64 elapsed, u64 freq) {
    return (elapsed / freq) * GCRT_TB_HZ + ((elapsed % freq) * GCRT_TB_HZ) / freq;
}

void gcrt_cpu_add_cycles(GcrtCpu* g, u64 cycles) {
    g->cycles += cycles;
}

u64 gcrt_cpu_ticks(const GcrtCpu* g) {
    return g->ticks;
}

void gcrt_cpu_set_clock(GcrtCpu* g, GcrtClock clock) {
    u64 freq;
    g->clock = clock;
    g->host_origin = host_now(&freq);
    g->ticks_origin = g->ticks;
}

void gcrt_cpu_sync_time(GcrtCpu* g) {
    CPUState* cpu = &g->state;
    /* TBL/TBU written by the game (ppc_mtspr modifies state.timebase): readjust the offset. */
    if (cpu->timebase != g->tb_synced)
        g->tb_offset = cpu->timebase - g->ticks;

    if (g->clock == GCRT_CLOCK_REALTIME) {
        u64 freq;
        u64 now = host_now(&freq);
        u64 t = g->ticks_origin + host_to_ticks(now - g->host_origin, freq);
        if (t > g->ticks)
            g->ticks = t;
    } else {
        u64 t = g->cycles / GCRT_CYCLES_PER_TB_TICK + g->wait_ticks;
        if (t > g->ticks)
            g->ticks = t;
    }
    cpu->timebase = g->ticks + g->tb_offset;
    g->tb_synced = cpu->timebase;

    if (g->native_timer_hook && g->ticks >= g->native_timer_deadline) {
        u64 t0 = g->profile ? gcrt_prof_now() : 0;
        g->native_timer_hook(g);
        if (g->profile) {
            g->prof[GCRT_PROF_TIMER] += gcrt_prof_now() - t0;
            g->prof_calls[GCRT_PROF_TIMER]++;
        }
    }
}

u64 gcrt_cpu_next_deadline(const GcrtCpu* g) {
    u64 dl = g->native_timer_deadline;
    if ((g->state.msr & GCRT_MSR_EE) && g->native_irq_deadline < dl)
        dl = g->native_irq_deadline;
    return dl;
}

/* Host wait with the real-time clock: at most this delay per call (the window and stop requests keep being served). */
#define WAIT_MAX_HOST_MS 4u

void gcrt_cpu_wait_until(GcrtCpu* g, u64 deadline) {
    if (deadline == UINT64_MAX || deadline <= g->ticks)
        return;
    g->waits++;
    const u64 before = g->ticks;
    if (g->clock == GCRT_CLOCK_REALTIME) {
        u64 freq;
        u64 now = host_now(&freq);
        /* Host instant of the deadline (same origins as gcrt_cpu_sync_time), capped. */
        const u64 span = deadline - g->ticks_origin;
        u64 target = g->host_origin + (span / GCRT_TB_HZ) * freq + ((span % GCRT_TB_HZ) * freq) / GCRT_TB_HZ;
        const u64 cap = now + (freq * WAIT_MAX_HOST_MS) / 1000u;
        if (target > cap)
            target = cap;
        while (now < target) {
            const u64 left_ms = ((target - now) * 1000u) / freq;
#if defined(_WIN32)
            if (left_ms >= 2u)
                Sleep((DWORD)(left_ms - 1u));
            else
                SwitchToThread();
#else
            (void)left_ms;
#endif
            now = host_now(&freq);
        }
        gcrt_cpu_sync_time(g);
    } else {
        /* Cycle clock (deterministic): emulated time jumps to the deadline. */
        const u64 t = g->cycles / GCRT_CYCLES_PER_TB_TICK + g->wait_ticks;
        if (deadline > t)
            g->wait_ticks += deadline - t;
        gcrt_cpu_sync_time(g);
    }
    if (g->ticks > before)
        g->waited_ticks += g->ticks - before;
}

static void dec_write(GcrtCpu* g, u32 value) {
    g->dec_value = value;
    g->dec_base_ticks = g->ticks;
}

/* ---------------------------------------------------------------------------------------------
 * Locked cache (16 KiB at 0xE0000000) and its DMA
 * ------------------------------------------------------------------------------------------- */

static bool locked_cache_offset(u32 address, u32 size, u32* offset) {
    if (address < GCRT_LOCKED_CACHE_BASE || address - GCRT_LOCKED_CACHE_BASE > GCRT_LOCKED_CACHE_SIZE - size)
        return false;
    *offset = address - GCRT_LOCKED_CACHE_BASE;
    return true;
}

/* DMA between RAM and the locked cache, triggered by a write to DMAL with the T bit.
 * Format [hypothesis: based on Dolphin's behavior (PowerPC/MMU), to be verified]:
 *   DMAU = memory address (bits 0-26) | length, high bits (5 bits)
 *   DMAL = address in the locked cache (bits 0-26) | LD (0x10) | length, low bits (0x0C) | T (0x02) | F (0x01)
 *   length in 32-byte lines, 0 = 128 lines; LD = 1: RAM -> cache, LD = 0: cache -> RAM.
 * The transfer is instantaneous (the game waits for completion by reading the queue length in HID2, always 0 here). */
static void locked_cache_dma(GcrtCpu* g) {
    u32 dmau = g->spr[SPR_DMAU];
    u32 dmal = g->spr[SPR_DMAL];
    u32 mem = dmau & ~31u;
    u32 cache = dmal & ~31u;
    u32 lines = ((dmau & 31u) << 2) | ((dmal >> 2) & 3u);
    if (lines == 0)
        lines = 128;
    u32 bytes = lines * 32u;
    u32 offset;
    if (!locked_cache_offset(cache, bytes, &offset)) {
        fprintf(stderr, "gcrt: locked cache DMA out of bounds (DMAU=%08X DMAL=%08X)\n", dmau, dmal);
        return;
    }
    if (dmal & 0x10u) {
        for (u32 i = 0; i < bytes; i += 4)
            write_be32(&g->locked_cache[offset + i], mem_read32(&g->state, mem + i));
    } else {
        for (u32 i = 0; i < bytes; i += 4)
            mem_write32(&g->state, mem + i, read_be32(&g->locked_cache[offset + i]));
    }
}

/* ---------------------------------------------------------------------------------------------
 * Special registers not modeled by CPUState (called by DolRecomp's ppc_mfspr / ppc_mtspr)
 * ------------------------------------------------------------------------------------------- */

static u32 spr_read(CPUState* cpu, u16 spr, u32 cia) {
    GcrtCpu* g = gcrt_from_state(cpu);
    (void)cia;
    switch (spr) {
    case SPR_DEC:
        return g->dec_value - (u32)(g->ticks - g->dec_base_ticks);
    case SPR_PVR:
        return PPC_GEKKO_PVR;
    default:
        break;
    }
    if (spr >= SPR_USER_MIRROR_FIRST && spr <= SPR_USER_MIRROR_LAST)
        spr = (u16)(spr + 16u);
    return g->spr[spr & 1023u];
}

static void spr_write(CPUState* cpu, u16 spr, u32 value, u32 cia) {
    GcrtCpu* g = gcrt_from_state(cpu);
    (void)cia;
    switch (spr) {
    case SPR_DEC:
        dec_write(g, value);
        return;
    case SPR_DMAL:
        g->spr[SPR_DMAL] = value;
        if (value & 0x2u) {
            locked_cache_dma(g);
            g->spr[SPR_DMAL] &= ~0x2u;
        }
        return;
    case SPR_WPAR:
        /* Address of the "write-gather pipe". The BNE bit (0x1, buffer not empty) is never set here. */
        g->spr[SPR_WPAR] = value & ~0x1Fu;
        return;
    default:
        break;
    }
    if (spr >= SPR_USER_MIRROR_FIRST && spr <= SPR_USER_MIRROR_LAST)
        spr = (u16)(spr + 16u);
    g->spr[spr & 1023u] = value;
}

static void cache_control(CPUState* cpu, u8 operation, u32 ea, u32 cia) {
    GcrtCpu* g = gcrt_from_state(cpu);
    (void)ea;
    (void)cia;
    /* No cache modeled: dcbst/dcbf/dcbi have no effect. icbi is counted: it signals code
     * copied or modified in memory (not executable by the recompiled code, see notes/recomp.md). */
    if (operation == PPC_CACHE_ICBI)
        g->icbi_count++;
}

/* ---------------------------------------------------------------------------------------------
 * Instructions that DolRecomp delegates to the host (ppc_fallback_instruction)
 *
 * The generated code does `ctx->pc = cia; ppc_fallback_instruction(ctx, raw, cia); return;`: after
 * emulation, pc is set to the next instruction (every address is a block entry point).
 * ------------------------------------------------------------------------------------------- */

static void instruction_fallback(CPUState* cpu, u32 raw, u32 cia) {
    GcrtCpu* g = gcrt_from_state(cpu);
    g->fallback_count++;

    if ((raw >> 26) == 31u) {
        u32 xo = (raw >> 1) & 0x3FFu;
        u32 rd = (raw >> 21) & 31u;
        u32 ra = (raw >> 16) & 31u;
        u32 rb = (raw >> 11) & 31u;
        u16 spr = (u16)(((raw >> 16) & 31u) | (((raw >> 11) & 31u) << 5));
        switch (xo) {
        case 339: { /* mfspr */
            g->fallback_kind[GCRT_FB_MFSPR]++;
            u32 value = ppc_mfspr(cpu, spr, cia);
            if (cpu->exception)
                return;
            cpu->gpr[rd] = value;
            cpu->pc = cia + 4u;
            return;
        }
        case 467: /* mtspr */
            g->fallback_kind[GCRT_FB_MTSPR]++;
            ppc_mtspr(cpu, spr, cpu->gpr[rd], cia);
            if (!cpu->exception)
                cpu->pc = cia + 4u;
            return;
        case 54:  /* dcbst */
        case 86:  /* dcbf */
        case 470: /* dcbi */
        case 982: { /* icbi */
            u8 op = xo == 54 ? PPC_CACHE_DCBST : xo == 86 ? PPC_CACHE_DCBF
                  : xo == 470 ? PPC_CACHE_DCBI : PPC_CACHE_ICBI;
            g->fallback_kind[op == PPC_CACHE_DCBST ? GCRT_FB_DCBST : op == PPC_CACHE_DCBF ? GCRT_FB_DCBF
                             : op == PPC_CACHE_DCBI ? GCRT_FB_DCBI : GCRT_FB_ICBI]++;
            u32 ea = (ra ? cpu->gpr[ra] : 0u) + cpu->gpr[rb];
            ppc_cache_control(cpu, op, ea, cia);
            if (!cpu->exception)
                cpu->pc = cia + 4u;
            return;
        }
        default:
            break;
        }
    }

    g->fallback_kind[GCRT_FB_OTHER]++;
    fprintf(stderr, "gcrt: unhandled instruction %08X at %08X -> program exception\n", raw, cia);
    ppc_program_exception(cpu, PPC_PROGRAM_ILLEGAL, cia);
}

/* ---------------------------------------------------------------------------------------------
 * Default bus: called by mem_readN and mem_writeN (cpu.c) for any address outside mapped RAM
 * ------------------------------------------------------------------------------------------- */

static u64 read_sized(const u8* p, u32 size) {
    switch (size) {
    case 1: return p[0];
    case 2: return read_be16(p);
    case 4: return read_be32(p);
    default: return read_be64(p);
    }
}

static void write_sized(u8* p, u32 size, u64 value) {
    switch (size) {
    case 1: p[0] = (u8)value; break;
    case 2: write_be16(p, (u16)value); break;
    case 4: write_be32(p, (u32)value); break;
    default: write_be64(p, value); break;
    }
}

static void unhandled_access(GcrtCpu* g, u32 address, u32 size, bool write, u64 value) {
    if (g->io_unhandled == 0) {
        g->io_first_address = address;
        g->io_first_pc = g->state.pc;
        g->io_first_write = write;
    }
    if (g->io_unhandled < g->io_log_limit) {
        if (write)
            fprintf(stderr, "gcrt: unhandled write%u %08X <- %08llX (pc=%08X lr=%08X)\n", size * 8u, address,
                    (unsigned long long)value, g->state.pc, g->state.lr);
        else
            fprintf(stderr, "gcrt: unhandled read%u %08X (pc=%08X lr=%08X)\n", size * 8u, address,
                    g->state.pc, g->state.lr);
    }
    g->io_unhandled++;
    if (g->stop_on_unhandled_io && g->stop == GCRT_STOP_NONE)
        g->stop = GCRT_STOP_UNHANDLED_IO;
}

/* Hardware access counter (gcrt_cpu.h, "Hardware register access counter"): every access that reaches the bus,
 * outside RAM and the locked cache, is counted here, before being handed to the platform bus (if attached). */
static void count_hw_access(GcrtCpu* g, u32 address, bool write) {
    int kind = gcrt_hw_kind_of(address);
    if (kind < 0)
        kind = GCRT_HW_OTHER;
    GcrtHwCount* c = &g->hw_count[kind];
    if (c->reads + c->writes == 0) {
        c->first_pc = g->state.pc;
        c->first_lr = g->state.lr;
        c->first_address = address;
        c->first_write = write;
    }
    if (write)
        c->writes++;
    else
        c->reads++;
}

static u64 bus_read(CPUState* cpu, u32 address, u8 size) {
    GcrtCpu* g = gcrt_from_state(cpu);
    u32 offset;
    /* Physical address (real-mode access, MSR[DR] = 0, e.g. the OS exception handlers). */
    if (address < cpu->ram_size && cpu->ram_size - address >= size)
        return read_sized(cpu->ram + address, size);
    if (locked_cache_offset(address, size, &offset))
        return read_sized(g->locked_cache + offset, size);
    count_hw_access(g, address, false);
    u64 value = 0;
    if (g->bus_read) {
        u64 t0 = g->profile ? gcrt_prof_now() : 0;
        bool handled = g->bus_read(g->bus_user, address, size, &value);
        if (g->profile) {
            u64 dt = gcrt_prof_now() - t0;
            g->prof[GCRT_PROF_BUS] += dt;
            g->prof_calls[GCRT_PROF_BUS]++;
            if (g->prof_in_code)
                g->prof_bus_in_code += dt;
        }
        if (handled)
            return value;
    }
    unhandled_access(g, address, size, false, 0);
    return 0;
}

static void bus_write(CPUState* cpu, u32 address, u64 value, u8 size) {
    GcrtCpu* g = gcrt_from_state(cpu);
    u32 offset;
    if (address < cpu->ram_size && cpu->ram_size - address >= size) {
        write_sized(cpu->ram + address, size, value);
        return;
    }
    if (locked_cache_offset(address, size, &offset)) {
        write_sized(g->locked_cache + offset, size, value);
        return;
    }
    count_hw_access(g, address, true);
    if (g->bus_write) {
        u64 t0 = g->profile ? gcrt_prof_now() : 0;
        bool handled = g->bus_write(g->bus_user, address, size, value);
        if (g->profile) {
            u64 dt = gcrt_prof_now() - t0;
            g->prof[GCRT_PROF_BUS] += dt;
            g->prof_calls[GCRT_PROF_BUS]++;
            if (g->prof_in_code)
                g->prof_bus_in_code += dt;
        }
        if (handled)
            return;
    }
    unhandled_access(g, address, size, true, value);
}

/* ---------------------------------------------------------------------------------------------
 * Initialization
 * ------------------------------------------------------------------------------------------- */

bool gcrt_cpu_init(GcrtCpu* g, u32 ram_size, const GcrtModuleTable* modules) {
    memset(g, 0, sizeof(*g));
    if (ram_size < GCRT_MEM1_SIZE)
        ram_size = GCRT_MEM1_SIZE;
    /* cpu_init allocates 24 MiB; replaced with the requested size (virtual area of the RELs beyond MEM1). */
    if (!cpu_init(&g->state))
        return false;
    if (ram_size != g->state.ram_size) {
        u8* ram = (u8*)calloc(1, ram_size);
        if (!ram) {
            fprintf(stderr, "gcrt: cannot allocate %u bytes of RAM\n", ram_size);
            cpu_free(&g->state);
            return false;
        }
        free(g->state.ram);
        g->state.ram = ram;
        g->state.ram_size = ram_size;
    }
    CPUState* cpu = &g->state;
    cpu->external_user_data = g;
    cpu->external_read = bus_read;
    cpu->external_write = bus_write;
    cpu->instruction_fallback = instruction_fallback;
    cpu->spr_read = spr_read;
    cpu->spr_write = spr_write;
    cpu->cache_control = cache_control;
    g->modules = modules;
    g->io_log_limit = 32;
    g->fault_log_limit = 16;
    g->native_irq_deadline = UINT64_MAX;
    g->native_timer_deadline = UINT64_MAX;
    /* DEC at boot: 0xFFFFFFFF (Dolphin SystemTimers::Init); a plain counter since milestone 10b. */
    g->dec_value = 0xFFFFFFFFu;
    /* History of the last blocks run (GCRT_GUEST_HISTORY, see gcrt_cpu.h). */
    {
        u32 want = GCRT_GUEST_HISTORY_DEFAULT;
        const char* env = getenv("GCRT_GUEST_HISTORY");
        if (env && env[0]) {
            const long n = strtol(env, NULL, 0);
            want = n < GCRT_TRACE_RING ? GCRT_TRACE_RING : n > (1L << 20) ? (1u << 20) : (u32)n;
        }
        u32 size = GCRT_TRACE_RING;
        while (size < want)
            size <<= 1;
        g->ring = size > GCRT_TRACE_RING ? (u32*)calloc(size, sizeof(u32)) : NULL;
        if (!g->ring) {
            g->ring = g->ring_fallback;
            size = GCRT_TRACE_RING;
        }
        g->ring_mask = size - 1u;
    }
    gcrt_cpu_set_clock(g, GCRT_CLOCK_REALTIME);
    return true;
}

void gcrt_cpu_free(GcrtCpu* g) {
    cpu_free(&g->state);
    free(g->prof_samples);
    g->prof_samples = NULL;
    if (g->ring != g->ring_fallback)
        free(g->ring);
    g->ring = g->ring_fallback;
    g->ring_mask = GCRT_TRACE_RING - 1u;
    if (g->trace) {
        fclose(g->trace);
        g->trace = NULL;
    }
}

void gcrt_cpu_set_bus(GcrtCpu* g, GcrtBusRead read, GcrtBusWrite write, void* user) {
    g->bus_read = read;
    g->bus_write = write;
    g->bus_user = user;
}
