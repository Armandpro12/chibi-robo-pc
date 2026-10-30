/* Native SDK OS (milestone 10b): threads, scheduling, contexts, lazy FPU, message queues, idle.
 *
 * Choice (notes/decisions.md, milestone 10b): native scheduler over the guest structures (option B of
 * the internal SDK notes § 13.7). The functions of OSThread.c, OSMessage.c and the context loading are replaced by host
 * C that maintains exactly the same structures in guest memory as the SDK (OSThread, ready queues RunQueue[32],
 * RunQueueBits / RunQueueHint / Reschedule, list of active threads, current thread and context in low memory): the
 * game, GX and the remaining recompiled functions (OSIsThreadTerminated, OSInitThreadQueue, OSSetCurrentContext...)
 * read and write them as before. Semantics taken from the SDK (disassembly of the DOL read function by function +
 * reference decompilation cbr_decomp/decomp_dtk_main, CC0, consulted; see the internal SDK notes § 13.4).
 *
 * Thread switch without an exception, without `rfi` and without state on the host stack: the whole state of a guest
 * thread is the processor state (CPUState) plus guest RAM (the recompiled code keeps no host frame between two blocks,
 * § 13.7 fact 1). A replacement that must switch to another thread:
 *   - stores the context of the current thread like OSSaveContext (r13-r31, GQR, CR, LR, CTR, XER, MSR, r1, r2,
 *     r3 = 1) and records on the host side its "continuation" (return to the caller with such a value and such a
 *     MSR[EE], or resumption of an interrupted OSSend/ReceiveMessage);
 *   - reloads the context of the chosen thread like OSLoadContext (including the OSDisableInterrupts window and the
 *     "exception" state of an interrupted thread), then applies the continuation of that thread if it has one;
 *   - hands control back to the chassis: the recompiled code continues at the pc of the new thread.
 * No thread switch is allowed during a nested call by the host (gcrt_guest_call): as if the scheduler were suspended
 * (which is already the case for the interrupt handlers and the callbacks).
 *
 * Idle: no thread ready any more -> like the SDK (current thread NULL, IdleContext context, interrupts enabled) but
 * instead of the SDK idle loop, the guest pc is a "magic" address handled by the host: the host waits for the next
 * event (gcrt_cpu_wait_until), the interrupts are delivered normally, and as soon as a thread becomes ready the
 * selection resumes. An OSYieldThread that only loops (no other thread ready, no write outside the stack since the
 * previous one, no event in between) likewise waits for the next event (at most 1 ms).
 *
 * Lazy FPU: like OSSwitchFPUContext (exception 7), but handled by the host without a vector: the floating-point
 * registers of the owner are saved into ITS context, those of the current context are loaded (guest functions
 * __OSSaveFPUContext / __OSLoadFPUContext, recompiled, without hardware access), resumption on the instruction.
 * System call (`sc`): the SDK one only does a synchronization (HID0[ABE]): no effect, return to SRR0.
 *
 * Generic: no reference to a game; the addresses of the OS static variables are found in the code recognized by
 * signature (gcrt_hle_decode_*). */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <stdlib.h>
#include <string.h>

/* OS low memory (SDK). */
#define LOW_CTX_PHYS 0x800000C0u
#define LOW_CTX 0x800000D4u
#define LOW_FPU_OWNER 0x800000D8u
#define LOW_ACTIVE_QUEUE 0x800000DCu /* head, tail (+4) */
#define LOW_CUR_THREAD 0x800000E4u

/* OSContext. */
#define CTX_GPR 0x000u
#define CTX_CR 0x080u
#define CTX_LR 0x084u
#define CTX_CTR 0x088u
#define CTX_XER 0x08Cu
#define CTX_FPR 0x090u
#define CTX_FPSCR 0x190u
#define CTX_SRR0 0x198u
#define CTX_SRR1 0x19Cu
#define CTX_MODE 0x1A0u
#define CTX_STATE 0x1A2u
#define CTX_GQR 0x1A4u
#define CTX_PSF 0x1C8u
#define CTX_STATE_FPSAVED 0x01u
#define CTX_STATE_EXC 0x02u

/* OSThread. */
#define TH_STATE 0x2C8u
#define TH_ATTR 0x2CAu
#define TH_SUSPEND 0x2CCu
#define TH_PRIORITY 0x2D0u
#define TH_BASE 0x2D4u
#define TH_VAL 0x2D8u
#define TH_QUEUE 0x2DCu
#define TH_LINK 0x2E0u /* next, prev (+4) */
#define TH_JOIN 0x2E8u
#define TH_MUTEX 0x2F0u
#define TH_MUTEXQ 0x2F4u
#define TH_ACTIVE 0x2FCu /* next, prev (+4) */
#define TH_STACK_BASE 0x304u
#define TH_STACK_END 0x308u
#define TH_ERROR 0x30Cu
#define TH_SPECIFIC 0x310u

enum { ST_NULL = 0, ST_READY = 1, ST_RUNNING = 2, ST_WAITING = 4, ST_MORIBUND = 8 };
#define ATTR_DETACH 1u
#define STACK_MAGIC 0xDEADBABEu

/* OSMutex: wait queue +0, owner +8, count +0xC, link (next, prev) +0x10. */
#define MX_QUEUE 0x00u
#define MX_THREAD 0x08u
#define MX_COUNT 0x0Cu
#define MX_LINK 0x10u

/* OSMessageQueue. */
#define MQ_SEND 0x00u
#define MQ_RECV 0x08u
#define MQ_ARRAY 0x10u
#define MQ_COUNT 0x14u
#define MQ_FIRST 0x18u
#define MQ_USED 0x1Cu
#define MSG_BLOCK 1u

/* MSR (750CL manual). */
#define MSR_POW 0x00040000u
#define MSR_EE 0x00008000u
#define MSR_FP 0x00002000u
#define MSR_IR 0x00000020u
#define MSR_DR 0x00000010u
#define MSR_RI 0x00000002u
#define MSR_RFI_MASK 0x87C0FFFFu /* bits taken from SRR1 by rfi (DolRecomp, PPC_MSR_RFI_MASK) */

/* Idle: "magic" address (outside guest memory) handled by the host. */
#define MAGIC_IDLE 0xF0000900u
/* Looping OSYieldThread: host wait of at most 1 emulated ms. */
#define YIELD_WAIT_MAX_TICKS (GCRT_TB_HZ / 1000u)
#define MAX_CONTS 64u

typedef enum { CONT_RETURN = 1, CONT_RECEIVE, CONT_SEND } ContKind;

typedef struct {
    u32 thread; /* 0: free */
    ContKind kind;
    u32 value;  /* CONT_RETURN: r3 returned to the caller */
    bool ee;    /* MSR[EE] of the caller (OSRestoreInterrupts of the SDK) */
    u32 a0, a1, a2;
} Cont;

static struct {
    bool installed;
    bool ready;
    u32 sda;
    u32 a_reschedule, a_bits, a_hint, a_switch_cb;
    u32 run_queue, idle_context;
    u32 fn_disable_interrupts, fn_exit_thread, fn_save_fpu, fn_load_fpu, fn_switch_fpu;
    u32 fn_send, fn_receive;
    u32 a_fpe_handler, a_fpscr_enable;
    bool idle;
    Cont conts[MAX_CONTS];
    u64 switches, idles, idle_waits, yields, yield_waits, fpu_switches, syscalls, saves, loads, refused_nested;
    u64 cb_calls;
    char missing[512];
    unsigned replaced;
    /* Looping OSYieldThread (see above). */
    struct {
        bool armed, dirty;
        u32 thread, lr;
        u64 deliveries, switches;
        u32 stack_lo, stack_hi; /* RAM offsets of the stack area where writes are allowed */
    } spin;
} s_os;


/* --------------------------------------------------------------------------------------------- */

static u32 R32(GcrtCpu* g, u32 a) { return mem_read32(&g->state, a); }
static void W32(GcrtCpu* g, u32 a, u32 v) { mem_write32(&g->state, a, v); }
static u16 R16(GcrtCpu* g, u32 a) { return mem_read16(&g->state, a); }
static void W16(GcrtCpu* g, u32 a, u16 v) { mem_write16(&g->state, a, v); }

static u32 cntlzw(u32 v) {
    u32 n = 0;
    if (!v)
        return 32u;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        n++;
    }
    return n;
}

bool gcrt_native_os_active(void) { return s_os.ready; }
/* Thread switches refused because a nested call by the host was running (select_thread); read by the decompilation
 * oracle, which runs originals in nested runs. */
u64 gcrt_native_os_refused_nested(void) { return s_os.refused_nested; }

u32 gcrt_native_os_sda(void) { return s_os.sda; }

/* --------------------------------------------------------------------------------------------- *
 * Thread queues (AddTail, AddPrio, RemoveItem, RemoveHead macros of the SDK: same writes)
 * --------------------------------------------------------------------------------------------- */

static void q_add_tail(GcrtCpu* g, u32 q, u32 t, u32 link) {
    const u32 prev = R32(g, q + 4u);
    if (!prev)
        W32(g, q, t);
    else
        W32(g, prev + link, t);
    W32(g, t + link + 4u, prev);
    W32(g, t + link, 0);
    W32(g, q + 4u, t);
}

static void q_add_prio(GcrtCpu* g, u32 q, u32 t) {
    u32 next = R32(g, q);
    while (next && (s32)R32(g, next + TH_PRIORITY) <= (s32)R32(g, t + TH_PRIORITY))
        next = R32(g, next + TH_LINK);
    if (!next) {
        q_add_tail(g, q, t, TH_LINK);
        return;
    }
    W32(g, t + TH_LINK, next);
    const u32 prev = R32(g, next + TH_LINK + 4u);
    W32(g, next + TH_LINK + 4u, t);
    W32(g, t + TH_LINK + 4u, prev);
    if (!prev)
        W32(g, q, t);
    else
        W32(g, prev + TH_LINK, t);
}

static void q_remove(GcrtCpu* g, u32 q, u32 t, u32 link) {
    const u32 next = R32(g, t + link), prev = R32(g, t + link + 4u);
    if (!next)
        W32(g, q + 4u, prev);
    else
        W32(g, next + link + 4u, prev);
    if (!prev)
        W32(g, q, next);
    else
        W32(g, prev + link, next);
}

static u32 q_remove_head(GcrtCpu* g, u32 q) {
    const u32 t = R32(g, q);
    const u32 next = R32(g, t + TH_LINK);
    if (!next)
        W32(g, q + 4u, 0);
    else
        W32(g, next + TH_LINK + 4u, 0);
    W32(g, q, next);
    return t;
}

static u32 bits(GcrtCpu* g) { return R32(g, s_os.a_bits); }

static void set_run(GcrtCpu* g, u32 t) {
    const u32 prio = R32(g, t + TH_PRIORITY);
    const u32 q = s_os.run_queue + 8u * prio;
    W32(g, t + TH_QUEUE, q);
    q_add_tail(g, q, t, TH_LINK);
    W32(g, s_os.a_bits, bits(g) | (1u << (31u - prio)));
    W32(g, s_os.a_hint, 1);
}

static void unset_run(GcrtCpu* g, u32 t) {
    const u32 q = R32(g, t + TH_QUEUE);
    q_remove(g, q, t, TH_LINK);
    if (!R32(g, q))
        W32(g, s_os.a_bits, bits(g) & ~(1u << (31u - R32(g, t + TH_PRIORITY))));
    W32(g, t + TH_QUEUE, 0);
}

/* __OSGetEffectivePriority: base, or better if a thread waits for a mutex that this thread holds. */
static s32 effective_priority(GcrtCpu* g, u32 t) {
    s32 prio = (s32)R32(g, t + TH_BASE);
    for (u32 m = R32(g, t + TH_MUTEXQ); m; m = R32(g, m + MX_LINK)) {
        const u32 blocked = R32(g, m + MX_QUEUE);
        if (blocked && (s32)R32(g, blocked + TH_PRIORITY) < prio)
            prio = (s32)R32(g, blocked + TH_PRIORITY);
    }
    return prio;
}

/* SetEffectivePriority (static in the SDK). */
static u32 set_effective_priority(GcrtCpu* g, u32 t, s32 prio) {
    switch (R16(g, t + TH_STATE)) {
    case ST_READY:
        unset_run(g, t);
        W32(g, t + TH_PRIORITY, (u32)prio);
        set_run(g, t);
        break;
    case ST_WAITING: {
        const u32 q = R32(g, t + TH_QUEUE);
        q_remove(g, q, t, TH_LINK);
        W32(g, t + TH_PRIORITY, (u32)prio);
        q_add_prio(g, q, t);
        const u32 m = R32(g, t + TH_MUTEX);
        if (m)
            return R32(g, m + MX_THREAD);
        break;
    }
    case ST_RUNNING:
        W32(g, s_os.a_hint, 1);
        W32(g, t + TH_PRIORITY, (u32)prio);
        break;
    default:
        break;
    }
    return 0;
}

/* UpdatePriority (static in the SDK, inlined). */
static void update_priority(GcrtCpu* g, u32 t) {
    while (t) {
        if ((s32)R32(g, t + TH_SUSPEND) > 0)
            break;
        const s32 prio = effective_priority(g, t);
        if ((s32)R32(g, t + TH_PRIORITY) == prio)
            break;
        t = set_effective_priority(g, t, prio);
    }
}

/* --------------------------------------------------------------------------------------------- *
 * Contexts (OSSetCurrentContext, OSClearContext, OSSaveContext, OSLoadContext of the SDK)
 * --------------------------------------------------------------------------------------------- */

void gcrt_os_set_current_context(GcrtCpu* g, u32 ctx) {
    CPUState* c = &g->state;
    W32(g, LOW_CTX, ctx);
    W32(g, LOW_CTX_PHYS, ctx & 0x3FFFFFFFu);
    if (R32(g, LOW_FPU_OWNER) == ctx) {
        W32(g, ctx + CTX_SRR1, R32(g, ctx + CTX_SRR1) | MSR_FP);
        c->msr |= MSR_RI;
    } else {
        W32(g, ctx + CTX_SRR1, R32(g, ctx + CTX_SRR1) & ~MSR_FP);
        c->msr = (c->msr & ~MSR_FP) | MSR_RI;
    }
}

void gcrt_os_clear_context(GcrtCpu* g, u32 ctx) {
    W16(g, ctx + CTX_MODE, 0);
    W16(g, ctx + CTX_STATE, 0);
    if (R32(g, LOW_FPU_OWNER) == ctx)
        W32(g, LOW_FPU_OWNER, 0);
}

static Cont* cont_slot(u32 thread, bool create) {
    Cont* free_slot = NULL;
    for (unsigned i = 0; i < MAX_CONTS; i++) {
        if (s_os.conts[i].thread == thread)
            return &s_os.conts[i];
        if (!s_os.conts[i].thread && !free_slot)
            free_slot = &s_os.conts[i];
    }
    if (create && !free_slot)
        fprintf(stderr, "gcrt native OS: no room left for thread continuations (%u)\n", MAX_CONTS);
    return create ? free_slot : NULL;
}

static void cont_drop(u32 thread) {
    Cont* k = cont_slot(thread, false);
    if (k)
        k->thread = 0;
}

/* Context of the thread that yields (OSSaveContext), and its continuation on the host side. */
static void save_voluntary(GcrtCpu* g, u32 t, const Cont* cont) {
    CPUState* c = &g->state;
    for (u32 r = 13; r < 32u; r++)
        W32(g, t + CTX_GPR + 4u * r, c->gpr[r]);
    for (u32 i = 1; i < 8u; i++)
        W32(g, t + CTX_GQR + 4u * i, c->gqr[i]);
    W32(g, t + CTX_CR, c->cr);
    W32(g, t + CTX_LR, c->lr);
    W32(g, t + CTX_SRR0, c->lr);
    W32(g, t + CTX_SRR1, c->msr & ~MSR_EE); /* the SDK saves within a section without interrupts */
    W32(g, t + CTX_CTR, c->ctr);
    W32(g, t + CTX_XER, c->xer);
    W32(g, t + CTX_GPR + 4u, c->gpr[1]);
    W32(g, t + CTX_GPR + 8u, c->gpr[2]);
    W32(g, t + CTX_GPR + 12u, 1u);
    s_os.saves++;
    Cont* k = cont_slot(t, true);
    if (k) {
        if (cont) {
            *k = *cont;
            k->thread = t;
        } else {
            k->thread = 0;
        }
    }
}

static void apply_cont(GcrtCpu* g, const Cont* k);

/* OSLoadContext: all the registers of a context, like `rfi` (MSR <- SRR1, pc <- SRR0), then the thread continuation. */
static void load_context(GcrtCpu* g, u32 ctx) {
    CPUState* c = &g->state;
    u32 srr0 = R32(g, ctx + CTX_SRR0);
    const u32 dis = s_os.fn_disable_interrupts;
    if (dis && srr0 > dis && srr0 < dis + 0xCu) {
        /* Interrupted between the mfmsr and the mtmsr of OSDisableInterrupts: the sequence restarts from the start. */
        srr0 = dis;
        W32(g, ctx + CTX_SRR0, srr0);
    }
    c->gpr[0] = R32(g, ctx + CTX_GPR);
    c->gpr[1] = R32(g, ctx + CTX_GPR + 4u);
    c->gpr[2] = R32(g, ctx + CTX_GPR + 8u);
    const u16 state = R16(g, ctx + CTX_STATE);
    u32 first = 13u;
    if (state & CTX_STATE_EXC) {
        W16(g, ctx + CTX_STATE, (u16)(state & ~CTX_STATE_EXC));
        first = 5u;
    }
    for (u32 r = first; r < 32u; r++)
        c->gpr[r] = R32(g, ctx + CTX_GPR + 4u * r);
    for (u32 i = 1; i < 8u; i++)
        c->gqr[i] = R32(g, ctx + CTX_GQR + 4u * i);
    c->cr = R32(g, ctx + CTX_CR);
    c->lr = R32(g, ctx + CTX_LR);
    c->ctr = R32(g, ctx + CTX_CTR);
    c->xer = R32(g, ctx + CTX_XER);
    c->msr &= ~(MSR_EE | MSR_RI);
    const u32 srr1 = R32(g, ctx + CTX_SRR1);
    c->srr0 = srr0;
    c->srr1 = srr1;
    c->gpr[4] = R32(g, ctx + CTX_GPR + 0x10u);
    c->gpr[3] = R32(g, ctx + CTX_GPR + 0x0Cu);
    /* rfi */
    c->msr = ((c->msr & ~MSR_RFI_MASK) | (srr1 & MSR_RFI_MASK)) & ~MSR_POW;
    c->pc = srr0 & ~3u;
    s_os.loads++;
    Cont* k = cont_slot(ctx, false);
    if (k) {
        const Cont copy = *k;
        k->thread = 0;
        apply_cont(g, &copy);
    }
}

/* --------------------------------------------------------------------------------------------- *
 * Scheduling (SelectThread of the SDK)
 * --------------------------------------------------------------------------------------------- */

/* The DolRecomp write journal (g_mem_write_journal) is plugged in only while a looping OSYieldThread is watched:
 * without it, a guest write costs nothing more. */
static void reset_spin(void) {
    if (s_os.spin.armed)
        ppc_set_mem_write_journal(NULL, NULL);
    s_os.spin.armed = false;
}

/* SwitchThreadCallback(from, to): the SDK default one is empty (blr) and is not called. */
static void switch_callback(GcrtCpu* g, u32 from, u32 to) {
    const u32 cb = R32(g, s_os.a_switch_cb);
    if (!cb || R32(g, cb) == 0x4E800020u)
        return;
    const u32 args[2] = {from, to};
    s_os.cb_calls++;
    gcrt_guest_call(g, cb, args, 2, NULL);
}

static void enter_idle(GcrtCpu* g) {
    CPUState* c = &g->state;
    switch_callback(g, R32(g, LOW_CUR_THREAD), 0);
    W32(g, LOW_CUR_THREAD, 0);
    gcrt_os_set_current_context(g, s_os.idle_context);
    c->msr |= MSR_EE; /* OSEnableInterrupts */
    c->pc = MAGIC_IDLE;
    s_os.idle = true;
    s_os.idles++;
    reset_spin();
    GCRT_TRACE(GCRT_TRACE_OS, "idle (no thread ready)");
}

/* Head of the best ready queue: it becomes the current thread, its context is reloaded. */
static void finish_select(GcrtCpu* g) {
    W32(g, s_os.a_hint, 0);
    const u32 prio = cntlzw(bits(g));
    const u32 q = s_os.run_queue + 8u * prio;
    const u32 next = q_remove_head(g, q);
    if (!R32(g, q))
        W32(g, s_os.a_bits, bits(g) & ~(1u << (31u - prio)));
    W32(g, next + TH_QUEUE, 0);
    W16(g, next + TH_STATE, ST_RUNNING);
    const u32 prev = R32(g, LOW_CUR_THREAD);
    switch_callback(g, prev, next);
    W32(g, LOW_CUR_THREAD, next);
    gcrt_os_set_current_context(g, next);
    if (prev != next)
        s_os.switches++;
    GCRT_TIMELINE("thread %08X -> %08X", prev, next); /* diagnostic timeline only (A17 measurements) */
    reset_spin();
    GCRT_TRACE(GCRT_TRACE_OS, "thread %08X -> %08X (priority %u)", prev, next, prio);
    load_context(g, next);
}

/* SelectThread(yield). Returns true if the processor state now belongs to another thread (or to the same thread
 * reloaded, or to idle): the caller must no longer touch the registers. `cont`: continuation of the current thread if
 * it is stored (it yields voluntarily). */
static bool select_thread(GcrtCpu* g, bool yield, const Cont* cont) {
    if ((s32)R32(g, s_os.a_reschedule) > 0)
        return false;
    const u32 ctx = R32(g, LOW_CTX);
    const u32 cur = R32(g, LOW_CUR_THREAD);
    if (ctx != cur) /* &cur->context: interrupt handler (temporary context) or idle */
        return false;
    if (gcrt_guest_call_depth() > 0) {
        /* Nested call by the host: impossible to switch threads in the middle (as with a suspended scheduler). */
        /* During a call of decompiled code into guest code, the refusal is a deferred switch: the run stops when
         * the outermost nested call returns (native_irq.c, gcrt_guest_call_note_refused_switch). */
        s_os.refused_nested++;
        gcrt_guest_call_note_refused_switch(cur);
        GCRT_TIMELINE("thread switch refused nested %08X", cur);
        GCRT_TRACE(GCRT_TRACE_OS, "thread switch refused during a nested call (thread %08X, pc %08X, lr %08X)", cur,
                   g->state.pc, g->state.lr);
        return false;
    }
    if (cur) {
        if (R16(g, cur + TH_STATE) == ST_RUNNING) {
            if (!yield) {
                const u32 prio = cntlzw(bits(g));
                if ((s32)R32(g, cur + TH_PRIORITY) <= (s32)prio)
                    return false;
            }
            W16(g, cur + TH_STATE, ST_READY);
            set_run(g, cur);
        }
        if (!(R16(g, cur + CTX_STATE) & CTX_STATE_EXC))
            save_voluntary(g, cur, cont);
    }
    if (bits(g) == 0) {
        enter_idle(g);
        return true;
    }
    finish_select(g);
    return true;
}

/* __OSReschedule */
static bool reschedule(GcrtCpu* g, const Cont* cont) {
    if (!R32(g, s_os.a_hint))
        return false;
    return select_thread(g, false, cont);
}

/* --------------------------------------------------------------------------------------------- *
 * End of a call: return to the caller (pc = LR) with r3 and the caller's MSR[EE]
 * --------------------------------------------------------------------------------------------- */

static void ret(GcrtCpu* g, u32 value, bool ee) {
    CPUState* c = &g->state;
    c->gpr[3] = value;
    c->msr = (c->msr & ~MSR_EE) | (ee ? MSR_EE : 0u);
    c->pc = c->lr;
}

static Cont cont_return(u32 value, bool ee) {
    Cont k;
    memset(&k, 0, sizeof(k));
    k.kind = CONT_RETURN;
    k.value = value;
    k.ee = ee;
    return k;
}

static bool entry_ee(GcrtCpu* g) { return (g->state.msr & MSR_EE) != 0; }
static u32 arg(GcrtCpu* g, unsigned i) { return g->state.gpr[3 + i]; }

/* --------------------------------------------------------------------------------------------- *
 * Waiting and waking up
 * --------------------------------------------------------------------------------------------- */

/* OSSleepThread(queue): true if the thread yielded. */
static bool sleep_core(GcrtCpu* g, u32 queue, const Cont* cont) {
    const u32 cur = R32(g, LOW_CUR_THREAD);
    W16(g, cur + TH_STATE, ST_WAITING);
    W32(g, cur + TH_QUEUE, queue);
    q_add_prio(g, queue, cur);
    W32(g, s_os.a_hint, 1);
    return reschedule(g, cont);
}

/* OSWakeupThread(queue): true if the current thread yielded (rescheduling). */
static bool wakeup_core(GcrtCpu* g, u32 queue, const Cont* cont) {
    while (R32(g, queue)) {
        const u32 t = q_remove_head(g, queue);
        W16(g, t + TH_STATE, ST_READY);
        if (!((s32)R32(g, t + TH_SUSPEND) > 0))
            set_run(g, t);
    }
    return reschedule(g, cont);
}

/* __OSUnlockAllMutex(thread): true if the current thread yielded. */
static bool unlock_all_mutex(GcrtCpu* g, u32 t, const Cont* cont) {
    for (;;) {
        const u32 q = t + TH_MUTEXQ;
        const u32 m = R32(g, q);
        if (!m)
            return false;
        /* RemoveHeadMutex */
        const u32 next = R32(g, m + MX_LINK);
        if (!next)
            W32(g, q + 4u, 0);
        else
            W32(g, next + MX_LINK + 4u, 0);
        W32(g, q, next);
        W32(g, m + MX_COUNT, 0);
        W32(g, m + MX_THREAD, 0);
        if (wakeup_core(g, m + MX_QUEUE, cont))
            return true;
    }
}

/* --------------------------------------------------------------------------------------------- *
 * Looping OSYieldThread: waiting for the next event
 * --------------------------------------------------------------------------------------------- */

static void spin_journal(u32 offset, u32 size, void* user) {
    (void)user;
    if (s_os.spin.armed && !(offset >= s_os.spin.stack_lo && offset + size <= s_os.spin.stack_hi)) {
        s_os.spin.dirty = true;
        ppc_set_mem_write_journal(NULL, NULL); /* verdict known: no need for the journal until the next arming */
    }
}

/* Next deadline of an event that can make a thread ready (UINT64_MAX: none). */
static u64 next_wake(GcrtCpu* g) {
    u64 dl = g->native_timer_deadline;
    const u64 nirq = gcrt_nirq_next_due(g);
    if (nirq < dl)
        dl = nirq;
    return dl;
}

/* Looping OSYieldThread: true if the same thread comes back to the same place without having written anything
 * outside its stack (below r1) since the previous one, with no thread switch nor interrupt in between: only an event
 * can then change what it is waiting for. Evaluated on entry, before any work by the host (which also writes to
 * memory). */
static bool yield_is_spin(GcrtCpu* g, u32 cur, u32 lr) {
    const bool spinning = s_os.spin.armed && s_os.spin.thread == cur && s_os.spin.lr == lr &&
                          s_os.spin.deliveries == g->deliveries && s_os.spin.switches == s_os.switches &&
                          !s_os.spin.dirty;
    reset_spin(); /* disarmed during the host's work (which also writes to memory) */
    return spinning;
}

/* The current thread takes over again after an OSYieldThread with no other thread ready: if it only loops, wait for
 * the next event (at most 1 emulated ms), then watch the writes until the next OSYieldThread. */
static void yield_spin(GcrtCpu* g, u32 cur, u32 lr, bool ee, bool spinning) {
    CPUState* c = &g->state;
    if (spinning && ee && (s32)R32(g, s_os.a_reschedule) <= 0 && gcrt_guest_call_depth() == 0) {
        const u64 now = g->ticks;
        u64 dl = next_wake(g);
        if (dl != UINT64_MAX && dl > now) {
            if (dl - now > YIELD_WAIT_MAX_TICKS)
                dl = now + YIELD_WAIT_MAX_TICKS;
            gcrt_cpu_wait_until(g, dl);
            s_os.yield_waits++;
        }
    }
    s_os.spin.armed = true;
    ppc_set_mem_write_journal(spin_journal, NULL);
    s_os.spin.dirty = false;
    s_os.spin.thread = cur;
    s_os.spin.lr = lr;
    s_os.spin.deliveries = g->deliveries;
    s_os.spin.switches = s_os.switches;
    const u32 sp = c->gpr[1] & 0x3FFFFFFFu;
    /* Allowed: the frames of the called functions (below r1) and the LR save word of the current frame (r1 + 4),
     * which any called function writes according to the PowerPC EABI. The local variables of the loop (r1 + 8 and
     * beyond) and all the rest of memory count as a state change. */
    s_os.spin.stack_hi = sp + 8u;
    s_os.spin.stack_lo = sp > 0x10000u ? sp - 0x10000u : 0u;
}

/* --------------------------------------------------------------------------------------------- *
 * Continuations of the threads that take over again
 * --------------------------------------------------------------------------------------------- */

static void receive_core(GcrtCpu* g, u32 mq, u32 msgp, u32 flags, bool ee);
static void send_core(GcrtCpu* g, u32 mq, u32 msg, u32 flags, bool ee);

static void apply_cont(GcrtCpu* g, const Cont* k) {
    switch (k->kind) {
    case CONT_RETURN:
        ret(g, k->value, k->ee);
        break;
    case CONT_RECEIVE:
        receive_core(g, k->a0, k->a1, k->a2, k->ee);
        break;
    case CONT_SEND:
        send_core(g, k->a0, k->a1, k->a2, k->ee);
        break;
    }
}

/* --------------------------------------------------------------------------------------------- *
 * Replacements: scheduler
 * --------------------------------------------------------------------------------------------- */

s32 gcrt_os_disable_scheduler(GcrtCpu* g) {
    const s32 old = (s32)R32(g, s_os.a_reschedule);
    W32(g, s_os.a_reschedule, (u32)(old + 1));
    return old;
}

s32 gcrt_os_enable_scheduler(GcrtCpu* g) {
    const s32 old = (s32)R32(g, s_os.a_reschedule);
    W32(g, s_os.a_reschedule, (u32)(old - 1));
    return old;
}

static bool hle_disable_scheduler(GcrtCpu* g, u32 address) {
    (void)address;
    ret(g, (u32)gcrt_os_disable_scheduler(g), entry_ee(g));
    return true;
}

static bool hle_enable_scheduler(GcrtCpu* g, u32 address) {
    (void)address;
    ret(g, (u32)gcrt_os_enable_scheduler(g), entry_ee(g));
    return true;
}

static bool hle_reschedule(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const u32 r3 = g->state.gpr[3];
    const Cont k = cont_return(r3, ee);
    if (!reschedule(g, &k))
        ret(g, r3, ee);
    return true;
}

/* SelectThread(yield): static in the SDK, called only by the replaced functions; kept to be safe. */
static bool hle_select_thread(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const Cont k = cont_return(0, ee);
    if (!select_thread(g, arg(g, 0) != 0, &k))
        ret(g, 0, ee);
    return true;
}

static bool hle_yield_thread(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* c = &g->state;
    const bool ee = entry_ee(g);
    const u32 cur = R32(g, LOW_CUR_THREAD);
    const u32 lr = c->lr;
    s_os.yields++;
    const bool spinning = yield_is_spin(g, cur, lr);
    const u64 switches = s_os.switches;
    const Cont k = cont_return(c->gpr[3], ee);
    if (!select_thread(g, true, &k))
        ret(g, c->gpr[3], ee);
    /* Same thread resumed without a change: possible wait loop. */
    if (s_os.switches == switches && R32(g, LOW_CUR_THREAD) == cur && cur && c->pc == lr)
        yield_spin(g, cur, lr, ee, spinning);
    return true;
}

static bool hle_sleep_thread(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const u32 r3 = g->state.gpr[3];
    const Cont k = cont_return(r3, ee);
    if (!sleep_core(g, arg(g, 0), &k))
        ret(g, r3, ee);
    return true;
}

static bool hle_wakeup_thread(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const u32 r3 = g->state.gpr[3];
    const Cont k = cont_return(r3, ee);
    if (!wakeup_core(g, arg(g, 0), &k))
        ret(g, r3, ee);
    return true;
}

/* OSResumeThread(thread) -> previous suspend count */
static bool hle_resume_thread(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const u32 t = arg(g, 0);
    const s32 count = (s32)R32(g, t + TH_SUSPEND);
    const s32 now = count - 1;
    W32(g, t + TH_SUSPEND, (u32)now);
    if (now < 0) {
        W32(g, t + TH_SUSPEND, 0);
    } else if (now == 0) {
        switch (R16(g, t + TH_STATE)) {
        case ST_READY:
            W32(g, t + TH_PRIORITY, (u32)effective_priority(g, t));
            set_run(g, t);
            break;
        case ST_WAITING: {
            const u32 q = R32(g, t + TH_QUEUE);
            q_remove(g, q, t, TH_LINK);
            W32(g, t + TH_PRIORITY, (u32)effective_priority(g, t));
            q_add_prio(g, q, t);
            const u32 m = R32(g, t + TH_MUTEX);
            if (m)
                update_priority(g, R32(g, m + MX_THREAD));
            break;
        }
        default:
            break;
        }
        const Cont k = cont_return((u32)count, ee);
        if (reschedule(g, &k))
            return true;
    }
    ret(g, (u32)count, ee);
    return true;
}

/* OSSuspendThread(thread) -> previous suspend count */
static bool hle_suspend_thread(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const u32 t = arg(g, 0);
    const s32 count = (s32)R32(g, t + TH_SUSPEND);
    W32(g, t + TH_SUSPEND, (u32)(count + 1));
    if (count == 0) {
        switch (R16(g, t + TH_STATE)) {
        case ST_RUNNING:
            W32(g, s_os.a_hint, 1);
            W16(g, t + TH_STATE, ST_READY);
            break;
        case ST_READY:
            unset_run(g, t);
            break;
        case ST_WAITING: {
            const u32 q = R32(g, t + TH_QUEUE);
            q_remove(g, q, t, TH_LINK);
            W32(g, t + TH_PRIORITY, 32u);
            q_add_tail(g, q, t, TH_LINK);
            const u32 m = R32(g, t + TH_MUTEX);
            if (m)
                update_priority(g, R32(g, m + MX_THREAD));
            break;
        }
        default:
            break;
        }
        const Cont k = cont_return((u32)count, ee);
        if (reschedule(g, &k))
            return true;
    }
    ret(g, (u32)count, ee);
    return true;
}

/* OSCreateThread(thread, func, param, stack, stackSize, priority, attr) -> BOOL */
static bool hle_create_thread(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* c = &g->state;
    const bool ee = entry_ee(g);
    const u32 t = arg(g, 0), func = arg(g, 1), param = arg(g, 2), stack = arg(g, 3), size = arg(g, 4);
    const s32 prio = (s32)arg(g, 5);
    const u32 attr = arg(g, 6);
    if (prio < 0 || prio > 31) {
        ret(g, 0, ee);
        return true;
    }
    cont_drop(t);
    W16(g, t + TH_STATE, ST_READY);
    W16(g, t + TH_ATTR, (u16)(attr & ATTR_DETACH));
    W32(g, t + TH_BASE, (u32)prio);
    W32(g, t + TH_PRIORITY, (u32)prio);
    W32(g, t + TH_SUSPEND, 1);
    W32(g, t + TH_VAL, 0xFFFFFFFFu);
    W32(g, t + TH_MUTEX, 0);
    W32(g, t + TH_JOIN, 0);
    W32(g, t + TH_JOIN + 4u, 0);
    W32(g, t + TH_MUTEXQ, 0);
    W32(g, t + TH_MUTEXQ + 4u, 0);
    const u32 top = stack & ~7u;
    W32(g, top - 8u, 0);
    W32(g, top - 4u, 0);
    /* OSInitContext(&thread->context, func, top - 8) */
    W32(g, t + CTX_SRR0, func);
    W32(g, t + CTX_GPR + 4u, top - 8u);
    W32(g, t + CTX_SRR1, 0x9032u);
    W32(g, t + CTX_CR, 0);
    W32(g, t + CTX_XER, 0);
    W32(g, t + CTX_GPR + 8u, c->gpr[2]);
    W32(g, t + CTX_GPR + 4u * 13u, c->gpr[13]);
    for (u32 r = 3; r <= 12u; r++)
        W32(g, t + CTX_GPR + 4u * r, 0);
    for (u32 r = 14; r < 32u; r++)
        W32(g, t + CTX_GPR + 4u * r, 0);
    for (u32 i = 0; i < 8u; i++)
        W32(g, t + CTX_GQR + 4u * i, 0);
    gcrt_os_clear_context(g, t);
    W32(g, t + CTX_LR, s_os.fn_exit_thread);
    W32(g, t + CTX_GPR + 12u, param);
    W32(g, t + TH_STACK_BASE, stack);
    W32(g, t + TH_STACK_END, stack - size);
    W32(g, stack - size, STACK_MAGIC);
    W32(g, t + TH_ERROR, 0);
    W32(g, t + TH_SPECIFIC, 0);
    W32(g, t + TH_SPECIFIC + 4u, 0);
    if (s_os.a_fpe_handler && R32(g, s_os.a_fpe_handler)) {
        /* Floating-point error handler installed: floating-point exceptions enabled, FP registers set to NaN. */
        W32(g, t + CTX_SRR1, R32(g, t + CTX_SRR1) | 0x900u);
        W16(g, t + CTX_STATE, (u16)(R16(g, t + CTX_STATE) | CTX_STATE_FPSAVED));
        W32(g, t + CTX_FPSCR + 4u, (R32(g, s_os.a_fpscr_enable) & 0xF8u) | 0x4u);
        for (u32 i = 0; i < 32u; i++) {
            W32(g, t + CTX_FPR + 8u * i, 0xFFFFFFFFu);
            W32(g, t + CTX_FPR + 8u * i + 4u, 0xFFFFFFFFu);
            W32(g, t + CTX_PSF + 8u * i, 0xFFFFFFFFu);
            W32(g, t + CTX_PSF + 8u * i + 4u, 0xFFFFFFFFu);
        }
    }
    q_add_tail(g, LOW_ACTIVE_QUEUE, t, TH_ACTIVE);
    GCRT_TRACE(GCRT_TRACE_OS, "OSCreateThread %08X: entry %08X, stack %08X (%u bytes), priority %d", t, func, stack,
               size, prio);
    ret(g, 1, ee);
    return true;
}

/* End of a thread (OSExitThread for the current thread, OSCancelThread for another one): context cleared, detached
 * or dying, mutexes released, threads waiting to join woken up. true if the current thread yielded. */
static bool end_thread(GcrtCpu* g, u32 t, u32 val, bool set_val, const Cont* cont) {
    gcrt_os_clear_context(g, t);
    cont_drop(t);
    if (R16(g, t + TH_ATTR) & ATTR_DETACH) {
        q_remove(g, LOW_ACTIVE_QUEUE, t, TH_ACTIVE);
        W16(g, t + TH_STATE, ST_NULL);
    } else {
        W16(g, t + TH_STATE, ST_MORIBUND);
        if (set_val)
            W32(g, t + TH_VAL, val);
    }
    if (unlock_all_mutex(g, t, cont))
        return true;
    return wakeup_core(g, t + TH_JOIN, cont);
}

static bool hle_exit_thread(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const u32 cur = R32(g, LOW_CUR_THREAD);
    const Cont k = cont_return(g->state.gpr[3], ee);
    GCRT_TRACE(GCRT_TRACE_OS, "OSExitThread %08X (value %08X)", cur, arg(g, 0));
    if (end_thread(g, cur, arg(g, 0), true, &k))
        return true;
    W32(g, s_os.a_hint, 1);
    if (!select_thread(g, false, &k))
        ret(g, g->state.gpr[3], ee);
    return true;
}

static bool hle_cancel_thread(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const u32 t = arg(g, 0);
    switch (R16(g, t + TH_STATE)) {
    case ST_READY:
        if (!((s32)R32(g, t + TH_SUSPEND) > 0))
            unset_run(g, t);
        break;
    case ST_RUNNING:
        W32(g, s_os.a_hint, 1);
        break;
    case ST_WAITING: {
        q_remove(g, R32(g, t + TH_QUEUE), t, TH_LINK);
        W32(g, t + TH_QUEUE, 0);
        const u32 m = R32(g, t + TH_MUTEX);
        if (!((s32)R32(g, t + TH_SUSPEND) > 0) && m)
            update_priority(g, R32(g, m + MX_THREAD));
        break;
    }
    default:
        ret(g, g->state.gpr[3], ee);
        return true;
    }
    GCRT_TRACE(GCRT_TRACE_OS, "OSCancelThread %08X", t);
    const Cont k = cont_return(g->state.gpr[3], ee);
    if (end_thread(g, t, 0, false, &k))
        return true;
    if (!reschedule(g, &k))
        ret(g, g->state.gpr[3], ee);
    return true;
}

static bool hle_unlock_all_mutex(GcrtCpu* g, u32 address) {
    (void)address;
    const bool ee = entry_ee(g);
    const Cont k = cont_return(g->state.gpr[3], ee);
    if (!unlock_all_mutex(g, arg(g, 0), &k))
        ret(g, g->state.gpr[3], ee);
    return true;
}

/* OSLoadContext(context): without `rfi`. */
static bool hle_load_context(GcrtCpu* g, u32 address) {
    (void)address;
    load_context(g, arg(g, 0));
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * Message queues (OSMessage.c)
 * --------------------------------------------------------------------------------------------- */

static Cont cont_msg(ContKind kind, u32 mq, u32 a, u32 flags, bool ee) {
    Cont k;
    memset(&k, 0, sizeof(k));
    k.kind = kind;
    k.a0 = mq;
    k.a1 = a;
    k.a2 = flags;
    k.ee = ee;
    return k;
}

/* Blocked with no thread switch possible (scheduler suspended): the console would spin forever; here the call is
 * simply restarted at the next block (same arguments). */
static void retry_call(GcrtCpu* g, u32 fn, u32 a0, u32 a1, u32 a2, bool ee) {
    CPUState* c = &g->state;
    c->gpr[3] = a0;
    c->gpr[4] = a1;
    c->gpr[5] = a2;
    c->msr = (c->msr & ~MSR_EE) | (ee ? MSR_EE : 0u);
    c->pc = fn;
}

static void send_core(GcrtCpu* g, u32 mq, u32 msg, u32 flags, bool ee) {
    const s32 count = (s32)R32(g, mq + MQ_COUNT);
    if (count <= (s32)R32(g, mq + MQ_USED)) {
        if (!(flags & MSG_BLOCK)) {
            ret(g, 0, ee);
            return;
        }
        const Cont k = cont_msg(CONT_SEND, mq, msg, flags, ee);
        if (!sleep_core(g, mq + MQ_SEND, &k))
            retry_call(g, s_os.fn_send, mq, msg, flags, ee);
        return;
    }
    const s32 index = ((s32)R32(g, mq + MQ_FIRST) + (s32)R32(g, mq + MQ_USED)) % count;
    W32(g, R32(g, mq + MQ_ARRAY) + 4u * (u32)index, msg);
    W32(g, mq + MQ_USED, R32(g, mq + MQ_USED) + 1u);
    const Cont k = cont_return(1, ee);
    if (!wakeup_core(g, mq + MQ_RECV, &k))
        ret(g, 1, ee);
}

static void receive_core(GcrtCpu* g, u32 mq, u32 msgp, u32 flags, bool ee) {
    if (R32(g, mq + MQ_USED) == 0) {
        if (!(flags & MSG_BLOCK)) {
            ret(g, 0, ee);
            return;
        }
        const Cont k = cont_msg(CONT_RECEIVE, mq, msgp, flags, ee);
        if (!sleep_core(g, mq + MQ_RECV, &k))
            retry_call(g, s_os.fn_receive, mq, msgp, flags, ee);
        return;
    }
    const u32 first = R32(g, mq + MQ_FIRST);
    if (msgp)
        W32(g, msgp, R32(g, R32(g, mq + MQ_ARRAY) + 4u * first));
    W32(g, mq + MQ_FIRST, (u32)(((s32)first + 1) % (s32)R32(g, mq + MQ_COUNT)));
    W32(g, mq + MQ_USED, R32(g, mq + MQ_USED) - 1u);
    const Cont k = cont_return(1, ee);
    if (!wakeup_core(g, mq + MQ_SEND, &k))
        ret(g, 1, ee);
}

static bool hle_send_message(GcrtCpu* g, u32 address) {
    (void)address;
    send_core(g, arg(g, 0), arg(g, 1), arg(g, 2), entry_ee(g));
    return true;
}

static bool hle_receive_message(GcrtCpu* g, u32 address) {
    (void)address;
    receive_core(g, arg(g, 0), arg(g, 1), arg(g, 2), entry_ee(g));
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * Idle
 * --------------------------------------------------------------------------------------------- */

static bool hle_idle(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* c = &g->state;
    if (bits(g)) {
        /* Exit of the SDK idle loop: OSDisableInterrupts, OSClearContext(&IdleContext), selection. */
        c->msr &= ~MSR_EE;
        gcrt_os_clear_context(g, s_os.idle_context);
        s_os.idle = false;
        finish_select(g);
        return true;
    }
    if (!(c->msr & MSR_EE))
        c->msr |= MSR_EE; /* idle always waits with interrupts enabled */
    if (gcrt_nirq_next_due(g) <= g->ticks)
        return true; /* an interrupt is waiting for its delivery (next block boundary) */
    const u64 dl = next_wake(g);
    if (dl == UINT64_MAX) {
        fprintf(stderr, "gcrt native OS: idle without any upcoming event (the game waits forever)\n");
        g->stop = GCRT_STOP_GUEST_HALT;
        return true;
    }
    gcrt_cpu_wait_until(g, dl);
    s_os.idle_waits++;
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * End of interrupt and deferred rescheduling (native_irq.c)
 * --------------------------------------------------------------------------------------------- */

void gcrt_os_load_context(GcrtCpu* g, u32 context) { load_context(g, context); }

void gcrt_os_interrupt_exit(GcrtCpu* g, u32 context) {
    if (R32(g, s_os.a_hint) && select_thread(g, false, NULL))
        return;
    load_context(g, context);
}

void gcrt_os_reschedule_return(GcrtCpu* g, u32 result) {
    /* pc and r3 already set by the caller (return of the replacement); caller's MSR[EE] unchanged. */
    const Cont k = cont_return(result, entry_ee(g));
    reschedule(g, &k);
}

void gcrt_os_note_delivery(void) { reset_spin(); }

/* --------------------------------------------------------------------------------------------- *
 * Synchronous exceptions without a vector: FPU unavailable (OSSwitchFPUContext), system call
 * --------------------------------------------------------------------------------------------- */

static void fpu_unavailable(GcrtCpu* g);

static bool exception_hook(GcrtCpu* g, u32 exception) {
    CPUState* c = &g->state;
    if (exception & PPC_EXC_FP_UNAVAILABLE) {
        fpu_unavailable(g);
        return true;
    }
    if (exception & PPC_EXC_SYSTEM_CALL) {
        /* SDK system call: synchronization (HID0[ABE] toggled then restored) then rfi. No effect here. */
        c->msr = ((c->msr & ~MSR_RFI_MASK) | (c->srr1 & MSR_RFI_MASK)) & ~MSR_POW;
        c->pc = c->srr0 & ~3u;
        s_os.syscalls++;
        return true;
    }
    return false;
}

void gcrt_native_os_fpu_claim(GcrtCpu* g, u32 at) {
    CPUState* c = &g->state;
    if (c->msr & MSR_FP)
        return;
    /* Same entry as the exception raised by the generated code (ppc_fp_available), then same handling. */
    const u32 pc = c->pc;
    ppc_take_exception(c, PPC_EXC_FP_UNAVAILABLE, PPC_VECTOR_FP_UNAVAILABLE, at, 0);
    c->exception &= ~PPC_EXC_FP_UNAVAILABLE;
    fpu_unavailable(g);
    c->pc = pc;
}

/* FPU unavailable: what the OS vector and OSSwitchFPUContext do; resumption at SRR0. */
static void fpu_unavailable(GcrtCpu* g) {
    CPUState* c = &g->state;
    {
        const u32 ctx = R32(g, LOW_CTX);
        /* What the OS vector (OSExceptionVector) stores and OSSwitchFPUContext then restores. */
        W32(g, ctx + CTX_GPR + 0x0Cu, c->gpr[3]);
        W32(g, ctx + CTX_GPR + 0x10u, c->gpr[4]);
        W32(g, ctx + CTX_GPR + 0x14u, c->gpr[5]);
        W32(g, ctx + CTX_CR, c->cr);
        W32(g, ctx + CTX_LR, c->lr);
        W32(g, ctx + CTX_CTR, c->ctr);
        W32(g, ctx + CTX_XER, c->xer);
        W32(g, ctx + CTX_SRR0, c->srr0);
        W32(g, ctx + CTX_SRR1, c->srr1);
        const u32 srr0 = c->srr0, srr1 = c->srr1;
        c->msr |= MSR_IR | MSR_DR | MSR_FP; /* handler: exception MSR + IR|DR, then FP (mfmsr ; ori ; mtmsr) */
        const u32 owner = R32(g, LOW_FPU_OWNER);
        W32(g, LOW_FPU_OWNER, ctx);
        if (owner != ctx) {
            if (owner) {
                const u32 a[3] = {0, 0, owner}; /* __OSSaveFPUContext: context in r5 */
                gcrt_guest_call(g, s_os.fn_save_fpu, a, 3, NULL);
            }
            const u32 b[3] = {0, ctx, 0}; /* __OSLoadFPUContext: context in r4 */
            gcrt_guest_call(g, s_os.fn_load_fpu, b, 3, NULL);
        }
        /* rfi to the floating-point instruction, MSR of the time of the exception with FP. */
        c->srr0 = srr0;
        c->srr1 = srr1 | MSR_FP;
        c->msr = ((c->msr & ~MSR_RFI_MASK) | (c->srr1 & MSR_RFI_MASK)) & ~MSR_POW;
        c->pc = srr0 & ~3u;
        s_os.fpu_switches++;
        GCRT_TRACE(GCRT_TRACE_OS, "FPU: owner %08X -> %08X (pc %08X)", owner, ctx, srr0);
    }
}

/* --------------------------------------------------------------------------------------------- *
 * Installation: OS variables found in the recognized code
 * --------------------------------------------------------------------------------------------- */

static u32 fn_of(const char* name, u32* size) {
    unsigned n = 0;
    const u32 a = gcrt_hle_find(name, &n);
    if (n != 1)
        return 0;
    if (size)
        *size = gcrt_hle_catalog_size(name);
    return a;
}

static bool find_variables(GcrtCpu* g) {
    u32 size = 0;
    /* The game's r13: end of __init_registers (lis r13, H ; ori r13, r13, L). */
    const u32 init_regs = fn_of("__init_registers", &size);
    if (!init_regs || !gcrt_hle_decode_reg_at_blr(g, init_regs, size, 13, &s_os.sda) || !s_os.sda) {
        fprintf(stderr, "gcrt native OS: the game's r13 not found (__init_registers)\n");
        return false;
    }
    /* __OSThreadInit: RunQueueBits = 0, RunQueueHint = FALSE, ... Reschedule = 0 (stw in that order), and
     * SwitchThreadCallback (lwz r12, d(r13) ; blrl). */
    const u32 thread_init = fn_of("__OSThreadInit", &size);
    s16 d_bits, d_hint, d_resched, d_cb;
    if (!thread_init || !gcrt_hle_decode_d_offset(g, thread_init, size, 36, 13, 0, &d_bits) ||
        !gcrt_hle_decode_d_offset(g, thread_init, size, 36, 13, 1, &d_hint) ||
        !gcrt_hle_decode_d_offset(g, thread_init, size, 36, 13, 2, &d_resched) ||
        !gcrt_hle_decode_d_offset(g, thread_init, size, 32, 13, 0, &d_cb)) {
        fprintf(stderr, "gcrt native OS: scheduler variables not found (__OSThreadInit)\n");
        return false;
    }
    s_os.a_bits = s_os.sda + (u32)(s32)d_bits;
    s_os.a_hint = s_os.sda + (u32)(s32)d_hint;
    s_os.a_reschedule = s_os.sda + (u32)(s32)d_resched;
    s_os.a_switch_cb = s_os.sda + (u32)(s32)d_cb;
    /* Check: OSDisableScheduler reads the same Reschedule variable. */
    u32 dsize = 0;
    const u32 dis_sched = fn_of("OSDisableScheduler", &dsize);
    s16 d_check;
    if (!dis_sched || !gcrt_hle_decode_d_offset(g, dis_sched, dsize, 32, 13, 0, &d_check) || d_check != d_resched) {
        fprintf(stderr, "gcrt native OS: Reschedule inconsistent between __OSThreadInit and OSDisableScheduler\n");
        return false;
    }
    /* SelectThread: RunQueue (lis r4 ; addi r31, r4, L: r31 keeps the base) and IdleContext (argument of the first
     * OSSetCurrentContext, the idle one; cross-checked with the first OSClearContext, same argument). */
    const u32 select = fn_of("SelectThread", &size);
    const u32 set_ctx = fn_of("OSSetCurrentContext", NULL);
    const u32 clear_ctx = fn_of("OSClearContext", NULL);
    u32 idle_check = 0;
    if (!select || !set_ctx || !clear_ctx ||
        !gcrt_hle_decode_reg_at_call(g, select, size, set_ctx, 0, 3, s_os.sda, 0, &s_os.idle_context) ||
        !gcrt_hle_decode_reg_at_call(g, select, size, set_ctx, 0, 31, s_os.sda, 0, &s_os.run_queue) ||
        !gcrt_hle_decode_reg_at_call(g, select, size, clear_ctx, 0, 3, s_os.sda, 0, &idle_check) ||
        idle_check != s_os.idle_context) {
        fprintf(stderr, "gcrt native OS: RunQueue / IdleContext not found (SelectThread %08X, OSSetCurrentContext "
                        "%08X, OSClearContext %08X: %08X %08X %08X)\n",
                select, set_ctx, clear_ctx, s_os.idle_context, s_os.run_queue, idle_check);
        return false;
    }
    s_os.fn_disable_interrupts = fn_of("OSDisableInterrupts", NULL);
    s_os.fn_exit_thread = fn_of("OSExitThread", NULL);
    s_os.fn_save_fpu = fn_of("__OSSaveFPUContext", NULL);
    s_os.fn_load_fpu = fn_of("__OSLoadFPUContext", NULL);
    s_os.fn_switch_fpu = fn_of("OSSwitchFPUContext", NULL);
    s_os.fn_send = fn_of("OSSendMessage", NULL);
    s_os.fn_receive = fn_of("OSReceiveMessage", NULL);
    if (!s_os.fn_disable_interrupts || !s_os.fn_exit_thread || !s_os.fn_save_fpu || !s_os.fn_load_fpu) {
        fprintf(stderr, "gcrt native OS: OS functions not found (OSDisableInterrupts, OSExitThread, "
                        "__OSSave/LoadFPUContext)\n");
        return false;
    }
    /* OSCreateThread: error handler table (+0x40 = floating-point error) and __OSFpscrEnableBits. */
    const u32 create = fn_of("OSCreateThread", &size);
    s16 d_fpscr;
    if (create && gcrt_hle_decode_d_offset(g, create, size, 32, 13, 0, &d_fpscr))
        s_os.a_fpscr_enable = s_os.sda + (u32)(s32)d_fpscr;
    /* The table is read right after OSDisableInterrupts: sequence lis rX, H ; addi rX, rX, L ; lwz rY, 0x40(rX). */
    for (u32 off = 0; create && off + 12u <= size; off += 4u) {
        const u32 w0 = R32(g, create + off), w1 = R32(g, create + off + 4u), w2 = R32(g, create + off + 8u);
        const u32 rd0 = (w0 >> 21) & 31u;
        if ((w0 >> 26) == 15u && ((w0 >> 16) & 31u) == 0 && (w1 >> 26) == 14u && ((w1 >> 16) & 31u) == rd0 &&
            (w2 >> 26) == 32u && ((w2 >> 16) & 31u) == ((w1 >> 21) & 31u) && (w2 & 0xFFFFu) == 0x40u) {
            s_os.a_fpe_handler = ((w0 & 0xFFFFu) << 16) + (u32)(s32)(s16)(w1 & 0xFFFFu) + 0x40u;
            break;
        }
    }
    return true;
}

void gcrt_native_os_install(GcrtCpu* g) {
    memset(&s_os, 0, sizeof(s_os));
    if (!find_variables(g)) {
        /* Since step 10b there are no more exception vectors nor mini-interpreter: without the native OS, the game
         * would stop further on at its first exception. So stop right away, with a clear message. */
        fprintf(stderr, "gcrt native OS: FATAL ERROR: SDK OS not recognized (scheduler variables not found);\n"
                        "this game cannot run without the native OS. See notes/hle.md (recognition by signature).\n");
        exit(2);
    }
    char* m = s_os.missing;
    const size_t ms = sizeof(s_os.missing);
    struct {
        const char* name;
        GcrtHleFn fn;
    } const k[] = {
        {"OSDisableScheduler", hle_disable_scheduler},
        {"OSEnableScheduler", hle_enable_scheduler},
        {"__OSReschedule", hle_reschedule},
        {"SelectThread", hle_select_thread},
        {"OSYieldThread", hle_yield_thread},
        {"OSCreateThread", hle_create_thread},
        {"OSExitThread", hle_exit_thread},
        {"OSCancelThread", hle_cancel_thread},
        {"OSResumeThread", hle_resume_thread},
        {"OSSuspendThread", hle_suspend_thread},
        {"OSSleepThread", hle_sleep_thread},
        {"OSWakeupThread", hle_wakeup_thread},
        {"OSLoadContext", hle_load_context},
        {"OSSendMessage", hle_send_message},
        {"OSReceiveMessage", hle_receive_message},
        {"__OSUnlockAllMutex", hle_unlock_all_mutex},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        s_os.replaced += gcrt_native_replace(g, k[i].name, "OS", k[i].fn, NULL, 0, NULL, m, ms);
    gcrt_hle_register(g, MAGIC_IDLE, "native OS idle", hle_idle);
    g->exception_hook = exception_hook;
    ppc_set_mem_write_journal(NULL, NULL);
    s_os.ready = true;
    gcrt_trace_printf(GCRT_TRACE_NATIVE,
                      "native OS: %u functions replaced, missing: %s; r13 %08X, RunQueue %08X, IdleContext %08X, "
                      "Reschedule %08X, RunQueueBits %08X, RunQueueHint %08X",
                      s_os.replaced, s_os.missing[0] ? s_os.missing : "none", s_os.sda, s_os.run_queue,
                      s_os.idle_context, s_os.a_reschedule, s_os.a_bits, s_os.a_hint);
}

void gcrt_native_os_report(FILE* out) {
    if (!s_os.ready)
        return;
    fprintf(out,
            "native OS: %llu thread switches, %llu times idle (%llu host waits), %llu "
            "OSYieldThread (%llu waits), %llu FPU switches, %llu system calls, %llu context saves and %llu "
            "context loads, %llu switches refused (nested call); %u functions replaced, missing: "
            "%s\n",
            (unsigned long long)s_os.switches, (unsigned long long)s_os.idles, (unsigned long long)s_os.idle_waits,
            (unsigned long long)s_os.yields, (unsigned long long)s_os.yield_waits,
            (unsigned long long)s_os.fpu_switches, (unsigned long long)s_os.syscalls, (unsigned long long)s_os.saves,
            (unsigned long long)s_os.loads, (unsigned long long)s_os.refused_nested, s_os.replaced,
            s_os.missing[0] ? s_os.missing : "none");
}
