/* Native GX drawing (milestone 10c): replacement of the SDK GX library functions that accessed the graphics hardware
 * (CP, PE, PI-FIFO registers, write-gather pipe) or its state (__GXData).
 *
 * Principle (choices and reasons: notes/decisions.md, step 10c section):
 *   - each replaced function redoes what the code recognized by signature does (disassembly of the SDK GX read function
 *     by function, the internal SDK notes § 14 and notes/runtime.md, step 10c section), with the SDK state kept by the
 *     host (native_gx.h, GxState); it produces the same commands, in the same order;
 *   - this stream is encoded directly in the format of Aurora's command processor (BP/CP/XF, draws, display lists:
 *     the format that Aurora's GX API writes itself) and handed over, through the GcrtGxSink extension point, to the
 *     gx/gx_bridge bridge, which decodes it (guest addresses -> pointers, display lists, textures, copies, end of
 *     frame) and passes it on to Aurora. Handed over in 32-byte packets at the same positions as the console received
 *     them: the end of frame (display copy) falls at the same point of the program as before;
 *   - no register any more: no more CP/PE/PI/MI access; the draw finish (GXSetDrawDone) is delivered as native
 *     interrupt #19 (PE "finish"), on the game thread, with the semantics of GXFinishInterruptHandler;
 *   - exception tolerated by the milestone 10 criterion: the vertices that the game writes itself, inline, to the
 *     write-gather pipe address (0xCC008000) between a GXBegin and the end of the announced data. They arrive here
 *     (gcrt_native_gx_pipe_write) and are appended as is to the stream; any write to the pipe outside an open draw is
 *     counted separately (WGPIPE_OUTSIDE counter) and must stay at zero.
 * Stay recompiled (no hardware access, no __GXData field read): __GXInitGX (sequence of API calls), GXInit*Obj*,
 * GXGet*, GXInitLight*, GXInitFifo*, GXInitTexCacheRegion, GXInitTlutRegion, GXGetTexBufferSize, __GetImageTileCount,
 * GXInitFogAdjTable, GXProject, GXAdjustForOverscan, GXSetTevDirect, GXDrawSphere, GXDrawCube* (never run here;
 * GXDrawCylinder is native, native_gx_draw.c), and the functions
 * that only read or set a guest variable (GXGetCPUFifo, GXGetGPFifo, GXSetBreakPtCallback, GXSetDrawDoneCallback,
 * GXSetDrawSyncCallback, GXWaitDrawDone): the host maintains those same variables. Generic. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "hle/native_gx.h"
#include "platform/trace.h"

#include <stdlib.h>

GxState gx;
GxSdkTables gx_tables;

/* OS interrupts (OSInterrupt.h): CP 17, PE token 18, PE finish 19; mask = 0x80000000 >> n. */
#define IRQ_PI_CP 17u
#define IRQ_PI_PE_TOKEN 18u
#define IRQ_PI_PE_FINISH 19u
#define PACKET 32u

static struct {
    bool installed;
    GcrtGxSink sink;
    bool have_sink;
    /* Stream waiting to be handed over (incomplete 32-byte packet, or draw in progress). */
    u8* buf;
    size_t len, cap;
    u64 delivered;   /* bytes already handed over to the command processor */
    u64 draw_end;    /* stream position where the data of the open draw end */
    /* Guest variables (SDA, r13) found in the recognized code. */
    u32 sda;
    s16 off_draw_done, off_draw_done_cb, off_finish_queue, off_cpu_fifo, off_gp_fifo;
    s16 off_fifo_thread, off_fifo_overflow;
    bool have_drawdone_vars, have_fifo_vars, have_fifo_init_vars;
    u32 fifo_obj;    /* static FIFO object of GXInit (returned to the game) */
    u32 version_var; /* r13: pointer to the version banner (OSRegisterVersion) */
    s16 off_version;
    bool have_version;
    /* Guest functions used. */
    u32 fn_init_fifo_base, fn_init_gx, fn_register_version, fn_set_handler, fn_init_queue, fn_wakeup,
        fn_get_current_thread, fn_mfhid2, fn_mtwpar, fn_mthid2;
    u32 fn_cp_handler, fn_token_handler, fn_finish_handler;
    /* Reports */
    u64 draw_dones, finish_delivered, draws, inline_bytes, pipe_writes, pipe_out_bytes, pipe_out_writes,
        native_during_draw, packets, aborts, native_vertex_bytes, native_vertex_outside;
    unsigned installed_count;
    char missing[1024];
} s;

static GcrtCpu* s_g; /* game processor (for the access counter) */
bool gx_draw_pending; /* a draw opened by GXBegin is still waiting for vertices */

/* ---------------------------------------------------------------------------------------------
 * Command stream
 * ------------------------------------------------------------------------------------------- */

static void grow(size_t need) {
    size_t cap = s.cap ? s.cap : 4096;
    while (cap < need)
        cap *= 2;
    u8* nb = (u8*)realloc(s.buf, cap);
    if (!nb) {
        fprintf(stderr, "gcrt native GX: out of memory for the stream (%zu bytes)\n", need);
        abort();
    }
    s.buf = nb;
    s.cap = cap;
}

void gxw_u8(u8 v) {
    if (s.len + 1 > s.cap)
        grow(s.len + 1);
    s.buf[s.len++] = v;
}

void gxw_u16(u16 v) {
    if (s.len + 2 > s.cap)
        grow(s.len + 2);
    s.buf[s.len++] = (u8)(v >> 8);
    s.buf[s.len++] = (u8)v;
}

void gxw_u32(u32 v) {
    if (s.len + 4 > s.cap)
        grow(s.len + 4);
    u8* p = s.buf + s.len;
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
    s.len += 4;
}

static u64 stream_pos(void) { return s.delivered + s.len; }

/* Hands the complete 32-byte packets over to the command processor (the rest waits, as in the console's write-gather
 * pipe: a command is seen by the graphics processor only once its packet is complete). */
static void deliver_packets(void) {
    const size_t n = s.len & ~(size_t)(PACKET - 1u);
    if (!n)
        return;
    if (s.have_sink)
        s.sink.consume(s.sink.user, s.buf, n);
    s.packets += n / PACKET;
    s.delivered += n;
    memmove(s.buf, s.buf + n, s.len - n);
    s.len -= n;
}

void gx_return(GcrtCpu* g, u32 result) {
    deliver_packets();
    g->state.gpr[3] = result;
    g->state.pc = g->state.lr;
}

/* A native function writes to the stream while a draw is still waiting for its vertices: the original stream would
 * have had the same byte sequence (the commands become "vertices" for the graphics processor); reported. */
void gx_check_draw_open(GcrtCpu* g) {
    if (gx_draw_pending) {
        if (stream_pos() < s.draw_end) {
            s.native_during_draw++;
            if (s.native_during_draw <= 8)
                gcrt_trace_printf(GCRT_TRACE_UNHANDLED,
                                  "native GX: GX call (pc %08X, lr %08X) before the end of the open draw's vertices "
                                  "(%llu bytes missing)",
                                  g->state.pc, g->state.lr, (unsigned long long)(s.draw_end - stream_pos()));
        }
        gx_draw_pending = false;
    }
}

void gcrt_native_set_gx_sink(const GcrtGxSink* sink) {
    if (sink && sink->consume) {
        s.sink = *sink;
        s.have_sink = true;
    } else {
        memset(&s.sink, 0, sizeof(s.sink));
        s.have_sink = false;
    }
}

/* Write by the game to the write-gather pipe address (0xCC008000-0xCC008FFF or its physical alias). */
void gcrt_native_gx_pipe_write(GcrtCpu* g, u32 address, u32 size, u64 value) {
    s.pipe_writes++;
    u8 bytes[8];
    if (size > 8)
        size = 8;
    for (u32 i = 0; i < size; i++)
        bytes[i] = (u8)(value >> ((size - 1u - i) * 8u));
    const u64 pos = stream_pos();
    u32 inside = 0;
    if (gx_draw_pending && pos < s.draw_end)
        inside = (u32)((s.draw_end - pos) < size ? (s.draw_end - pos) : size);
    if (inside < size) {
        /* Outside an open draw: never expected (milestone 10 criterion). Kept in the stream so the game goes on. */
        s.pipe_out_writes++;
        s.pipe_out_bytes += size - inside;
        gcrt_hw_count_add(g, GCRT_HW_WGPIPE_OUTSIDE, address, true);
        if (s.pipe_out_writes <= 8)
            gcrt_trace_printf(GCRT_TRACE_UNHANDLED,
                              "native GX: write of %u byte(s) to the pipe outside an open draw (address %08X, pc %08X, "
                              "lr %08X)",
                              size - inside, address, g->state.pc, g->state.lr);
    }
    s.inline_bytes += inside;
    if (s.len + size > s.cap)
        grow(s.len + size);
    memcpy(s.buf + s.len, bytes, size);
    s.len += size;
    if (gx_draw_pending && stream_pos() >= s.draw_end) {
        gx_draw_pending = false; /* all the data of the draw have arrived */
        deliver_packets();
    }
}

void gx_emit_vertex_u32(u32 v) {
    const u64 pos = stream_pos();
    if (!(gx_draw_pending && pos + 4u <= s.draw_end)) {
        /* Never expected: the shape always opens its draw before its vertices. */
        s.native_vertex_outside++;
        if (s.native_vertex_outside <= 8)
            gcrt_trace_printf(GCRT_TRACE_UNHANDLED, "native GX: SDK shape vertex outside an open draw");
    }
    if (s.len + 4u > s.cap)
        grow(s.len + 4u);
    s.buf[s.len++] = (u8)(v >> 24);
    s.buf[s.len++] = (u8)(v >> 16);
    s.buf[s.len++] = (u8)(v >> 8);
    s.buf[s.len++] = (u8)v;
    s.native_vertex_bytes += 4u;
    if (gx_draw_pending && stream_pos() >= s.draw_end) {
        gx_draw_pending = false;
        deliver_packets();
    }
}

/* Opens a draw: `bytes` bytes of vertices follow in the stream (written inline by the game to the pipe). */
static void open_draw(u32 bytes) {
    if (!bytes)
        return;
    gx_draw_pending = true;
    s.draw_end = stream_pos() + bytes;
}

/* ---------------------------------------------------------------------------------------------
 * Shared internal functions
 * ------------------------------------------------------------------------------------------- */

u32 gx_cvt_fp2unsigned(double x) {
    if (x < 0.0)
        return 0;
    if (!(x < 4294967296.0))
        return 0xFFFFFFFFu;
    if (x < 2147483648.0)
        return (u32)(s32)x;
    return (u32)(s32)(x - 2147483648.0) + 0x80000000u;
}

void gx_flush_zeros(void) {
    for (int i = 0; i < 8; i++)
        gxw_u32(0);
}

/* __GXSendFlushPrim: strip of vNum zero vertices (XF flush), then bpSentNot = 1. */
void gx_send_flush_prim(void) {
    const u32 num = (u32)gx.vnum * (u32)gx.vlim;
    gxw_u8(0x98);
    gxw_u16(gx.vnum);
    for (u32 i = 0; i < num; i += 4)
        gxw_u32(0);
    gx.bp_sent_not = 1;
}

/* Size of a vertex of format `fmt` (current host VCD and VAT; same computation as the GX bridge and as Dolphin's
 * VertexLoaderBase: 1-byte matrix indices, direct or indexed positions/normals/colors/coordinates). */
static u32 comp_size(u32 f) { return f <= 1u ? 1u : (f <= 3u ? 2u : 4u); }

u32 gx_vertex_size(u32 fmt) {
    static const u8 k_color[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    fmt &= 7u;
    const u32 a = gx.vat_a[fmt], b = gx.vat_b[fmt], c = gx.vat_c[fmt];
    const u32 lo = gx.vcd_lo, hi = gx.vcd_hi;
    u32 size = 0;
    for (u32 i = 0; i < 9; i++)
        size += (lo >> i) & 1u;
    /* type: 0 absent, 1 direct, 2 8-bit index, 3 16-bit index */
#define ATTR(type, direct, indices) \
    do { \
        const u32 t_ = (type); \
        size += t_ == 1u ? (direct) : t_ == 2u ? (indices) : t_ == 3u ? 2u * (indices) : 0u; \
    } while (0)
    ATTR((lo >> 9) & 3u, ((a & 1u) ? 3u : 2u) * comp_size((a >> 1) & 7u), 1u);
    {
        const bool ntb = (a >> 9) & 1u;
        const bool idx3 = (a >> 31) & 1u;
        ATTR((lo >> 11) & 3u, (ntb ? 9u : 3u) * comp_size((a >> 10) & 7u), (ntb && idx3) ? 3u : 1u);
    }
    ATTR((lo >> 13) & 3u, k_color[(a >> 14) & 7u], 1u);
    ATTR((lo >> 15) & 3u, k_color[(a >> 18) & 7u], 1u);
    const u32 tc_cnt[8] = {(a >> 21) & 1u, b & 1u, (b >> 9) & 1u, (b >> 18) & 1u,
                           (b >> 27) & 1u, (c >> 5) & 1u, (c >> 14) & 1u, (c >> 23) & 1u};
    const u32 tc_fmt[8] = {(a >> 22) & 7u, (b >> 1) & 7u, (b >> 10) & 7u, (b >> 19) & 7u,
                           (b >> 28) & 7u, (c >> 6) & 7u, (c >> 15) & 7u, (c >> 24) & 7u};
    for (u32 t = 0; t < 8; t++)
        ATTR((hi >> (t * 2u)) & 3u, (tc_cnt[t] ? 2u : 1u) * comp_size(tc_fmt[t]), 1u);
#undef ATTR
    return size;
}

/* ---------------------------------------------------------------------------------------------
 * Draw finish (GXSetDrawDone, PE "finish" interrupt)
 * ------------------------------------------------------------------------------------------- */

static u32 sda_var(s16 off) { return s.sda + (u32)(s32)off; }

/* Like GXFinishInterruptHandler, without registers: DrawDone = 1, guest callback in a temporary exception context,
 * then OSWakeupThread(&FinishQueue). Called on the game thread by the native interrupt entry. */
static void on_finish(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    (void)interrupt;
    (void)user;
    s.finish_delivered++;
    if (!s.have_drawdone_vars)
        return;
    mem_write8(&g->state, sda_var(s.off_draw_done), 1);
    const u32 cb = mem_read32(&g->state, sda_var(s.off_draw_done_cb));
    if (cb) {
        const u32 tmp = gcrt_native_exception_context_begin(g);
        gcrt_native_callback(g, cb, NULL, 0);
        gcrt_native_exception_context_end(g, tmp, context);
    }
    if (s.fn_wakeup && g->stop == GCRT_STOP_NONE) {
        const u32 queue = sda_var(s.off_finish_queue);
        gcrt_native_callback(g, s.fn_wakeup, &queue, 1);
    }
    GCRT_TRACE(GCRT_TRACE_GX, "draw finish delivered (callback %08X)", cb);
}

/* BP 0x45 (draw finish) + dirty state + flush + DrawDone = 0, then the "finish" interrupt is posted: as on the
 * console, it fires as soon as the graphics processor has processed the command (here: packets handed over, reads
 * done). */
static void set_draw_done(GcrtCpu* g) {
    gxw_bp(0x45000002u);
    if (gx.dirty_state)
        gx_set_dirty_state(g);
    gx_flush_zeros();
    if (s.have_drawdone_vars)
        mem_write8(&g->state, sda_var(s.off_draw_done), 0);
    deliver_packets();
    s.draw_dones++;
    gcrt_nirq_post(g, IRQ_PI_PE_FINISH, g->ticks, on_finish, NULL);
}

static bool hle_set_draw_done(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    set_draw_done(g);
    gx_return(g, g->state.gpr[3]);
    return true;
}

/* GXDrawDone = GXSetDrawDone then GXWaitDrawDone (recompiled: it sleeps on the guest queue as long as DrawDone is 0):
 * the native part is done here, then the rest is handed over to GXWaitDrawDone, which returns to the caller. */
static u32 s_fn_wait_draw_done;
static bool hle_draw_done(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gxw_bp(0x45000002u);
    if (gx.dirty_state)
        gx_set_dirty_state(g);
    gx_flush_zeros();
    if (s.have_drawdone_vars)
        mem_write8(&g->state, sda_var(s.off_draw_done), 0);
    deliver_packets();
    s.draw_dones++;
    gcrt_nirq_post(g, IRQ_PI_PE_FINISH, g->ticks, on_finish, NULL);
    if (s_fn_wait_draw_done) {
        g->state.pc = s_fn_wait_draw_done; /* tail call: lr unchanged */
        return true;
    }
    gx_return(g, 0);
    return true;
}

/* GXFlush: dirty state, 32 zero bytes (packet pushed), PPCSync. */
static bool hle_flush(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    if (gx.dirty_state)
        gx_set_dirty_state(g);
    gx_flush_zeros();
    gx_return(g, 0);
    return true;
}

/* GXPixModeSync: BP pe_ctrl; bpSentNot = 0. */
static bool hle_pix_mode_sync(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gxw_bp(gx.pe_ctrl);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* GXAbortFrame / __GXAbort: the original resets the FIFO (PI 0x3018) after waiting for the memory counters to settle.
 * Native [hypothesis: reset and fatal error paths, not exercised]: the incomplete packet and the partial command of
 * the command processor are discarded, then, like the SDK, revision bits emitted again, clean state, 32 zero
 * bytes. */
static void abort_stream(void) {
    s.len = 0;
    gx_draw_pending = false;
    s.aborts++;
    if (s.have_sink && s.sink.abort)
        s.sink.abort(s.sink.user);
}

static bool hle_abort_frame(GcrtCpu* g, u32 address) {
    (void)address;
    abort_stream();
    u32 gp = 0;
    if (s.have_fifo_vars)
        gp = mem_read32(&g->state, sda_var(s.off_gp_fifo));
    if (gp) {
        gx_init_revision_bits();
        gx.dirty_state = 0;
        gx_flush_zeros();
    }
    gcrt_trace_printf(GCRT_TRACE_GX, "GXAbortFrame: GX stream discarded");
    gx_return(g, 0);
    return true;
}

static bool hle_abort(GcrtCpu* g, u32 address) {
    (void)address;
    abort_stream();
    gx_return(g, 0);
    return true;
}

/* GXSetMisc(token, val): 1 = XF flush (vNum), 2 = saving of the display list context, 3 = wait for the copy before
 * aborting. */
static bool hle_set_misc(GcrtCpu* g, u32 address) {
    (void)address;
    const s32 token = (s32)gx_arg(g, 0);
    const u32 val = gx_arg(g, 1);
    switch (token) {
    case 1:
        gx.vnum = (u16)val;
        gx.vnum_not = gx.vnum == 0 ? 1 : 0;
        gx.bp_sent_not = 1;
        if (gx.vnum)
            gx.dirty_state |= GX_DIRTY_VCD;
        break;
    case 2:
        gx.dl_save_context = val != 0;
        break;
    case 3:
        gx.abt_wait_pe_copy = val != 0;
        break;
    default:
        break;
    }
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * FIFO (no registers any more: only the guest variables read by GXGetCPUFifo/GXGetGPFifo are maintained)
 * ------------------------------------------------------------------------------------------- */

static void set_cpu_fifo(GcrtCpu* g, u32 fifo) {
    if (s.have_fifo_vars)
        mem_write32(&g->state, sda_var(s.off_cpu_fifo), fifo);
}

static void set_gp_fifo(GcrtCpu* g, u32 fifo) {
    if (s.have_fifo_vars)
        mem_write32(&g->state, sda_var(s.off_gp_fifo), fifo);
}

static bool hle_set_cpu_fifo(GcrtCpu* g, u32 address) {
    (void)address;
    set_cpu_fifo(g, gx_arg(g, 0));
    gx_return(g, 0);
    return true;
}

static bool hle_set_gp_fifo(GcrtCpu* g, u32 address) {
    (void)address;
    set_gp_fifo(g, gx_arg(g, 0));
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Initialization (GXInit)
 * ------------------------------------------------------------------------------------------- */

static void guest_call(GcrtCpu* g, u32 fn, const u32* args, unsigned n, u32* result) {
    if (fn)
        gcrt_guest_call(g, fn, args, n, result);
}

/* __GXInitRevisionBits: VAT revision bits (A bit 30, B bit 31) written to the CP, XF 0x1000 and 0x1012, BP 0x58. */
void gx_init_revision_bits(void) {
    for (u32 i = 0; i < 8; i++) {
        gx_set(&gx.vat_a[i], 1, 30, 1);
        gx_set(&gx.vat_b[i], 1, 31, 1);
        gxw_cp((u8)(0x80u | i), gx.vat_b[i]);
    }
    gxw_xf(0x1000, 0x3Fu);
    gxw_xf(0x1012, 1u);
    gxw_bp(0x5800000Fu);
}

static bool hle_init_revision_bits(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_init_revision_bits();
    gx_return(g, 0);
    return true;
}

static bool hle_init(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 base = gx_arg(g, 0), size = gx_arg(g, 1);
    CPUState* c = &g->state;
    gx_enter(g);
    /* (Initial state: that of the SDK structure in uninitialized memory, zero; no clearing, like the original, if
     * GXInit were called again.) */
    /* OSRegisterVersion(__GXVersion) */
    if (s.have_version && s.fn_register_version) {
        const u32 banner = mem_read32(c, sda_var(s.off_version));
        guest_call(g, s.fn_register_version, &banner, 1, NULL);
    }
    gx.in_disp_list = 0;
    gx.dl_save_context = 1;
    gx.abt_wait_pe_copy = 1;
    gx.tcs_man_enab = 0;
    gx.tev_tc_enab = 0;
    /* GXSetMisc(GX_MT_XF_FLUSH, 0) */
    gx.vnum = 0;
    gx.vnum_not = 1;
    gx.bp_sent_not = 1;
    /* __GXFifoInit: CP handler, CP interrupt unmasked, current thread, null FIFOs. */
    if (s.fn_set_handler && s.fn_cp_handler) {
        const u32 a[2] = {IRQ_PI_CP, s.fn_cp_handler};
        guest_call(g, s.fn_set_handler, a, 2, NULL);
    }
    gcrt_os_unmask_interrupts(g, 0x80000000u >> IRQ_PI_CP);
    if (s.have_fifo_init_vars) {
        u32 thread = 0;
        guest_call(g, s.fn_get_current_thread, NULL, 0, &thread);
        mem_write32(c, sda_var(s.off_fifo_thread), thread);
        mem_write32(c, sda_var(s.off_fifo_overflow), 0);
    }
    set_cpu_fifo(g, 0);
    set_gp_fifo(g, 0);
    /* GXInitFifoBase(&FifoObj, base, size) (recompiled); GXSetCPUFifo / GXSetGPFifo (linked mode). */
    if (s.fn_init_fifo_base && s.fifo_obj) {
        const u32 a[3] = {s.fifo_obj, base, size};
        guest_call(g, s.fn_init_fifo_base, a, 3, NULL);
    }
    set_cpu_fifo(g, s.fifo_obj);
    set_gp_fifo(g, s.fifo_obj);
    /* (OSRegisterResetFunction(__GXShutdown): not reproduced, the native OSResetSystem does not call the reset
     * functions.) */
    /* __GXPEInit: token and finish handlers, FinishQueue initialized, PE interrupts unmasked. */
    if (s.fn_set_handler) {
        if (s.fn_token_handler) {
            const u32 a[2] = {IRQ_PI_PE_TOKEN, s.fn_token_handler};
            guest_call(g, s.fn_set_handler, a, 2, NULL);
        }
        if (s.fn_finish_handler) {
            const u32 a[2] = {IRQ_PI_PE_FINISH, s.fn_finish_handler};
            guest_call(g, s.fn_set_handler, a, 2, NULL);
        }
    }
    if (s.have_drawdone_vars && s.fn_init_queue) {
        const u32 queue = sda_var(s.off_finish_queue);
        guest_call(g, s.fn_init_queue, &queue, 1, NULL);
    }
    gcrt_os_unmask_interrupts(g, 0x80000000u >> IRQ_PI_PE_TOKEN);
    gcrt_os_unmask_interrupts(g, 0x80000000u >> IRQ_PI_PE_FINISH);
    /* EnableWriteGatherPipe: WPAR = 0x0C008000, HID2[WPE] (processor registers, not peripheral ones). */
    if (s.fn_mfhid2 && s.fn_mtwpar && s.fn_mthid2) {
        u32 hid2 = 0;
        guest_call(g, s.fn_mfhid2, NULL, 0, &hid2);
        const u32 wpar = 0x0C008000u;
        guest_call(g, s.fn_mtwpar, &wpar, 1, NULL);
        const u32 v = hid2 | 0x40000000u;
        guest_call(g, s.fn_mthid2, &v, 1, NULL);
    }
    /* Shadow registers: genMode, BP mask, line and point size, TEV, SU, scissors, PE modes. */
    gx.gen_mode = 0;
    gx_set(&gx.gen_mode, 0x00, 24, 8);
    gx.bp_mask = 0xFF;
    gx_set(&gx.bp_mask, 0x0F, 24, 8);
    gx.lp_size = 0;
    gx_set(&gx.lp_size, 0x22, 24, 8);
    for (u32 i = 0; i < 16; i++) {
        gx.tevc[i] = 0;
        gx.teva[i] = 0;
        gx.tref[i / 2] = 0;
        gx.texmap_id[i] = 0xFF;
        gx_set(&gx.tevc[i], 0xC0 + i * 2, 24, 8);
        gx_set(&gx.teva[i], 0xC1 + i * 2, 24, 8);
        gx_set(&gx.tev_ksel[i / 2], 0xF6 + i / 2, 24, 8);
        gx_set(&gx.tref[i / 2], 0x28 + i / 2, 24, 8);
    }
    gx.iref = 0;
    gx_set(&gx.iref, 0x27, 24, 8);
    for (u32 i = 0; i < 8; i++) {
        gx.su_ts0[i] = 0;
        gx.su_ts1[i] = 0;
        gx_set(&gx.su_ts0[i], 0x30 + i * 2, 24, 8);
        gx_set(&gx.su_ts1[i], 0x31 + i * 2, 24, 8);
    }
    gx_set(&gx.su_scis0, 0x20, 24, 8);
    gx_set(&gx.su_scis1, 0x21, 24, 8);
    gx_set(&gx.cmode0, 0x41, 24, 8);
    gx_set(&gx.cmode1, 0x42, 24, 8);
    gx_set(&gx.zmode, 0x40, 24, 8);
    gx_set(&gx.pe_ctrl, 0x43, 24, 8);
    gx_set(&gx.cp_tex, 0, 7, 2);
    gx_fpu(g); /* first floating-point instruction of the original (lfs of zScale) */
    gx.z_scale = 16777216.0f;
    gx.z_offset = 0.0f;
    gx.dirty_state = 0;
    gx.dirty_vat = 0;
    /* Bus clock (low memory 0x800000F8) / 500: counters of BP 0x69 and 0x46. */
    const u32 bus = mem_read32(c, 0x800000F8u) / 500u;
    gx_flush_texture_state();
    gxw_bp((bus / 2048u) | 0x69000400u);
    gx_flush_texture_state();
    gxw_bp((bus / 4224u) | 0x46000200u);
    gx_init_revision_bits();
    /* Texture cache and TLUT regions (TMEM addresses of the SDK, read from the guest). */
    for (u32 i = 0; i < 8; i++) {
        u32 t[6] = {0};
        if (gx_tables.tex_regions)
            for (u32 k = 0; k < 6; k++)
                t[k] = mem_read32(c, gx_tables.tex_regions + 4u * (i + 8u * k));
        gx_init_tex_cache_region(&gx.tex_regions0[i], 0, t[0], 0, t[1], 0);
        gx_init_tex_cache_region(&gx.tex_regions1[i], 0, t[2], 0, t[3], 0);
        gx_init_tex_cache_region(&gx.tex_regions2[i], 1, t[4], 0, t[5], 0);
    }
    for (u32 i = 0; i < 16; i++)
        gx_init_tlut_region(&gx.tlut_regions[i], 0xC0000u + 0x2000u * i, 16);
    for (u32 i = 0; i < 4; i++)
        gx_init_tlut_region(&gx.tlut_regions[16 + i], 0xE0000u + 0x8000u * i, 64);
    /* (CP 0x06 = 0: selection of the performance counters, direct register without effect here.) */
    gx_set(&gx.perf_sel, 0, 4, 4);
    gxw_cp(0x20, gx.perf_sel);
    gxw_xf(0x1006, 0);
    gxw_bp(0x23000000u);
    gxw_bp(0x24000000u);
    gxw_bp(0x67000000u);
    gx_set_indirect_mask(0);
    gx_set_tmem_config(2);
    deliver_packets();
    /* __GXInitGX (recompiled): default state through the API (native functions). */
    if (s.fn_init_gx)
        guest_call(g, s.fn_init_gx, NULL, 0, NULL);
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native GXInit: guest FIFO %08X (base %08X, %u bytes)", s.fifo_obj, base,
                      size);
    gx_return(g, s.fifo_obj);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Draws and display lists
 * ------------------------------------------------------------------------------------------- */

void gx_set_dirty_state(GcrtCpu* g) {
    (void)g;
    const u32 d = gx.dirty_state;
    if (d & GX_DIRTY_SU_TEX)
        gx_set_su_tex_regs();
    /* (bit 2: __GXUpdateBPMask, empty in this SDK) */
    if (d & GX_DIRTY_GEN_MODE) {
        gxw_bp(gx.gen_mode);
        gx.bp_sent_not = 0;
    }
    if (d & GX_DIRTY_VCD) {
        gx_set_vcd();
    }
    if (d & GX_DIRTY_VAT) {
        gx_set_vat();
    }
    if (d & (GX_DIRTY_VCD | GX_DIRTY_VAT)) {
        gx_calculate_vlim();
    }
    gx.dirty_state = 0;
}

static bool hle_set_dirty_state(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set_dirty_state(g);
    gx_return(g, 0);
    return true;
}

static bool hle_send_flush_prim(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_send_flush_prim();
    gx_return(g, 0);
    return true;
}

/* GXBegin(type, fmt, n): dirty state, XF flush if requested, draw header; the vertices follow inline. */
static bool hle_begin(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 type = gx_arg(g, 0), fmt = gx_arg(g, 1), n = gx_arg(g, 2) & 0xFFFFu;
    gx_enter(g);
    if (gx.dirty_state)
        gx_set_dirty_state(g);
    if (gx.vnum_not == 0 && gx.bp_sent_not == 0)
        gx_send_flush_prim();
    gxw_u8((u8)(fmt | type));
    gxw_u16((u16)n);
    s.draws++;
    open_draw(gx_vertex_size(fmt) * n);
    gx_return(g, g->state.gpr[3]);
    return true;
}

/* GXCallDisplayList(list, nbytes): dirty state, XF flush if requested, command 0x40 (the list is read from RAM by the
 * command processor: it is data). */
static bool hle_call_display_list(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 list = gx_arg(g, 0), nbytes = gx_arg(g, 1);
    gx_enter(g);
    if (gx.dirty_state)
        gx_set_dirty_state(g);
    if (gx.vnum_not == 0 && gx.bp_sent_not == 0)
        gx_send_flush_prim();
    gxw_u8(0x40);
    gxw_u32(list);
    gxw_u32(nbytes);
    gx_return(g, 0);
    return true;
}

static bool hle_invalidate_vtx_cache(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gxw_u8(0x48);
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Performance counters (GXSetGPMetric, GXClearGPMetric): only the commands of the stream are reproduced; the direct
 * writes of CP registers (counter selection) have no purpose any more. Values: disassembly of the SDK.
 * ------------------------------------------------------------------------------------------- */

static bool hle_set_gp_metric(GcrtCpu* g, u32 address) {
    (void)address;
    const s32 perf0 = (s32)gx_arg(g, 0), perf1 = (s32)gx_arg(g, 1);
    gx_enter(g);
    /* Stop the previous counters. */
    const s32 old0 = (s32)gx.perf0;
    if (old0 == 0x22 || (old0 >= 0 && old0 < 0x0B))
        gxw_xf(0x1006, 0);
    else if (old0 >= 0x0B && old0 < 0x1B)
        gxw_bp(0x23000000u);
    else if (old0 >= 0x1B && old0 < 0x22)
        gxw_bp(0x24000000u);
    const s32 old1 = (s32)gx.perf1;
    if (old1 == 0x15 || (old1 >= 0 && old1 < 9)) {
        gxw_bp(0x67000000u);
    } else if (old1 >= 9 && old1 < 0x11) {
        gx_set(&gx.perf_sel, 0, 4, 4);
        gxw_cp(0x20, gx.perf_sel);
    }
    /* (0x11-0x14: direct CP register, no effect here) */
    gx.perf0 = (u32)perf0;
    static const u16 k_xf0[11] = {0x273, 0x14A, 0x16B, 0x084, 0x0C6, 0x210, 0x252, 0x231, 0x1AD, 0x1CE, 0x153};
    static const u32 k_bp0[23] = {0x2300AE7Fu, 0x23008E7Fu, 0x23009E7Fu, 0x23001E7Fu, 0x2300AC3Fu, 0x2300AC7Fu,
                                  0x2300ACBFu, 0x2300ACFFu, 0x2300AD3Fu, 0x2300AD7Fu, 0x2300ADBFu, 0x2300ADFFu,
                                  0x2300AE3Fu, 0x2300A27Fu, 0x2300A67Fu, 0x2300AA7Fu, 0x2402C0C6u, 0x2402C16Bu,
                                  0x2402C0E7u, 0x2402C108u, 0x2402C129u, 0x2402C14Au, 0x2402C1ADu};
    if (perf0 >= 0 && perf0 <= 9)
        gxw_xf(0x1006, k_xf0[perf0]);
    else if (perf0 == 34)
        gxw_xf(0x1006, 0x21);
    else if (perf0 == 10)
        gxw_xf(0x1006, k_xf0[10]);
    else if (perf0 >= 11 && perf0 <= 33)
        gxw_bp(k_bp0[perf0 - 11]);
    gx.perf1 = (u32)perf1;
    static const u16 k_bp1[10] = {0x42, 0x84, 0x63, 0x129, 0x14B, 0x18D, 0x1CF, 0x211, 0x252, 0x21};
    if (perf1 >= 0 && perf1 <= 8)
        gxw_bp(0x67000000u + k_bp1[perf1]);
    else if (perf1 == 21)
        gxw_bp(0x67000000u + 0x21u);
    else if (perf1 >= 9 && perf1 <= 16) {
        static const u8 k_sel[8] = {2, 3, 4, 5, 6, 7, 9, 8};
        gx_set(&gx.perf_sel, k_sel[perf1 - 9], 4, 4);
        gxw_cp(0x20, gx.perf_sel);
    }
    /* (17-20: direct CP register, no effect here) */
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_clear_gp_metric(GcrtCpu* g, u32 address) {
    (void)address;
    gx_return(g, 0); /* direct CP register (clearing of the counters): no effect here */
    return true;
}

/* GXPoke*: direct accesses of the processor to the EFB (PE registers); no processor access to the EFB in a native
 * port: recorded without effect. */
static bool hle_noop(GcrtCpu* g, u32 address) {
    (void)address;
    gx_return(g, 0);
    return true;
}

/* Dead functions (CP/PE interrupt handlers, internal FIFO, reset): a call to one of them reveals a forgotten path. */
static bool hle_dead(GcrtCpu* g, u32 address) {
    fprintf(stderr, "gcrt native GX: dead function called at %08X (lr %08X): ignored\n", address, g->state.lr);
    gx_return(g, 0);
    return true;
}

/* Default region callbacks: they are no longer called (the replacements use the host regions when the installed
 * callback is this one). A direct call by the game returns 0. */
static bool hle_default_region_cb(GcrtCpu* g, u32 address) {
    fprintf(stderr, "gcrt native GX: default region callback called directly at %08X (lr %08X): 0 returned\n",
            address, g->state.lr);
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Reading the recognized code
 * ------------------------------------------------------------------------------------------- */

/* `lis rA, H` at fn + lis_off then `addi rD, rA, L` (or subi) at fn + addi_off: returns (H << 16) + L. */
static bool decode_lis_addi(GcrtCpu* g, u32 fn, u32 lis_off, u32 addi_off, u32* out) {
    if (!fn)
        return false;
    const u32 w1 = mem_read32(&g->state, fn + lis_off), w2 = mem_read32(&g->state, fn + addi_off);
    const u32 ra = (w1 >> 21) & 31u;
    if ((w1 >> 26) != 15u || ((w1 >> 16) & 31u) != 0 || (w2 >> 26) != 14u || ((w2 >> 16) & 31u) != ra)
        return false;
    *out = ((w1 & 0xFFFFu) << 16) + (u32)(s32)(s16)(w2 & 0xFFFFu);
    return true;
}

static u32 fn_unique(const char* name) {
    unsigned n = 0;
    const u32 a = gcrt_hle_find(name, &n);
    return n == 1 ? a : 0;
}

static bool find_guest_variables(GcrtCpu* g) {
    s.sda = gcrt_native_os_sda();
    if (!s.sda)
        s.sda = g->state.gpr[13];
    bool ok = true;
    /* GXFinishInterruptHandler: lwz (#0: __peReg, #1: callback), stb DrawDone, addi r3, r13, &FinishQueue. */
    const u32 fin = fn_unique("GXFinishInterruptHandler");
    const u32 fin_size = gcrt_hle_catalog_size("GXFinishInterruptHandler");
    s.have_drawdone_vars = fin && gcrt_hle_decode_d_offset(g, fin, fin_size, 32, 13, 1, &s.off_draw_done_cb) &&
                           gcrt_hle_decode_d_offset(g, fin, fin_size, 38, 13, 0, &s.off_draw_done) &&
                           gcrt_hle_decode_d_offset(g, fin, fin_size, 14, 13, 0, &s.off_finish_queue);
    s.fn_finish_handler = fin;
    ok &= s.have_drawdone_vars;
    /* GXSetCPUFifo: stw &CPUFifo (#0), lwz &GPFifo (#0). */
    const u32 cpu = fn_unique("GXSetCPUFifo");
    const u32 cpu_size = gcrt_hle_catalog_size("GXSetCPUFifo");
    s.have_fifo_vars = cpu && gcrt_hle_decode_d_offset(g, cpu, cpu_size, 36, 13, 0, &s.off_cpu_fifo) &&
                       gcrt_hle_decode_d_offset(g, cpu, cpu_size, 32, 13, 0, &s.off_gp_fifo);
    ok &= s.have_fifo_vars;
    /* __GXFifoInit: stw __GXCurrentThread (#0), overflow flag (#1). */
    const u32 fi = fn_unique("__GXFifoInit");
    s.have_fifo_init_vars = fi && gcrt_hle_decode_d_offset(g, fi, gcrt_hle_catalog_size("__GXFifoInit"), 36, 13, 0,
                                                           &s.off_fifo_thread) &&
                            gcrt_hle_decode_d_offset(g, fi, gcrt_hle_catalog_size("__GXFifoInit"), 36, 13, 1,
                                                     &s.off_fifo_overflow);
    /* GXInit: lwz r3, __GXVersion(r13); static FIFO object (lis r4 / subi r31 at +0x18 / +0x20); texture region table
     * (lis r3 / subi r0 at +0x42C / +0x434). */
    const u32 init = fn_unique("GXInit");
    s.have_version = init && gcrt_hle_decode_d_offset(g, init, gcrt_hle_catalog_size("GXInit"), 32, 13, 0, &s.off_version);
    if (!decode_lis_addi(g, init, 0x18, 0x20, &s.fifo_obj)) {
        fprintf(stderr, "gcrt native GX: GXInit FIFO object not found (unexpected code)\n");
        ok = false;
    }
    if (!decode_lis_addi(g, init, 0x42C, 0x434, &gx_tables.tex_regions))
        fprintf(stderr, "gcrt native GX: GXInit texture region table not found\n");
    /* Tables of GXSetTevOp (+4/+8), GXSetTevOrder (+0xB8/+0xC0), GXSetPixelFmt (+0/+0xC). */
    if (!decode_lis_addi(g, fn_unique("GXSetTevOp"), 0x4, 0x8, &gx_tables.tev_op))
        fprintf(stderr, "gcrt native GX: GXSetTevOp tables not found\n"), ok = false;
    if (!decode_lis_addi(g, fn_unique("GXSetTevOrder"), 0xB8, 0xC0, &gx_tables.tev_order_c2r))
        fprintf(stderr, "gcrt native GX: GXSetTevOrder table not found\n"), ok = false;
    if (!decode_lis_addi(g, fn_unique("GXSetPixelFmt"), 0x0, 0xC, &gx_tables.pixel_fmt))
        fprintf(stderr, "gcrt native GX: GXSetPixelFmt table not found\n"), ok = false;
    gx_tables.ok = ok;
    return ok;
}

/* ---------------------------------------------------------------------------------------------
 * Installation and reports
 * ------------------------------------------------------------------------------------------- */

static GcrtCpu* s_install_g;

void gx_replace(GcrtCpu* g, const char* name, GcrtHleFn fn, const char* const* following, unsigned following_count,
                const char* preceding) {
    s.installed_count += gcrt_native_replace(g, name, "GX", fn, following, following_count, preceding, s.missing,
                                             sizeof(s.missing));
}

void gcrt_native_gx_install(GcrtCpu* g) {
    s_g = g;
    s_install_g = g;
    s.missing[0] = '\0';
    s.installed_count = 0;
    if (!fn_unique("GXInit") || !fn_unique("GXBegin")) {
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "native GX: GX library not recognized (GXInit/GXBegin absent)");
        return;
    }
    if (!find_guest_variables(g)) {
        fprintf(stderr, "gcrt native GX: ERROR: variables or tables of the SDK GX not found (SDK version not "
                        "recognized?): drawing cannot be replaced\n");
        return;
    }
    s.fn_init_fifo_base = fn_unique("GXInitFifoBase");
    s.fn_init_gx = fn_unique("__GXInitGX");
    s.fn_register_version = fn_unique("OSRegisterVersion");
    s.fn_set_handler = fn_unique("__OSSetInterruptHandler");
    s.fn_init_queue = fn_unique("OSInitThreadQueue");
    s.fn_wakeup = fn_unique("OSWakeupThread");
    s.fn_get_current_thread = fn_unique("OSGetCurrentThread");
    s.fn_mfhid2 = fn_unique("PPCMfhid2");
    s.fn_mtwpar = fn_unique("PPCMtwpar");
    s.fn_mthid2 = fn_unique("PPCMthid2");
    s.fn_cp_handler = fn_unique("GXCPInterruptHandler");
    s.fn_token_handler = fn_unique("GXTokenInterruptHandler");
    s_fn_wait_draw_done = fn_unique("GXWaitDrawDone");
    if (!s.fn_init_gx || !s.fn_init_fifo_base)
        fprintf(stderr, "gcrt native GX: __GXInitGX or GXInitFifoBase not found: incomplete initialization\n");

    /* Initialization, FIFO, synchronization */
    gx_replace(g, "GXInit", hle_init, NULL, 0, NULL);
    gx_replace(g, "__GXInitRevisionBits", hle_init_revision_bits, NULL, 0, NULL);
    gx_replace(g, "GXSetCPUFifo", hle_set_cpu_fifo, NULL, 0, NULL);
    gx_replace(g, "GXSetGPFifo", hle_set_gp_fifo, NULL, 0, NULL);
    gx_replace(g, "GXSetMisc", hle_set_misc, NULL, 0, NULL);
    gx_replace(g, "GXFlush", hle_flush, NULL, 0, NULL);
    gx_replace(g, "GXAbortFrame", hle_abort_frame, NULL, 0, NULL);
    gx_replace(g, "__GXAbort", hle_abort, NULL, 0, NULL);
    gx_replace(g, "GXSetDrawDone", hle_set_draw_done, NULL, 0, NULL);
    gx_replace(g, "GXDrawDone", hle_draw_done, NULL, 0, NULL);
    gx_replace(g, "GXPixModeSync", hle_pix_mode_sync, NULL, 0, NULL);
    /* Draws, display lists, dirty state */
    gx_replace(g, "GXBegin", hle_begin, NULL, 0, NULL);
    gx_replace(g, "__GXSendFlushPrim", hle_send_flush_prim, NULL, 0, NULL);
    gx_replace(g, "__GXSetDirtyState", hle_set_dirty_state, NULL, 0, NULL);
    gx_replace(g, "GXCallDisplayList", hle_call_display_list, NULL, 0, NULL);
    gx_replace(g, "GXInvalidateVtxCache", hle_invalidate_vtx_cache, NULL, 0, NULL);
    /* Performance counters */
    gx_replace(g, "GXSetGPMetric", hle_set_gp_metric, NULL, 0, NULL);
    gx_replace(g, "GXClearGPMetric", hle_clear_gp_metric, NULL, 0, NULL);
    /* Direct processor accesses to the EFB (PE): no effect */
    static const char* const pokes[] = {"GXPokeAlphaMode", "GXPokeAlphaRead",   "GXPokeAlphaUpdate", "GXPokeBlendMode",
                                        "GXPokeColorUpdate", "GXPokeDstAlpha", "GXPokeDither",      "GXPokeZMode"};
    for (size_t i = 0; i < sizeof(pokes) / sizeof(pokes[0]); i++)
        gx_replace(g, pokes[i], hle_noop, NULL, 0, NULL);
    /* Dead: interrupt handlers, internal FIFO, PE, reset */
    static const char* const dead[] = {"GXCPInterruptHandler", "GXTokenInterruptHandler", "GXFinishInterruptHandler",
                                       "__GXPEInit",           "__GXFifoInit",            "__GXFifoReadEnable",
                                       "__GXFifoReadDisable",  "__GXFifoLink",            "__GXWriteFifoIntEnable",
                                       "__GXWriteFifoIntReset", "__GXCleanGPFifo",        "__GXShutdown"};
    for (size_t i = 0; i < sizeof(dead) / sizeof(dead[0]); i++)
        gx_replace(g, dead[i], hle_dead, NULL, 0, NULL);
    gx_replace(g, "__GXDefaultTexRegionCallback", hle_default_region_cb, NULL, 0, NULL);
    gx_replace(g, "__GXDefaultTlutRegionCallback", hle_default_region_cb, NULL, 0, NULL);

    gcrt_native_gx_attr_install(g);
    gcrt_native_gx_tev_install(g);
    gcrt_native_gx_draw_install(g);
    s.installed = true;
    gcrt_trace_printf(GCRT_TRACE_NATIVE,
                      "native GX: %u functions replaced, missing: %s; DrawDone r13%+d, callback r13%+d, FinishQueue "
                      "r13%+d, CPU/GP FIFO r13%+d/%+d, FIFO object %08X",
                      s.installed_count, s.missing[0] ? s.missing : "none", s.off_draw_done, s.off_draw_done_cb,
                      s.off_finish_queue, s.off_cpu_fifo, s.off_gp_fifo, s.fifo_obj);
}

bool gcrt_native_gx_active(void) { return s.installed; }

void gcrt_native_gx_stats(GcrtGxStats* out) {
    memset(out, 0, sizeof(*out));
    out->draw_dones = s.draw_dones;
    out->finish_delivered = s.finish_delivered;
    out->draws = s.draws;
    out->inline_bytes = s.inline_bytes;
    out->pipe_writes = s.pipe_writes;
    out->pipe_out_writes = s.pipe_out_writes;
    out->packets = s.packets;
}

void gcrt_native_gx_report(FILE* out) {
    if (!s.installed) {
        fprintf(out, "native GX: not installed\n");
        return;
    }
    fprintf(out,
            "native GX: %llu draws opened by GXBegin, %llu inline vertex bytes (%llu writes to the pipe, of which "
            "%llu outside a draw), %llu 32-byte packets handed over, %llu draw finishes (%llu delivered), %llu GX "
            "calls before the end of a draw, %llu aborts; %llu vertex bytes produced natively (SDK shapes, of which "
            "%llu outside a draw); %u functions replaced, missing: %s\n",
            (unsigned long long)s.draws, (unsigned long long)s.inline_bytes, (unsigned long long)s.pipe_writes,
            (unsigned long long)s.pipe_out_writes, (unsigned long long)s.packets, (unsigned long long)s.draw_dones,
            (unsigned long long)s.finish_delivered, (unsigned long long)s.native_during_draw,
            (unsigned long long)s.aborts, (unsigned long long)s.native_vertex_bytes,
            (unsigned long long)s.native_vertex_outside, s.installed_count, s.missing[0] ? s.missing : "none");
    gcrt_native_gx_draw_report(out);
}
