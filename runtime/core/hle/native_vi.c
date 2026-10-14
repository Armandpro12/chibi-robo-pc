/* Native display and pacing (milestone 10a): replacement of the SDK VI functions (vi.c).
 *
 * Boundary (the internal SDK notes § 4.3, reverse agent): VIInit, VIConfigure, VIConfigurePan, VIFlush,
 * VISetNextFrameBuffer, VISetBlack, VISetPreRetraceCallback, VISetPostRetraceCallback, VIGetTvFormat, VIGetNextField,
 * VIGetCurrentLine (+ VIGetDTVStatus if present). Stay recompiled: VIWaitForRetrace and VIGetRetraceCount, which
 * read the SDK retrace counter and wait queue: these two variables stay the game's own (addresses read in the code
 * of VIWaitForRetrace). __VIRetraceHandler is never installed: no access to the VI registers.
 *
 * Pacing (faithful to the console and to the emulated reference hw/vi.cpp, removed in step 10a, kept outside this
 * repository's public history): one field every numHalfLines half-lines
 * of the applied mode (SDK timing table: 625 in PAL, 525 in NTSC/EURGB60/MPAL...), half-line = HLW x
 * 2 x CPU clock / VI clock (27 MHz, 54 MHz in progressive): exactly 50 Hz in PAL, 59.94 Hz in NTSC/EURGB60.
 * Fields counted from instant 0 like the reference. At each field start: platform callback (presentation, PC
 * pacing, window, reports), then retrace delivered like the VI interrupt (#24) if VIInit has happened; the game
 * programs its display interrupts (setInterruptRegs) at the start of each field: the reference fires them at that
 * same instant. A retrace not delivered yet is not doubled (latched interrupt bits of the console: a single pass of
 * the handler).
 *
 * Retrace (like __VIRetraceHandler): retraceCount++, temporary exception context, PreCB(retraceCount), pending
 * VIFlush applied (mode, frame buffer, black; an interlaced/non-interlaced change waits for an odd field) then
 * SIRefreshSamplingRate, PostCB(retraceCount), OSWakeupThread(&retraceQueue).
 * Presentation: the image is always presented at the EFB -> XFB copy (GX bridge, 10c); the chosen buffer and the
 * black flag are tracked but have no effect on the window, as in the emulated reference.
 * Approximation: a frequency change ("60 Hz" choice) takes effect over the whole current field instead of the next
 * half-line. Generic: no reference to a game. */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <string.h>

/* Formats and modes (SDK vi.h). */
enum { TV_NTSC = 0, TV_PAL = 1, TV_MPAL = 2, TV_DEBUG = 3, TV_DEBUG_PAL = 4, TV_EURGB60 = 5, TV_GCA = 6 };
enum { VI_INTERLACE = 0, VI_NON_INTERLACE = 1, VI_PROGRESSIVE = 2, VI_3D = 3 };
#define VI_TVMODE(tv, mode) (((tv) << 2) + (mode))
#define LOW_TV_BOOTROM 0x800000CCu

/* Useful timings from the SDK table (vi.c, timing[]): half-lines per field and HLW. */
typedef struct {
    u16 num_half_lines;
    u16 hlw;
    const char* name;
} Timing;

static const Timing k_timing[10] = {
    {525, 429, "NTSC INT"}, {526, 429, "NTSC DS"}, {625, 432, "PAL INT"}, {624, 432, "PAL DS"},
    {525, 429, "MPAL INT"}, {526, 429, "MPAL DS"}, {1050, 429, "NTSC PRO"}, {1050, 429, "NTSC 3D"},
    {525, 429, "GCA INT"},  {1050, 429, "GCA DS"},
};

/* getTiming(mode) of the SDK: index into k_timing, -1 if unknown. */
static int timing_index(u32 mode) {
    switch (mode) {
    case VI_TVMODE(TV_NTSC, VI_INTERLACE): return 0;
    case VI_TVMODE(TV_NTSC, VI_NON_INTERLACE): return 1;
    case VI_TVMODE(TV_PAL, VI_INTERLACE): return 2;
    case VI_TVMODE(TV_PAL, VI_NON_INTERLACE): return 3;
    case VI_TVMODE(TV_EURGB60, VI_INTERLACE): return 0;
    case VI_TVMODE(TV_EURGB60, VI_NON_INTERLACE): return 1;
    case VI_TVMODE(TV_MPAL, VI_INTERLACE): return 4;
    case VI_TVMODE(TV_MPAL, VI_NON_INTERLACE): return 5;
    case VI_TVMODE(TV_NTSC, VI_PROGRESSIVE): return 6;
    case VI_TVMODE(TV_NTSC, VI_3D): return 7;
    case VI_TVMODE(TV_DEBUG_PAL, VI_INTERLACE): return 2;
    case VI_TVMODE(TV_DEBUG_PAL, VI_NON_INTERLACE): return 3;
    case VI_TVMODE(TV_GCA, VI_INTERLACE): return 8;
    case VI_TVMODE(TV_GCA, VI_PROGRESSIVE): return 9;
    default: return -1;
    }
}

typedef struct {
    bool installed_ok;
    bool initialized;
    /* Game variables (SDA) read in VIWaitForRetrace. */
    s32 count_offset, queue_offset;
    bool vars_found;
    u32 sda;
    u32 fn_wakeup, fn_init_queue, fn_refresh_sampling;
    /* State of vi.c */
    u32 tv_bootrom;
    u32 non_inter;     /* HorVer.nonInter */
    u32 tv;            /* HorVer.tv (requested mode) */
    int timing;        /* HorVer.timing (requested) */
    bool clock54;      /* requested VI clock (progressive) */
    u32 curr_tv;       /* CurrTvMode (applied) */
    int curr_timing;   /* CurrTiming (applied) */
    bool curr_clock54;
    u32 change_mode, shdw_change_mode;
    bool flush_flag;
    u32 buf_addr, next_buf_addr, curr_buf_addr;
    bool fb_set, is_black;
    u32 disp_pos_y;
    u32 pre_cb, post_cb;
    /* Field timeline (ticks) */
    u64 field_start;
    u64 field_ticks;
    u64 half_cycles; /* duration of a half-line in cycles */
    bool odd;        /* current field is odd (first of a frame) */
    u64 fields;
    bool retrace_pending;
    u64 retraces;
    u64 flushes;
    u32 timer_id;
    GcrtFieldHook hook;
    void* hook_user;
    unsigned installed;
    char missing[512];
} NativeVi;

static NativeVi s_vi;

static void on_field(GcrtCpu* g, void* user);

static u32 arg(GcrtCpu* g, int i) { return g->state.gpr[3 + i]; }

void gcrt_native_set_field_hook(GcrtFieldHook hook, void* user) {
    s_vi.hook = hook;
    s_vi.hook_user = user;
}

u64 gcrt_native_vi_fields(void) { return s_vi.installed_ok ? s_vi.fields : 0; }
u64 gcrt_native_vi_retraces(void) { return s_vi.installed_ok ? s_vi.retraces : 0; }

/* Durations of a half-line (cycles) and of a field (ticks) for a timing. */
static void timing_durations(int timing, bool clock54, u64* half_cycles, u64* field_ticks) {
    const Timing* t = &k_timing[timing >= 0 ? timing : 2];
    const u64 per_sample = 2ull * GCRT_CPU_HZ / (clock54 ? 54000000ull : 27000000ull);
    *half_cycles = per_sample * t->hlw;
    *field_ticks = (*half_cycles * t->num_half_lines) / GCRT_CYCLES_PER_TB_TICK;
}

static u32 count_address(void) { return s_vi.sda + (u32)s_vi.count_offset; }
static u32 queue_address(void) { return s_vi.sda + (u32)s_vi.queue_offset; }

/* ---------------------------------------------------------------------------------------------
 * Retrace
 * ------------------------------------------------------------------------------------------- */

/* VISetRegs: applies the state copied by the last VIFlush; refused during an even field if the
 * interlaced/non-interlaced mode changes. */
static bool apply_flush(GcrtCpu* g) {
    if (s_vi.shdw_change_mode == 1 && !s_vi.odd)
        return false;
    s_vi.shdw_change_mode = 0;
    const bool timing_changed = s_vi.curr_timing != s_vi.timing || s_vi.curr_clock54 != s_vi.clock54;
    s_vi.curr_timing = s_vi.timing;
    s_vi.curr_tv = s_vi.tv;
    s_vi.curr_clock54 = s_vi.clock54;
    s_vi.curr_buf_addr = s_vi.next_buf_addr;
    s_vi.flushes++;
    if (timing_changed) {
        u64 half = 0, field = 0;
        timing_durations(s_vi.curr_timing, s_vi.curr_clock54, &half, &field);
        s_vi.half_cycles = half;
        s_vi.field_ticks = field;
        /* The current field takes the new duration (approximation, see the header). */
        gcrt_ntimer_cancel(g, s_vi.timer_id);
        s_vi.timer_id = gcrt_ntimer_set(g, s_vi.field_start + s_vi.field_ticks, on_field, NULL);
        gcrt_trace_printf(GCRT_TRACE_NVI, "video mode applied: %s (tv %u), field of %llu ticks (%.3f Hz)",
                          k_timing[s_vi.curr_timing].name, s_vi.curr_tv, (unsigned long long)field,
                          (double)GCRT_TB_HZ / (double)field);
    }
    return true;
}

static void on_retrace(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    (void)interrupt;
    (void)user;
    CPUState* c = &g->state;
    s_vi.retrace_pending = false;
    s_vi.retraces++;
    const u32 count = mem_read32(c, count_address()) + 1u;
    mem_write32(c, count_address(), count);
    const u32 tmp = gcrt_native_exception_context_begin(g);
    if (s_vi.pre_cb)
        gcrt_native_callback(g, s_vi.pre_cb, &count, 1);
    if (s_vi.flush_flag && g->stop == GCRT_STOP_NONE) {
        if (apply_flush(g)) {
            s_vi.flush_flag = false;
            if (s_vi.fn_refresh_sampling)
                gcrt_native_callback(g, s_vi.fn_refresh_sampling, NULL, 0);
        }
    }
    if (s_vi.post_cb && g->stop == GCRT_STOP_NONE) {
        const u32 fn_clear = gcrt_native_os_fn(GCRT_OSFN_CLEAR_CONTEXT);
        if (fn_clear)
            gcrt_guest_call(g, fn_clear, &tmp, 1, NULL);
        gcrt_native_callback(g, s_vi.post_cb, &count, 1);
    }
    if (s_vi.fn_wakeup && g->stop == GCRT_STOP_NONE) {
        const u32 queue = queue_address();
        gcrt_native_callback(g, s_vi.fn_wakeup, &queue, 1);
    }
    gcrt_native_exception_context_end(g, tmp, context);
    GCRT_TRACE(GCRT_TRACE_NVI, "retrace %u (field %llu, %s)", count, (unsigned long long)s_vi.fields,
               s_vi.odd ? "odd" : "even");
}

/* Start of a field (native timer, independent of MSR[EE]). */
static void on_field(GcrtCpu* g, void* user) {
    (void)user;
    s_vi.field_start += s_vi.field_ticks;
    s_vi.odd = !s_vi.odd;
    s_vi.fields++;
    if (s_vi.hook)
        s_vi.hook(s_vi.hook_user, s_vi.fields, s_vi.odd);
    if (s_vi.initialized && !s_vi.retrace_pending) {
        s_vi.retrace_pending = true;
        gcrt_nirq_post(g, GCRT_OS_IRQ_PI_VI, s_vi.field_start, on_retrace, NULL);
    }
    s_vi.timer_id = gcrt_ntimer_set(g, s_vi.field_start + s_vi.field_ticks, on_field, NULL);
}

/* ---------------------------------------------------------------------------------------------
 * Replacements
 * ------------------------------------------------------------------------------------------- */

static bool hle_vi_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    if (!s_vi.initialized) {
        CPUState* c = &g->state;
        s_vi.sda = c->gpr[13];
        /* State left by the IPL (DCR: video on, boot format, interlaced); not reproduced: filter registers,
         * display offset from the SRAM (ImportAdjustingValues), banner. */
        s_vi.tv_bootrom = mem_read32(c, LOW_TV_BOOTROM);
        s_vi.non_inter = VI_INTERLACE;
        s_vi.tv = s_vi.tv_bootrom == TV_PAL ? TV_PAL : s_vi.tv_bootrom == TV_MPAL ? TV_MPAL : TV_NTSC;
        const u32 tv = s_vi.tv == TV_DEBUG ? TV_NTSC : s_vi.tv;
        s_vi.timing = timing_index(VI_TVMODE(tv, s_vi.non_inter));
        s_vi.curr_timing = s_vi.timing;
        s_vi.curr_tv = s_vi.tv;
        s_vi.clock54 = s_vi.curr_clock54 = false;
        s_vi.change_mode = s_vi.shdw_change_mode = 0;
        s_vi.flush_flag = false;
        s_vi.disp_pos_y = 0;
        s_vi.is_black = true;
        s_vi.pre_cb = s_vi.post_cb = 0;
        if (s_vi.vars_found) {
            mem_write32(c, count_address(), 0);
            const u32 queue = queue_address();
            if (s_vi.fn_init_queue)
                gcrt_guest_call(g, s_vi.fn_init_queue, &queue, 1, NULL);
        }
        const u32 unmask = gcrt_native_os_fn(GCRT_OSFN_UNMASK_INTERRUPTS);
        if (unmask) {
            const u32 mask = 0x80000000u >> GCRT_OS_IRQ_PI_VI;
            gcrt_guest_call(g, unmask, &mask, 1, NULL);
        }
        s_vi.initialized = true;
        GCRT_TRACE(GCRT_TRACE_NVI, "native VIInit: %s, counter %08X, queue %08X", k_timing[s_vi.curr_timing].name,
                   count_address(), queue_address());
    }
    gcrt_native_return(g, 0);
    return true;
}

/* VIConfigure(const GXRenderModeObj* rm) */
static bool hle_vi_configure(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    CPUState* c = &g->state;
    const u32 rm = arg(g, 0);
    const u32 vi_tv_mode = mem_read32(c, rm + 0x00);
    const u32 vi_y_origin = mem_read16(c, rm + 0x0C);
    const u32 new_non_inter = vi_tv_mode & 3u;
    if (s_vi.non_inter != new_non_inter) {
        s_vi.change_mode = 1;
        s_vi.non_inter = new_non_inter;
    }
    const u32 tv_game = vi_tv_mode >> 2;
    const u32 tv_boot = mem_read32(c, LOW_TV_BOOTROM);
    bool allowed;
    switch (tv_boot) {
    case TV_MPAL: case TV_NTSC: case TV_GCA:
        allowed = tv_game == TV_NTSC || tv_game == TV_MPAL || tv_game == TV_GCA;
        break;
    case TV_PAL: case TV_EURGB60:
        allowed = tv_game == TV_PAL || tv_game == TV_EURGB60;
        break;
    default:
        allowed = false;
        break;
    }
    if (!allowed) {
        /* SDK OSErrorLine: the game halts. */
        fprintf(stderr, "gcrt native: VIConfigure(): Tried to change mode from (%u) to (%u), which is forbidden\n",
                tv_boot, tv_game);
        g->stop = GCRT_STOP_GUEST_HALT;
        gcrt_native_return(g, 0);
        return true;
    }
    s_vi.tv = (tv_game == TV_NTSC || tv_game == TV_MPAL) ? tv_boot : tv_game;
    s_vi.disp_pos_y = s_vi.non_inter == VI_NON_INTERLACE ? vi_y_origin * 2u : vi_y_origin;
    const int t = timing_index(VI_TVMODE(s_vi.tv, s_vi.non_inter));
    if (t >= 0)
        s_vi.timing = t;
    s_vi.clock54 = VI_TVMODE(TV_NTSC, VI_PROGRESSIVE) == vi_tv_mode || VI_TVMODE(TV_NTSC, VI_3D) == vi_tv_mode ||
                   VI_TVMODE(TV_GCA, VI_PROGRESSIVE) == vi_tv_mode;
    GCRT_TRACE(GCRT_TRACE_NVI, "VIConfigure: mode %u (tv %u -> %u, %s), Y origin %u", vi_tv_mode, tv_game, s_vi.tv,
               t >= 0 ? k_timing[t].name : "?", vi_y_origin);
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_vi_configure_pan(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, 0); /* panning: registers only, no effect natively */
    return true;
}

static bool hle_vi_flush(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_vi.shdw_change_mode |= s_vi.change_mode;
    s_vi.change_mode = 0;
    s_vi.flush_flag = true;
    s_vi.next_buf_addr = s_vi.buf_addr;
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_vi_set_next_frame_buffer(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_vi.buf_addr = arg(g, 0);
    s_vi.fb_set = true;
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_vi_set_black(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_vi.is_black = arg(g, 0) != 0;
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_vi_set_pre_retrace_callback(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 old = s_vi.pre_cb;
    s_vi.pre_cb = arg(g, 0);
    gcrt_native_return(g, old);
    return true;
}

static bool hle_vi_set_post_retrace_callback(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 old = s_vi.post_cb;
    s_vi.post_cb = arg(g, 0);
    gcrt_native_return(g, old);
    return true;
}

static bool hle_vi_get_tv_format(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    u32 fmt;
    switch (s_vi.curr_tv) {
    case TV_NTSC: case TV_DEBUG: case TV_GCA: fmt = TV_NTSC; break;
    case TV_PAL: case TV_DEBUG_PAL: fmt = TV_PAL; break;
    default: fmt = s_vi.curr_tv; break; /* EURGB60, MPAL */
    }
    gcrt_native_return(g, fmt);
    return true;
}

/* Current half-line in the field (0 at the start). */
static u32 current_half_line(GcrtCpu* g) {
    const u64 elapsed_cycles = (g->ticks - s_vi.field_start) * GCRT_CYCLES_PER_TB_TICK;
    const u64 h = s_vi.half_cycles ? elapsed_cycles / s_vi.half_cycles : 0;
    const u32 n = k_timing[s_vi.curr_timing >= 0 ? s_vi.curr_timing : 2].num_half_lines;
    return (u32)(h < n ? h : n - 1u);
}

static bool hle_vi_get_next_field(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 even_odd = s_vi.odd ? 1u : 0u; /* getCurrentFieldEvenOdd */
    gcrt_native_return(g, (even_odd ^ 1u) ^ (s_vi.disp_pos_y & 1u));
    return true;
}

static bool hle_vi_get_current_line(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, current_half_line(g) >> 1);
    return true;
}

/* VIGetDTVStatus() (absent from some DOLs: linked only if the game calls it): bit 0 of DTV_STAT = component cable
 * plugged in. Native: 0 (no cable), like the emulated reference (register not handled, read as 0): the game does not
 * offer progressive scan. Difference with Dolphin, which sets this bit by default (SYSCONF "PGS" = true): see
 * notes/runtime.md § 16.7. */
static bool hle_vi_get_dtv_status(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, 0);
    return true;
}

/* OSGetFontEncode() (OS, OSFont.c; outside the VI library but reads the VI DTV_STAT register in NTSC):
 *   static u16 fontEncode = 0xFFFF; if fontEncode != 0xFFFF, return it; otherwise, in NTSC (TV mode 0 at
 *   0x800000CC): SJIS (1) if bit 1 of DTV_STAT ("Japanese console") is set, ANSI (0) otherwise; in the other
 *   modes: ANSI; then ParseString = ParseStringS (OS function pointer); returns fontEncode.
 * [verified: disassembly of two versions of a game, notes/runtime.md § 16.7]. Native: same thing, the "Japanese" bit
 * coming from the disc region (4th character of the identifier at 0x80000003 = 'J'), as Dolphin sets it
 * (VideoInterface.cpp: ntsc_j = NTSC-J region); the emulated reference read 0 (register not handled): identical for
 * non-Japanese discs.
 * The two variables (cache and ParseString) are found in the code recognized by signature: `lhz r3, d(r13)` at the
 * start, `lis r4, H ; lhz ; addi r0, r4, L ; stw r0, d2(r13)` at the end. */
typedef struct {
    bool ok;
    s16 cache_offset, parse_offset; /* relative to r13 */
    u32 parse_value;                /* ParseStringS */
} FontEncodeCode;
static FontEncodeCode s_font;

static bool decode_font_encode(GcrtCpu* g, u32 fn) {
    CPUState* c = &g->state;
    const u32 w0 = mem_read32(c, fn), w_lis = mem_read32(c, fn + 0x50u), w_addi = mem_read32(c, fn + 0x58u),
              w_stw = mem_read32(c, fn + 0x5Cu), w_blr = mem_read32(c, fn + 0x60u);
    const bool shape = (w0 & 0xFFFF0000u) == 0xA06D0000u            /* lhz r3, d(r13) */
                       && (w_lis & 0xFFFF0000u) == 0x3C800000u       /* lis r4, H */
                       && (w_addi & 0xFFFF0000u) == 0x38040000u      /* addi r0, r4, L */
                       && (w_stw & 0xFFFF0000u) == 0x900D0000u       /* stw r0, d2(r13) */
                       && w_blr == 0x4E800020u;
    if (!shape)
        return false;
    s_font.cache_offset = (s16)(w0 & 0xFFFFu);
    s_font.parse_offset = (s16)(w_stw & 0xFFFFu);
    s_font.parse_value = ((w_lis & 0xFFFFu) << 16) + (u32)(s32)(s16)(w_addi & 0xFFFFu);
    s_font.ok = true;
    return true;
}

static bool hle_os_get_font_encode(GcrtCpu* g, u32 address) {
    (void)address;
    if (!s_font.ok)
        return false; /* code not recognized: the original runs */
    gcrt_native_enter(g);
    CPUState* c = &g->state;
    const u32 sda = c->gpr[13];
    const u32 cache = sda + (u32)(s32)s_font.cache_offset;
    u16 encode = mem_read16(c, cache);
    if (encode == 0xFFFFu) {
        const bool ntsc = mem_read32(c, LOW_TV_BOOTROM) == TV_NTSC;
        const bool japanese = mem_read8(c, 0x80000003u) == 'J';
        encode = (ntsc && japanese) ? 1u : 0u;
        mem_write16(c, cache, encode);
        mem_write32(c, sda + (u32)(s32)s_font.parse_offset, s_font.parse_value);
        GCRT_TRACE(GCRT_TRACE_NVI, "native OSGetFontEncode: %s", encode ? "SJIS" : "ANSI");
    }
    gcrt_native_return(g, encode);
    return true;
}

/* SDK variables read by VIWaitForRetrace: `lwz rX, d(r13)` (retraceCount) and `addi r3, r13, d`
 * (&retraceQueue). */
static bool find_guest_variables(GcrtCpu* g) {
    unsigned n = 0;
    const u32 fn = gcrt_hle_find("VIWaitForRetrace", &n);
    if (n != 1)
        return false;
    bool count = false, queue = false;
    for (u32 i = 0; i < 0x54u / 4u; i++) {
        const u32 w = mem_read32(&g->state, fn + 4u * i);
        const u32 op = w >> 26, rd = (w >> 21) & 31u, ra = (w >> 16) & 31u;
        if (!count && op == 32u && ra == 13u) {
            s_vi.count_offset = (s16)(w & 0xFFFFu);
            count = true;
        } else if (!queue && op == 14u && ra == 13u && rd == 3u) {
            s_vi.queue_offset = (s16)(w & 0xFFFFu);
            queue = true;
        }
    }
    return count && queue;
}

void gcrt_native_vi_install(GcrtCpu* g) {
    static const char* const after_handler[] = {"__VIRetraceHandler"};
    static const char* const after_pre[] = {"__VIRetraceHandler", "VISetPreRetraceCallback"};
    s_vi.vars_found = find_guest_variables(g);
    if (!s_vi.vars_found) {
        fprintf(stderr, "gcrt native: ERROR: VIWaitForRetrace variables not found (SDK version not "
                        "recognized?): no vertical retrace, the game will stay stuck in VIWaitForRetrace\n");
        return;
    }
    s_vi.fn_wakeup = gcrt_native_sdk_function("OSWakeupThread");
    s_vi.fn_init_queue = gcrt_native_sdk_function("OSInitThreadQueue");
    s_vi.fn_refresh_sampling = gcrt_native_sdk_function("SIRefreshSamplingRate");
    s_vi.installed = 0;
    s_vi.missing[0] = '\0';
    char* m = s_vi.missing;
    const size_t ms = sizeof(s_vi.missing);
    s_vi.installed += gcrt_native_replace(g, "VIInit", "VI", hle_vi_init, NULL, 0, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VIConfigure", "VI", hle_vi_configure, NULL, 0, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VIConfigurePan", "VI", hle_vi_configure_pan, NULL, 0, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VIFlush", "VI", hle_vi_flush, NULL, 0, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VISetNextFrameBuffer", "VI", hle_vi_set_next_frame_buffer, NULL, 0,
                                          NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VISetBlack", "VI", hle_vi_set_black, NULL, 0, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VISetPreRetraceCallback", "VI", hle_vi_set_pre_retrace_callback,
                                          after_handler, 1, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VISetPostRetraceCallback", "VI", hle_vi_set_post_retrace_callback,
                                          after_pre, 2, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VIGetTvFormat", "VI", hle_vi_get_tv_format, NULL, 0, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VIGetNextField", "VI", hle_vi_get_next_field, NULL, 0, NULL, m, ms);
    s_vi.installed += gcrt_native_replace(g, "VIGetCurrentLine", "VI", hle_vi_get_current_line, NULL, 0, NULL, m, ms);
    unsigned dtv = 0;
    gcrt_hle_find("VIGetDTVStatus", &dtv);
    if (dtv)
        s_vi.installed += gcrt_native_replace(g, "VIGetDTVStatus", "VI", hle_vi_get_dtv_status, NULL, 0, NULL, m, ms);
    /* Outside the VI library but reads a VI register (in NTSC). */
    unsigned font_count = 0;
    const u32 font_fn = gcrt_hle_find("OSGetFontEncode", &font_count);
    if (font_count == 1 && decode_font_encode(g, font_fn))
        s_vi.installed += gcrt_native_replace(g, "OSGetFontEncode", "OS", hle_os_get_font_encode, NULL, 0, NULL, m, ms);
    else if (font_count)
        fprintf(stderr, "gcrt native: OSGetFontEncode recognized %u times or unexpected code: kept recompiled\n",
                font_count);

    /* Timeline: fields counted from instant 0, in the boot format (low memory 0x800000CC). */
    const u32 boot = mem_read32(&g->state, LOW_TV_BOOTROM);
    s_vi.curr_tv = boot == TV_PAL ? TV_PAL : boot == TV_MPAL ? TV_MPAL : TV_NTSC;
    s_vi.curr_timing = timing_index(VI_TVMODE(s_vi.curr_tv, VI_INTERLACE));
    timing_durations(s_vi.curr_timing, false, &s_vi.half_cycles, &s_vi.field_ticks);
    s_vi.field_start = 0;
    s_vi.odd = true; /* field in progress at instant 0: odd; the first one announced (field_ticks) is even */
    s_vi.installed_ok = true;
    s_vi.timer_id = gcrt_ntimer_set(g, s_vi.field_ticks, on_field, NULL);
    gcrt_trace_printf(GCRT_TRACE_NATIVE,
                      "native VI: %u functions replaced, missing: %s; %s, field of %llu ticks (%.3f Hz), "
                      "retraceCount r13%+d, retraceQueue r13%+d",
                      s_vi.installed, s_vi.missing[0] ? s_vi.missing : "none", k_timing[s_vi.curr_timing].name,
                      (unsigned long long)s_vi.field_ticks, (double)GCRT_TB_HZ / (double)s_vi.field_ticks,
                      s_vi.count_offset, s_vi.queue_offset);
}

void gcrt_native_vi_report(FILE* out) {
    fprintf(out, "native VI: %llu fields, %llu retraces delivered, %llu VIFlush applied; mode %s (tv %u), black %d, "
                 "XFB %08X; %u functions replaced, missing: %s\n",
            (unsigned long long)s_vi.fields, (unsigned long long)s_vi.retraces, (unsigned long long)s_vi.flushes,
            s_vi.curr_timing >= 0 ? k_timing[s_vi.curr_timing].name : "?", s_vi.curr_tv, s_vi.is_black ? 1 : 0,
            s_vi.curr_buf_addr, s_vi.installed, s_vi.missing[0] ? s_vi.missing : "none");
}
