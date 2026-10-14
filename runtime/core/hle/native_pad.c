/* Native controllers (milestone 10a): replacement of the SDK PAD functions (and the SI functions they use).
 *
 * Boundary (the internal SDK notes § 2.3, reverse agent): PADInit, PADRead, PADReset, PADRecalibrate,
 * PADControlMotor, PADSetSpec, PADSetSamplingCallback, and on the SI side: SIInit (called by OSInit),
 * SISetSamplingRate and SIRefreshSamplingRate (called every frame by __VIRetraceHandler). All the other SI*
 * functions and the internal callbacks of Pad.c are then no longer called: no access to the SI registers.
 * __PADDisableRecalibration (variable in low memory, no hardware) stays recompiled.
 *
 * Reproduced semantics (SDK, Pad.c/SIBios.c of the reference decompilation, consulted only; reference emulation
 * hw/si.cpp, removed in step 10a, kept outside this repository's public history):
 *   - a standard GameCube controller (SI type 0x09000000: rumble motor present) on each port where the platform
 *     reports one; its origin is perfectly centered ("origin" response of the reference emulation and of
 *     Dolphin), so the origin subtraction and the bounds of SPEC2_MakeStatus are the identity;
 *   - default SDK analog mode (AnalogMode 0x300, "mode 3"): sticks and triggers on 8 bits, analog A/B buttons
 *     absent (0);
 *   - PADRead fills the 4 PADStatus: port without a controller -> err = PAD_ERR_NO_CONTROLLER (-1), first 10 bytes
 *     at 0; enabled controller -> err = PAD_ERR_NONE; returns the mask of the enabled ports that have a motor;
 *   - a controller is "enabled" only after PADInit/PADReset/PADRecalibrate of its port (the SDK probes the port then
 *     reads the origin: on the emulated reference, finished before the next read); unplugged -> disabled.
 * Reading the state: at the time of PADRead (keyboard, SDL controllers and scripted inputs, through the platform);
 * the console read the last response of the SI polling paced by the VI (difference of at most one poll).
 * Generic: no reference to a game. */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <string.h>

#define PAD_CHANS 4
#define PAD_ERR_NONE 0
#define PAD_ERR_NO_CONTROLLER ((u8)-1)
#define PAD_STATUS_SIZE 12u
/* Button bits returned to the game (PAD_BUTTON_*; 0x0080 "interference" and 0x2000 "origin" never returned). */
#define PAD_BUTTONS_MASK 0x1F7Fu

typedef struct {
    bool initialized;
    u32 enabled_bits;   /* enabled ports (PAD_CHAN0_BIT = 0x80000000 >> port) */
    u32 spec;
    u32 sampling_callback;
    u32 sampling_rate;
    /* Report */
    u64 reads, resets, motor_commands, si_calls;
    u64 port_reads[PAD_CHANS];
    unsigned installed;
    char missing[512];
} NativePad;

static NativePad s_pad = {.spec = 5};

static u32 arg(GcrtCpu* g, int i) { return g->state.gpr[3 + i]; }
static u32 chan_bit(int chan) { return 0x80000000u >> chan; }

/* PADReset(mask) / PADRecalibrate(mask): probes the requested ports; each port that is present is enabled. */
static void reset_ports(u32 mask) {
    s_pad.resets++;
    for (int chan = 0; chan < PAD_CHANS; chan++) {
        if (!(mask & chan_bit(chan)))
            continue;
        GcrtPadInput in;
        const bool present = gcrt_native_read_pad(chan, &in);
        const bool was = (s_pad.enabled_bits & chan_bit(chan)) != 0;
        if (present)
            s_pad.enabled_bits |= chan_bit(chan);
        else
            s_pad.enabled_bits &= ~chan_bit(chan);
        if (present != was || !s_pad.initialized)
            GCRT_TRACE(GCRT_TRACE_PAD, "port %d: %s", chan, present ? "controller enabled" : "no controller");
    }
}

static bool hle_pad_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    if (!s_pad.initialized) {
        /* Not reproduced: OSRegisterVersion (banner), wireless (__PADFixBits), SIRefreshSamplingRate, reset
         * function (OnReset: stops the SI polling, not applicable). */
        s_pad.initialized = true;
        reset_ports(0xF0000000u);
        GCRT_TRACE(GCRT_TRACE_PAD, "native PADInit (spec %u)", s_pad.spec);
    }
    gcrt_native_return(g, 1);
    return true;
}

static bool hle_pad_reset(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    reset_ports(arg(g, 0));
    gcrt_native_return(g, 1);
    return true;
}

static void write_status(GcrtCpu* g, u32 status, const GcrtPadInput* in, u8 err) {
    CPUState* c = &g->state;
    if (err != PAD_ERR_NONE) {
        for (u32 i = 0; i < 10u; i++)
            mem_write8(c, status + i, 0);
    } else {
        mem_write16(c, status + 0, (u16)(in->buttons & PAD_BUTTONS_MASK));
        mem_write8(c, status + 2, (u8)in->stick_x);
        mem_write8(c, status + 3, (u8)in->stick_y);
        mem_write8(c, status + 4, (u8)in->substick_x);
        mem_write8(c, status + 5, (u8)in->substick_y);
        mem_write8(c, status + 6, in->trigger_l);
        mem_write8(c, status + 7, in->trigger_r);
        mem_write8(c, status + 8, 0); /* analog mode 3: no analog buttons */
        mem_write8(c, status + 9, 0);
    }
    mem_write8(c, status + 10, err);
}

/* PADRead(PADStatus status[4]) -> mask of the ports with a motor. */
static bool hle_pad_read(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 status = arg(g, 0);
    u32 motor = 0;
    s_pad.reads++;
    for (int chan = 0; chan < PAD_CHANS; chan++) {
        const u32 st = status + (u32)chan * PAD_STATUS_SIZE;
        GcrtPadInput in;
        const bool present = gcrt_native_read_pad(chan, &in);
        if (!(s_pad.enabled_bits & chan_bit(chan))) {
            write_status(g, st, &in, PAD_ERR_NO_CONTROLLER);
            continue;
        }
        if (!present) {
            /* Unplugged: the SDK disables the port (PADDisable) and returns "no controller". */
            s_pad.enabled_bits &= ~chan_bit(chan);
            GCRT_TRACE(GCRT_TRACE_PAD, "port %d: controller unplugged", chan);
            write_status(g, st, &in, PAD_ERR_NO_CONTROLLER);
            continue;
        }
        motor |= chan_bit(chan); /* standard controller: motor present (SI type without SI_GC_NOMOTOR) */
        s_pad.port_reads[chan]++;
        write_status(g, st, &in, PAD_ERR_NONE);
    }
    gcrt_native_return(g, motor);
    return true;
}

/* PADControlMotor(chan, command): 0 stop, 1 rumble, 2 hard stop; only on an enabled port. */
static bool hle_pad_control_motor(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 chan = arg(g, 0);
    if (chan < PAD_CHANS && (s_pad.enabled_bits & chan_bit((int)chan))) {
        s_pad.motor_commands++;
        gcrt_native_rumble((int)chan, (int)(arg(g, 1) & 3u));
    }
    gcrt_native_return(g, 0);
    return true;
}

/* PADSetSpec(spec): only specs 2 to 5 (retail controllers, SPEC2_MakeStatus) are reproduced. */
static bool hle_pad_set_spec(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_pad.spec = arg(g, 0);
    if (s_pad.spec < 2)
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "native PAD: spec %u (prototypes) not reproduced, treated as spec 5",
                          s_pad.spec);
    gcrt_native_return(g, 0);
    return true;
}

/* PADSetSamplingCallback(cb) -> previous callback. The callback would be called at each SI poll: not delivered
 * natively ([hypothesis]: no known game uses it outside OnReset; not verified), reported if it is installed. */
static bool hle_pad_set_sampling_callback(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 old = s_pad.sampling_callback;
    s_pad.sampling_callback = arg(g, 0);
    if (s_pad.sampling_callback)
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "native PAD: sampling callback %08X installed but not delivered",
                          s_pad.sampling_callback);
    gcrt_native_return(g, old);
    return true;
}

/* SIInit() (OSInit): nothing to initialize (no SI interrupt, no polling). */
static bool hle_si_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_pad.si_calls++;
    GCRT_TRACE(GCRT_TRACE_PAD, "native SIInit");
    gcrt_native_return(g, 0);
    return true;
}

/* SISetSamplingRate(msec) / SIRefreshSamplingRate(): SI polling rate, not applicable natively. */
static bool hle_si_set_sampling_rate(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_pad.si_calls++;
    s_pad.sampling_rate = arg(g, 0) > 11u ? 11u : arg(g, 0);
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_si_refresh_sampling_rate(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_pad.si_calls++;
    gcrt_native_return(g, 0);
    return true;
}

typedef struct {
    const char* name;
    const char* library;
    GcrtHleFn fn;
} Replacement;

static const Replacement k_replacements[] = {
    {"PADInit", "PAD", hle_pad_init},
    {"PADReset", "PAD", hle_pad_reset},
    {"PADRecalibrate", "PAD", hle_pad_reset},
    {"PADRead", "PAD", hle_pad_read},
    {"PADControlMotor", "PAD", hle_pad_control_motor},
    {"PADSetSpec", "PAD", hle_pad_set_spec},
    {"PADSetSamplingCallback", "PAD", hle_pad_set_sampling_callback},
    {"SIInit", "SI", hle_si_init},
    {"SISetSamplingRate", "SI", hle_si_set_sampling_rate},
    {"SIRefreshSamplingRate", "SI", hle_si_refresh_sampling_rate},
};

void gcrt_native_pad_install(GcrtCpu* g) {
    s_pad.installed = 0;
    s_pad.missing[0] = '\0';
    for (size_t i = 0; i < sizeof(k_replacements) / sizeof(k_replacements[0]); i++) {
        if (gcrt_hle_replace_in_library(g, k_replacements[i].name, k_replacements[i].library, k_replacements[i].fn)) {
            s_pad.installed++;
        } else {
            size_t used = strlen(s_pad.missing);
            snprintf(s_pad.missing + used, sizeof(s_pad.missing) - used, "%s%s", used ? " " : "",
                     k_replacements[i].name);
        }
    }
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native PAD: %u functions replaced, missing: %s", s_pad.installed,
                      s_pad.missing[0] ? s_pad.missing : "none");
}

void gcrt_native_pad_report(FILE* out) {
    fprintf(out, "native PAD: %llu reads (port 0: %llu), %llu resets, %llu rumble commands, %llu "
                 "SI calls; enabled ports %08X; %u functions replaced, missing: %s\n",
            (unsigned long long)s_pad.reads, (unsigned long long)s_pad.port_reads[0], (unsigned long long)s_pad.resets,
            (unsigned long long)s_pad.motor_commands, (unsigned long long)s_pad.si_calls, s_pad.enabled_bits,
            s_pad.installed, s_pad.missing[0] ? s_pad.missing : "none");
}
