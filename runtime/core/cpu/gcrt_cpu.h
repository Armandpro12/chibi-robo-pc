/* CPU layer of the generic GameCube runtime (gcrt).
 *
 * Role: host the C code produced by DolRecomp (C backend). The generated code manipulates a `CPUState`
 * (external/DolRecomp/src/cpu/cpu.h) and calls the helpers of `cpu.c`. Around it, this module adds:
 *   - the registration of the recompiled "modules" (DOL + REL), each providing its own lookup
 *     function `dolrecomp_find_original` (see module_glue.c.in);
 *   - the "chassis" loop: each time the generated code returns, `pc` designates what comes next; we look for the
 *     module that contains it and call its block function again;
 *   - the delivery of native interrupts at block boundaries, and the time base. Milestone 10: no emulated hardware
 *     at all (neither decrementer nor external interrupt line: the OS alarms and all interrupt sources are
 *     native, hle/native_irq.c); the synchronous exceptions "FPU unavailable" and "system
 *     call" are handled by the host (exception_hook), without a vector; the guest processor's wait is a real
 *     wait (gcrt_cpu_wait_until);
 *   - the instructions that DolRecomp delegates to the host (`ppc_fallback_instruction`);
 *   - the default bus for accesses outside RAM: hardware access counter (milestone 10) and platform extension
 *     point (vertices written inline to the write-gather pipe, handed to native drawing).
 * No reference to any specific game here (reusability rule).
 */
#ifndef GCRT_CPU_H
#define GCRT_CPU_H

#include "cpu/cpu.h" /* external/DolRecomp/src/cpu/cpu.h */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Main memory as seen by the game (MEM1). The host RAM may be larger (virtual area of the RELs). */
#define GCRT_MEM1_SIZE 0x01800000u
#define GCRT_LOCKED_CACHE_BASE 0xE0000000u
#define GCRT_LOCKED_CACHE_SIZE 0x4000u

/* Clocks: Gekko 486 MHz, bus 162 MHz, time base and decrementer = bus / 4 = 40.5 MHz,
 * i.e. 12 CPU cycles per tick. [hypothesis: published frequencies, also written by the IPL at
 * 0x800000F8/0x800000FC (162000000 and 486000000), to be confirmed against Dolphin] */
#define GCRT_CPU_HZ 486000000u
#define GCRT_BUS_HZ 162000000u
#define GCRT_TB_HZ (GCRT_BUS_HZ / 4u)
#define GCRT_CYCLES_PER_TB_TICK (GCRT_CPU_HZ / GCRT_TB_HZ)

/* MSR bits used by the chassis. */
#define GCRT_MSR_EE 0x00008000u
#define GCRT_MSR_RI 0x00000002u


typedef void (*GcrtGuestFn)(CPUState* cpu);
typedef GcrtGuestFn (*GcrtFindFn)(u32 address);

/* A recompiled module (the DOL or a REL). */
typedef struct GcrtModule {
    const char* name;
    u32 entry_point; /* entry point (DOL) or prolog (REL), 0 if none */
    GcrtFindFn find; /* returns the block function that can run `address`, or NULL */
} GcrtModule;

typedef struct GcrtModuleTable {
    const GcrtModule* const* modules;
    unsigned count;
} GcrtModuleTable;

/* Accesses outside RAM (hardware addresses): return true if the access is handled. */
typedef bool (*GcrtBusRead)(void* user, u32 address, u32 size, u64* value);
typedef bool (*GcrtBusWrite)(void* user, u32 address, u32 size, u64 value);

/* Callback called before each block: return true if the host itself ran the code at `pc`
 * (replacement of a function, HLE). It must then have updated `pc` (usually to `lr`). */
typedef bool (*GcrtHostCall)(CPUState* cpu, u32 address);

struct GcrtCpu;
/* Synchronous exception raised by the code (`exception` = bits of cpu->exception, already cleared; SRR0/SRR1 and MSR
 * already set by ppc_take_exception): return true if the host handled it itself (pc and MSR restored, no vector),
 * false otherwise. */
typedef bool (*GcrtExceptionHook)(struct GcrtCpu* g, u32 exception);

typedef enum GcrtClock {
    GCRT_CLOCK_REALTIME = 0, /* ticks = host real time x 40.5 MHz (default: the game runs at its own speed) */
    GCRT_CLOCK_CYCLES,       /* ticks = guest cycles / 12 (deterministic, for tests) */
} GcrtClock;

typedef enum GcrtStopReason {
    GCRT_STOP_NONE = 0,
    GCRT_STOP_BLOCK_LIMIT,
    GCRT_STOP_TIME_LIMIT,
    GCRT_STOP_UNKNOWN_PC,    /* pc outside the recompiled modules (no more interpreter since milestone 10b) */
    GCRT_STOP_NO_PROGRESS,   /* the block returned without executing anything (pc outside the block's cases) */
    GCRT_STOP_EXCEPTION,     /* PowerPC exception, with --stop-on-exception or --stop-on-fault */
    GCRT_STOP_UNHANDLED_IO,  /* unhandled hardware access, with the "stop at the first access" option */
    GCRT_STOP_REQUESTED,
    GCRT_STOP_GUEST_HALT,    /* the game halted itself (OSPanic, PPCHalt): see the printed message */
} GcrtStopReason;

typedef struct GcrtRunLimits {
    u64 max_blocks;     /* 0: unlimited */
    double max_seconds; /* 0: unlimited */
} GcrtRunLimits;

/* History of the last guest blocks run (crash reports): GCRT_GUEST_HISTORY entries, power of two between 64 and
   1048576 (default 1024 = 4 KiB, one 4-byte store per block). GCRT_TRACE_RING is the size of the fallback array used
   when the allocation fails. */
#define GCRT_TRACE_RING 64
#define GCRT_GUEST_HISTORY_DEFAULT 1024u

/* Host profiling categories. */
enum {
    GCRT_PROF_CODE = 0, /* generated code (blocks), MMIO included */
    GCRT_PROF_HLE,      /* HLE replacements (host_call) */
    GCRT_PROF_BUS,      /* accesses outside RAM: vertices written to the write-gather pipe (native drawing) */
    GCRT_PROF_TIMER,    /* native timers (VI fields, audio DMA, frame rendering...) */
    GCRT_PROF_SYNC,     /* clock synchronization (excluding the timer) */
    GCRT_PROF_COUNT
};

/* Kinds of delegated instructions (ppc_fallback_instruction). */
enum {
    GCRT_FB_MFSPR = 0, GCRT_FB_MTSPR, GCRT_FB_DCBST, GCRT_FB_DCBF, GCRT_FB_DCBI, GCRT_FB_ICBI, GCRT_FB_OTHER,
    GCRT_FALLBACK_KINDS
};
#define GCRT_VECTOR_SLOTS 0x18 /* per-vector counters (0x100 * n, n < 0x18) */

/* Hardware register access counter (milestone 10: "no emulated hardware" criterion).
 * Counted at the CPU bus boundary (cpu_core.c): every guest access outside RAM goes through it (no emulated hardware
 * at all since step 10d). Blocks of 0xCC000000 (and of its physical alias 0x0C000000), from the console's memory
 * map (Dolphin, Core/HW/MMIO.h and HW/Memmap.cpp):
 *   CP 0x0000-0x0FFF, PE 0x1000-0x1FFF, VI 0x2000-0x2FFF, PI 0x3000-0x3FFF, MI 0x4000-0x4FFF,
 *   DSP 0x5000-0x5FFF excluding ARAM (mailboxes, DSPCR, AI audio DMA 0x5030-0x503F),
 *   AR 0x5010-0x502F (ARAM controller and DMA, inside the DSP block), DI 0x6000-0x63FF,
 *   SI 0x6400-0x67FF, EXI 0x6800-0x6BFF, AI 0x6C00-0x6FFF, WGPIPE 0x8000-0x8FFF (write-gather pipe).
 * Outside this block: EFB (0xC8000000-0xC8FFFFFF and 0x08000000-0x08FFFFFF, CPU access to the EFB) and OTHER
 * (any other access outside RAM and outside the locked cache). Exported format: notes/runtime.md, section 12.1. */
enum {
    GCRT_HW_CP = 0, GCRT_HW_PE, GCRT_HW_VI, GCRT_HW_PI, GCRT_HW_MI, GCRT_HW_DSP, GCRT_HW_AR, GCRT_HW_DI,
    GCRT_HW_SI, GCRT_HW_EXI, GCRT_HW_AI, GCRT_HW_WGPIPE, GCRT_HW_EFB, GCRT_HW_OTHER,
    /* Subset of WGPIPE (milestone 10c): writes to the pipe outside a draw opened by GXBegin, counted by native
     * drawing (hle/native_gx.c); must stay at 0 (only vertices written inline during a draw are tolerated). */
    GCRT_HW_WGPIPE_OUTSIDE,
    GCRT_HW_KINDS
};

typedef struct GcrtHwCount {
    u64 reads;
    u64 writes;
    u32 first_pc;      /* pc of the instruction of the first access */
    u32 first_lr;      /* lr at that moment (often the caller of the faulty function) */
    u32 first_address;
    bool first_write;
} GcrtHwCount;

typedef struct GcrtCpu {
    CPUState state;

    /* Special registers not modeled by CPUState (SPRG, BAT, HID0/1, L2CR, PMC, WPAR, DMA...). */
    u32 spr[1024];
    u8 locked_cache[GCRT_LOCKED_CACHE_SIZE];

    /* Time. `ticks` is monotonic (40.5 MHz); the game's time base = ticks + tb_offset
     * (the game may write TBL/TBU; the boot makes it start from the console time, like Dolphin).
     * Cycle clock: ticks = cycles / 12 + wait_ticks (time spent in host waits). */
    GcrtClock clock;
    u64 cycles;           /* guest cycles consumed (downcount of the generated code) */
    u64 ticks;
    u64 tb_offset;
    u64 tb_synced;        /* value of state.timebase after the last synchronization */
    u64 host_origin;      /* host clock at ticks = 0 (real-time mode) */
    u64 ticks_origin;
    u64 wait_ticks;       /* cycle clock: emulated time skipped by gcrt_cpu_wait_until */
    u64 waits;            /* host waits (OS idle, recognized busy waits) */
    u64 waited_ticks;     /* emulated time spent in these waits */
    /* SPR DEC: plain counter (read = written value minus the elapsed ticks); no more decrementer exception
     * since milestone 10b (the OS alarms are native timers, hle/native_os_alarm.c). */
    u32 dec_value;
    u64 dec_base_ticks;
    u64 deliveries;       /* native interrupts delivered: used to timestamp events */
    GcrtExceptionHook exception_hook; /* synchronous exceptions handled by the host (hle/native_os.c) */

    /* Recompiled modules and last module found (cache). */
    const GcrtModuleTable* modules;
    const GcrtModule* last_module;

    /* Accesses outside RAM (provided by the platform; NULL or refusal: unhandled access, logged). */
    GcrtBusRead bus_read;
    GcrtBusWrite bus_write;
    void* bus_user;
    GcrtHostCall host_call;

    /* Diagnostics. */
    bool stop_on_unhandled_io;
    bool stop_on_exception; /* any synchronous exception (including sc) */
    bool stop_on_fault;     /* program, DSI, ISI, alignment, machine check */
    u32 io_log_limit;       /* number of unhandled accesses logged (the following ones are only counted) */
    u64 io_unhandled;       /* unhandled accesses */
    u32 io_first_address;   /* first unhandled access */
    u32 io_first_pc;
    bool io_first_write;
    u64 fallback_count;     /* instructions delegated to the host */
    u64 fallback_kind[GCRT_FALLBACK_KINDS]; /* per kind (GCRT_FB_*; names in the host profile report, chassis.c) */
    GcrtHwCount hw_count[GCRT_HW_KINDS];    /* hardware register accesses, per subsystem */
    GcrtHwCount hw_count_boot[GCRT_HW_KINDS]; /* those of the boot, set apart by gcrt_hw_count_mark_boot (none
                                               * since milestone 10b: static boot) */
    bool hw_count_boot_marked;

    /* Native interrupts (milestone 10, hle/native_irq.c): callback called at a block boundary when the
     * ticks reach `native_irq_deadline` and MSR[EE] = 1. It decides by itself (OS masks) and prepares
     * the interrupt entry. */
    void (*native_irq_hook)(struct GcrtCpu* g);
    u64 native_irq_deadline; /* UINT64_MAX: nothing pending */
    /* Native timers (hle/native_irq.c): called by gcrt_cpu_sync_time when the ticks reach
     * `native_timer_deadline`, whatever MSR[EE] is. */
    void (*native_timer_hook)(struct GcrtCpu* g);
    u64 native_timer_deadline;

    /* Recognized busy waits of the game (cpu/idle_skip.c): replaced by a host wait until the next
     * event; enabled by the platform with real-time pacing. */
    bool idle_skip;
    struct GcrtIdleState* idle;
    /* host_call filter (one bit per RAM instruction, NULL: no filter): the chassis only calls host_call
     * for a marked address or one outside RAM. Any code that installs host_call must mark its addresses
     * (gcrt_cpu_mark_host_call: HLE replacements, the platform's hit counters). */
    u8* host_call_filter;
    u64 idle_skips;
    u64 idle_skipped_ticks;
    u32 idle_loops;

    /* Profiling (--profile): host time per category, in ticks of the processor counter (rdtsc). */
    bool profile;
    u64 prof[GCRT_PROF_COUNT];
    u64 prof_calls[GCRT_PROF_COUNT];
    u64 prof_bus_in_code; /* share of the MMIO time spent during generated code */
    int prof_in_code;
    /* Sampling of the guest pc, weighted by time (once per period): 256-byte buckets over RAM. */
    u32* prof_samples;
    u64 prof_next_sample;
    u64 prof_sample_period;
    /* Periodic reports (--profile-period): one line per window, as deltas since the previous one. */
    double prof_period_s;
    u64 prof_sample_from_ticks; /* pc sampling only after this guest instant (--profile-from) */
    u64 prof_next_window;
    u64 prof_window_start;
    u64 prof_last[GCRT_PROF_COUNT];
    u64 prof_last_bus_in_code;
    u64 prof_last_fallback;
    u64 prof_last_blocks;
    u64 icbi_count;
    u64 blocks;
    u64 vector_count[GCRT_VECTOR_SLOTS]; /* exceptions and interrupts delivered, per vector */
    u32 fault_log_limit;
    u32 faults_logged;
    GcrtStopReason stop;
    FILE* trace;            /* trace of each block (pc, lr, r1) */
    u64 trace_limit;
    u32* ring;              /* last blocks run: ring[ring_pos & ring_mask] (GCRT_GUEST_HISTORY) */
    u32 ring_mask;
    u32 ring_pos;
    u32 ring_fallback[GCRT_TRACE_RING];
    u32 last_exception;     /* copy of CPUState.exception at the time of the stop */
    u32 last_exception_srr0;
} GcrtCpu;

/* Initializes the CPU state and allocates `ram_size` bytes of host RAM (>= GCRT_MEM1_SIZE). */
bool gcrt_cpu_init(GcrtCpu* g, u32 ram_size, const GcrtModuleTable* modules);
void gcrt_cpu_free(GcrtCpu* g);
static inline GcrtCpu* gcrt_from_state(CPUState* cpu) { return (GcrtCpu*)cpu->external_user_data; }

/* --- Accesses outside RAM, time --- */

/* Attaches the bus for accesses outside RAM and outside the locked cache (platform: write-gather pipe). */
void gcrt_cpu_set_bus(GcrtCpu* g, GcrtBusRead read, GcrtBusWrite write, void* user);
/* Monotonic ticks at 40.5 MHz (same clock as the time base and the decrementer). */
u64 gcrt_cpu_ticks(const GcrtCpu* g);

/* --- Time --- */
void gcrt_cpu_set_clock(GcrtCpu* g, GcrtClock clock);
/* Counts guest cycles (advances the time base in GCRT_CLOCK_CYCLES mode). */
void gcrt_cpu_add_cycles(GcrtCpu* g, u64 cycles);
/* Updates ticks and the time base, then calls the timers whose deadline has passed. */
void gcrt_cpu_sync_time(GcrtCpu* g);
/* The guest processor has nothing to do before `deadline` (ticks): host wait. Real-time clock: the host
 * sleeps (at most a few milliseconds per call, the caller tries again); cycle clock: emulated time jumps
 * to the deadline (wait_ticks). No effect if the deadline has passed or is UINT64_MAX. */
void gcrt_cpu_wait_until(GcrtCpu* g, u64 deadline);
/* Next deadline known to the CPU layer (native timers, native interrupts if MSR[EE] = 1),
 * UINT64_MAX if there is none. */
u64 gcrt_cpu_next_deadline(const GcrtCpu* g);
/* Recognized busy waits (cpu/idle_skip.c). `enable` allocates the state on the first call; `try_skip` is called by
 * the chassis before a DOL block: true if the host waited for the next event (the block is not executed). */
bool gcrt_cpu_idle_skip_enable(GcrtCpu* g, bool on);
/* Declares an address handled by host_call (see host_call_filter). */
void gcrt_cpu_mark_host_call(GcrtCpu* g, u32 address);
bool gcrt_cpu_idle_try_skip(GcrtCpu* g, u32 pc);
void gcrt_cpu_idle_report(const GcrtCpu* g, FILE* out);

/* --- Execution --- */
/* Block function for `pc` (no alias), or NULL. */
GcrtGuestFn gcrt_cpu_lookup(GcrtCpu* g, u32 pc, const GcrtModule** module_out);
/* Chassis loop: runs from state.pc until a stop condition. */
GcrtStopReason gcrt_cpu_run(GcrtCpu* g, const GcrtRunLimits* limits);
void gcrt_cpu_request_stop(GcrtCpu* g);

/* --- Diagnostics --- */
const char* gcrt_stop_reason_name(GcrtStopReason reason);
/* Writes the CPU state (registers, last blocks, counters). */
void gcrt_cpu_dump(GcrtCpu* g, FILE* out);
/* Hardware access counter (hw_counter.c). Subsystem of an address (-1: RAM, locked cache). */
int gcrt_hw_kind_of(u32 address);
const char* gcrt_hw_kind_name(int kind);
/* Total of the counted accesses (WGPIPE_OUTSIDE, a subset of WGPIPE, is not added a second time). */
u64 gcrt_hw_count_total(const GcrtCpu* g);
/* Adds an access to counter `kind` (with pc and lr of the first one), for a category decided outside the bus
 * (WGPIPE_OUTSIDE). */
void gcrt_hw_count_add(GcrtCpu* g, int kind, u32 address, bool write);
/* End of boot (preparation by the runtime; the apploader is no longer run since milestone 10b): the accesses
 * counted until then are set apart ("boot"), the game's counter restarts from zero. */
void gcrt_hw_count_mark_boot(GcrtCpu* g);
/* Human-readable report (one line per subsystem touched). */
void gcrt_hw_count_report(const GcrtCpu* g, FILE* out);
/* JSON object on a single line (format "gcrt-hwcount/1", notes/runtime.md), without a trailing newline. */
void gcrt_hw_count_json(const GcrtCpu* g, FILE* out);
/* Compact summary "CP=r/w ..." of the subsystems touched (for periodic reports); returns `buf`. */
const char* gcrt_hw_count_summary(const GcrtCpu* g, char* buf, size_t size);
/* Writes `length` bytes of guest memory (virtual or physical address), 4 words per line. */
void gcrt_cpu_dump_memory(GcrtCpu* g, u32 address, u32 length, FILE* out);
/* Host processor counter for profiling, and its frequency (measured once). */
u64 gcrt_prof_now(void);
double gcrt_prof_ticks_per_second(void);
/* Profiling report: time per category and delegated instructions, relative to `frames` frames (0: none). */
void gcrt_cpu_profile_report(GcrtCpu* g, FILE* out, double wall_seconds, u64 frames);
/* Host profiler sampling the game thread (host_profiler.c, Windows, PDB symbols). */
void gcrt_host_profiler_start(void);
void gcrt_host_profiler_report(FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_CPU_H */
