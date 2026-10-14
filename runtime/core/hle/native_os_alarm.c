/* Native OS alarms (milestone 10b): OSSetAlarm, OSSetPeriodicAlarm, OSCancelAlarm without a hardware decrementer.
 *
 * "High" boundary of the internal SDK notes § 13.0.1 item 2: the alarm queue stays the SDK one, in guest memory
 * (AlarmQueue, sorted by deadline, FIFO on ties; OSAlarm: handler +0, tag +4, fire +8, prev +0x10, next +0x14,
 * period +0x18, start +0x20), maintained by the same writes as InsertAlarm / OSCancelAlarm; OSInitAlarm,
 * OSCreateAlarm and OnReset@OSAlarm stay recompiled (no hardware access, same structures). The decrementer that
 * SetTimer programmed becomes a host timer: a native interrupt of the "decrementer" class (not maskable by the OS,
 * delivered only if MSR[EE] = 1, after the external interrupts), due at the same instant as DEC going below zero
 * (delta + 1 tick, delta clamped like SetTimer: < 0 -> 0, >= 2^31 -> 0x7FFFFFFF).
 * At the deadline, what DecrementerExceptionCallback did (one alarm per pass): head not due yet -> reprogramming;
 * otherwise removal, handler cleared, re-insertion if periodic (before the call), reprogramming, then
 * OSDisableScheduler, temporary context, handler(alarm, interrupted context), context restored, OSEnableScheduler,
 * __OSReschedule and resumption of the interrupted context (native).
 * System time = time base + *(u64*)0x800030D8 (__OSGetSystemTime). Generic, no reference to a game. */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <string.h>

#define LOW_SYSTEM_TIME 0x800030D8u

#define AL_HANDLER 0x00u
#define AL_FIRE 0x08u
#define AL_PREV 0x10u
#define AL_NEXT 0x14u
#define AL_PERIOD 0x18u
#define AL_START 0x20u

static struct {
    bool ready;
    u32 queue; /* AlarmQueue (head, tail +4) */
    u32 event; /* pending native interrupt (the "decrementer"), 0 if none */
    u64 sets, periodic, cancels, fired, early, empty;
    unsigned replaced;
    char missing[256];
} s_al;

static u32 R32(GcrtCpu* g, u32 a) { return mem_read32(&g->state, a); }
static void W32(GcrtCpu* g, u32 a, u32 v) { mem_write32(&g->state, a, v); }
static u64 R64(GcrtCpu* g, u32 a) { return ((u64)R32(g, a) << 32) | R32(g, a + 4u); }
static void W64(GcrtCpu* g, u32 a, u64 v) {
    W32(g, a, (u32)(v >> 32));
    W32(g, a + 4u, (u32)v);
}

/* __OSGetSystemTime */
static u64 system_time(GcrtCpu* g) { return g->state.timebase + R64(g, LOW_SYSTEM_TIME); }

static void on_decrementer(GcrtCpu* g, unsigned interrupt, u32 context, void* user);

/* SetTimer(alarm): PPCMtdec(delta); the exception fires when DEC goes below zero, delta + 1 ticks later. */
static void set_timer(GcrtCpu* g, u32 alarm) {
    const s64 delta = (s64)(R64(g, alarm + AL_FIRE) - system_time(g));
    u64 dec;
    if (delta < 0)
        dec = 0;
    else if (delta < 0x80000000ll)
        dec = (u64)delta;
    else
        dec = 0x7FFFFFFFu;
    if (s_al.event)
        gcrt_nirq_cancel(g, s_al.event);
    s_al.event = gcrt_nirq_post(g, GCRT_NIRQ_DECREMENTER, g->ticks + dec + 1u, on_decrementer, NULL);
}

static void insert_alarm(GcrtCpu* g, u32 alarm, u64 fire, u32 handler) {
    const u64 period = R64(g, alarm + AL_PERIOD);
    if ((s64)period > 0) {
        const u64 now = system_time(g);
        const u64 start = R64(g, alarm + AL_START);
        fire = start;
        if ((s64)start < (s64)now)
            fire += period * ((now - start) / period + 1u);
    }
    W32(g, alarm + AL_HANDLER, handler);
    W64(g, alarm + AL_FIRE, fire);
    for (u32 next = R32(g, s_al.queue); next; next = R32(g, next + AL_NEXT)) {
        if ((s64)R64(g, next + AL_FIRE) <= (s64)fire)
            continue;
        W32(g, alarm + AL_PREV, R32(g, next + AL_PREV));
        W32(g, next + AL_PREV, alarm);
        W32(g, alarm + AL_NEXT, next);
        const u32 prev = R32(g, alarm + AL_PREV);
        if (prev) {
            W32(g, prev + AL_NEXT, alarm);
        } else {
            W32(g, s_al.queue, alarm);
            set_timer(g, alarm);
        }
        return;
    }
    W32(g, alarm + AL_NEXT, 0);
    const u32 prev = R32(g, s_al.queue + 4u);
    W32(g, s_al.queue + 4u, alarm);
    W32(g, alarm + AL_PREV, prev);
    if (prev) {
        W32(g, prev + AL_NEXT, alarm);
    } else {
        W32(g, s_al.queue, alarm);
        W32(g, s_al.queue + 4u, alarm);
        set_timer(g, alarm);
    }
}

/* DecrementerExceptionCallback, delivered as a native interrupt (decrementer class). */
static void on_decrementer(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    (void)interrupt;
    (void)user;
    s_al.event = 0;
    const u32 alarm = R32(g, s_al.queue);
    if (!alarm) {
        s_al.empty++;
        gcrt_nirq_skip_reschedule(); /* OSLoadContext(context): direct return */
        return;
    }
    if ((s64)system_time(g) < (s64)R64(g, alarm + AL_FIRE)) {
        s_al.early++;
        set_timer(g, alarm);
        gcrt_nirq_skip_reschedule();
        return;
    }
    const u32 next = R32(g, alarm + AL_NEXT);
    W32(g, s_al.queue, next);
    if (!next)
        W32(g, s_al.queue + 4u, 0);
    else
        W32(g, next + AL_PREV, 0);
    const u32 handler = R32(g, alarm + AL_HANDLER);
    W32(g, alarm + AL_HANDLER, 0);
    if ((s64)R64(g, alarm + AL_PERIOD) > 0)
        insert_alarm(g, alarm, 0, handler);
    if (R32(g, s_al.queue))
        set_timer(g, R32(g, s_al.queue));
    s_al.fired++;
    GCRT_TRACE(GCRT_TRACE_OS, "alarm %08X due: handler %08X", alarm, handler);
    gcrt_os_disable_scheduler(g);
    const u32 tmp = gcrt_native_exception_context_begin(g);
    const u32 args[2] = {alarm, context};
    if (handler)
        gcrt_guest_call(g, handler, args, 2, NULL);
    gcrt_native_exception_context_end(g, tmp, context);
    gcrt_os_enable_scheduler(g);
}

static bool entry_ee(GcrtCpu* g) { return (g->state.msr & 0x8000u) != 0; }

static void ret(GcrtCpu* g, bool ee) {
    CPUState* c = &g->state;
    c->msr = (c->msr & ~0x8000u) | (ee ? 0x8000u : 0u);
    c->pc = c->lr;
}

/* OSSetAlarm(alarm, tick (r5:r6), handler (r7)) */
static bool hle_set_alarm(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* c = &g->state;
    const u32 alarm = c->gpr[3];
    const u64 tick = ((u64)c->gpr[5] << 32) | c->gpr[6];
    W64(g, alarm + AL_PERIOD, 0);
    insert_alarm(g, alarm, system_time(g) + tick, c->gpr[7]);
    s_al.sets++;
    GCRT_TRACE(GCRT_TRACE_OS, "OSSetAlarm %08X: in %llu ticks, handler %08X", alarm, (unsigned long long)tick,
               c->gpr[7]);
    ret(g, entry_ee(g));
    return true;
}

/* OSSetPeriodicAlarm(alarm, start (r5:r6), period (r7:r8), handler (r9)) */
static bool hle_set_periodic_alarm(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* c = &g->state;
    const u32 alarm = c->gpr[3];
    const u64 start = ((u64)c->gpr[5] << 32) | c->gpr[6];
    const u64 period = ((u64)c->gpr[7] << 32) | c->gpr[8];
    W64(g, alarm + AL_PERIOD, period);
    W64(g, alarm + AL_START, start + R64(g, LOW_SYSTEM_TIME)); /* __OSTimeToSystemTime */
    insert_alarm(g, alarm, 0, c->gpr[9]);
    s_al.periodic++;
    GCRT_TRACE(GCRT_TRACE_OS, "OSSetPeriodicAlarm %08X: period %llu ticks, handler %08X", alarm,
               (unsigned long long)period, c->gpr[9]);
    ret(g, entry_ee(g));
    return true;
}

/* OSCancelAlarm(alarm) */
static bool hle_cancel_alarm(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 alarm = g->state.gpr[3];
    if (R32(g, alarm + AL_HANDLER)) {
        const u32 next = R32(g, alarm + AL_NEXT), prev = R32(g, alarm + AL_PREV);
        if (!next)
            W32(g, s_al.queue + 4u, prev);
        else
            W32(g, next + AL_PREV, prev);
        if (prev) {
            W32(g, prev + AL_NEXT, next);
        } else {
            W32(g, s_al.queue, next);
            if (next)
                set_timer(g, next);
        }
        W32(g, alarm + AL_HANDLER, 0);
        s_al.cancels++;
    }
    ret(g, entry_ee(g));
    return true;
}

/* Functions that are dead with native alarms: a call to one of them reveals a forgotten path. */
static bool hle_dead(GcrtCpu* g, u32 address) {
    fprintf(stderr, "gcrt native OS: decrementer function called at %08X (lr %08X) while the alarms are "
                    "native: ignored\n", address, g->state.lr);
    g->state.pc = g->state.lr;
    return true;
}

void gcrt_native_os_alarm_install(GcrtCpu* g) {
    memset(&s_al, 0, sizeof(s_al));
    if (!gcrt_native_os_active())
        return;
    unsigned n = 0;
    const u32 insert = gcrt_hle_find("InsertAlarm", &n);
    s16 d = 0;
    u32 sda = 0;
    if (n != 1 || !gcrt_hle_decode_d_offset(g, insert, gcrt_hle_catalog_size("InsertAlarm"), 32, 13, 0, &d) ||
        !(sda = gcrt_native_os_sda())) {
        fprintf(stderr, "gcrt native OS: alarm queue not found (InsertAlarm): alarms not replaced\n");
        return;
    }
    s_al.queue = sda + (u32)(s32)d;
    char* m = s_al.missing;
    const size_t ms = sizeof(s_al.missing);
    s_al.replaced += gcrt_native_replace(g, "OSSetAlarm", "OS", hle_set_alarm, NULL, 0, NULL, m, ms);
    s_al.replaced += gcrt_native_replace(g, "OSSetPeriodicAlarm", "OS", hle_set_periodic_alarm, NULL, 0, NULL, m, ms);
    s_al.replaced += gcrt_native_replace(g, "OSCancelAlarm", "OS", hle_cancel_alarm, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "PPCMtdec", "PPC", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "InsertAlarm", "OS", hle_dead, NULL, 0, NULL, m, ms);
    s_al.ready = true;
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native alarms: %u functions replaced, missing: %s; AlarmQueue %08X",
                      s_al.replaced, s_al.missing[0] ? s_al.missing : "none", s_al.queue);
}

void gcrt_native_os_alarm_report(FILE* out) {
    if (!s_al.ready)
        return;
    fprintf(out, "native alarms: %llu OSSetAlarm, %llu OSSetPeriodicAlarm, %llu cancellations, %llu firings "
                 "(%llu early, %llu on an empty queue)\n",
            (unsigned long long)s_al.sets, (unsigned long long)s_al.periodic, (unsigned long long)s_al.cancels,
            (unsigned long long)s_al.fired, (unsigned long long)s_al.early, (unsigned long long)s_al.empty);
}
