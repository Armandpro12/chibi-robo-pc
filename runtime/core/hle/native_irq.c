/* Native interrupts and calls of guest functions from native code (milestone 10). See gcrt_native.h and
 * notes/runtime.md § 12.3 (native callbacks).
 *
 * What the console does for an external interrupt (SDK, reference decompilation consulted only:
 * OSException/OS.c __OSExceptionVector, OSInterrupt.c ExternalInterruptHandler and __OSDispatchInterrupt,
 * OSContext.c OSLoadContext):
 *   1. the vector saves r3-r5, CR, LR, CTR, XER, SRR0 (interrupted pc), SRR1 (interrupted MSR) into the current
 *      context (pointer at 0x800000C0/0x800000D4) and marks it with the OS_CONTEXT_STATE_EXC state (0x02); the
 *      handler runs with the exception MSR (EE, FP, IR, DR, RI... at 0) plus IR|DR;
 *   2. ExternalInterruptHandler saves r0-r2, r6-r31, GQR1-7, then `stwu r1, -8(r1)` (same stack);
 *   3. __OSDispatchInterrupt picks the cause, then OSDisableScheduler; handler(interrupt, context);
 *      OSEnableScheduler; __OSReschedule (may switch to another thread: the context marked "exception" is not
 *      saved again); OSLoadContext(context) (restores everything and `rfi`).
 * Here, the host performs the same sequence at a block boundary: steps 1-2 natively, OSDisableScheduler, native
 * handler and OSEnableScheduler (MSR[EE] = 0: no thread switch possible during the handler), then __OSReschedule
 * and OSLoadContext. Milestone 10b: the OS is native (native_os.c): suspending the scheduler, rescheduling and
 * reloading the context are done by the host, without `rfi`; a thread switch is allowed there. The OS alarms
 * (native_os_alarm.c) go through the same queue, in a "decrementer" class: not maskable by the OS, delivered after
 * the external interrupts, and their callback manages the scheduler itself (like DecrementerExceptionCallback).
 * Step 10d: all sources are native (no more interrupt line from emulated hardware). Several external interrupts due
 * at the same block boundary are delivered in the order of __OSDispatchInterrupt: the SDK priority table, then the
 * lowest number within a group (cntlzw), one per entry, like the console where the exception is taken again as
 * long as a cause stays pending. Floating-point registers are not saved, as on the console (lazy FPU switching of the
 * OS: the handler runs with MSR[FP] = 0). Generic: only the SDK conventions are used. */
#include "hle/gcrt_native.h"
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/gcrt_path.h"
#include "platform/trace.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* OS low memory (SDK). */
#define LOW_INTERRUPT_MASK_PRIOR 0x800000C4u   /* __OSPriorInterruptMask */
#define LOW_INTERRUPT_MASK_CURRENT 0x800000C8u /* __OSCurrentInterruptMask */
#define LOW_CURRENT_CONTEXT 0x800000D4u        /* current OSContext* (virtual address) */

/* OSContext (SDK, size 0x2C8). */
#define CTX_GPR 0x000u
#define CTX_CR 0x080u
#define CTX_LR 0x084u
#define CTX_CTR 0x088u
#define CTX_XER 0x08Cu
#define CTX_SRR0 0x198u
#define CTX_SRR1 0x19Cu
#define CTX_MODE 0x1A0u
#define CTX_STATE 0x1A2u
#define CTX_GQR 0x1A4u
#define CTX_SIZE 0x2C8u
#define CTX_STATE_EXC 0x02u

/* MSR: bits cleared by an exception (750CL manual: POW, EE, PR, FP, FE0, SE, BE, FE1, IR, DR, RI),
 * then IR|DR set again by the OS vector. */
#define MSR_POW 0x00040000u
#define MSR_EE 0x00008000u
#define MSR_PR 0x00004000u
#define MSR_FP 0x00002000u
#define MSR_FE0 0x00000800u
#define MSR_SE 0x00000400u
#define MSR_BE 0x00000200u
#define MSR_FE1 0x00000100u
#define MSR_IR 0x00000020u
#define MSR_DR 0x00000010u
#define MSR_RI 0x00000002u
#define MSR_EXCEPTION_CLEARS (MSR_POW | MSR_EE | MSR_PR | MSR_FP | MSR_FE0 | MSR_SE | MSR_BE | MSR_FE1 | MSR_IR | MSR_DR | MSR_RI)

#define EXCEPTION_EXTERNAL 4u
#define EXCEPTION_DECREMENTER 8u
/* Fixed return address after __OSReschedule (guest OS, without the native OS: outside guest memory, like the
 * "magic" returns of hle.c; the context address is stored on the guest stack). */
#define MAGIC_AFTER_RESCHEDULE 0xF0000800u
/* Stack frame of the native entry (below the one of `stwu r1, -8(r1)`): +0 back chain, +4 callee LR,
 * +8 address of the interrupted context. */
#define ENTRY_FRAME 0x10u

#define MAX_EVENTS 128u
#define NESTED_CALL_BLOCK_BUDGET 200000000ull
/* Blocks between two checks of the held-interrupt limit (gcrt_guest_call_set_held_irq_limit, opt-in). */
#define HELD_IRQ_SLICE_BLOCKS 1000000ull

typedef struct {
    u32 id;
    unsigned interrupt;
    u64 due;
    GcrtNativeIrqFn fn;
    void* user;
} Event;

/* Priority table of the SDK external interrupts (InterruptPrioTable of __OSDispatchInterrupt): groups by
 * decreasing priority; within a group, the lowest number first (cntlzw). Mask = 0x80000000 >> number. Read in the
 * DOL of a game (the 11 words of the table, identical in two regions of the disc) and in its dispatch code, which
 * scans the groups in order and takes cntlzw of the first non-empty intersection with the pending causes. */
#define PRIO_M(n) (0x80000000u >> (n))
static const u32 k_sdk_priority[] = {
    PRIO_M(23),                                                          /* PI_ERROR */
    PRIO_M(25),                                                          /* PI_DEBUG */
    PRIO_M(0) | PRIO_M(1) | PRIO_M(2) | PRIO_M(3) | PRIO_M(4),           /* MEM */
    PRIO_M(22),                                                          /* PI_RSW */
    PRIO_M(24),                                                          /* PI_VI */
    PRIO_M(18) | PRIO_M(19),                                             /* PI_PE */
    PRIO_M(26),                                                          /* PI_HSP */
    PRIO_M(6) | PRIO_M(7) | PRIO_M(8) | PRIO_M(9) | PRIO_M(10) | PRIO_M(11) | PRIO_M(12) | PRIO_M(13) |
        PRIO_M(14) | PRIO_M(15) | PRIO_M(16) | PRIO_M(20) | PRIO_M(21), /* DSP_ARAM, DSP_DSP, AI, EXI, SI, DI */
    PRIO_M(5),                                                           /* DSP_AI */
    PRIO_M(17),                                                          /* PI_CP */
    0xFFFFFFFFu,
};

/* Rank of an interrupt in the SDK table (lower = delivered first). */
static unsigned sdk_rank(unsigned interrupt) {
    const u32 bit = PRIO_M(interrupt & 31u);
    for (unsigned i = 0; i < sizeof(k_sdk_priority) / sizeof(k_sdk_priority[0]); i++)
        if (k_sdk_priority[i] & bit)
            return i * 32u + (interrupt & 31u);
    return 0xFFFFu;
}

#define CLASS_COUNT 33u /* 32 OS interrupts + the decrementer class */
static Event s_events[MAX_EVENTS];
static unsigned s_event_count;
static u32 s_next_id = 1;
static bool s_initialized;
static u32 s_os_fn[GCRT_OSFN_COUNT];
#define s_fn_disable_scheduler s_os_fn[GCRT_OSFN_DISABLE_SCHEDULER]
#define s_fn_enable_scheduler s_os_fn[GCRT_OSFN_ENABLE_SCHEDULER]
#define s_fn_reschedule s_os_fn[GCRT_OSFN_RESCHEDULE]
#define s_fn_load_context s_os_fn[GCRT_OSFN_LOAD_CONTEXT]
#define s_fn_clear_context s_os_fn[GCRT_OSFN_CLEAR_CONTEXT]
#define s_fn_set_current_context s_os_fn[GCRT_OSFN_SET_CURRENT_CONTEXT]
static u64 s_delivered, s_masked_checks, s_guest_calls, s_guest_call_failures, s_deferred_reschedules;
static u64 s_delivered_by_irq[CLASS_COUNT];
/* Native interrupt handler in progress (depth); replacements in progress on the game thread and suspension of the
 * scheduler for their callbacks. */
static int s_interrupt_depth;
static int s_call_depth;
static bool s_sched_held;
static int s_guest_call_depth;
static bool s_skip_reschedule;
static GcrtPadSource s_pad_source;
static GcrtPadRumble s_pad_rumble;
static void* s_pad_user;

/* ---------------------------------------------------------------------------------------------
 * Diagnostic timeline (GCRT_TIMELINE)
 * ------------------------------------------------------------------------------------------- */

bool g_gcrt_timeline_on;
static FILE* s_timeline;
static GcrtCpu* s_timeline_cpu;

static void timeline_open(GcrtCpu* g) {
    s_timeline_cpu = g;
    char path[4096]; /* UTF-8 path (wide-character environment on Windows, gcrt_path.h) */
    if (!gcrt_getenv_utf8("GCRT_TIMELINE", path, sizeof(path)) || s_timeline)
        return;
    s_timeline = gcrt_fopen_utf8(path, "w");
    if (!s_timeline) {
        fprintf(stderr, "gcrt native: cannot write the timeline %s\n", path);
        return;
    }
    setvbuf(s_timeline, NULL, _IOFBF, 1u << 20);
    g_gcrt_timeline_on = true;
}

void gcrt_timeline(const char* fmt, ...) {
    if (!s_timeline)
        return;
    const GcrtCpu* g = s_timeline_cpu;
    fprintf(s_timeline, "%llu %08X %d ", g ? (unsigned long long)g->ticks : 0ull, g ? g->state.pc : 0u,
            s_interrupt_depth);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_timeline, fmt, ap);
    va_end(ap);
    fputc('\n', s_timeline);
}

static bool in_ram(const CPUState* c, u32 address, u32 size) {
    return address >= GC_RAM_BASE && address - GC_RAM_BASE <= c->ram_size && c->ram_size - (address - GC_RAM_BASE) >= size;
}

u32 gcrt_native_sdk_function(const char* name) {
    unsigned count = 0;
    u32 address = gcrt_hle_find(name, &count);
    if (count != 1) {
        fprintf(stderr, "gcrt native: %s %s by signature\n", name,
                count ? "recognized several times" : "not found");
        return count ? address : 0;
    }
    return address;
}

/* ---------------------------------------------------------------------------------------------
 * Nested calls
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    bool returned;
    u32 r3;
} CallState;

static void call_returned(GcrtCpu* g, u32 original_return, void* data) {
    (void)original_return;
    CallState* state;
    memcpy(&state, data, sizeof(state));
    state->returned = true;
    state->r3 = g->state.gpr[3];
    gcrt_cpu_request_stop(g);
}

int gcrt_guest_call_depth(void) { return s_guest_call_depth; }

/* Held-interrupt limit (opt-in, see gcrt_native.h). s_outer_ee: MSR[EE] of the outermost nested call when it started
 * (the nested levels below it run with EE = 0 whatever the game had). */
static u64 s_held_irq_limit;
static bool s_outer_ee;

void gcrt_guest_call_set_held_irq_limit(u64 ticks) { s_held_irq_limit = ticks; }

/* Oldest due and unmasked event (external interrupt or alarm), or NULL. */
static const Event* oldest_held_event(GcrtCpu* g) {
    const Event* best = NULL;
    for (unsigned i = 0; i < s_event_count; i++) {
        const Event* e = &s_events[i];
        if (e->due <= g->ticks && !gcrt_nirq_masked(g, e->interrupt) && (!best || e->due < best->due))
            best = e;
    }
    return best;
}

/* Runs a nested call until it returns or stops. Default (no limit, or the outermost call started with EE = 0, e.g. a
 * handler's callback): one run of the chassis with the block budget, as before the limit existed. With the limit:
 * slices of HELD_IRQ_SLICE_BLOCKS blocks (a run of the chassis resumes at the pc where the previous one stopped, on a
 * block boundary), and after each slice the oldest held interrupt is checked against the limit. */
static GcrtStopReason run_nested(GcrtCpu* g, u32 function) {
    const u64 end = g->blocks + NESTED_CALL_BLOCK_BUDGET;
    if (!s_held_irq_limit || !s_outer_ee) {
        GcrtRunLimits limits = {end, 0.0};
        return gcrt_cpu_run(g, &limits);
    }
    const u64 entry_blocks = g->blocks;
    for (;;) {
        GcrtRunLimits limits = {g->blocks + HELD_IRQ_SLICE_BLOCKS < end ? g->blocks + HELD_IRQ_SLICE_BLOCKS : end, 0.0};
        const GcrtStopReason reason = gcrt_cpu_run(g, &limits);
        if (reason != GCRT_STOP_BLOCK_LIMIT || g->blocks >= end)
            return reason;
        const Event* e = oldest_held_event(g);
        if (e && g->ticks - e->due > s_held_irq_limit) {
            fprintf(stderr,
                    "gcrt native: the nested call to %08X (depth %d, pc %08X) held the %s %u for %llu ticks (limit "
                    "%llu) after %llu blocks: a callee waiting for an interrupt or another thread "
                    "never returns in a nested call; stop\n",
                    function, s_guest_call_depth, g->state.pc,
                    e->interrupt >= GCRT_NIRQ_DECREMENTER ? "alarm class" : "interrupt", e->interrupt,
                    (unsigned long long)(g->ticks - e->due), (unsigned long long)s_held_irq_limit,
                    (unsigned long long)(g->blocks - entry_blocks));
            g->stop = GCRT_STOP_HELD_INTERRUPT;
            return g->stop;
        }
    }
}

bool gcrt_guest_call(GcrtCpu* g, u32 function, const u32* args, unsigned count, u32* result) {
    CPUState* c = &g->state;
    u32 gpr[32];
    memcpy(gpr, c->gpr, sizeof(gpr));
    const u32 pc = c->pc, lr = c->lr, ctr = c->ctr, cr = c->cr, xer = c->xer, msr = c->msr;
    const u32 srr0 = c->srr0, srr1 = c->srr1;
    const GcrtStopReason outer_stop = g->stop;

    CallState state = {false, 0};
    CallState* state_ptr = &state;
    for (unsigned i = 0; i < count && i < 8; i++)
        c->gpr[3 + i] = args[i];
    c->lr = gcrt_hle_push_return(g, 0, call_returned, &state_ptr, sizeof(state_ptr));
    c->pc = function;
    c->msr &= ~MSR_EE;
    s_guest_calls++;
    if (s_guest_call_depth == 0)
        s_outer_ee = (msr & MSR_EE) != 0;
    s_guest_call_depth++;
    GcrtStopReason reason = run_nested(g, function);
    s_guest_call_depth--;

    memcpy(c->gpr, gpr, sizeof(gpr));
    c->pc = pc;
    c->lr = lr;
    c->ctr = ctr;
    c->cr = cr;
    c->xer = xer;
    /* MSR: what the callee did is kept (e.g. OSSetCurrentContext sets RI to 1 and FP according to the owner of the
     * FPU, as in an SDK interrupt handler); only EE gets back its value from before the call. */
    c->msr = (c->msr & ~MSR_EE) | (msr & MSR_EE);
    c->srr0 = srr0;
    c->srr1 = srr1;
    if (state.returned) {
        g->stop = outer_stop;
    } else {
        /* Stop during the call (window closed, limit, exception...): propagated to the outer loop. */
        s_guest_call_failures++;
        /* A requested stop (window closed, end of scenario) may happen during a nested call: normal. */
        if (reason != GCRT_STOP_REQUESTED && reason != GCRT_STOP_TIME_LIMIT)
            fprintf(stderr, "gcrt native: the nested call to %08X did not return (%s)\n", function,
                    gcrt_stop_reason_name(reason));
        g->stop = reason != GCRT_STOP_NONE ? reason : GCRT_STOP_REQUESTED;
    }
    if (result)
        *result = state.r3;
    return state.returned;
}

bool gcrt_guest_call_prepared(GcrtCpu* g, u32 function) {
    CPUState* c = &g->state;
    /* Same rules as gcrt_guest_call (MSR[EE] = 0, depth counted: no thread switch during the call, stop
     * propagated), but the caller has set the argument registers and reads the results itself: only PC, LR, MSR[EE],
     * SRR0/1 are restored; integer, floating-point and condition registers keep what the callee left (the caller of a
     * function may not rely on the volatile ones; the callee preserves the others, as the PowerPC EABI requires). */
    const u32 pc = c->pc, lr = c->lr, msr = c->msr, srr0 = c->srr0, srr1 = c->srr1;
    const GcrtStopReason outer_stop = g->stop;
    CallState state = {false, 0};
    CallState* state_ptr = &state;
    c->lr = gcrt_hle_push_return(g, 0, call_returned, &state_ptr, sizeof(state_ptr));
    c->pc = function;
    c->msr &= ~MSR_EE;
    s_guest_calls++;
    if (s_guest_call_depth == 0)
        s_outer_ee = (msr & MSR_EE) != 0;
    s_guest_call_depth++;
    GcrtStopReason reason = run_nested(g, function);
    s_guest_call_depth--;
    c->pc = pc;
    c->lr = lr;
    c->msr = (c->msr & ~MSR_EE) | (msr & MSR_EE);
    c->srr0 = srr0;
    c->srr1 = srr1;
    if (state.returned) {
        g->stop = outer_stop;
    } else {
        s_guest_call_failures++;
        if (reason != GCRT_STOP_REQUESTED && reason != GCRT_STOP_TIME_LIMIT)
            fprintf(stderr, "gcrt native: the nested call to %08X did not return (%s)\n", function,
                    gcrt_stop_reason_name(reason));
        g->stop = reason != GCRT_STOP_NONE ? reason : GCRT_STOP_REQUESTED;
    }
    return state.returned;
}

bool gcrt_guest_call_exception_context(GcrtCpu* g, u32 context, u32 function, const u32* args, unsigned count) {
    if (gcrt_native_os_active()) {
        const u32 tmp = gcrt_native_exception_context_begin(g);
        const bool ok = gcrt_guest_call(g, function, args, count, NULL);
        gcrt_native_exception_context_end(g, tmp, context);
        return ok;
    }
    if (!s_fn_clear_context || !s_fn_set_current_context)
        return gcrt_guest_call(g, function, args, count, NULL);
    CPUState* c = &g->state;
    /* Temporary OSContext on the guest stack, like the local variable of the SDK handlers. */
    const u32 sp = c->gpr[1];
    const u32 frame = (sp - (CTX_SIZE + 0x18u)) & ~0x1Fu;
    const u32 tmp = frame + 0x10u;
    mem_write32(c, frame, sp); /* back chain */
    c->gpr[1] = frame;
    bool ok = gcrt_guest_call(g, s_fn_clear_context, &tmp, 1, NULL) &&
              gcrt_guest_call(g, s_fn_set_current_context, &tmp, 1, NULL);
    if (ok)
        ok = gcrt_guest_call(g, function, args, count, NULL);
    if (g->stop == GCRT_STOP_NONE || ok) {
        gcrt_guest_call(g, s_fn_clear_context, &tmp, 1, NULL);
        gcrt_guest_call(g, s_fn_set_current_context, &context, 1, NULL);
    }
    c->gpr[1] = sp;
    return ok;
}

/* ---------------------------------------------------------------------------------------------
 * Native interrupt queue
 * ------------------------------------------------------------------------------------------- */

static void update_deadline(GcrtCpu* g) {
    u64 next = UINT64_MAX;
    for (unsigned i = 0; i < s_event_count; i++)
        if (s_events[i].due < next)
            next = s_events[i].due;
    g->native_irq_deadline = next;
}

bool gcrt_nirq_masked(GcrtCpu* g, unsigned interrupt) {
    if (interrupt >= GCRT_NIRQ_DECREMENTER)
        return false; /* the decrementer has no OS mask, only MSR[EE] */
    CPUState* c = &g->state;
    const u32 masks = mem_read32(c, LOW_INTERRUPT_MASK_PRIOR) | mem_read32(c, LOW_INTERRUPT_MASK_CURRENT);
    return (masks & (0x80000000u >> (interrupt & 31u))) != 0;
}

unsigned gcrt_nirq_pending(void) {
    return s_event_count;
}

u64 gcrt_nirq_next_due(GcrtCpu* g) {
    u64 next = UINT64_MAX;
    for (unsigned i = 0; i < s_event_count; i++)
        if (s_events[i].due < next && !gcrt_nirq_masked(g, s_events[i].interrupt))
            next = s_events[i].due;
    return next;
}

void gcrt_nirq_skip_reschedule(void) { s_skip_reschedule = true; }

u32 gcrt_nirq_post(GcrtCpu* g, unsigned interrupt, u64 due_ticks, GcrtNativeIrqFn fn, void* user) {
    if (s_event_count >= MAX_EVENTS) {
        fprintf(stderr, "gcrt native: native interrupt queue full (%u)\n", MAX_EVENTS);
        return 0;
    }
    Event* e = &s_events[s_event_count++];
    e->id = s_next_id++;
    if (s_next_id == 0)
        s_next_id = 1;
    e->interrupt = interrupt >= GCRT_NIRQ_DECREMENTER ? GCRT_NIRQ_DECREMENTER : (interrupt & 31u);
    e->due = due_ticks;
    e->fn = fn;
    e->user = user;
    update_deadline(g);
    GCRT_TRACE(GCRT_TRACE_NIRQ, "native interrupt %u posted (#%u, deadline %llu, now %llu)", e->interrupt,
               e->id, (unsigned long long)due_ticks, (unsigned long long)g->ticks);
    GCRT_TIMELINE("nirq posted %u n%u due %llu", e->interrupt, e->id, (unsigned long long)due_ticks);
    return e->id;
}

bool gcrt_nirq_cancel(GcrtCpu* g, u32 id) {
    for (unsigned i = 0; i < s_event_count; i++) {
        if (s_events[i].id == id) {
            GCRT_TIMELINE("nirq cancelled %u n%u due %llu", s_events[i].interrupt, s_events[i].id,
                          (unsigned long long)s_events[i].due);
            s_events[i] = s_events[--s_event_count];
            update_deadline(g);
            return true;
        }
    }
    return false;
}

/* Interrupt entry by the host (see the file header). */
static void enter_interrupt(GcrtCpu* g, const Event* e, u32 context) {
    CPUState* c = &g->state;
    const u32 pc = c->pc, msr = c->msr;
    const bool decrementer = e->interrupt >= GCRT_NIRQ_DECREMENTER;

    /* 1-2. Vector and ExternalInterruptHandler (or DecrementerExceptionHandler): the whole state into the current
     * context. */
    for (u32 i = 0; i < 32u; i++)
        mem_write32(c, context + CTX_GPR + 4u * i, c->gpr[i]);
    mem_write32(c, context + CTX_CR, c->cr);
    mem_write32(c, context + CTX_LR, c->lr);
    mem_write32(c, context + CTX_CTR, c->ctr);
    mem_write32(c, context + CTX_XER, c->xer);
    mem_write32(c, context + CTX_SRR0, pc);
    mem_write32(c, context + CTX_SRR1, msr);
    for (u32 i = 1; i < 8u; i++)
        mem_write32(c, context + CTX_GQR + 4u * i, c->gqr[i]);
    mem_write16(c, context + CTX_STATE, (u16)(mem_read16(c, context + CTX_STATE) | CTX_STATE_EXC));
    c->srr0 = pc;
    c->srr1 = msr;
    c->msr = (msr & ~MSR_EXCEPTION_CLEARS) | MSR_IR | MSR_DR;
    c->reserve_valid = false;
    c->gpr[3] = decrementer ? EXCEPTION_DECREMENTER : EXCEPTION_EXTERNAL;
    c->gpr[4] = context;
    const u32 sp0 = c->gpr[1];
    const u32 sp = sp0 - 8u; /* stwu r1, -8(r1) */
    mem_write32(c, sp, sp0);
    const u32 frame = sp - ENTRY_FRAME;
    mem_write32(c, frame, sp);
    mem_write32(c, frame + 8u, context);
    c->gpr[1] = frame;

    s_delivered++;
    s_delivered_by_irq[e->interrupt < CLASS_COUNT ? e->interrupt : CLASS_COUNT - 1u]++;
    GCRT_TIMELINE("nirq delivered %u n%u due %llu", e->interrupt, e->id, (unsigned long long)e->due);
    GCRT_TRACE(GCRT_TRACE_NIRQ, "native interrupt %u delivered (#%u): pc=%08X, context %08X, delay %llu ticks",
               e->interrupt, e->id, pc, context, (unsigned long long)(g->ticks - e->due));

    if (gcrt_native_os_active()) {
        /* Native OS: suspension of the scheduler, handler, then native __OSReschedule and OSLoadContext. An alarm
         * (decrementer class) manages the scheduler itself, like DecrementerExceptionCallback. */
        gcrt_os_note_delivery();
        s_skip_reschedule = false;
        s_interrupt_depth++;
        if (decrementer) {
            if (g->stop == GCRT_STOP_NONE)
                e->fn(g, e->interrupt, context, e->user);
        } else {
            gcrt_os_disable_scheduler(g);
            if (g->stop == GCRT_STOP_NONE)
                e->fn(g, e->interrupt, context, e->user);
            gcrt_os_enable_scheduler(g);
        }
        s_interrupt_depth--;
        c->gpr[1] = frame;
        if (s_skip_reschedule) {
            s_skip_reschedule = false;
            gcrt_os_load_context(g, context);
        } else {
            gcrt_os_interrupt_exit(g, context);
        }
        return;
    }

    /* 3. __OSDispatchInterrupt: OSDisableScheduler; handler; OSEnableScheduler (nested). */
    gcrt_guest_call(g, s_fn_disable_scheduler, NULL, 0, NULL);
    s_interrupt_depth++;
    if (g->stop == GCRT_STOP_NONE)
        e->fn(g, e->interrupt, context, e->user);
    s_interrupt_depth--;
    c->gpr[1] = frame;
    if (g->stop == GCRT_STOP_NONE)
        gcrt_guest_call(g, s_fn_enable_scheduler, NULL, 0, NULL);
    /* Then __OSReschedule() and OSLoadContext(context), run by the chassis. */
    c->gpr[1] = frame;
    c->lr = MAGIC_AFTER_RESCHEDULE;
    c->pc = s_fn_reschedule;
}

static bool after_reschedule(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* c = &g->state;
    const u32 context = mem_read32(c, c->gpr[1] + 8u);
    c->gpr[3] = context;
    c->pc = s_fn_load_context;
    return true;
}

/* Chassis callback (block boundary, MSR[EE] = 1, ticks >= nearest deadline). First the due and unmasked external
 * interrupts, one per call, chosen like __OSDispatchInterrupt: rank in the SDK priority table (equal numbers: by
 * deadline, then order of posting); then the decrementer class (alarms), by deadline then order of posting. */
static void irq_hook(GcrtCpu* g) {
    CPUState* c = &g->state;
    const u32 context = mem_read32(c, LOW_CURRENT_CONTEXT);
    if (!in_ram(c, context, CTX_SIZE))
        return; /* OS not initialized yet: wait */
    int best = -1, best_dec = -1;
    for (unsigned i = 0; i < s_event_count; i++) {
        const Event* e = &s_events[i];
        if (e->due > g->ticks)
            continue;
        if (e->interrupt >= GCRT_NIRQ_DECREMENTER) {
            if (best_dec < 0 || e->due < s_events[best_dec].due ||
                (e->due == s_events[best_dec].due && e->id < s_events[best_dec].id))
                best_dec = (int)i;
            continue;
        }
        s_masked_checks++;
        if (gcrt_nirq_masked(g, e->interrupt))
            continue;
        if (best < 0) {
            best = (int)i;
            continue;
        }
        const Event* b = &s_events[best];
        const unsigned re = sdk_rank(e->interrupt), rb = sdk_rank(b->interrupt);
        if (re < rb || (re == rb && (e->due < b->due || (e->due == b->due && e->id < b->id))))
            best = (int)i;
    }
    if (best < 0)
        best = best_dec;
    if (best < 0)
        return;
    const Event e = s_events[best];
    s_events[best] = s_events[--s_event_count];
    update_deadline(g);
    enter_interrupt(g, &e, context);
}

void gcrt_nirq_init(GcrtCpu* g) {
    timeline_open(g);
    if (s_initialized)
        return;
    static const char* const names[GCRT_OSFN_COUNT] = {
        "OSDisableScheduler", "OSEnableScheduler", "__OSReschedule", "OSLoadContext",
        "OSClearContext", "OSSetCurrentContext", "__OSMaskInterrupts", "__OSUnmaskInterrupts",
    };
    for (int i = 0; i < GCRT_OSFN_COUNT; i++)
        s_os_fn[i] = gcrt_native_sdk_function(names[i]);
    if (!s_fn_disable_scheduler || !s_fn_enable_scheduler || !s_fn_reschedule || !s_fn_load_context) {
        fprintf(stderr, "gcrt native: OS functions missing, the native interrupts will not be delivered\n");
        return;
    }
    gcrt_hle_register(g, MAGIC_AFTER_RESCHEDULE, "return from __OSReschedule (native interrupt)", after_reschedule);
    g->native_irq_hook = irq_hook;
    update_deadline(g);
    s_initialized = true;
}

void gcrt_nirq_report(FILE* out) {
    if (!s_initialized)
        return;
    fprintf(out, "native interrupts: %llu delivered", (unsigned long long)s_delivered);
    for (unsigned i = 0; i < CLASS_COUNT; i++)
        if (s_delivered_by_irq[i]) {
            if (i >= GCRT_NIRQ_DECREMENTER)
                fprintf(out, " (alarms: %llu)", (unsigned long long)s_delivered_by_irq[i]);
            else
                fprintf(out, " (#%u: %llu)", i, (unsigned long long)s_delivered_by_irq[i]);
        }
    fprintf(out, ", %u pending; nested calls %llu (%llu failures); deferred reschedules %llu\n",
            s_event_count, (unsigned long long)s_guest_calls, (unsigned long long)s_guest_call_failures,
            (unsigned long long)s_deferred_reschedules);
}

/* ---------------------------------------------------------------------------------------------
 * Shared helpers of the replacements (gcrt_native_modules.h)
 * ------------------------------------------------------------------------------------------- */

u32 gcrt_native_os_fn(GcrtOsFn which) {
    return (unsigned)which < GCRT_OSFN_COUNT ? s_os_fn[which] : 0;
}

bool gcrt_native_in_interrupt(void) {
    return s_interrupt_depth > 0;
}

void gcrt_native_enter(GcrtCpu* g) {
    (void)g;
    if (!s_interrupt_depth)
        s_call_depth++;
}

bool gcrt_native_callback(GcrtCpu* g, u32 function, const u32* args, unsigned count) {
    if (!function)
        return true;
    if (!s_interrupt_depth && !s_sched_held) {
        if (gcrt_native_os_active()) {
            gcrt_os_disable_scheduler(g);
            s_sched_held = true;
        } else if (s_fn_disable_scheduler) {
            gcrt_guest_call(g, s_fn_disable_scheduler, NULL, 0, NULL);
            s_sched_held = true;
        }
    }
    return gcrt_guest_call(g, function, args, count, NULL);
}

typedef struct {
    u32 result;
} ReturnValue;

static void return_with_value(GcrtCpu* g, u32 original_return, void* data) {
    ReturnValue v;
    memcpy(&v, data, sizeof(v));
    g->state.gpr[3] = v.result;
    g->state.pc = original_return;
}

void gcrt_native_return(GcrtCpu* g, u32 result) {
    CPUState* c = &g->state;
    c->gpr[3] = result;
    c->pc = c->lr;
    if (s_interrupt_depth)
        return;
    if (s_call_depth > 0)
        s_call_depth--;
    if (s_call_depth == 0 && s_sched_held) {
        /* As if the rescheduling requested by a callback (OSWakeupThread) happened now:
         * OSEnableScheduler then __OSReschedule, and return to the caller with the result. */
        s_sched_held = false;
        if (gcrt_native_os_active()) {
            gcrt_os_enable_scheduler(g);
            if (g->stop == GCRT_STOP_NONE) {
                s_deferred_reschedules++;
                gcrt_os_reschedule_return(g, result);
            }
            return;
        }
        gcrt_guest_call(g, s_fn_enable_scheduler, NULL, 0, NULL);
        if (s_fn_reschedule && g->stop == GCRT_STOP_NONE) {
            ReturnValue v = {result};
            c->lr = gcrt_hle_push_return(g, c->pc, return_with_value, &v, sizeof(v));
            c->pc = s_fn_reschedule;
            s_deferred_reschedules++;
        }
    }
}

u32 gcrt_native_exception_context_begin(GcrtCpu* g) {
    CPUState* c = &g->state;
    const u32 sp = c->gpr[1];
    const u32 frame = (sp - (CTX_SIZE + 0x18u)) & ~0x1Fu;
    const u32 tmp = frame + 0x10u;
    mem_write32(c, frame, sp); /* back chain */
    mem_write32(c, frame + 8u, sp);
    c->gpr[1] = frame;
    if (gcrt_native_os_active()) {
        gcrt_os_clear_context(g, tmp);
        gcrt_os_set_current_context(g, tmp);
    } else if (s_fn_clear_context && s_fn_set_current_context) {
        gcrt_guest_call(g, s_fn_clear_context, &tmp, 1, NULL);
        gcrt_guest_call(g, s_fn_set_current_context, &tmp, 1, NULL);
    }
    return tmp;
}

void gcrt_native_exception_context_end(GcrtCpu* g, u32 tmp, u32 context) {
    CPUState* c = &g->state;
    if (gcrt_native_os_active()) {
        gcrt_os_clear_context(g, tmp);
        gcrt_os_set_current_context(g, context);
    } else if (s_fn_clear_context && s_fn_set_current_context && g->stop == GCRT_STOP_NONE) {
        gcrt_guest_call(g, s_fn_clear_context, &tmp, 1, NULL);
        gcrt_guest_call(g, s_fn_set_current_context, &context, 1, NULL);
    }
    c->gpr[1] = mem_read32(c, tmp - 0x10u + 8u);
}

void gcrt_native_set_pad_source(GcrtPadSource source, GcrtPadRumble rumble, void* user) {
    s_pad_source = source;
    s_pad_rumble = rumble;
    s_pad_user = user;
}

bool gcrt_native_read_pad(int port, GcrtPadInput* out) {
    memset(out, 0, sizeof(*out));
    return s_pad_source && s_pad_source(s_pad_user, port, out);
}

void gcrt_native_rumble(int port, int command) {
    if (s_pad_rumble)
        s_pad_rumble(s_pad_user, port, command);
}

u8* gcrt_native_ram(GcrtCpu* g, u32 address, u32 size) {
    CPUState* c = &g->state;
    u32 phys = address;
    if (address >= GC_RAM_UNCACHED)
        phys = address - GC_RAM_UNCACHED;
    else if (address >= GC_RAM_BASE)
        phys = address - GC_RAM_BASE;
    if (phys > GCRT_MEM1_SIZE || size > GCRT_MEM1_SIZE - phys || phys > c->ram_size || size > c->ram_size - phys)
        return NULL;
    return c->ram + phys;
}

/* ---------------------------------------------------------------------------------------------
 * Native timers (gcrt_native_modules.h): independent of MSR[EE], called by gcrt_cpu_sync_time
 * ------------------------------------------------------------------------------------------- */

#define MAX_TIMERS 16u

typedef struct {
    u32 id;
    u64 due;
    GcrtNativeTimerFn fn;
    void* user;
} Timer;

static Timer s_timers[MAX_TIMERS];
static unsigned s_timer_count;
static u32 s_next_timer_id = 1;

static void update_timer_deadline(GcrtCpu* g) {
    u64 next = UINT64_MAX;
    for (unsigned i = 0; i < s_timer_count; i++)
        if (s_timers[i].due < next)
            next = s_timers[i].due;
    g->native_timer_deadline = next;
}

static void timer_hook(GcrtCpu* g) {
    /* Several timers may be due: the oldest first, each removed before its callback (which may reschedule
     * one). */
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < s_timer_count; i++)
            if (s_timers[i].due <= g->ticks && (best < 0 || s_timers[i].due < s_timers[best].due))
                best = (int)i;
        if (best < 0)
            break;
        const Timer t = s_timers[best];
        s_timers[best] = s_timers[--s_timer_count];
        update_timer_deadline(g);
        t.fn(g, t.user);
    }
    update_timer_deadline(g);
}

u32 gcrt_ntimer_set(GcrtCpu* g, u64 due_ticks, GcrtNativeTimerFn fn, void* user) {
    if (s_timer_count >= MAX_TIMERS) {
        fprintf(stderr, "gcrt native: too many native timers (%u)\n", MAX_TIMERS);
        return 0;
    }
    Timer* t = &s_timers[s_timer_count++];
    t->id = s_next_timer_id++;
    if (s_next_timer_id == 0)
        s_next_timer_id = 1;
    t->due = due_ticks;
    t->fn = fn;
    t->user = user;
    g->native_timer_hook = timer_hook;
    update_timer_deadline(g);
    return t->id;
}

bool gcrt_ntimer_cancel(GcrtCpu* g, u32 id) {
    for (unsigned i = 0; i < s_timer_count; i++) {
        if (s_timers[i].id == id) {
            s_timers[i] = s_timers[--s_timer_count];
            update_timer_deadline(g);
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------------------------
 * Installing a replacement (choice of the copy when the signature is shared)
 * ------------------------------------------------------------------------------------------- */

bool gcrt_native_replace(GcrtCpu* g, const char* name, const char* library, GcrtHleFn fn,
                         const char* const* following, unsigned following_count, const char* preceding,
                         char* missing, size_t missing_size) {
    u32 one = 0;
    unsigned n = gcrt_hle_find_all(name, &one, 1);
    u32 address = 0;
    if (n == 1) {
        address = one;
    } else if (n > 1) {
        if (following && following_count)
            address = gcrt_hle_resolve_following(name, following, following_count);
        if (!address && preceding)
            address = gcrt_hle_resolve_preceding(name, preceding);
        if (address && gcrt_hle_trace_enabled())
            fprintf(stderr, "gcrt native: %s recognized %u times, copy %08X kept by anchoring\n", name, n, address);
    }
    bool ok = false;
    if (address)
        ok = gcrt_hle_register(g, address, name, fn);
    else if (n > 1)
        ok = gcrt_hle_replace_in_library(g, name, library, fn) != 0;
    if (!ok && missing) {
        size_t used = strlen(missing);
        snprintf(missing + used, missing_size - used, "%s%s", used ? " " : "", name);
    }
    return ok;
}
