/* Native GX drawing (milestone 10c): internal to the hle/native_gx*.c files. See native_gx.c for the principle.
 *
 * The state of the SDK GX library ("__GXData") is kept by the host, in GxState, with the same fields and the same
 * bit arithmetic as the code recognized by signature (SDK GX whose banner is dated November 10, 2004): no dependency
 * on the layout of the structure in guest memory. The functions write the same commands (BP 0x61, CP 0x08, XF 0x10,
 * draws 0x80-0xBF, display lists 0x40...) as the original, in the same order; the stream is handed over to Aurora's
 * command processor (bridge gx/gx_bridge) in 32-byte packets, at the same positions as the console's graphics
 * processor received it. Generic: no reference to a game. */
#ifndef GCRT_NATIVE_GX_H
#define GCRT_NATIVE_GX_H

#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Texture cache region (SDK GXTexRegion, 16 bytes) and TLUT region (GXTlutRegion, 16 bytes). */
typedef struct GxTexRegion {
    u32 image1, image2;
    u16 size_even, size_odd;
    u8 is32b_mipmap, is_cached;
} GxTexRegion;

typedef struct GxTlutRegion {
    u32 w[4]; /* +0 TLUT load (BP 0x65), +4 word 0 of the TLUT object, +8 word 1 (BP 0x64), +C word 2 */
} GxTlutRegion;

/* SDK state (fields of __GXData; the original offset is given for review). */
typedef struct GxState {
    u16 vnum_not, bp_sent_not, vnum, vlim; /* +0 +2 +4 +6 */
    u32 vcd_lo, vcd_hi;                     /* +0x14 +0x18 */
    u32 vat_a[8], vat_b[8], vat_c[8];       /* +0x1C +0x3C +0x5C */
    u32 lp_size;                            /* +0x7C */
    u32 mat_idx_a, mat_idx_b;               /* +0x80 +0x84 */
    u32 amb_color[2], mat_color[2];         /* +0xA8 +0xB0 */
    u32 su_ts0[8], su_ts1[8];               /* +0xB8 +0xD8 */
    u32 su_scis0, su_scis1;                 /* +0xF8 +0xFC */
    u32 tref[8];                            /* +0x100 */
    u32 iref;                               /* +0x120 */
    u32 bp_mask;                            /* +0x124 */
    u32 ind_tex_scale0, ind_tex_scale1;     /* +0x128 +0x12C */
    u32 tevc[16], teva[16];                 /* +0x130 +0x170 */
    u32 tev_ksel[8];                        /* +0x1B0 */
    u32 cmode0, cmode1, zmode, pe_ctrl;     /* +0x1D0 +0x1D4 +0x1D8 +0x1DC */
    u32 cp_disp_src, cp_disp_size, cp_disp_stride, cp_disp; /* +0x1E0.. */
    u32 cp_tex_src, cp_tex_size, cp_tex_stride, cp_tex;     /* +0x1F0.. */
    u8 cp_tex_z;                            /* +0x200 */
    u32 gen_mode;                           /* +0x204 */
    GxTexRegion tex_regions0[8], tex_regions1[8], tex_regions2[8]; /* +0x208 +0x288 +0x308 */
    GxTlutRegion tlut_regions[20];          /* +0x388 */
    u32 tex_region_cb, tlut_region_cb;      /* +0x4C8 +0x4CC (guest addresses) */
    u32 nrm_type;                           /* +0x4D0 */
    u8 has_nrms, has_bi_nrms;               /* +0x4D4 +0x4D5 */
    u32 proj_type;                          /* +0x4D8 */
    u32 proj_mtx[6];                        /* +0x4DC (float bits) */
    float vp_left, vp_top, vp_wd, vp_ht, vp_nearz, vp_farz; /* +0x4F4.. */
    float z_offset, z_scale;                /* +0x50C +0x510 */
    u32 t_image0[8], t_mode0[8];            /* +0x514 +0x534 */
    u32 texmap_id[16];                      /* +0x554 */
    u32 tcs_man_enab, tev_tc_enab;          /* +0x594 +0x598 */
    u32 perf0, perf1, perf_sel;             /* +0x59C +0x5A0 +0x5A4 */
    u8 in_disp_list, dl_save_context, abt_wait_pe_copy, dirty_vat; /* +0x5A8.. */
    u32 dirty_state;                        /* +0x5AC */
} GxState;

extern GxState gx;

/* Bits of dirty_state (order of __GXSetDirtyState). */
enum { GX_DIRTY_SU_TEX = 1u, GX_DIRTY_BP_MASK = 2u, GX_DIRTY_GEN_MODE = 4u, GX_DIRTY_VCD = 8u, GX_DIRTY_VAT = 0x10u };

/* --- Stream writing (native_gx.c) --- */
void gxw_u8(u8 v);
void gxw_u16(u16 v);
void gxw_u32(u32 v);
static inline void gxw_bp(u32 v) {
    gxw_u8(0x61);
    gxw_u32(v);
}
static inline void gxw_cp(u8 reg, u32 v) {
    gxw_u8(0x08);
    gxw_u8(reg);
    gxw_u32(v);
}
static inline void gxw_xf(u32 reg, u32 v) {
    gxw_u8(0x10);
    gxw_u32(reg);
    gxw_u32(v);
}
/* Replaces bits [shift, shift + width) of *reg with value (like rlwimi: the value is truncated to the field). */
static inline void gx_set(u32* reg, u32 value, unsigned shift, unsigned width) {
    const u32 mask = (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u)) << shift;
    *reg = (*reg & ~mask) | ((value << shift) & mask);
}
static inline u32 gx_get(u32 reg, unsigned shift, unsigned width) {
    return (reg >> shift) & (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u));
}
static inline u32 f2u(float f) {
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}
static inline float u2f(u32 v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}

/* Single-precision floating-point arithmetic of the recompiled code (DolRecomp cpu.c, ppc_fadds/fsubs/fmuls/fdivs):
 * computed in double, then rounded once to single. force_25_bit of fmuls has no effect on a frC operand that is already
 * single precision, which is always the case here. Not reproduced (issue 48):
 *   - FPSCR[NI] = 1 (non-IEEE mode), which the SDK's OSInit sets (PPCSetFpNonIEEEMode, mtfsb1 29; US and PAL
 *     disassembly): cpu.c (force_single) then turns a result whose double value is below 2^-126 in magnitude into a
 *     zero of its sign, where these helpers return the IEEE value (a subnormal, or 2^-126 when the value rounds up to
 *     it). A sum or difference of two singles gives such a result only with an operand below 2^-103 in magnitude, and
 *     256 / y (GXSetDispCopyYScale) never does for a finite single y. Products and the other quotients (the viewport
 *     and fog fields) have no such bound here: they are covered only by the measurement of issue 48, which found no
 *     such result, so the helpers keep the IEEE value; a caller that can receive tiny operands must flush like
 *     force_single;
 *   - a NaN result: cpu.c returns the first NaN operand made quiet, else the default NaN 0x7FC00000; the host returns
 *     its own, which depends on the host (for example 0xFFC00000 on x86 for an invalid operation such as inf - inf);
 *   - the FPSCR status bits (FPRF, FI, FR, sticky exceptions) are not updated; no exception is enabled. */
static inline float gx_fadds(double a, double b) { return (float)(a + b); }
static inline float gx_fsubs(double a, double b) { return (float)(a - b); }
static inline float gx_fmuls(double a, double b) { return (float)(a * b); }
static inline float gx_fdivs(double a, double b) { return (float)(a / b); }
/* __cvt_fp2unsigned of the Metrowerks runtime (double -> u32 conversion by truncation, clamped). */
u32 gx_cvt_fp2unsigned(double x);

/* Draw opened by GXBegin whose vertices have not all arrived yet; to be checked on entry to each replacement that
 * writes to the stream (diagnostic: the stream stays that of the original). */
extern bool gx_draw_pending;
void gx_check_draw_open(GcrtCpu* g);
static inline void gx_enter(GcrtCpu* g) {
    if (gx_draw_pending)
        gx_check_draw_open(g);
}

/* The original function executes floating-point instructions (floating-point arguments, psq_l...): if the current
 * thread does not own the FPU (MSR[FP] = 0), its first floating-point instruction would have raised "FPU unavailable"
 * and the OS would have given it back its floating-point registers. Same effect here, before reading or writing a
 * floating-point register (otherwise we would read those of another thread). Without the native OS: nothing (the
 * replacement then reads the registers as they are). */
static inline void gx_fpu(GcrtCpu* g) {
    if (!(g->state.msr & 0x00002000u) && gcrt_native_os_active())
        gcrt_native_os_fpu_claim(g, g->state.pc);
}

/* --- Guest access --- */
static inline u32 gx_arg(GcrtCpu* g, unsigned i) { return g->state.gpr[3 + i]; }
static inline double gx_farg(GcrtCpu* g, unsigned i) { return g->state.fpr[1 + i]; }
/* End of a replacement: r3 = result, return to the caller, complete packets handed over to the command processor. */
void gx_return(GcrtCpu* g, u32 result);

/* --- Internal SDK functions, shared (same effects as the originals) --- */
void gx_set_dirty_state(GcrtCpu* g);      /* __GXSetDirtyState */
void gx_send_flush_prim(void);            /* __GXSendFlushPrim */
void gx_set_matrix_index(u32 attr);       /* __GXSetMatrixIndex */
void gx_flush_texture_state(void);        /* __GXFlushTextureState */
void gx_set_indirect_mask(u32 mask);      /* __GXSetIndirectMask */
void gx_set_tmem_config(u32 config);      /* __GXSetTmemConfig */
void gx_init_revision_bits(void);         /* __GXInitRevisionBits */
void gx_set_su_tex_regs(void);            /* __GXSetSUTexRegs */
void gx_set_vcd(void);                    /* __GXSetVCD */
void gx_set_vat(void);                    /* __GXSetVAT */
void gx_calculate_vlim(void);             /* __GXCalculateVLim */
void gx_flush_zeros(void);                /* 8 zero words (GXFlush, GXSetDrawDone) */
u32 gx_vertex_size(u32 fmt);              /* size of a vertex of the current format (host VCD/VAT) */
void gx_get_image_tile_count(u32 fmt, u32 wd, u32 ht, u32* row, u32* col, u32* cmp); /* __GetImageTileCount */
void gx_init_tex_cache_region(GxTexRegion* r, u32 is32b_mipmap, u32 tmem_even, u32 size_even, u32 tmem_odd,
                              u32 size_odd); /* GXInitTexCacheRegion */
void gx_init_tlut_region(GxTlutRegion* r, u32 tmem, u32 size); /* GXInitTlutRegion */

/* SDK tables read from guest memory (addresses found in the recognized code; no data copied). */
typedef struct GxSdkTables {
    bool ok;
    u32 tev_op;       /* GXSetTevOp: 4 tables of 5 words (color stage 0, following stages, alpha stage 0, following) */
    u32 tev_order_c2r; /* GXSetTevOrder: color channel -> rasterizer selection */
    u32 pixel_fmt;    /* GXSetPixelFmt: pixel format -> PE format */
    u32 tex_regions;  /* GXInit: TMEM addresses of the texture regions (6 x 8 words) */
} GxSdkTables;
extern GxSdkTables gx_tables;

/* Replacement of a catalog function (optional anchoring); counts the installed and the missing ones. */
void gx_replace(GcrtCpu* g, const char* name, GcrtHleFn fn, const char* const* following, unsigned following_count,
                const char* preceding);

/* Vertex produced by a native replacement (SDK shapes, native_gx_draw.c): appended to the stream of the draw opened
 * by GXBegin, exactly like a 4-byte write to the pipe, but without any hardware access. */
void gx_emit_vertex_u32(u32 v);

/* Installation of the submodules. */
void gcrt_native_gx_attr_install(GcrtCpu* g);
void gcrt_native_gx_tev_install(GcrtCpu* g);
void gcrt_native_gx_draw_install(GcrtCpu* g);
/* Report of the SDK shapes (native_gx_draw.c). */
void gcrt_native_gx_draw_report(FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_NATIVE_GX_H */
