/* Internal to the native replacements (hle/native*.c): per-subsystem modules and shared helpers.
 * See gcrt_native.h for the public interface. */
#ifndef GCRT_NATIVE_MODULES_H
#define GCRT_NATIVE_MODULES_H

#include "hle/gcrt_hle.h"
#include "hle/gcrt_native.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Disc drive (native_dvd.c). */
void gcrt_native_dvd_install(GcrtCpu* g, const char* disc_root);
void gcrt_native_dvd_report(FILE* out);

/* Controllers (native_pad.c). */
void gcrt_native_pad_install(GcrtCpu* g);
void gcrt_native_pad_report(FILE* out);

/* Audio memory (native_ar.c), audio: DSP and AI (native_audio.c, step 10d), display (native_vi.c), caches
 * (native_cache.c). */
void gcrt_native_ar_install(GcrtCpu* g);
void gcrt_native_ar_report(FILE* out);
void gcrt_native_audio_install(GcrtCpu* g);
void gcrt_native_audio_report(FILE* out);
void gcrt_native_vi_install(GcrtCpu* g);
void gcrt_native_vi_report(FILE* out);
void gcrt_native_cache_install(GcrtCpu* g);
void gcrt_native_cache_report(FILE* out);
/* GX drawing (native_gx*.c, milestone 10c). */
void gcrt_native_gx_install(GcrtCpu* g);
void gcrt_native_gx_report(FILE* out);
/* Memory card (native_card.c, milestone 7). */
void gcrt_native_card_install(GcrtCpu* g);
void gcrt_native_card_report(FILE* out);

/* Native OS (native_os.c, milestone 10b): threads, scheduling, contexts, lazy FPU, message queues, idle;
 * alarms (native_os_alarm.c). */
void gcrt_native_os_install(GcrtCpu* g);
void gcrt_native_os_report(FILE* out);
void gcrt_native_os_alarm_install(GcrtCpu* g);
void gcrt_native_os_alarm_report(FILE* out);
/* Is the native OS in place (OS variables found, replacements installed)? */
bool gcrt_native_os_active(void);
/* The game's r13 (small data base), read in __init_registers; 0 if unknown. */
u32 gcrt_native_os_sda(void);
/* Native OS services for the other replacements (same effects as the SDK functions). */
s32 gcrt_os_disable_scheduler(GcrtCpu* g);                 /* OSDisableScheduler: previous value */
s32 gcrt_os_enable_scheduler(GcrtCpu* g);                  /* OSEnableScheduler */
void gcrt_os_set_current_context(GcrtCpu* g, u32 context); /* OSSetCurrentContext */
void gcrt_os_clear_context(GcrtCpu* g, u32 context);       /* OSClearContext */
void gcrt_os_load_context(GcrtCpu* g, u32 context);        /* OSLoadContext (no rfi), thread continuation included */
/* End of an interrupt: __OSReschedule then OSLoadContext(context); may switch to another thread. */
void gcrt_os_interrupt_exit(GcrtCpu* g, u32 context);
/* End of a replacement (pc = LR and r3 = result already set) after callbacks that may have woken threads up:
 * __OSReschedule; the current thread will resume at this return later if it yields. */
void gcrt_os_reschedule_return(GcrtCpu* g, u32 result);
/* An interrupt has just been delivered (detection of OSYieldThread loops). */
void gcrt_os_note_delivery(void);
/* Lazy FPU for a native replacement that is going to read or write the floating-point registers while MSR[FP] = 0:
 * same effect as the "FPU unavailable" exception raised by the first floating-point instruction of the replaced
 * function (address `at`): context stored, FPU of the previous owner saved, that of the current thread loaded. No
 * effect if MSR[FP] = 1. */
void gcrt_native_os_fpu_claim(GcrtCpu* g, u32 at);
/* System part of the native OS (native_os_sys.c): interrupts without a vector, reset, memory protection, IPL SRAM
 * and ROM, EXI bus without devices, DSP bootstrap, system call. */
void gcrt_native_os_sys_install(GcrtCpu* g);
void gcrt_native_os_sys_report(FILE* out);
/* Native __OSMaskInterrupts / __OSUnmaskInterrupts: previous OS mask. */
u32 gcrt_os_mask_interrupts(GcrtCpu* g, u32 mask);
u32 gcrt_os_unmask_interrupts(GcrtCpu* g, u32 mask);

/* Replacement of a catalog function: unique copy, otherwise anchored copy (`following`, chain of names for
 * gcrt_hle_resolve_following, or `preceding`), otherwise the only copy in the range of library `library`.
 * Returns true if installed; otherwise appends the name to `missing`. */
bool gcrt_native_replace(GcrtCpu* g, const char* name, const char* library, GcrtHleFn fn,
                         const char* const* following, unsigned following_count, const char* preceding,
                         char* missing, size_t missing_size);

/* Native timers (independent of MSR[EE]: pacing, video fields). Called at a block boundary as soon as the ticks
 * reach the deadline; a timer fires only once (reschedule it in the callback). */
typedef void (*GcrtNativeTimerFn)(GcrtCpu* g, void* user);
u32 gcrt_ntimer_set(GcrtCpu* g, u64 due_ticks, GcrtNativeTimerFn fn, void* user);
bool gcrt_ntimer_cancel(GcrtCpu* g, u32 id);

/* --- Shared helpers (native_irq.c) --- */

/* Replacement called by the game (game thread): to be called on entry, then finish with gcrt_native_return. */
void gcrt_native_enter(GcrtCpu* g);
/* End of a replacement: r3 = `result`, return to the caller (lr). If guest callbacks were called outside an
 * interrupt, the scheduler had been suspended (gcrt_native_callback): it is restored, then __OSReschedule is run
 * before the return (a thread woken by a callback takes over as on the console, right after the replacement
 * instead of during the callback). */
void gcrt_native_return(GcrtCpu* g, u32 result);
/* Guest callback called by a replacement: in a native interrupt handler, direct call (MSR[EE] at 0, scheduler
 * already suspended); on the game thread, the scheduler is suspended first (a nested call must not switch
 * threads). */
bool gcrt_native_callback(GcrtCpu* g, u32 function, const u32* args, unsigned count);
/* Inside a native interrupt handler? */
bool gcrt_native_in_interrupt(void);
/* Temporary exception context around interrupt-time processing, like the SDK handlers:
 * OSClearContext(&tmp); OSSetCurrentContext(&tmp) ... OSClearContext(&tmp); OSSetCurrentContext(context).
 * `tmp` is taken on the guest stack (r1 lowered down to _end). */
u32 gcrt_native_exception_context_begin(GcrtCpu* g);
void gcrt_native_exception_context_end(GcrtCpu* g, u32 tmp, u32 context);

/* Guest OS functions used by the replacements (0 if not found). */
typedef enum GcrtOsFn {
    GCRT_OSFN_DISABLE_SCHEDULER = 0,
    GCRT_OSFN_ENABLE_SCHEDULER,
    GCRT_OSFN_RESCHEDULE,
    GCRT_OSFN_LOAD_CONTEXT,
    GCRT_OSFN_CLEAR_CONTEXT,
    GCRT_OSFN_SET_CURRENT_CONTEXT,
    GCRT_OSFN_MASK_INTERRUPTS,
    GCRT_OSFN_UNMASK_INTERRUPTS,
    GCRT_OSFN_COUNT
} GcrtOsFn;
u32 gcrt_native_os_fn(GcrtOsFn which);

/* Reads the controller input (source provided by the platform). */
bool gcrt_native_read_pad(int port, GcrtPadInput* out);
void gcrt_native_rumble(int port, int command);

/* Access to guest RAM by virtual or physical address (0x8..., 0xC..., physical < 24 MiB): host pointer to `size`
 * bytes, or NULL if the range falls outside main memory. */
u8* gcrt_native_ram(GcrtCpu* g, u32 address, u32 size);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_NATIVE_MODULES_H */
