/* Native replacements of the SDK functions that touch the hardware (milestone 10: "no emulated hardware").
 *
 * Principle (the internal stage-1 plan, milestone 10 and its method): the hardware registers are no longer simulated;
 * the SDK functions that use them are recognized by signature (catalog hle/sdk_signatures.tsv) and replaced by native
 * code (files of the disc image, SDL, window...). Since step 10d, no hardware is emulated any more (hw/ and sched/
 * removed; reference: the last build that still emulated the hardware, kept outside this repository's public
 * history): disc, controllers, display, caches and audio memory since step 10a, memory card since milestone 7, OS
 * since 10b, GX drawing since 10c, DSP and AI since 10d.
 *
 * Three generic mechanisms, none specific to a game:
 *   1. Installation of the replacements (native.c); every subsystem is native, none can be emulated.
 *   2. Native interrupts (native_irq.c): a native replacement that must "call back later" (end of a disc read,
 *      controller polling...) posts an event; it is delivered on the game thread, at a block boundary of the
 *      chassis, exactly as the OS external interrupt entry would do it: current context saved (OSContext,
 *      "exception" state), MSR[EE] = 0, scheduler suspended during the handler, then __OSReschedule and
 *      OSLoadContext. It is delivered only if MSR[EE] = 1 and if the game has not masked that interrupt (OS masks
 *      in low memory 0x800000C4/0x800000C8).
 *      Details and choice of the delivery points: notes/runtime.md § 12.3 (native callbacks).
 *   3. Calls of guest functions from native code (nested execution until the return, interrupts masked during
 *      the call).
 */
#ifndef GCRT_NATIVE_H
#define GCRT_NATIVE_H

#include "cpu/gcrt_cpu.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------
 * Installation
 * ---------------------------------------------------------------------------------------------- */
/* "native: dvd pad vi cache card gx ar dsp ai" */
const char* gcrt_native_describe(char* buf, size_t size);
/* Installs the native replacements. To be called after the signature scan of the DOL and gcrt_hle_install_defaults,
 * before the first instruction. `disc_root`: root of the extracted disc. */
void gcrt_native_install(GcrtCpu* g, const char* disc_root);
/* Report of the native replacements (calls, delivered interrupts...). */
void gcrt_native_report(FILE* out);

/* ------------------------------------------------------------------------------------------------
 * Native interrupts
 * ---------------------------------------------------------------------------------------------- */
/* OS interrupt numbers (__OS_INTERRUPT_* of the SDK); mask = 0x80000000 >> number. */
enum {
    GCRT_OS_IRQ_DSP_AI = 5,
    GCRT_OS_IRQ_DSP_ARAM = 6,
    GCRT_OS_IRQ_DSP_DSP = 7,
    GCRT_OS_IRQ_AI_AI = 8,
    GCRT_OS_IRQ_PI_SI = 20,
    GCRT_OS_IRQ_PI_DI = 21,
    GCRT_OS_IRQ_PI_VI = 24,
};

/* "Decrementer" class (milestone 10b): native OS alarms (native_os_alarm.c). Not maskable by the OS, delivered only
 * if MSR[EE] = 1 and after the due external interrupts; its handler manages the scheduler itself (no automatic
 * suspension) and may request a direct return (gcrt_nirq_skip_reschedule). */
#define GCRT_NIRQ_DECREMENTER 32u

/* Native handler of an interrupt: called in interrupt context (MSR[EE] = 0, scheduler suspended); `context` = guest
 * address of the interrupted OSContext (`context` parameter of the SDK handlers). It may call guest functions
 * (gcrt_guest_call...). */
typedef void (*GcrtNativeIrqFn)(GcrtCpu* g, unsigned interrupt, u32 context, void* user);

/* Installs the mechanism (called by gcrt_native_install; no effect the second time). */
void gcrt_nirq_init(GcrtCpu* g);
/* Posts an interrupt: delivered once, at the first block boundary where ticks >= `due_ticks`, MSR[EE] = 1 and the
 * interrupt is not masked by the OS. When several external interrupts are due at the same boundary, the one chosen
 * is the one __OSDispatchInterrupt would pick on the console: the SDK priority table (InterruptPrioTable), then the
 * lowest number within a group; equal numbers by deadline, then order of posting. The decrementer class comes after
 * the external interrupts. Returns an identifier (> 0), 0 if the queue is full. */
u32 gcrt_nirq_post(GcrtCpu* g, unsigned interrupt, u64 due_ticks, GcrtNativeIrqFn fn, void* user);
bool gcrt_nirq_cancel(GcrtCpu* g, u32 id);
/* Is the interrupt masked by the OS (__OSMaskInterrupts)? */
bool gcrt_nirq_masked(GcrtCpu* g, unsigned interrupt);
/* Pending events. */
unsigned gcrt_nirq_pending(void);
/* Deadline of the next deliverable event (not masked by the OS), UINT64_MAX if there is none. */
u64 gcrt_nirq_next_due(GcrtCpu* g);
/* In a decrementer-class handler: resume the interrupted context without rescheduling (like the direct
 * OSLoadContext of DecrementerExceptionCallback when the head of the queue is not due). */
void gcrt_nirq_skip_reschedule(void);
/* Report: interrupts delivered per number, nested calls. */
void gcrt_nirq_report(FILE* out);

/* Diagnostic timeline (GCRT_TIMELINE=file): one line per timestamped event, "ticks pc depth message" (ticks at
 * 40.5 MHz, current guest pc, native interrupt depth), to compare two executables instant by instant (native
 * interrupts posted and delivered, native audio events...). Zero cost when it is off (one test). */
extern bool g_gcrt_timeline_on;
void gcrt_timeline(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;
#define GCRT_TIMELINE(...)                                                                                              \
    do {                                                                                                               \
        if (g_gcrt_timeline_on)                                                                                        \
            gcrt_timeline(__VA_ARGS__);                                                                                \
    } while (0)

/* ------------------------------------------------------------------------------------------------
 * Calls of guest functions from native code
 * ---------------------------------------------------------------------------------------------- */
/* Calls `function(args...)` (r3...r10) and runs until it returns, MSR[EE] = 0 during the call (no interrupt and
 * no thread switch can slip in). All integer registers, LR, CTR, CR, XER, MSR, SRR0/1 are restored afterwards;
 * *result = r3 on return. The stack is the current one (r1). */
bool gcrt_guest_call(GcrtCpu* g, u32 function, const u32* args, unsigned count, u32* result);
/* Depth of the nested calls in progress (0: normal execution of the game). No thread switch is allowed during a
 * nested call. */
int gcrt_guest_call_depth(void);
/* Calls a callback "in exception context", as the SDK interrupt handlers do (e.g. __DVDInterruptHandler):
 * OSClearContext(&tmp); OSSetCurrentContext(&tmp); function(args); OSClearContext(&tmp);
 * OSSetCurrentContext(`context`). `tmp` is an OSContext taken on the guest stack. If
 * OSClearContext/OSSetCurrentContext cannot be found, direct call. */
bool gcrt_guest_call_exception_context(GcrtCpu* g, u32 context, u32 function, const u32* args, unsigned count);

/* Address of an SDK function recognized exactly once by signature (0 otherwise, with a message). */
u32 gcrt_native_sdk_function(const char* name);

/* ------------------------------------------------------------------------------------------------
 * Controller inputs (provided by the platform: keyboard, SDL controllers, scripted inputs)
 * ---------------------------------------------------------------------------------------------- */
typedef struct GcrtPadInput {
    u16 buttons; /* SDK PAD_BUTTON_* bits (A = 0x0100, B = 0x0200, START = 0x1000, D-pad 0x000F...) */
    s8 stick_x, stick_y, substick_x, substick_y; /* centered on 0 */
    u8 trigger_l, trigger_r, analog_a, analog_b;
} GcrtPadInput;
/* Reads the state of the port (0-3); returns false if no controller is plugged into it. */
typedef bool (*GcrtPadSource)(void* user, int port, GcrtPadInput* out);
typedef void (*GcrtPadRumble)(void* user, int port, int command);
void gcrt_native_set_pad_source(GcrtPadSource source, GcrtPadRumble rumble, void* user);

/* ------------------------------------------------------------------------------------------------
 * Links with the platform (native VI, native ARAM, native audio)
 * ---------------------------------------------------------------------------------------------- */
/* Start of each video field produced by the native VI (platform pacing, window events, reports): `field` = field
 * number since startup (1 = first), `odd` = odd field. */
typedef void (*GcrtFieldHook)(void* user, u64 field, bool odd);
void gcrt_native_set_field_hook(GcrtFieldHook hook, void* user);
/* Fields produced and retraces delivered to the game by the native VI. */
u64 gcrt_native_vi_fields(void);
u64 gcrt_native_vi_retraces(void);
/* Native drive: commands executed and bytes read (platform reports). */
void gcrt_native_dvd_stats(u64* commands, u64* bytes);
/* Audio memory (ARAM, 16 MiB, big-endian): host buffer allocated by the native AR functions, shared with the DSP
 * microcodes (NULL before installation). */
u8* gcrt_native_aram(u32* size);

/* Output of the native audio DMA (step 10d, native_audio.c): each 32-byte block read from the guest buffer, as it
 * is in memory (8 stereo 16-bit big-endian frames, RIGHT sample then LEFT), at `rate` Hz (32028 or 48042: divisors
 * 3372 and 2248 of 108 MHz); one silent block per period when the DMA is stopped (like Dolphin). */
typedef void (*GcrtAudioSink)(void* user, const u8* block32, u32 rate);
void gcrt_native_set_audio_sink(GcrtAudioSink sink, void* user);
/* HLE DSP microcodes (runtime/core/audio, dsp_ucode.h), plugged in by the platform: start of a task (fingerprint
 * of its IRAM code, returned), CPU -> DSP mail, update every millisecond, name. */
typedef struct GcrtDspBackend {
    void* user;
    u32 (*boot)(void* user, u32 iram_ram_addr, u32 iram_length, u32 start_pc);
    void (*mail)(void* user, u32 mail);
    void (*update)(void* user);
    const char* (*name)(void* user);
} GcrtDspBackend;
void gcrt_native_set_dsp_backend(const GcrtDspBackend* backend);
/* CPU side of the DSP -> CPU mailbox, for the microcodes: mail stored in the queue; `interrupt`: interrupt #7 when
 * it becomes the first of the queue (after `delay_cycles` nominal cycles if it already is). */
void gcrt_native_dsp_push_mail(u32 mail, bool interrupt, u32 delay_cycles);
/* Microcode change: pending mails are dropped. */
void gcrt_native_dsp_clear_mails(void);
typedef struct GcrtNativeAudioStats {
    u64 dma_blocks;        /* 32-byte blocks read by the active audio DMA */
    u64 dma_silent_blocks; /* silent blocks (DMA stopped) */
    u64 dma_cycles;        /* buffers read in full (DMA interrupts raised) */
    u64 aid_interrupts;    /* interrupts #5 delivered */
    u64 dsp_interrupts;    /* interrupts #7 delivered */
    u64 mails_to_dsp, mails_to_cpu;
    const char* ucode;     /* current microcode */
} GcrtNativeAudioStats;
void gcrt_native_audio_stats(GcrtNativeAudioStats* out);

/* ------------------------------------------------------------------------------------------------
 * Native GX drawing (milestone 10c, native_gx*.c)
 * ---------------------------------------------------------------------------------------------- */
/* Destination of the GX command stream produced by the replacements (command processor format: BP/CP/XF, draws,
 * display lists), in program order, in 32-byte packets; the platform plugs the bridge to Aurora into it
 * (gx/gx_bridge). `abort`: stream interrupted (GXAbortFrame), partial command to discard. */
typedef struct GcrtGxSink {
    void* user;
    void (*consume)(void* user, const u8* data, size_t size);
    void (*abort)(void* user);
} GcrtGxSink;
void gcrt_native_set_gx_sink(const GcrtGxSink* sink);
/* Write by the game to the write-gather pipe address (0xCC008000-0xCC008FFF, alias 0x0C008000): vertices written
 * inline after GXBegin (exception tolerated by the milestone 10 criterion). Outside an open draw: WGPIPE_OUTSIDE
 * counter. `value`: bytes in guest order, `size` from 1 to 8. */
void gcrt_native_gx_pipe_write(GcrtCpu* g, u32 address, u32 size, u64 value);
/* Is GX drawing replaced natively (library recognized, variables found)? */
bool gcrt_native_gx_active(void);
typedef struct GcrtGxStats {
    u64 draw_dones;       /* GXSetDrawDone / GXDrawDone */
    u64 finish_delivered; /* "draw finish" interrupts delivered */
    u64 draws;            /* draws opened by GXBegin */
    u64 inline_bytes;     /* vertex bytes written inline to the pipe during an open draw */
    u64 pipe_writes;      /* writes to the pipe */
    u64 pipe_out_writes;  /* writes to the pipe outside an open draw (must stay at 0) */
    u64 packets;          /* 32-byte packets handed over */
} GcrtGxStats;
void gcrt_native_gx_stats(GcrtGxStats* out);

/* ------------------------------------------------------------------------------------------------
 * Native memory card (milestone 7, native_card.c)
 * ---------------------------------------------------------------------------------------------- */
/* Size of the card always inserted in slot A, in megabits (4, 8, 16, 32, 64 or 128: 59, 123, 251, 507, 1019
 * or 2043 free blocks). Default: 16 (251-block card). To be called by the game before gcrt_main if needed;
 * GCRT_CARD_MBIT overrides it (tests). The files are in gcrt_save_dir() (platform/gcrt_user_dir.h). */
void gcrt_native_card_set_size(unsigned size_mbit);

/* Disc image (UTF-8 path): GCRT_DISC_IMAGE, otherwise the "image" field of <disc_root>/../manifest.json; a relative
 * "image" is relative to the manifest's folder, and an absolute one that no longer exists is looked for under the same
 * file name next to the manifest (installed copy moved or renamed). Returns false if no path is found. */
bool gcrt_disc_image_path(const char* disc_root, char* out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_NATIVE_H */
