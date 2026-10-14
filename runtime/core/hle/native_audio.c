/* Native audio (step 10d of milestone 10): replacement of the SDK AI (ai.c) and DSP (dsp.c, dsp_task.c) functions,
 * the only sound functions that touch the hardware; the AX library and what lies above it (the game's sound library,
 * THP videos) stay recompiled. See the internal SDK notes § 15 (inventory, boundary, semantics) and notes/runtime.md
 * (step 10d section). Generic: no reference to a game; static variables found in the recognized code.
 *
 * What replaces the hardware:
 *   - audio DMA (AI): the host itself reads the output buffer in guest memory, one 32-byte block (8 16-bit stereo
 *     frames, right then left) at the converter rate (divisors 3372 / 2248 of 108 MHz: 32,028.5 Hz / 48,042.7 Hz),
 *     hands it over to the host output (SDL, WAV: gcrt_native_set_audio_sink) and, when the buffer is exhausted,
 *     takes again the address of the last AIInitDMA and delivers interrupt #5 (DSP_AI) on the game thread; its handler
 *     redoes __AIDHandler (temporary context, __AID_Active reentrancy guard, user callback, on the AIInit stack if
 *     needed). No disc streaming (DTK): interrupt #8 does not exist.
 *   - DSP: no more mailboxes nor control register. DSPSendMailToDSP hands the mail over to the HLE microcode
 *     (runtime/core/audio, plugged in by the platform: gcrt_native_set_dsp_backend); the mails from the microcode to
 *     the CPU are stored here in a queue, with interrupt #7 (DSP_DSP) delivered on the game thread; its handler
 *     redoes __DSPHandler (tasks, init/resume/done/request callbacks, task switch). The tasks stay the SDK
 *     DSPTaskInfo in guest memory; DSPAddTask starts the first task by giving the HLE microcode the fingerprint of its
 *     IRAM code, like the DSP ROM.
 *
 * Instants: identical to those of the emulated reference (hw/dsp, hw/ai, sched/, removed; the last build that still
 * emulated the hardware, kept outside this repository's public history, and the executable of step 10c), measured
 * with GCRT_TIMELINE. The audio events (DMA block, microcode update every millisecond, deferred DSP interrupt, first
 * DMA interrupt) are timed like those of the former scheduler: deadline = time of the last processing of audio
 * events + delay; a delay is expressed in nominal Gekko cycles (time base ticks × 12) because the constants come
 * from Dolphin in that unit (DSP_YIELD 2,500 cycles after the command list, first DMA interrupt 200 cycles after its
 * activation, block 36 × divisor) and because the order of two deadlines of the same tick must stay that of the
 * reference. It is not a clock in executed cycles: everything is timed on the host ticks (g->ticks), through a native
 * timer. Interrupts #5 and #7 are delivered before the other due external native interrupts, in the order of the
 * SDK priority table among themselves (DSP_DSP then DSP_AI): that is the order of the reference, where they arrived
 * through the external interrupt line of the emulated hardware (gcrt_nirq_post_ex, GCRT_NIRQ_FIRST).
 *
 * Sources: disassembly of the recognized SDK (AI and DSP 0x2301 libraries); reference decompilation
 * external/cbr_decomp/decomp_dtk_main/src/Dolphin/{ai/ai.c, dsp/dsp.c, dsp/dsp_task.c} (CC0, consulted only);
 * Dolphin e6f3ae17 (GPL-2.0-or-later) for the audio DMA (HW/DSP.cpp UpdateAudioDMA, SystemTimers.cpp,
 * AudioInterface.cpp) and the mail protocol (DSPHLE/MailHandler.cpp). */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <stdlib.h>
#include <string.h>

/* --- Constants of the replaced hardware (Dolphin e6f3ae17) --- */
#define AI_DIVISOR_32K 3372u /* AudioInterface.cpp: 108 MHz / 3372 = 32,028.5 Hz */
#define AI_DIVISOR_48K 2248u /* 108 MHz / 2248 = 48,042.7 Hz */
#define AI_BLOCK_CYCLES(div) (36ull * (div)) /* SystemTimers.cpp: CPU 486 MHz × 32 B / (4 B × rate) */
#define AID_START_DELAY_CYCLES 200          /* DSP.cpp: first interrupt 200 cycles after the activation */
#define DSP_UPDATE_CYCLES (GCRT_CPU_HZ / 1000u) /* update of the HLE microcodes: every millisecond */
#define DSP_ROM_READY 0x8071FEEDu

/* AICR (bits kept by the host, like the register: PSTAT, AISFR, AIINTMSK, AIINT, AIINTVLD, AIDFR). */
#define AICR_PSTAT 0x01u
#define AICR_AISFR 0x02u
#define AICR_AIINTMSK 0x04u
#define AICR_AIINT 0x08u
#define AICR_AIINTVLD 0x10u
#define AICR_SCRESET 0x20u
#define AICR_AIDFR 0x40u
#define AICR_KEPT (AICR_PSTAT | AICR_AISFR | AICR_AIINTMSK | AICR_AIINTVLD | AICR_AIDFR)

/* DSPTaskInfo (SDK dsp.h). */
#define TASK_STATE 0x00u
#define TASK_PRIORITY 0x04u
#define TASK_FLAGS 0x08u
#define TASK_IRAM_MMEM 0x0Cu
#define TASK_IRAM_LENGTH 0x10u
#define TASK_IRAM_ADDR 0x14u
#define TASK_DRAM_MMEM 0x18u
#define TASK_DRAM_LENGTH 0x1Cu
#define TASK_DRAM_ADDR 0x20u
#define TASK_INIT_VECTOR 0x24u
#define TASK_RESUME_VECTOR 0x26u
#define TASK_INIT_CB 0x28u
#define TASK_RES_CB 0x2Cu
#define TASK_DONE_CB 0x30u
#define TASK_REQ_CB 0x34u
#define TASK_NEXT 0x38u
#define TASK_PREV 0x3Cu
#define TASK_STATE_INIT 0u
#define TASK_STATE_RUN 1u
#define TASK_STATE_YIELD 2u
#define TASK_STATE_DONE 3u
#define TASK_FLAG_ACTIVE 1u
#define TASK_FLAG_CANCEL 2u

/* Task management mails (SDK dsp.h). */
#define DSP_MAIL_INIT 0xDCD10000u
#define DSP_MAIL_RESUME 0xDCD10001u
#define DSP_MAIL_YIELD 0xDCD10002u
#define DSP_MAIL_DONE 0xDCD10003u
#define DSP_MAIL_REQUEST 0xDCD10004u
#define CPU_MAIL_NEW_UCODE 0xCDD10001u
#define CPU_MAIL_RESET 0xCDD10002u
#define CPU_MAIL_CONTINUE 0xCDD10003u

/* Stack frames of the SDK handlers (stwu r1, -size; temporary context at sp + 0x10; r31 at size - 4). */
#define AID_HANDLER_FRAME 0x2E0u
#define DSP_HANDLER_FRAME 0x2E8u
#define STACK_SWITCH_FRAME 0x18u

/* Events: microcode update (1 ms), audio DMA block, deferred DSP interrupt, first DMA interrupt after the
 * activation. */
enum { EV_UPDATE = 0, EV_BLOCK, EV_DSP_INT, EV_AID_START, EV_KINDS };

#define MAX_EVENTS 32u
#define MAIL_QUEUE 64u

typedef struct {
    int kind;
    u64 when;  /* nominal cycles (ticks × 12) */
    u64 order; /* tie-break between equal deadlines: scheduling order */
} AudioEvent;

typedef struct {
    u32 mail;
    bool interrupt_after;
} QueuedMail;

static struct {
    bool installed;
    GcrtCpu* g;
    u32 sda;
    unsigned replaced;
    char missing[1024];

    /* Platform hookups. */
    GcrtAudioSink sink;
    void* sink_user;
    GcrtDspBackend backend;
    bool have_backend;
    const char* last_ucode; /* name of the microcode when unplugged (final report) */

    /* Events (timed like the former scheduler). */
    AudioEvent events[MAX_EVENTS];
    unsigned event_count;
    u64 now;      /* cycles: time of the last processing of events */
    u64 order;
    bool running;
    u64 deadline; /* ticks: deadline of the native timer, UINT64_MAX if none */
    u32 timer_id;

    /* AI: variables of ai.c (guest addresses). */
    u32 v_ais_callback, v_aid_callback, v_callback_stack, v_old_stack, v_ai_init_flag, v_aid_active, v_ai_version;
    s16 ai_init_stores[14]; /* r13 displacements of the 14 stw of AIInit, in order */
    bool ai_init_stores_ok;
    u32 fn_aid_handler, fn_ais_handler; /* guest handlers registered by AIInit (never called) */
    /* AI: state that the registers held. */
    u32 aicr, aivr, aiit;
    u32 dma_start;   /* AID_MADDR: 0x03FFFFE0 */
    u16 dma_control; /* AID_LEN: bit 15 active, bits 0-14 32-byte blocks */
    u32 dma_current;
    u32 dma_remaining;
    bool aid_pending; /* DSPCSR.AIDINT raised, not acknowledged yet */

    /* DSP: variables of dsp.c and dsp_task.c. */
    u32 v_dsp_init_flag, v_dsp_version, v_tmp_task, v_curr_task, v_last_task, v_first_task, v_rude_task, v_rude_pending;
    u32 fn_dsp_handler;
    /* SDK debug messages (__DSP_debug_printf, same code as OSReport: printed by its replacement): constant format
     * and arguments read in the code of DSPInit and __DSP_boot_task. */
    u32 fn_debug_printf;
    u32 init_msg[3];
    bool init_msg_ok;
    u32 boot_msg[6];
    bool boot_msg_ok;
    u32 fn_register_version, fn_set_handler;
    /* DSP: state that the registers held. */
    bool halted;
    u32 cpu_mailbox;
    QueuedMail mails[MAIL_QUEUE];
    unsigned mail_head, mail_count;
    u32 last_mail;
    bool dsp_pending; /* DSPCSR.DSPINT raised, not acknowledged yet */

    /* Report. */
    u64 blocks, silent_blocks, dma_cycles, aid_raised, aid_delivered, aid_reentries, aid_no_callback, dma_starts;
    u64 mails_to_dsp, mails_to_cpu, mails_dropped, dsp_raised, dsp_delivered, dsp_empty, tasks_added, boots;
    u64 task_switches, asserts, cancels, rate_changes, stack_switches;
    u64 dsp_by_mail[6];
} s_au;

static u32 R32(u32 a) { return mem_read32(&s_au.g->state, a); }
static void W32(u32 a, u32 v) { mem_write32(&s_au.g->state, a, v); }
static u16 R16(u32 a) { return mem_read16(&s_au.g->state, a); }
static u32 arg(GcrtCpu* g, unsigned i) { return g->state.gpr[3 + i]; }

/* ---------------------------------------------------------------------------------------------
 * Audio events: same instants as the scheduler of the reference. A single native timer, at the nearest deadline; when
 * it fires, "now" = ticks × 12, then all the due events in order (deadline, scheduling order). Scheduling outside
 * the processing starts from the last "now" (like the former scheduler, which only advanced at its deadlines).
 * ------------------------------------------------------------------------------------------- */

static void on_timer(GcrtCpu* g, void* user);

static u64 earliest_event(void) {
    u64 best = UINT64_MAX;
    for (unsigned i = 0; i < s_au.event_count; i++)
        if (s_au.events[i].when < best)
            best = s_au.events[i].when;
    return best;
}

static void arm_timer(GcrtCpu* g, u64 deadline) {
    if (s_au.timer_id)
        gcrt_ntimer_cancel(g, s_au.timer_id);
    s_au.timer_id = 0;
    s_au.deadline = deadline;
    if (deadline != UINT64_MAX)
        s_au.timer_id = gcrt_ntimer_set(g, deadline, on_timer, NULL);
}

static void schedule(GcrtCpu* g, int kind, s64 delay_cycles) {
    if (s_au.event_count >= MAX_EVENTS) {
        fprintf(stderr, "gcrt native audio: too many pending events (%u)\n", MAX_EVENTS);
        return;
    }
    const u64 previous = earliest_event();
    const s64 when = (s64)s_au.now + delay_cycles;
    AudioEvent* e = &s_au.events[s_au.event_count++];
    e->kind = kind;
    e->when = when > 0 ? (u64)when : 0u;
    e->order = s_au.order++;
    if (!s_au.running && e->when < previous) {
        /* New nearest event: the timer is brought forward if needed (never pushed back). */
        const u64 ticks = (e->when + GCRT_CYCLES_PER_TB_TICK - 1u) / GCRT_CYCLES_PER_TB_TICK;
        if (ticks < s_au.deadline)
            arm_timer(g, ticks);
    }
}

static void cancel(int kind) {
    for (unsigned i = 0; i < s_au.event_count;) {
        if (s_au.events[i].kind == kind)
            s_au.events[i] = s_au.events[--s_au.event_count];
        else
            i++;
    }
}

static void run_event(GcrtCpu* g, int kind, s64 late);

static void on_timer(GcrtCpu* g, void* user) {
    (void)user;
    s_au.timer_id = 0;
    const u64 now = g->ticks * GCRT_CYCLES_PER_TB_TICK;
    if (now > s_au.now)
        s_au.now = now;
    s_au.running = true;
    for (;;) {
        int best = -1;
        for (unsigned i = 0; i < s_au.event_count; i++) {
            const AudioEvent* e = &s_au.events[i];
            if (e->when > s_au.now)
                continue;
            if (best < 0 || e->when < s_au.events[best].when ||
                (e->when == s_au.events[best].when && e->order < s_au.events[best].order))
                best = (int)i;
        }
        if (best < 0)
            break;
        const AudioEvent e = s_au.events[best];
        s_au.events[best] = s_au.events[--s_au.event_count];
        run_event(g, e.kind, (s64)(s_au.now - e.when));
    }
    s_au.running = false;
    const u64 next = earliest_event();
    arm_timer(g, next == UINT64_MAX ? UINT64_MAX : (next + GCRT_CYCLES_PER_TB_TICK - 1u) / GCRT_CYCLES_PER_TB_TICK);
}

/* ---------------------------------------------------------------------------------------------
 * Interrupts #5 (DSP_AI) and #7 (DSP_DSP): level-triggered (like DSPCSR), delivered on the game thread
 * ------------------------------------------------------------------------------------------- */

static void on_aid_interrupt(GcrtCpu* g, unsigned interrupt, u32 context, void* user);
static void on_dsp_interrupt(GcrtCpu* g, unsigned interrupt, u32 context, void* user);

static void raise_aid(GcrtCpu* g) {
    s_au.aid_raised++;
    if (s_au.aid_pending)
        return; /* already raised, not acknowledged yet: a single delivery (level) */
    s_au.aid_pending = true;
    gcrt_nirq_post_ex(g, GCRT_OS_IRQ_DSP_AI, g->ticks, on_aid_interrupt, NULL, GCRT_NIRQ_FIRST);
}

static void raise_dsp(GcrtCpu* g) {
    s_au.dsp_raised++;
    if (s_au.dsp_pending)
        return;
    s_au.dsp_pending = true;
    gcrt_nirq_post_ex(g, GCRT_OS_IRQ_DSP_DSP, g->ticks, on_dsp_interrupt, NULL, GCRT_NIRQ_FIRST);
}

/* ---------------------------------------------------------------------------------------------
 * Audio DMA
 * ------------------------------------------------------------------------------------------- */

static u32 dma_divisor(void) { return (s_au.aicr & AICR_AIDFR) ? AI_DIVISOR_32K : AI_DIVISOR_48K; }

static void audio_block(GcrtCpu* g, s64 late) {
    const u32 divisor = dma_divisor();
    schedule(g, EV_BLOCK, (s64)AI_BLOCK_CYCLES(divisor) - late);
    static const u8 silence[32];
    const u32 rate = 108000000u / divisor;
    if (!(s_au.dma_control & 0x8000u)) {
        /* DMA stopped: one silent block per period (like Dolphin), so that the duration of the output follows the
         * emulated time. */
        s_au.silent_blocks++;
        if (s_au.sink)
            s_au.sink(s_au.sink_user, silence, rate);
        return;
    }
    const CPUState* c = &g->state;
    const u32 mem1 = c->ram_size < GCRT_MEM1_SIZE ? c->ram_size : GCRT_MEM1_SIZE;
    const u32 a = s_au.dma_current & 0x03FFFFE0u;
    if (s_au.sink) {
        const bool in_ram = a < mem1 && mem1 - a >= 32u;
        s_au.sink(s_au.sink_user, in_ram ? c->ram + a : silence, rate);
    }
    s_au.blocks++;
    if (s_au.dma_remaining != 0) {
        s_au.dma_remaining--;
        s_au.dma_current += 32u;
    }
    if (s_au.dma_remaining == 0) {
        s_au.dma_current = s_au.dma_start;
        s_au.dma_remaining = s_au.dma_control & 0x7FFFu;
        s_au.dma_cycles++;
        GCRT_TIMELINE("ai aid raised schednow=%llu", (unsigned long long)s_au.now);
        GCRT_TRACE(GCRT_TRACE_AI, "audio DMA: buffer 0x%08X read (%u blocks), DMA interrupt", s_au.dma_start,
                   s_au.dma_control & 0x7FFFu);
        raise_aid(g);
    }
}

/* Write of the length and of the enable bit (AID_LEN register): like the hardware, the activation reloads the
 * address and the length and announces the first interrupt 200 cycles later. */
static void write_dma_control(GcrtCpu* g, u16 value) {
    const bool was_enabled = (s_au.dma_control & 0x8000u) != 0;
    s_au.dma_control = value;
    if (!was_enabled && (value & 0x8000u)) {
        s_au.dma_current = s_au.dma_start;
        s_au.dma_remaining = value & 0x7FFFu;
        s_au.dma_starts++;
        cancel(EV_AID_START);
        schedule(g, EV_AID_START, AID_START_DELAY_CYCLES);
        GCRT_TIMELINE("ai dma enabled schednow=%llu", (unsigned long long)s_au.now);
        GCRT_TRACE(GCRT_TRACE_AI, "audio DMA enabled: 0x%08X, %u blocks", s_au.dma_start, value & 0x7FFFu);
    }
}

static void run_event(GcrtCpu* g, int kind, s64 late) {
    switch (kind) {
    case EV_UPDATE:
        GCRT_TIMELINE("dsp update schednow=%llu", (unsigned long long)s_au.now);
        schedule(g, EV_UPDATE, (s64)DSP_UPDATE_CYCLES - late);
        if (s_au.have_backend && s_au.backend.update)
            s_au.backend.update(s_au.backend.user);
        break;
    case EV_BLOCK:
        audio_block(g, late);
        break;
    case EV_DSP_INT:
        GCRT_TIMELINE("dsp int raised schednow=%llu", (unsigned long long)s_au.now);
        raise_dsp(g);
        break;
    case EV_AID_START:
        GCRT_TIMELINE("ai aid start raised schednow=%llu", (unsigned long long)s_au.now);
        raise_aid(g);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------------------------
 * DSP -> CPU mailbox (host side of the former MailHandler)
 * ------------------------------------------------------------------------------------------- */

static QueuedMail* mail_front(void) { return s_au.mail_count ? &s_au.mails[s_au.mail_head] : NULL; }

void gcrt_native_dsp_push_mail(u32 mail, bool interrupt, u32 delay_cycles) {
    if (!s_au.installed)
        return;
    GcrtCpu* g = s_au.g;
    if (interrupt) {
        if (s_au.mail_count == 0)
            schedule(g, EV_DSP_INT, (s64)delay_cycles);
        else
            mail_front()->interrupt_after = true;
    }
    if (s_au.mail_count >= MAIL_QUEUE) {
        s_au.mails_dropped++;
        fprintf(stderr, "gcrt native audio: DSP mail queue full, mail %08X lost\n", mail);
        return;
    }
    s_au.mails[(s_au.mail_head + s_au.mail_count) % MAIL_QUEUE] = (QueuedMail){mail, false};
    s_au.mail_count++;
    s_au.mails_to_cpu++;
    GCRT_TIMELINE("dsp mail->cpu %08X int=%d delay=%u schednow=%llu", mail, interrupt ? 1 : 0, delay_cycles,
                  (unsigned long long)s_au.now);
    GCRT_TRACE(GCRT_TRACE_DSP, "DSP -> CPU mail %08X%s", mail, interrupt ? " (interrupt)" : "");
}

void gcrt_native_dsp_clear_mails(void) {
    s_au.mail_head = 0;
    s_au.mail_count = 0;
}

/* Read of the high half (DSP_MAILBOX_OUT_HI): bit 15 = mail present. */
static u32 mail_read_hi(void) {
    if (!s_au.halted && s_au.mail_count)
        s_au.last_mail = mail_front()->mail;
    return s_au.last_mail >> 16;
}

/* Read of the low half: consumes the mail (and raises the next interrupt if it announced one). */
static u32 mail_read_lo(GcrtCpu* g) {
    if (!s_au.halted && s_au.mail_count) {
        const QueuedMail m = *mail_front();
        s_au.mail_head = (s_au.mail_head + 1u) % MAIL_QUEUE;
        s_au.mail_count--;
        s_au.last_mail = m.mail;
        if (m.interrupt_after)
            schedule(g, EV_DSP_INT, 0);
    }
    const u32 v = s_au.last_mail & 0xFFFFu;
    s_au.last_mail &= 0x7FFFFFFFu;
    return v;
}

static u32 read_mail_from_dsp(GcrtCpu* g) {
    const u32 hi = mail_read_hi();
    return (hi << 16) | mail_read_lo(g);
}

static bool mail_from_dsp_present(void) { return ((mail_read_hi() >> 15) & 1u) != 0; }

/* DSPSendMailToDSP: handed over to the HLE microcode (processed immediately, like the reference), then "taken" (bit 31
 * cleared). */
static void send_mail_to_dsp(GcrtCpu* g, u32 mail) {
    (void)g;
    s_au.cpu_mailbox = mail;
    s_au.mails_to_dsp++;
    GCRT_TIMELINE("dsp mail->dsp %08X schednow=%llu", mail, (unsigned long long)s_au.now);
    GCRT_TRACE(GCRT_TRACE_DSP, "CPU -> DSP mail %08X (%s)", mail,
               s_au.have_backend && s_au.backend.name ? s_au.backend.name(s_au.backend.user) : "no microcode");
    if (s_au.have_backend && s_au.backend.mail)
        s_au.backend.mail(s_au.backend.user, mail);
    s_au.cpu_mailbox &= 0x7FFFFFFFu;
}

/* ---------------------------------------------------------------------------------------------
 * Stack frame and temporary context of the SDK handlers
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    u32 sp;
    u32 tmp;
} HandlerFrame;

static HandlerFrame handler_begin(GcrtCpu* g, u32 size) {
    CPUState* c = &g->state;
    HandlerFrame f;
    f.sp = c->gpr[1];
    const u32 frame = f.sp - size;
    W32(frame, f.sp);                  /* stwu r1, -size(r1): back chain */
    W32(frame + size - 4u, c->gpr[31]); /* stw r31 (saved by the original) */
    c->gpr[1] = frame;
    f.tmp = frame + 0x10u;
    gcrt_os_clear_context(g, f.tmp);
    gcrt_os_set_current_context(g, f.tmp);
    return f;
}

static void handler_end(GcrtCpu* g, const HandlerFrame* f, u32 context) {
    gcrt_os_clear_context(g, f->tmp);
    gcrt_os_set_current_context(g, context);
    g->state.gpr[1] = f->sp;
}

static void call_guest(GcrtCpu* g, u32 function, const u32* args, unsigned count) {
    if (function)
        gcrt_native_callback(g, function, args, count);
}

/* ---------------------------------------------------------------------------------------------
 * __AIDHandler (interrupt #5)
 * ------------------------------------------------------------------------------------------- */

static void on_aid_interrupt(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    (void)interrupt;
    (void)user;
    s_au.aid_pending = false; /* DSPCSR = (DSPCSR & ~0xA0) | AIDINT: acknowledgement */
    s_au.aid_delivered++;
    HandlerFrame f = handler_begin(g, AID_HANDLER_FRAME);
    const u32 cb = R32(s_au.v_aid_callback);
    if (!cb) {
        s_au.aid_no_callback++;
    } else if (R32(s_au.v_aid_active)) {
        s_au.aid_reentries++; /* callback already in progress (the callback re-enabled the interrupts): nothing */
    } else {
        W32(s_au.v_aid_active, 1);
        const u32 stack = R32(s_au.v_callback_stack);
        if (stack) {
            /* __AICallbackStackSwitch: frame, current stack stored in __OldStack, callback on the AIInit stack. */
            CPUState* c = &g->state;
            const u32 sp = c->gpr[1];
            const u32 frame = sp - STACK_SWITCH_FRAME;
            W32(frame, sp);
            W32(frame + STACK_SWITCH_FRAME - 4u, c->gpr[31]);
            if (s_au.v_old_stack)
                W32(s_au.v_old_stack, frame);
            c->gpr[1] = stack - 8u;
            s_au.stack_switches++;
            call_guest(g, cb, NULL, 0);
            c->gpr[1] = sp;
        } else {
            call_guest(g, cb, NULL, 0);
        }
        W32(s_au.v_aid_active, 0);
    }
    handler_end(g, &f, context);
}

/* ---------------------------------------------------------------------------------------------
 * DSP tasks (dsp_task.c) and __DSPHandler (interrupt #7)
 * ------------------------------------------------------------------------------------------- */

/* __DSP_exec_task(curr, next): save of the DRAM of the current task, load of the next one. */
static void exec_task(GcrtCpu* g, u32 curr, u32 next) {
    if (curr) {
        send_mail_to_dsp(g, R32(curr + TASK_DRAM_MMEM));
        send_mail_to_dsp(g, R32(curr + TASK_DRAM_LENGTH));
        send_mail_to_dsp(g, R32(curr + TASK_DRAM_ADDR));
    } else {
        send_mail_to_dsp(g, 0);
        send_mail_to_dsp(g, 0);
        send_mail_to_dsp(g, 0);
    }
    send_mail_to_dsp(g, R32(next + TASK_IRAM_MMEM));
    send_mail_to_dsp(g, R32(next + TASK_IRAM_LENGTH));
    send_mail_to_dsp(g, R32(next + TASK_IRAM_ADDR));
    if (R32(next + TASK_STATE) == TASK_STATE_INIT) {
        send_mail_to_dsp(g, R16(next + TASK_INIT_VECTOR));
        send_mail_to_dsp(g, 0);
        send_mail_to_dsp(g, 0);
        send_mail_to_dsp(g, 0);
    } else {
        send_mail_to_dsp(g, R16(next + TASK_RESUME_VECTOR));
        send_mail_to_dsp(g, R32(next + TASK_DRAM_MMEM));
        send_mail_to_dsp(g, R32(next + TASK_DRAM_LENGTH));
        send_mail_to_dsp(g, R32(next + TASK_DRAM_ADDR));
    }
    s_au.task_switches++;
}

/* __DSP_insert_task: list sorted by priority (lowest value first, after the others on ties). */
static void insert_task(u32 task) {
    const u32 first = R32(s_au.v_first_task);
    if (!first) {
        W32(s_au.v_curr_task, task);
        W32(s_au.v_last_task, task);
        W32(s_au.v_first_task, task);
        W32(task + TASK_PREV, 0);
        W32(task + TASK_NEXT, 0);
        return;
    }
    u32 temp = first;
    while (temp) {
        if (R32(task + TASK_PRIORITY) < R32(temp + TASK_PRIORITY)) {
            W32(task + TASK_PREV, R32(temp + TASK_PREV));
            W32(temp + TASK_PREV, task);
            W32(task + TASK_NEXT, temp);
            const u32 prev = R32(task + TASK_PREV);
            if (!prev)
                W32(s_au.v_first_task, task);
            else
                W32(prev + TASK_NEXT, task);
            return;
        }
        temp = R32(temp + TASK_NEXT);
    }
    const u32 last = R32(s_au.v_last_task);
    W32(last + TASK_NEXT, task);
    W32(task + TASK_NEXT, 0);
    W32(task + TASK_PREV, R32(s_au.v_last_task));
    W32(s_au.v_last_task, task);
}

/* __DSP_remove_task */
static void remove_task(u32 task) {
    W32(task + TASK_FLAGS, 0);
    W32(task + TASK_STATE, TASK_STATE_DONE);
    if (R32(s_au.v_first_task) == task) {
        const u32 next = R32(task + TASK_NEXT);
        if (next) {
            W32(s_au.v_first_task, next);
            W32(next + TASK_PREV, 0);
        } else {
            W32(s_au.v_curr_task, 0);
            W32(s_au.v_last_task, 0);
            W32(s_au.v_first_task, 0);
        }
    } else if (R32(s_au.v_last_task) == task) {
        const u32 prev = R32(task + TASK_PREV);
        W32(s_au.v_last_task, prev);
        W32(prev + TASK_NEXT, 0);
        W32(s_au.v_curr_task, R32(s_au.v_first_task));
    } else {
        const u32 next = R32(task + TASK_NEXT), prev = R32(task + TASK_PREV);
        W32(s_au.v_curr_task, next);
        W32(prev + TASK_NEXT, next);
        W32(next + TASK_PREV, prev);
    }
}

/* __DSP_boot_task: the ROM waits for its description; here, fingerprint of the IRAM code given to the HLE microcode. */
static void boot_task(GcrtCpu* g, u32 task) {
    /* The ROM announces that it is ready (0x8071FEED): mail consumed as by the original. */
    if (mail_from_dsp_present())
        read_mail_from_dsp(g);
    s_au.boots++;
    const u32 iram = R32(task + TASK_IRAM_MMEM), length = R32(task + TASK_IRAM_LENGTH);
    const u32 vector = R16(task + TASK_INIT_VECTOR);
    if (s_au.boot_msg_ok) {
        /* Messages of the original (after the mails), same values. */
        const u32 values[6] = {task, iram, R32(task + TASK_IRAM_ADDR), length, R32(task + TASK_DRAM_LENGTH), vector};
        for (unsigned i = 0; i < 6u; i++) {
            const u32 a[2] = {s_au.boot_msg[i], values[i]};
            call_guest(g, s_au.fn_debug_printf, a, 2);
        }
    }
    GCRT_TIMELINE("dsp task start %08X iram %08X length %u", task, iram, length);
    if (s_au.have_backend && s_au.backend.boot) {
        const u32 crc = s_au.backend.boot(s_au.backend.user, iram, length, vector);
        GCRT_TRACE(GCRT_TRACE_DSP, "DSP task %08X started: IRAM %08X (%u bytes), entry %04X, fingerprint %08X (%s)",
                   task, iram, length, vector, crc, s_au.backend.name ? s_au.backend.name(s_au.backend.user) : "?");
    } else {
        fprintf(stderr, "gcrt native audio: no HLE microcode plugged in, DSP task %08X does not start\n", task);
    }
}

static void count_mail(u32 mail) {
    if ((mail & 0xFFFF0000u) == 0xDCD10000u && (mail & 0xFFFFu) < 5u)
        s_au.dsp_by_mail[mail & 0xFFFFu]++;
    else
        s_au.dsp_by_mail[5]++;
}

static void on_dsp_interrupt(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    (void)interrupt;
    (void)user;
    s_au.dsp_pending = false; /* DSPCSR = (DSPCSR & ~0x28) | DSPINT: acknowledgement */
    s_au.dsp_delivered++;
    HandlerFrame f = handler_begin(g, DSP_HANDLER_FRAME);
    if (!mail_from_dsp_present()) {
        /* The original would wait for a mail forever; with the HLE microcode, a DSP interrupt always comes with its
         * mail. [hypothesis]: never observed. */
        s_au.dsp_empty++;
        GCRT_TRACE(GCRT_TRACE_UNHANDLED, "DSP interrupt without mail: ignored");
        handler_end(g, &f, context);
        return;
    }
    u32 mail = read_mail_from_dsp(g);
    count_mail(mail);
    u32 curr = R32(s_au.v_curr_task);
    if (curr && (R32(curr + TASK_FLAGS) & TASK_FLAG_CANCEL) && mail == DSP_MAIL_YIELD)
        mail = DSP_MAIL_DONE;
    const u32 rude = R32(s_au.v_rude_task);
    const bool rude_pending = R32(s_au.v_rude_pending) != 0;
    switch (mail) {
    case DSP_MAIL_INIT:
        W32(curr + TASK_STATE, TASK_STATE_RUN);
        call_guest(g, R32(curr + TASK_INIT_CB), &curr, 1);
        break;
    case DSP_MAIL_RESUME:
        W32(curr + TASK_STATE, TASK_STATE_RUN);
        call_guest(g, R32(curr + TASK_RES_CB), &curr, 1);
        break;
    case DSP_MAIL_YIELD:
        if (rude_pending) {
            if (curr == rude) {
                send_mail_to_dsp(g, CPU_MAIL_CONTINUE);
                W32(s_au.v_rude_task, 0);
                W32(s_au.v_rude_pending, 0);
                call_guest(g, R32(curr + TASK_RES_CB), &curr, 1);
            } else {
                send_mail_to_dsp(g, CPU_MAIL_NEW_UCODE);
                exec_task(g, curr, rude);
                W32(curr + TASK_STATE, TASK_STATE_YIELD);
                W32(s_au.v_rude_pending, 0);
                W32(s_au.v_curr_task, rude);
                W32(s_au.v_rude_task, 0);
            }
        } else if (!R32(curr + TASK_NEXT)) {
            const u32 first = R32(s_au.v_first_task);
            if (curr == first) {
                send_mail_to_dsp(g, CPU_MAIL_CONTINUE);
                call_guest(g, R32(curr + TASK_RES_CB), &curr, 1);
            } else {
                send_mail_to_dsp(g, CPU_MAIL_NEW_UCODE);
                exec_task(g, curr, first);
                W32(curr + TASK_STATE, TASK_STATE_YIELD);
                W32(s_au.v_curr_task, first);
            }
        } else {
            const u32 next = R32(curr + TASK_NEXT);
            send_mail_to_dsp(g, CPU_MAIL_NEW_UCODE);
            exec_task(g, curr, next);
            W32(curr + TASK_STATE, TASK_STATE_YIELD);
            W32(s_au.v_curr_task, next);
        }
        break;
    case DSP_MAIL_DONE:
        if (rude_pending) {
            call_guest(g, R32(curr + TASK_DONE_CB), &curr, 1);
            send_mail_to_dsp(g, CPU_MAIL_NEW_UCODE);
            exec_task(g, 0, rude);
            remove_task(R32(s_au.v_curr_task));
            W32(s_au.v_rude_pending, 0);
            W32(s_au.v_curr_task, rude);
            W32(s_au.v_rude_task, 0);
        } else if (!R32(curr + TASK_NEXT)) {
            const u32 first = R32(s_au.v_first_task);
            if (curr == first) {
                call_guest(g, R32(curr + TASK_DONE_CB), &curr, 1);
                send_mail_to_dsp(g, CPU_MAIL_RESET);
                W32(R32(s_au.v_curr_task) + TASK_STATE, TASK_STATE_DONE);
                remove_task(R32(s_au.v_curr_task));
            } else {
                call_guest(g, R32(curr + TASK_DONE_CB), &curr, 1);
                send_mail_to_dsp(g, CPU_MAIL_NEW_UCODE);
                W32(R32(s_au.v_curr_task) + TASK_STATE, TASK_STATE_DONE);
                exec_task(g, 0, R32(s_au.v_first_task));
                W32(s_au.v_curr_task, R32(s_au.v_first_task));
                remove_task(R32(s_au.v_last_task));
            }
        } else {
            call_guest(g, R32(curr + TASK_DONE_CB), &curr, 1);
            send_mail_to_dsp(g, CPU_MAIL_NEW_UCODE);
            W32(R32(s_au.v_curr_task) + TASK_STATE, TASK_STATE_DONE);
            exec_task(g, 0, R32(R32(s_au.v_curr_task) + TASK_NEXT));
            W32(s_au.v_curr_task, R32(R32(s_au.v_curr_task) + TASK_NEXT));
            remove_task(R32(R32(s_au.v_curr_task) + TASK_PREV));
        }
        break;
    case DSP_MAIL_REQUEST:
        call_guest(g, R32(curr + TASK_REQ_CB), &curr, 1);
        break;
    default:
        GCRT_TRACE(GCRT_TRACE_DSP, "DSP interrupt: mail %08X not handled (task %08X)", mail, curr);
        break;
    }
    handler_end(g, &f, context);
}

/* ---------------------------------------------------------------------------------------------
 * Replaced AI functions (ai.c)
 * ------------------------------------------------------------------------------------------- */

static u32 vol_left(void) { return s_au.aivr & 0xFFu; }
static u32 vol_right(void) { return (s_au.aivr >> 8) & 0xFFu; }
static void set_vol_left(u32 v) { s_au.aivr = (s_au.aivr & ~0xFFu) | (v & 0xFFu); }
static void set_vol_right(u32 v) { s_au.aivr = (s_au.aivr & ~0xFF00u) | ((v & 0xFFu) << 8); }

/* Write of AICR: SCRESET resets the stream counter (no stream here), AIINT is acknowledged by writing 1. */
static void write_aicr(u32 value) {
    const u32 before = s_au.aicr;
    u32 next = (s_au.aicr & AICR_AIINT) | (value & AICR_KEPT);
    if (value & AICR_AIINT)
        next &= ~AICR_AIINT;
    s_au.aicr = next;
    if ((before ^ next) & AICR_AIDFR) {
        s_au.rate_changes++;
        GCRT_TRACE(GCRT_TRACE_AI, "audio DMA at %s kHz", (next & AICR_AIDFR) ? "32" : "48");
    }
}

static bool hle_ai_register_dma_callback(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 old = R32(s_au.v_aid_callback);
    W32(s_au.v_aid_callback, arg(g, 0));
    GCRT_TRACE(GCRT_TRACE_AI, "AIRegisterDMACallback(%08X) -> %08X (lr %08X)", arg(g, 0), old, g->state.lr);
    gcrt_native_return(g, old);
    return true;
}

static bool hle_ai_init_dma(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 addr = arg(g, 0), length = arg(g, 1);
    /* AID_MADDR_H/L then AID_LEN, like the registers (10 high bits, 16 low bits that are multiples of 32). */
    s_au.dma_start = (((addr >> 16) & 0x3FFu) << 16) | (addr & 0xFFE0u);
    write_dma_control(g, (u16)((s_au.dma_control & 0x8000u) | ((length >> 5) & 0xFFFFu)));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_start_dma(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    write_dma_control(g, (u16)(s_au.dma_control | 0x8000u));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_stop_dma(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    write_dma_control(g, (u16)(s_au.dma_control & ~0x8000u));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_get_dma_start_addr(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, s_au.dma_start);
    return true;
}

static void ai_set_stream_play_state(u32 state) {
    if (state == (s_au.aicr & AICR_PSTAT))
        return;
    if (((s_au.aicr >> 1) & 1u) == 0 && state == 1u) {
        const u32 right = vol_right(), left = vol_left();
        set_vol_right(0);
        set_vol_left(0);
        /* __AI_SRC_INIT (synchronization of the converter on the stream counter): not applicable, no stream. */
        write_aicr((s_au.aicr & ~AICR_SCRESET) | AICR_SCRESET);
        write_aicr((s_au.aicr & ~AICR_PSTAT) | AICR_PSTAT);
        set_vol_left(right); /* crossed volumes: like the SDK of this version */
        set_vol_right(left);
    } else {
        write_aicr((s_au.aicr & ~AICR_PSTAT) | (state & 1u));
    }
}

static bool hle_ai_set_stream_play_state(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    ai_set_stream_play_state(arg(g, 0));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_get_stream_play_state(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, s_au.aicr & AICR_PSTAT);
    return true;
}

static u32 ai_get_dsp_sample_rate(void) { return ((s_au.aicr >> 6) & 1u) ^ 1u; }

static void ai_set_dsp_sample_rate(u32 rate) {
    if (rate == ai_get_dsp_sample_rate())
        return;
    write_aicr(s_au.aicr & ~AICR_AIDFR);
    if (rate == 0) {
        const u32 left = vol_left(), right = vol_right(), state = s_au.aicr & AICR_PSTAT;
        const u32 stream_rate = (s_au.aicr >> 1) & 1u;
        set_vol_left(0);
        set_vol_right(0);
        write_aicr((s_au.aicr & ~AICR_SCRESET) | AICR_SCRESET);
        write_aicr((s_au.aicr & ~AICR_AISFR) | (stream_rate << 1));
        write_aicr((s_au.aicr & ~AICR_PSTAT) | state);
        write_aicr(s_au.aicr | AICR_AIDFR);
        set_vol_left(left);
        set_vol_right(right);
    }
}

static bool hle_ai_set_dsp_sample_rate(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    ai_set_dsp_sample_rate(arg(g, 0));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_get_dsp_sample_rate(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, ai_get_dsp_sample_rate());
    return true;
}

static void ai_set_stream_sample_rate(u32 rate) {
    if (rate == ((s_au.aicr >> 1) & 1u))
        return;
    const u32 state = s_au.aicr & AICR_PSTAT, left = vol_left(), right = vol_right();
    set_vol_right(0);
    set_vol_left(0);
    const u32 dfr = s_au.aicr & AICR_AIDFR;
    write_aicr(s_au.aicr & ~AICR_AIDFR);
    write_aicr(s_au.aicr | dfr);
    write_aicr((s_au.aicr & ~AICR_SCRESET) | AICR_SCRESET);
    write_aicr((s_au.aicr & ~AICR_AISFR) | ((rate & 1u) << 1));
    ai_set_stream_play_state(state);
    set_vol_left(left);
    set_vol_right(right);
}

static bool hle_ai_set_stream_sample_rate(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    ai_set_stream_sample_rate(arg(g, 0));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_get_stream_sample_rate(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, (s_au.aicr >> 1) & 1u);
    return true;
}

static bool hle_ai_set_stream_vol_left(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    set_vol_left(arg(g, 0));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_get_stream_vol_left(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, vol_left());
    return true;
}

static bool hle_ai_set_stream_vol_right(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    set_vol_right(arg(g, 0));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_ai_get_stream_vol_right(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, vol_right());
    return true;
}

/* OSNanosecondsToTicks of the SDK, with the 32-bit arithmetic of the AIInit code (bus clock at 0x800000F8). */
static u32 ns_to_ticks(GcrtCpu* g, u32 ns) {
    const u32 timer = R32(0x800000F8u) >> 2;
    const u32 per_8us = (u32)(((u64)0x431BDE83u * timer) >> 32) >> 15;
    const u32 product = per_8us * ns;
    (void)g;
    return (u32)(((u64)0x10624DD3u * product) >> 32) >> 9;
}

static bool hle_ai_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 stack = arg(g, 0);
    if (R32(s_au.v_ai_init_flag) == 1u) {
        gcrt_native_return(g, 0);
        return true;
    }
    if (s_au.v_ai_version && s_au.fn_register_version) {
        const u32 banner = R32(s_au.v_ai_version);
        call_guest(g, s_au.fn_register_version, &banner, 1);
    }
    /* Durations of __AI_SRC_INIT (bound_32KHz, bound_48KHz, min_wait, buffer, max_wait), stored like the original
     * (no live code reads them: __AI_SRC_INIT is no longer called). Values in ns from AIInit (ai.c of the reference
     * decompilation, CC0, and immediates of the recognized code). */
    if (s_au.ai_init_stores_ok) {
        static const u32 ns[5] = {31524u, 42024u, 42000u, 3000u, 63000u};
        for (unsigned i = 0; i < 5u; i++)
            W32(s_au.sda + (u32)(s32)s_au.ai_init_stores[i], ns_to_ticks(g, ns[i]));
        for (unsigned i = 5; i < 10u; i++)
            W32(s_au.sda + (u32)(s32)s_au.ai_init_stores[i], 0);
    }
    write_aicr((s_au.aicr & ~AICR_SCRESET) | AICR_SCRESET); /* stream counter reset to zero */
    s_au.aivr &= ~0xFF00u;                                 /* AISetStreamVolRight(0) */
    s_au.aivr &= ~0xFFu;                                   /* AISetStreamVolLeft(0) */
    s_au.aiit = 0;                                         /* AISetStreamTrigger(0) */
    ai_set_stream_sample_rate(1);
    ai_set_dsp_sample_rate(0);
    W32(s_au.v_ais_callback, 0);
    W32(s_au.v_aid_callback, 0);
    W32(s_au.v_callback_stack, stack);
    if (s_au.fn_set_handler && s_au.fn_aid_handler) {
        const u32 a[2] = {GCRT_OS_IRQ_DSP_AI, s_au.fn_aid_handler};
        call_guest(g, s_au.fn_set_handler, a, 2); /* OS table like the original (never called: native delivery) */
    }
    gcrt_os_unmask_interrupts(g, 0x80000000u >> GCRT_OS_IRQ_DSP_AI);
    if (s_au.fn_set_handler && s_au.fn_ais_handler) {
        const u32 a[2] = {GCRT_OS_IRQ_AI_AI, s_au.fn_ais_handler};
        call_guest(g, s_au.fn_set_handler, a, 2);
    }
    gcrt_os_unmask_interrupts(g, 0x80000000u >> GCRT_OS_IRQ_AI_AI);
    W32(s_au.v_ai_init_flag, 1);
    GCRT_TRACE(GCRT_TRACE_AI, "native AIInit: callback stack %08X, DMA at %s kHz", stack,
               (s_au.aicr & AICR_AIDFR) ? "32" : "48");
    gcrt_native_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Replaced DSP functions (dsp.c, dsp_task.c)
 * ------------------------------------------------------------------------------------------- */

static bool hle_dsp_check_mail_to_dsp(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, (s_au.cpu_mailbox >> 31) & 1u);
    return true;
}

static bool hle_dsp_check_mail_from_dsp(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, mail_from_dsp_present() ? 1u : 0u);
    return true;
}

static bool hle_dsp_read_mail_from_dsp(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, read_mail_from_dsp(g));
    return true;
}

static bool hle_dsp_send_mail_to_dsp(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    send_mail_to_dsp(g, arg(g, 0));
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_dsp_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    if (s_au.init_msg_ok)
        call_guest(g, s_au.fn_debug_printf, s_au.init_msg, 3); /* "DSPInit(): Build Date: ...", on every call */
    if (R32(s_au.v_dsp_init_flag) == 1u) {
        gcrt_native_return(g, 0);
        return true;
    }
    if (s_au.v_dsp_version && s_au.fn_register_version) {
        const u32 banner = R32(s_au.v_dsp_version);
        call_guest(g, s_au.fn_register_version, &banner, 1);
    }
    if (s_au.fn_set_handler && s_au.fn_dsp_handler) {
        const u32 a[2] = {GCRT_OS_IRQ_DSP_DSP, s_au.fn_dsp_handler};
        call_guest(g, s_au.fn_set_handler, a, 2);
    }
    gcrt_os_unmask_interrupts(g, 0x80000000u >> GCRT_OS_IRQ_DSP_DSP);
    s_au.halted = false; /* DSPCSR: HALT cleared, the DSP (ROM) runs */
    W32(s_au.v_tmp_task, 0);
    W32(s_au.v_curr_task, 0);
    W32(s_au.v_last_task, 0);
    W32(s_au.v_first_task, 0);
    W32(s_au.v_dsp_init_flag, 1);
    GCRT_TRACE(GCRT_TRACE_DSP, "native DSPInit");
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_dsp_add_task(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 task = arg(g, 0);
    insert_task(task);
    W32(task + TASK_STATE, TASK_STATE_INIT);
    W32(task + TASK_FLAGS, TASK_FLAG_ACTIVE);
    s_au.tasks_added++;
    if (task == R32(s_au.v_first_task))
        boot_task(g, task);
    gcrt_native_return(g, task);
    return true;
}

static bool hle_dsp_cancel_task(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 task = arg(g, 0);
    W32(task + TASK_FLAGS, R32(task + TASK_FLAGS) | TASK_FLAG_CANCEL);
    s_au.cancels++;
    gcrt_native_return(g, task);
    return true;
}

static bool hle_dsp_assert_task(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 task = arg(g, 0);
    const u32 curr = R32(s_au.v_curr_task);
    s_au.asserts++;
    u32 result = 0;
    if (curr == task) {
        W32(s_au.v_rude_task, task);
        W32(s_au.v_rude_pending, 1);
        result = task;
    } else if (R32(task + TASK_PRIORITY) < R32(curr + TASK_PRIORITY)) {
        W32(s_au.v_rude_task, task);
        W32(s_au.v_rude_pending, 1);
        /* Current task running: the original interrupts the DSP (DSPCSR.PIINT); the HLE microcode has no interruptible
         * state (like the reference and Dolphin), it yields at its next end of list. [hypothesis] */
        result = task;
    }
    gcrt_native_return(g, result);
    return true;
}

/* Dead functions (replaced handlers and internal helpers): a call to one of them reveals a forgotten path. */
static bool hle_dead(GcrtCpu* g, u32 address) {
    fprintf(stderr, "gcrt native audio: dead function called at %08X (lr %08X): ignored\n", address, g->state.lr);
    g->state.gpr[3] = 0;
    g->state.pc = g->state.lr;
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------------------------- */

static u32 unique(const char* name) {
    unsigned n = 0;
    const u32 a = gcrt_hle_find(name, &n);
    return n == 1 ? a : 0;
}

static u32 sda_var(GcrtCpu* g, u32 fn, const char* name, unsigned opcode, unsigned nth) {
    s16 d = 0;
    if (!fn || !gcrt_hle_decode_d_offset(g, fn, gcrt_hle_catalog_size(name), opcode, 13, nth, &d))
        return 0;
    return s_au.sda + (u32)(s32)d;
}

/* Address built by the `nth` "lis rX, H ; addi rX, rX, L" pair of the function (0 if absent). */
static u32 lis_addi(GcrtCpu* g, u32 fn, u32 size, unsigned nth) {
    unsigned seen = 0;
    for (u32 off = 0; fn && off + 8u <= size; off += 4u) {
        const u32 w0 = mem_read32(&g->state, fn + off), w1 = mem_read32(&g->state, fn + off + 4u);
        const u32 rd = (w0 >> 21) & 31u;
        if ((w0 >> 26) == 15u && ((w0 >> 16) & 31u) == 0 && (w1 >> 26) == 14u && ((w1 >> 21) & 31u) == rd &&
            ((w1 >> 16) & 31u) == rd) {
            if (seen++ == nth)
                return ((w0 & 0xFFFFu) << 16) + (u32)(s32)(s16)(w1 & 0xFFFFu);
        }
    }
    return 0;
}

static bool replace_dsp_check_mail(GcrtCpu* g) {
    /* DSPCheckMailToDSP and DSPCheckMailFromDSP have the same masked code: the one that ends at the start of
     * DSPReadMailFromDSP is "From", the one before it is "To" (order of dsp.c). */
    const u32 from = gcrt_hle_resolve_preceding("DSPCheckMailFromDSP", "DSPReadMailFromDSP");
    const u32 size = gcrt_hle_catalog_size("DSPCheckMailToDSP");
    u32 copies[8];
    const unsigned n = gcrt_hle_find_all("DSPCheckMailToDSP", copies, 8);
    u32 to = 0;
    for (unsigned i = 0; i < n && i < 8u; i++)
        if (from && size && copies[i] == from - size)
            to = copies[i];
    bool ok = true;
    if (from)
        ok &= gcrt_hle_register(g, from, "DSPCheckMailFromDSP", hle_dsp_check_mail_from_dsp);
    else
        ok = false;
    if (to)
        ok &= gcrt_hle_register(g, to, "DSPCheckMailToDSP", hle_dsp_check_mail_to_dsp);
    else
        ok = false;
    s_au.replaced += (from ? 1u : 0u) + (to ? 1u : 0u);
    if (!from || !to) {
        size_t used = strlen(s_au.missing);
        snprintf(s_au.missing + used, sizeof(s_au.missing) - used, "%s%s%s", used ? " " : "",
                 from ? "" : "DSPCheckMailFromDSP", to ? "" : " DSPCheckMailToDSP");
    }
    return ok;
}

void gcrt_native_audio_install(GcrtCpu* g) {
    GcrtAudioSink sink = s_au.sink;
    void* sink_user = s_au.sink_user;
    GcrtDspBackend backend = s_au.backend;
    const bool have_backend = s_au.have_backend;
    memset(&s_au, 0, sizeof(s_au));
    s_au.sink = sink;
    s_au.sink_user = sink_user;
    s_au.backend = backend;
    s_au.have_backend = have_backend;
    s_au.g = g;
    s_au.deadline = UINT64_MAX;
    if (!gcrt_native_os_active()) {
        fprintf(stderr, "gcrt native audio: native OS absent, audio not replaced\n");
        return;
    }
    s_au.sda = gcrt_native_os_sda();
    char* m = s_au.missing;
    const size_t ms = sizeof(s_au.missing);
    unsigned* n = &s_au.replaced;

    /* Variables of ai.c, read in the recognized code. */
    const u32 ai_init = unique("AIInit");
    const u32 aid_handler = unique("__AIDHandler");
    const u32 ais_handler = unique("__AISHandler");
    const u32 stack_switch = unique("__AICallbackStackSwitch");
    s_au.v_aid_callback = sda_var(g, aid_handler, "__AIDHandler", 32, 0);
    s_au.v_aid_active = sda_var(g, aid_handler, "__AIDHandler", 32, 1);
    s_au.v_callback_stack = sda_var(g, aid_handler, "__AIDHandler", 32, 2);
    s_au.v_ais_callback = sda_var(g, ais_handler, "__AISHandler", 32, 0);
    s_au.v_ai_init_flag = sda_var(g, ai_init, "AIInit", 32, 0);
    s_au.v_ai_version = sda_var(g, ai_init, "AIInit", 32, 1);
    s_au.v_old_stack = lis_addi(g, stack_switch, gcrt_hle_catalog_size("__AICallbackStackSwitch"), 0);
    s_au.ai_init_stores_ok = ai_init != 0;
    for (unsigned i = 0; i < 14u && s_au.ai_init_stores_ok; i++)
        s_au.ai_init_stores_ok =
            gcrt_hle_decode_d_offset(g, ai_init, gcrt_hle_catalog_size("AIInit"), 36, 13, i, &s_au.ai_init_stores[i]);
    const u32 set_handler = gcrt_native_sdk_function("__OSSetInterruptHandler");
    s_au.fn_set_handler = set_handler;
    s_au.fn_register_version = gcrt_native_sdk_function("OSRegisterVersion");
    if (ai_init && set_handler) {
        const u32 size = gcrt_hle_catalog_size("AIInit");
        gcrt_hle_decode_reg_at_call(g, ai_init, size, set_handler, 0, 4, s_au.sda, 0, &s_au.fn_aid_handler);
        gcrt_hle_decode_reg_at_call(g, ai_init, size, set_handler, 1, 4, s_au.sda, 0, &s_au.fn_ais_handler);
    }
    /* Variables of dsp.c and dsp_task.c. */
    const u32 dsp_init = unique("DSPInit");
    const u32 dsp_assert = unique("DSPAssertTask");
    s_au.v_dsp_init_flag = sda_var(g, dsp_init, "DSPInit", 32, 0);
    s_au.v_dsp_version = sda_var(g, dsp_init, "DSPInit", 32, 1);
    s_au.v_tmp_task = sda_var(g, dsp_init, "DSPInit", 36, 0);
    s_au.v_curr_task = sda_var(g, dsp_init, "DSPInit", 36, 1);
    s_au.v_last_task = sda_var(g, dsp_init, "DSPInit", 36, 2);
    s_au.v_first_task = sda_var(g, dsp_init, "DSPInit", 36, 3);
    s_au.v_rude_task = sda_var(g, dsp_assert, "DSPAssertTask", 36, 0);
    s_au.v_rude_pending = sda_var(g, dsp_assert, "DSPAssertTask", 36, 1);
    if (dsp_init && set_handler)
        gcrt_hle_decode_reg_at_call(g, dsp_init, gcrt_hle_catalog_size("DSPInit"), set_handler, 0, 4, s_au.sda, 0,
                                    &s_au.fn_dsp_handler);
    /* __DSP_debug_printf: copy that ends at the start of __DSPHandler (same code as OSReport and other empty
     * functions); constant formats and arguments of its calls in DSPInit (1) and __DSP_boot_task (6). */
    s_au.fn_debug_printf = gcrt_hle_resolve_preceding("__DSP_debug_printf", "__DSPHandler");
    if (s_au.fn_debug_printf && dsp_init) {
        const u32 size = gcrt_hle_catalog_size("DSPInit");
        s_au.init_msg_ok = true;
        for (unsigned r = 0; r < 3u; r++)
            s_au.init_msg_ok &= gcrt_hle_decode_reg_at_call(g, dsp_init, size, s_au.fn_debug_printf, 0, 3 + r, s_au.sda,
                                                            0, &s_au.init_msg[r]);
    }
    const u32 boot = unique("__DSP_boot_task");
    if (s_au.fn_debug_printf && boot) {
        const u32 size = gcrt_hle_catalog_size("__DSP_boot_task");
        s_au.boot_msg_ok = true;
        for (unsigned i = 0; i < 6u; i++)
            s_au.boot_msg_ok &= gcrt_hle_decode_reg_at_call(g, boot, size, s_au.fn_debug_printf, i, 3, s_au.sda, 0,
                                                            &s_au.boot_msg[i]);
    }
    const bool vars = s_au.v_aid_callback && s_au.v_aid_active && s_au.v_callback_stack && s_au.v_ai_init_flag &&
                      s_au.v_dsp_init_flag && s_au.v_curr_task && s_au.v_first_task && s_au.v_last_task &&
                      s_au.v_rude_task && s_au.v_rude_pending && s_au.v_tmp_task;
    if (!vars) {
        fprintf(stderr, "gcrt native audio: variables of the AI/DSP libraries not found (AIInit %08X, __AIDHandler "
                        "%08X, DSPInit %08X, DSPAssertTask %08X): audio not replaced\n",
                ai_init, aid_handler, dsp_init, dsp_assert);
        return;
    }

    /* Initial state (like the hardware at power-on, Dolphin): DMA at 32 kHz, stream at 48 kHz, DSP halted (HALT)
     * until DSPInit. */
    s_au.aicr = AICR_AISFR | AICR_AIDFR;
    s_au.halted = true;
    s_au.now = 0;
    /* Same scheduling order as the reference: DSP update, then first DMA block. */
    schedule(g, EV_UPDATE, (s64)DSP_UPDATE_CYCLES);
    schedule(g, EV_BLOCK, (s64)AI_BLOCK_CYCLES(dma_divisor()));

    /* AI (ai.c) */
    *n += gcrt_native_replace(g, "AIRegisterDMACallback", "AI", hle_ai_register_dma_callback, NULL, 0, "AIInitDMA", m, ms);
    *n += gcrt_native_replace(g, "AIInitDMA", "AI", hle_ai_init_dma, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIStartDMA", "AI", hle_ai_start_dma, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIStopDMA", "AI", hle_ai_stop_dma, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIGetDMAStartAddr", "AI", hle_ai_get_dma_start_addr, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AISetStreamPlayState", "AI", hle_ai_set_stream_play_state, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIGetStreamPlayState", "AI", hle_ai_get_stream_play_state, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AISetDSPSampleRate", "AI", hle_ai_set_dsp_sample_rate, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIGetDSPSampleRate", "AI", hle_ai_get_dsp_sample_rate, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AISetStreamSampleRate", "AI", hle_ai_set_stream_sample_rate, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIGetStreamSampleRate", "AI", hle_ai_get_stream_sample_rate, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AISetStreamVolLeft", "AI", hle_ai_set_stream_vol_left, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIGetStreamVolLeft", "AI", hle_ai_get_stream_vol_left, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AISetStreamVolRight", "AI", hle_ai_set_stream_vol_right, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIGetStreamVolRight", "AI", hle_ai_get_stream_vol_right, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "AIInit", "AI", hle_ai_init, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__AISHandler", "AI", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__AIDHandler", "AI", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__AICallbackStackSwitch", "AI", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__AI_SRC_INIT", "AI", hle_dead, NULL, 0, NULL, m, ms);

    /* DSP (dsp.c, dsp_task.c) */
    replace_dsp_check_mail(g);
    *n += gcrt_native_replace(g, "DSPReadMailFromDSP", "DSP", hle_dsp_read_mail_from_dsp, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "DSPSendMailToDSP", "DSP", hle_dsp_send_mail_to_dsp, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "DSPInit", "DSP", hle_dsp_init, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "DSPAddTask", "DSP", hle_dsp_add_task, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "DSPCancelTask", "DSP", hle_dsp_cancel_task, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "DSPAssertTask", "DSP", hle_dsp_assert_task, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__DSPHandler", "DSP", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__DSP_exec_task", "DSP", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__DSP_boot_task", "DSP", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__DSP_insert_task", "DSP", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__DSP_remove_task", "DSP", hle_dead, NULL, 0, NULL, m, ms);
    /* __DSP_debug_printf is not trapped: its code is that of OSReport, already replaced by the game log. */

    s_au.installed = true;
    gcrt_trace_printf(GCRT_TRACE_NATIVE,
                      "native audio (AI, DSP): %u functions replaced, missing: %s; variables: __AID_Callback %08X, "
                      "__AID_Active %08X, __CallbackStack %08X, __OldStack %08X, __AI_init_flag %08X, "
                      "__DSP_init_flag %08X, tasks: current %08X, first %08X, last %08X, priority %08X; "
                      "registered guest handlers: %08X, %08X, %08X; DSP debug messages %08X (%s)",
                      s_au.replaced, s_au.missing[0] ? s_au.missing : "none", s_au.v_aid_callback, s_au.v_aid_active,
                      s_au.v_callback_stack, s_au.v_old_stack, s_au.v_ai_init_flag, s_au.v_dsp_init_flag,
                      s_au.v_curr_task, s_au.v_first_task, s_au.v_last_task, s_au.v_rude_task, s_au.fn_aid_handler,
                      s_au.fn_ais_handler, s_au.fn_dsp_handler, s_au.fn_debug_printf,
                      s_au.init_msg_ok && s_au.boot_msg_ok ? "formats read" : "formats not found");
}

void gcrt_native_set_audio_sink(GcrtAudioSink sink, void* user) {
    s_au.sink = sink;
    s_au.sink_user = user;
}

static const char* ucode_name(void) {
    if (s_au.have_backend && s_au.backend.name)
        return s_au.backend.name(s_au.backend.user);
    return s_au.last_ucode ? s_au.last_ucode : "none";
}

void gcrt_native_set_dsp_backend(const GcrtDspBackend* backend) {
    if (s_au.have_backend && s_au.backend.name)
        s_au.last_ucode = s_au.backend.name(s_au.backend.user);
    if (backend) {
        s_au.backend = *backend;
        s_au.have_backend = true;
    } else {
        memset(&s_au.backend, 0, sizeof(s_au.backend));
        s_au.have_backend = false;
    }
}

void gcrt_native_audio_stats(GcrtNativeAudioStats* out) {
    memset(out, 0, sizeof(*out));
    out->dma_blocks = s_au.blocks;
    out->dma_silent_blocks = s_au.silent_blocks;
    out->dma_cycles = s_au.dma_cycles;
    out->aid_interrupts = s_au.aid_delivered;
    out->dsp_interrupts = s_au.dsp_delivered;
    out->mails_to_dsp = s_au.mails_to_dsp;
    out->mails_to_cpu = s_au.mails_to_cpu;
    out->ucode = ucode_name();
}

void gcrt_native_audio_report(FILE* out) {
    if (!s_au.installed)
        return;
    fprintf(out,
            "native audio: DMA %llu blocks (+%llu silent, DMA stopped), %llu buffers read, %u Hz, %llu activations; "
            "DMA interrupts %llu raised, %llu delivered (%llu callbacks already in progress, %llu without a callback, "
            "%llu on the AIInit stack); DSP: %llu mails to the DSP, %llu to the CPU (%llu lost), interrupts %llu "
            "raised, %llu delivered (INIT %llu, RESUME %llu, YIELD %llu, DONE %llu, REQUEST %llu, others %llu, without "
            "mail %llu); tasks added %llu, starts %llu, switches %llu, DSPAssertTask %llu, "
            "DSPCancelTask %llu; rate changes %llu; microcode %s; %u functions replaced, missing: %s\n",
            (unsigned long long)s_au.blocks, (unsigned long long)s_au.silent_blocks,
            (unsigned long long)s_au.dma_cycles, 108000000u / dma_divisor(), (unsigned long long)s_au.dma_starts,
            (unsigned long long)s_au.aid_raised, (unsigned long long)s_au.aid_delivered,
            (unsigned long long)s_au.aid_reentries, (unsigned long long)s_au.aid_no_callback,
            (unsigned long long)s_au.stack_switches, (unsigned long long)s_au.mails_to_dsp,
            (unsigned long long)s_au.mails_to_cpu, (unsigned long long)s_au.mails_dropped,
            (unsigned long long)s_au.dsp_raised, (unsigned long long)s_au.dsp_delivered,
            (unsigned long long)s_au.dsp_by_mail[0], (unsigned long long)s_au.dsp_by_mail[1],
            (unsigned long long)s_au.dsp_by_mail[2], (unsigned long long)s_au.dsp_by_mail[3],
            (unsigned long long)s_au.dsp_by_mail[4], (unsigned long long)s_au.dsp_by_mail[5],
            (unsigned long long)s_au.dsp_empty, (unsigned long long)s_au.tasks_added, (unsigned long long)s_au.boots,
            (unsigned long long)s_au.task_switches, (unsigned long long)s_au.asserts, (unsigned long long)s_au.cancels,
            (unsigned long long)s_au.rate_changes,
            ucode_name(), s_au.replaced,
            s_au.missing[0] ? s_au.missing : "none");
}
