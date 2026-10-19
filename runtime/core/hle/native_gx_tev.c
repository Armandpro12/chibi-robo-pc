/* Native GX drawing (milestone 10c): textures and TLUTs (decoded guest objects, TMEM regions), coordinate size
 * registers (SU), indirect textures, TEV combiners, pixel operations and fog. Each replacement redoes the recognized
 * function of the SDK GX (disassembly read function by function). See native_gx.c. Generic. */
#include "hle/native_gx.h"
#include "platform/trace.h"

#include <stdio.h>
#include <stdlib.h>

static u32 s_default_tex_cb, s_default_tlut_cb; /* guest addresses of the SDK default region callbacks */
static u64 s_guest_region_calls;

/* GCRT_GX_TEV_COLOR_TRACE=1 (diagnostic): one stderr line per GXSetTevColor / GXSetTevKColor call,
 * "TEVCOLOR C|K <id> <RRGGBBAA> <return address>", to compare the colors the game passes with another run or an
 * emulator (game code may build them partly from uninitialized stack memory). */
static int s_tev_color_trace = -1;

static void tev_color_trace(GcrtCpu* g, char kind, u32 id, u32 color) {
    if (s_tev_color_trace < 0) {
        const char* e = getenv("GCRT_GX_TEV_COLOR_TRACE");
        s_tev_color_trace = e && e[0] == '1';
    }
    if (s_tev_color_trace)
        fprintf(stderr, "TEVCOLOR %c %u %08X %08X\n", kind, (unsigned)id, (unsigned)color, (unsigned)g->state.lr);
}

/* BP register identifiers of a texture map (register map of the graphics processor: maps 0-3 at 0x80+, 4-7 at
 * 0xA0+; same values as the SDK tables). */
static u32 tex_reg(u32 base, u32 map) {
    map &= 7u;
    return map < 4u ? base + map : base + 0x20u + (map - 4u);
}

/* ---------------------------------------------------------------------------------------------
 * TMEM regions
 * ------------------------------------------------------------------------------------------- */

void gx_init_tex_cache_region(GxTexRegion* r, u32 is32b_mipmap, u32 tmem_even, u32 size_even, u32 tmem_odd,
                              u32 size_odd) {
    u32 sz = 0;
    switch ((s32)size_even) {
    case 0: sz = 3; break;
    case 1: sz = 4; break;
    case 2: sz = 5; break;
    default: break;
    }
    r->image1 = 0;
    gx_set(&r->image1, tmem_even >> 5, 0, 15);
    gx_set(&r->image1, sz, 15, 3);
    gx_set(&r->image1, sz, 18, 3);
    gx_set(&r->image1, 0, 21, 1);
    switch ((s32)size_odd) {
    case 0: sz = 3; break;
    case 1: sz = 4; break;
    case 2: sz = 5; break;
    case 3: sz = 0; break;
    default: break;
    }
    r->image2 = 0;
    gx_set(&r->image2, tmem_odd >> 5, 0, 15);
    gx_set(&r->image2, sz, 15, 3);
    gx_set(&r->image2, sz, 18, 3);
    r->is32b_mipmap = (u8)is32b_mipmap;
    r->is_cached = 1;
}

void gx_init_tlut_region(GxTlutRegion* r, u32 tmem, u32 size) {
    r->w[0] = 0;
    gx_set(&r->w[0], (tmem - 0x80000u) >> 9, 0, 10);
    gx_set(&r->w[0], size, 10, 11);
    gx_set(&r->w[0], 0x65, 24, 8);
}

/* Texture region: the host one (SDK default callback) or a guest structure (game callback). */
typedef struct {
    GxTexRegion* host;
    u32 guest;
} TexRegionRef;

static TexRegionRef tex_region_for(GcrtCpu* g, u32 obj, u32 map) {
    TexRegionRef r = {NULL, 0};
    if (gx.tex_region_cb == s_default_tex_cb) {
        /* __GXDefaultTexRegionCallback: RGBA8 -> regions 2 (mipmap) or 1; indexed colors -> 0; others -> 1
         * (mipmap) or 0. */
        const u32 fmt = mem_read32(&g->state, obj + 0x14);
        const bool mip = (mem_read8(&g->state, obj + 0x1F) & 1u) == 1u;
        const s32 id = (s32)map % 8;
        const u32 i = (u32)id & 7u;
        switch (fmt) {
        case 6:
            r.host = mip ? &gx.tex_regions2[i] : &gx.tex_regions1[i];
            break;
        case 8: case 9: case 10:
            r.host = &gx.tex_regions0[i];
            break;
        default:
            r.host = mip ? &gx.tex_regions1[i] : &gx.tex_regions0[i];
            break;
        }
        return r;
    }
    if (gx.tex_region_cb) {
        const u32 args[2] = {obj, map};
        s_guest_region_calls++;
        gcrt_guest_call(g, gx.tex_region_cb, args, 2, &r.guest);
    }
    return r;
}

/* TLUT region: the host one (default callback: index < 20) or a guest structure. */
typedef struct {
    GxTlutRegion* host;
    u32 guest;
} TlutRegionRef;

static TlutRegionRef tlut_region_for(GcrtCpu* g, u32 name) {
    TlutRegionRef r = {NULL, 0};
    if (gx.tlut_region_cb == s_default_tlut_cb) {
        if (name < 20u)
            r.host = &gx.tlut_regions[name];
        return r;
    }
    if (gx.tlut_region_cb) {
        s_guest_region_calls++;
        gcrt_guest_call(g, gx.tlut_region_cb, &name, 1, &r.guest);
    }
    return r;
}

static u32 tlut_get(GcrtCpu* g, TlutRegionRef r, unsigned i) {
    if (r.host)
        return r.host->w[i];
    return r.guest ? mem_read32(&g->state, r.guest + 4u * i) : 0;
}

static void tlut_put(GcrtCpu* g, TlutRegionRef r, unsigned i, u32 v) {
    if (r.host)
        r.host->w[i] = v;
    else if (r.guest)
        mem_write32(&g->state, r.guest + 4u * i, v);
}

/* ---------------------------------------------------------------------------------------------
 * Textures and TLUTs
 * ------------------------------------------------------------------------------------------- */

/* GXLoadTexObjPreLoaded(obj, region, map): register identifiers set in the object and the region (guest or host
 * memory), BP mode0/mode1/image0/image1/image2/image3, TLUT of the indexed colors. */
static void load_tex_obj_preloaded(GcrtCpu* g, u32 obj, TexRegionRef region, u32 map) {
    CPUState* c = &g->state;
    u32 mode0 = mem_read32(c, obj + 0x0), mode1 = mem_read32(c, obj + 0x4), image0 = mem_read32(c, obj + 0x8);
    u32 image3 = mem_read32(c, obj + 0xC);
    gx_set(&mode0, tex_reg(0x80, map), 24, 8);
    gx_set(&mode1, tex_reg(0x84, map), 24, 8);
    gx_set(&image0, tex_reg(0x88, map), 24, 8);
    gx_set(&image3, tex_reg(0x94, map), 24, 8);
    mem_write32(c, obj + 0x0, mode0);
    mem_write32(c, obj + 0x4, mode1);
    mem_write32(c, obj + 0x8, image0);
    u32 image1 = 0, image2 = 0;
    if (region.host) {
        gx_set(&region.host->image1, tex_reg(0x8C, map), 24, 8);
        gx_set(&region.host->image2, tex_reg(0x90, map), 24, 8);
        image1 = region.host->image1;
        image2 = region.host->image2;
    } else if (region.guest) {
        image1 = mem_read32(c, region.guest + 0);
        image2 = mem_read32(c, region.guest + 4);
        gx_set(&image1, tex_reg(0x8C, map), 24, 8);
        gx_set(&image2, tex_reg(0x90, map), 24, 8);
        mem_write32(c, region.guest + 0, image1);
        mem_write32(c, region.guest + 4, image2);
    }
    mem_write32(c, obj + 0xC, image3);
    gxw_bp(mode0);
    gxw_bp(mode1);
    gxw_bp(image0);
    gxw_bp(image1);
    gxw_bp(image2);
    gxw_bp(image3);
    if (!(mem_read8(c, obj + 0x1F) & 2u)) {
        const TlutRegionRef t = tlut_region_for(g, mem_read32(c, obj + 0x18));
        u32 tl = tlut_get(g, t, 1);
        gx_set(&tl, tex_reg(0x98, map), 24, 8);
        tlut_put(g, t, 1, tl);
        gxw_bp(tl);
    }
    gx.t_image0[map & 7u] = image0;
    gx.t_mode0[map & 7u] = mode0;
    gx.dirty_state |= GX_DIRTY_SU_TEX;
    gx.bp_sent_not = 0;
}

static bool hle_load_tex_obj_preloaded(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const TexRegionRef r = {NULL, gx_arg(g, 1)};
    load_tex_obj_preloaded(g, gx_arg(g, 0), r, gx_arg(g, 2));
    gx_return(g, 0);
    return true;
}

static bool hle_load_tex_obj(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 obj = gx_arg(g, 0), map = gx_arg(g, 1);
    const TexRegionRef r = tex_region_for(g, obj, map);
    load_tex_obj_preloaded(g, obj, r, map);
    gx_return(g, 0);
    return true;
}

/* GXLoadTlut(tlutObj, name): BP 0x64 (address), BP 0x65 (load into the TMEM), then region and object updated. */
static bool hle_load_tlut(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    CPUState* c = &g->state;
    const u32 obj = gx_arg(g, 0);
    const TlutRegionRef r = tlut_region_for(g, gx_arg(g, 1));
    gx_flush_texture_state();
    gxw_bp(mem_read32(c, obj + 4));
    gxw_bp(tlut_get(g, r, 0));
    gx_flush_texture_state();
    u32 w0 = mem_read32(c, obj + 0);
    gx_set(&w0, tlut_get(g, r, 0), 0, 10);
    mem_write32(c, obj + 0, w0);
    tlut_put(g, r, 1, w0);
    tlut_put(g, r, 2, mem_read32(c, obj + 4));
    tlut_put(g, r, 3, mem_read32(c, obj + 8));
    gx_return(g, 0);
    return true;
}

static bool hle_invalidate_tex_all(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_flush_texture_state();
    gxw_bp(0x66001000u);
    gxw_bp(0x66001100u);
    gx_flush_texture_state();
    gx_return(g, 0);
    return true;
}

static bool hle_set_tex_region_callback(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 old = gx.tex_region_cb;
    gx.tex_region_cb = gx_arg(g, 0);
    gx_return(g, old);
    return true;
}

static bool hle_set_tlut_region_callback(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 old = gx.tlut_region_cb;
    gx.tlut_region_cb = gx_arg(g, 0);
    gx_return(g, old);
    return true;
}

/* __SetSURegs(map, coord): texture size and wrap mode in the SU registers of the coordinate. */
static void set_su_regs(u32 map, u32 coord) {
    map &= 7u;
    coord &= 7u;
    const u32 image0 = gx.t_image0[map], mode0 = gx.t_mode0[map];
    gx_set(&gx.su_ts0[coord], image0 & 0x3FFu, 0, 16);
    gx_set(&gx.su_ts1[coord], (image0 >> 10) & 0x3FFu, 0, 16);
    gx_set(&gx.su_ts0[coord], (mode0 & 3u) == 1u, 16, 1);
    gx_set(&gx.su_ts1[coord], ((mode0 >> 2) & 3u) == 1u, 16, 1);
    gxw_bp(gx.su_ts0[coord]);
    gxw_bp(gx.su_ts1[coord]);
    gx.bp_sent_not = 0;
}

static bool hle_set_su_regs(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    set_su_regs(gx_arg(g, 0), gx_arg(g, 1));
    gx_return(g, 0);
    return true;
}

/* __GXSetSUTexRegs: indirect stages then TEV stages that read a texture (coordinates not set manually). */
void gx_set_su_tex_regs(void) {
    if (gx.tcs_man_enab == 0xFFu)
        return;
    const u32 n_stages = ((gx.gen_mode >> 10) & 0xFu) + 1u;
    const u32 n_ind = (gx.gen_mode >> 16) & 7u;
    u32 map = 0, coord = 0;
    for (u32 i = 0; i < n_ind; i++) {
        switch (i) {
        case 0: map = gx.iref & 7u, coord = (gx.iref >> 3) & 7u; break;
        case 1: map = (gx.iref >> 6) & 7u, coord = (gx.iref >> 9) & 7u; break;
        case 2: map = (gx.iref >> 12) & 7u, coord = (gx.iref >> 15) & 7u; break;
        case 3: map = (gx.iref >> 18) & 7u, coord = (gx.iref >> 21) & 7u; break;
        default: break;
        }
        if (!(gx.tcs_man_enab & (1u << coord)))
            set_su_regs(map, coord);
    }
    for (u32 i = 0; i < n_stages && i < 16u; i++) {
        const u32 m = gx.texmap_id[i] & ~0x100u;
        const u32 tref = gx.tref[i / 2u];
        const u32 cd = (i & 1u) ? (tref >> 15) & 7u : (tref >> 3) & 7u;
        if (m != 0xFFu && !(gx.tcs_man_enab & (1u << cd)) && (gx.tev_tc_enab & (1u << i)))
            set_su_regs(m, cd);
    }
}

static bool hle_set_su_tex_regs(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set_su_tex_regs();
    gx_return(g, 0);
    return true;
}

/* __GXSetTmemConfig(config): TMEM layout (cache regions 0x8C-0x93 and 0xAC-0xB3); values from the SDK
 * instructions (three configurations). */
void gx_set_tmem_config(u32 config) {
    static const u32 k_common[9] = {0x8C0D8000u, 0x900DC000u, 0x8D0D8800u, 0x910DC800u, 0x8E0D9000u,
                                    0x920DD000u, 0x8F0D9800u, 0x930DD800u, 0xAC0DA000u};
    static const u32 k_two[7] = {0xB00DC400u, 0xAD0DA800u, 0xB10DCC00u, 0xAE0DB000u,
                                 0xB20DD400u, 0xAF0DB800u, 0xB30DDC00u};
    static const u32 k_one[7] = {0xB00DE000u, 0xAD0DA800u, 0xB10DE800u, 0xAE0DB000u,
                                 0xB20DF000u, 0xAF0DB800u, 0xB30DF800u};
    static const u32 k_default[16] = {0x8C0D8000u, 0x900DC000u, 0x8D0D8400u, 0x910DC400u, 0x8E0D8800u, 0x920DC800u,
                                      0x8F0D8C00u, 0x930DCC00u, 0xAC0D9000u, 0xB00DD000u, 0xAD0D9400u, 0xB10DD400u,
                                      0xAE0D9800u, 0xB20DD800u, 0xAF0D9C00u, 0xB30DDC00u};
    const s32 c = (s32)config;
    if (c == 1 || c == 2) {
        for (int i = 0; i < 9; i++)
            gxw_bp(k_common[i]);
        const u32* tail = c == 2 ? k_two : k_one;
        for (int i = 0; i < 7; i++)
            gxw_bp(tail[i]);
    } else {
        for (int i = 0; i < 16; i++)
            gxw_bp(k_default[i]);
    }
}

static bool hle_set_tmem_config(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set_tmem_config(gx_arg(g, 0));
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Indirect textures and BP mask
 * ------------------------------------------------------------------------------------------- */

void gx_flush_texture_state(void) {
    gxw_bp(gx.bp_mask);
    gx.bp_sent_not = 0;
}

void gx_set_indirect_mask(u32 mask) {
    gx_set(&gx.bp_mask, mask & 0xFFu, 0, 8);
    gxw_bp(gx.bp_mask);
    gx.bp_sent_not = 0;
}

static bool hle_flush_texture_state(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_flush_texture_state();
    gx_return(g, 0);
    return true;
}

static bool hle_set_indirect_mask(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set_indirect_mask(gx_arg(g, 0));
    gx_return(g, 0);
    return true;
}

/* GXSetTevIndirect(stage, indStage, fmt, bias, mtx, wrapS, wrapT, addPrev, utcLod, alphaSel): the last two
 * arguments are on the caller's stack (r1 + 8: low-order byte at +0xB; r1 + 0xC). */
static bool hle_set_tev_indirect(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 sp = g->state.gpr[1];
    const u32 utc_lod = mem_read8(&g->state, sp + 0xB), alpha_sel = mem_read32(&g->state, sp + 0xC);
    u32 reg = 0;
    gx_set(&reg, gx_arg(g, 1), 0, 2);
    gx_set(&reg, gx_arg(g, 2), 2, 2);
    gx_set(&reg, gx_arg(g, 3), 4, 3);
    gx_set(&reg, alpha_sel, 7, 2);
    gx_set(&reg, gx_arg(g, 4), 9, 4);
    gx_set(&reg, gx_arg(g, 5), 13, 3);
    gx_set(&reg, gx_arg(g, 6), 16, 3);
    gx_set(&reg, utc_lod, 19, 1);
    gx_set(&reg, gx_arg(g, 7), 20, 1);
    gx_set(&reg, gx_arg(g, 0) + 0x10u, 24, 8);
    gxw_bp(reg);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_ind_tex_coord_scale(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const s32 stage = (s32)gx_arg(g, 0);
    const u32 ss = gx_arg(g, 1), ts = gx_arg(g, 2);
    u32* reg = NULL;
    u32 id = 0, sh = 0;
    switch (stage) {
    case 0: reg = &gx.ind_tex_scale0, id = 0x25, sh = 0; break;
    case 1: reg = &gx.ind_tex_scale0, id = 0x25, sh = 8; break;
    case 2: reg = &gx.ind_tex_scale1, id = 0x26, sh = 0; break;
    case 3: reg = &gx.ind_tex_scale1, id = 0x26, sh = 8; break;
    default: break;
    }
    if (reg) {
        gx_set(reg, ss, sh, 4);
        gx_set(reg, ts, sh + 4u, 4);
        gx_set(reg, id, 24, 8);
        gxw_bp(*reg);
    }
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_num_ind_stages(GcrtCpu* g, u32 address) {
    (void)address;
    gx_set(&gx.gen_mode, gx_arg(g, 0) & 0xFFu, 16, 3);
    gx.dirty_state |= GX_DIRTY_GEN_MODE | GX_DIRTY_BP_MASK;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * TEV combiners
 * ------------------------------------------------------------------------------------------- */

static bool hle_set_tev_op(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 stage = gx_arg(g, 0) & 15u, mode = gx_arg(g, 1);
    const u32 t = gx_tables.tev_op;
    const u32 ct = stage == 0 ? t + mode * 4u : t + 0x14u + mode * 4u;
    const u32 at = stage == 0 ? t + 0x28u + mode * 4u : t + 0x3Cu + mode * 4u;
    gx.tevc[stage] = (gx.tevc[stage] & 0xFF000000u) | (mem_read32(&g->state, ct) & 0x00FFFFFFu);
    gxw_bp(gx.tevc[stage]);
    gx.teva[stage] = (gx.teva[stage] & 0xFF00000Fu) | (mem_read32(&g->state, at) & 0x00FFFFF0u);
    gxw_bp(gx.teva[stage]);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_color_in(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32* r = &gx.tevc[gx_arg(g, 0) & 15u];
    gx_set(r, gx_arg(g, 1), 12, 4);
    gx_set(r, gx_arg(g, 2), 8, 4);
    gx_set(r, gx_arg(g, 3), 4, 4);
    gx_set(r, gx_arg(g, 4), 0, 4);
    gxw_bp(*r);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_alpha_in(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32* r = &gx.teva[gx_arg(g, 0) & 15u];
    gx_set(r, gx_arg(g, 1), 13, 3);
    gx_set(r, gx_arg(g, 2), 10, 3);
    gx_set(r, gx_arg(g, 3), 7, 3);
    gx_set(r, gx_arg(g, 4), 4, 3);
    gxw_bp(*r);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* GXSetTevColorOp / GXSetTevAlphaOp(stage, op, bias, scale, clamp, out). */
static void tev_op(GcrtCpu* g, u32* r) {
    const u32 op = gx_arg(g, 1), bias = gx_arg(g, 2), scale = gx_arg(g, 3), clamp = gx_arg(g, 4), out = gx_arg(g, 5);
    gx_set(r, op, 18, 1);
    if ((s32)op <= 1) {
        gx_set(r, scale, 20, 2);
        gx_set(r, bias, 16, 2);
    } else {
        gx_set(r, op >> 1, 20, 2);
        gx_set(r, 3, 16, 2);
    }
    gx_set(r, clamp, 19, 1);
    gx_set(r, out, 22, 2);
    gxw_bp(*r);
    gx.bp_sent_not = 0;
}

static bool hle_set_tev_color_op(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    tev_op(g, &gx.tevc[gx_arg(g, 0) & 15u]);
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_alpha_op(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    tev_op(g, &gx.teva[gx_arg(g, 0) & 15u]);
    gx_return(g, 0);
    return true;
}

/* GXSetTevColor(id, color*): color registers (red/alpha then blue/green; the latter written three times, like the
 * SDK). */
static bool hle_set_tev_color(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 id = gx_arg(g, 0), color = mem_read32(&g->state, gx_arg(g, 1));
    tev_color_trace(g, 'C', id, color);
    const u32 r = color >> 24, gg = (color >> 16) & 0xFFu, b = (color >> 8) & 0xFFu, a = color & 0xFFu;
    const u32 ra = ((id * 2u + 0xE0u) << 24) | r | (a << 12);
    const u32 bg = ((id * 2u + 0xE1u) << 24) | b | (gg << 12);
    gxw_bp(ra);
    gxw_bp(bg);
    gxw_bp(bg);
    gxw_bp(bg);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_color_s10(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 id = gx_arg(g, 0), p = gx_arg(g, 1);
    const u32 w0 = mem_read32(&g->state, p), w1 = mem_read32(&g->state, p + 4);
    const u32 ra = ((id * 2u + 0xE0u) << 24) | ((w0 >> 16) & 0x7FFu) | ((w1 & 0x7FFu) << 12);
    const u32 bg = ((id * 2u + 0xE1u) << 24) | ((w1 >> 16) & 0x7FFu) | ((w0 & 0x7FFu) << 12);
    gxw_bp(ra);
    gxw_bp(bg);
    gxw_bp(bg);
    gxw_bp(bg);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_k_color(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 id = gx_arg(g, 0), color = mem_read32(&g->state, gx_arg(g, 1));
    tev_color_trace(g, 'K', id, color);
    const u32 r = color >> 24, gg = (color >> 16) & 0xFFu, b = (color >> 8) & 0xFFu, a = color & 0xFFu;
    gxw_bp(((id * 2u + 0xE0u) << 24) | (8u << 20) | r | (a << 12));
    gxw_bp(((id * 2u + 0xE1u) << 24) | (8u << 20) | b | (gg << 12));
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_k_color_sel(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const s32 stage = (s32)gx_arg(g, 0);
    u32* k = &gx.tev_ksel[(u32)(stage >> 1) & 7u];
    if (stage & 1)
        gx_set(k, gx_arg(g, 1), 14, 5);
    else
        gx_set(k, gx_arg(g, 1), 4, 5);
    gxw_bp(*k);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_k_alpha_sel(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const s32 stage = (s32)gx_arg(g, 0);
    u32* k = &gx.tev_ksel[(u32)(stage >> 1) & 7u];
    if (stage & 1)
        gx_set(k, gx_arg(g, 1), 19, 5);
    else
        gx_set(k, gx_arg(g, 1), 9, 5);
    gxw_bp(*k);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_swap_mode(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32* r = &gx.teva[gx_arg(g, 0) & 15u];
    gx_set(r, gx_arg(g, 1), 0, 2);
    gx_set(r, gx_arg(g, 2), 2, 2);
    gxw_bp(*r);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_tev_swap_mode_table(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 t = gx_arg(g, 0);
    u32* k0 = &gx.tev_ksel[(t * 2u) & 7u];
    u32* k1 = &gx.tev_ksel[(t * 2u + 1u) & 7u];
    gx_set(k0, gx_arg(g, 1), 0, 2);
    gx_set(k0, gx_arg(g, 2), 2, 2);
    gxw_bp(*k0);
    gx_set(k1, gx_arg(g, 3), 0, 2);
    gx_set(k1, gx_arg(g, 4), 2, 2);
    gxw_bp(*k1);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_alpha_compare(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32 reg = 0xF3000000u;
    gx_set(&reg, gx_arg(g, 1) & 0xFFu, 0, 8);
    gx_set(&reg, gx_arg(g, 4) & 0xFFu, 8, 8);
    gx_set(&reg, gx_arg(g, 0), 16, 3);
    gx_set(&reg, gx_arg(g, 3), 19, 3);
    gx_set(&reg, gx_arg(g, 2), 22, 2);
    gxw_bp(reg);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_z_texture(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 op = gx_arg(g, 0), fmt = gx_arg(g, 1), bias = gx_arg(g, 2);
    u32 env0 = 0;
    gx_set(&env0, bias, 0, 24);
    gx_set(&env0, 0xF4, 24, 8);
    u32 type;
    switch (fmt) {
    case 0x11: type = 0; break;
    case 0x13: type = 1; break;
    default: type = 2; break; /* GX_TF_Z24X8 and others */
    }
    u32 env1 = 0;
    gx_set(&env1, type, 0, 2);
    gx_set(&env1, op, 2, 2);
    gx_set(&env1, 0xF5, 24, 8);
    gxw_bp(env0);
    gxw_bp(env1);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* GXSetTevOrder(stage, coord, map, color): TREF of the stage (texture, coordinate, color channel through the SDK
 * table), stages that read a coordinate (tev_tc_enab) and maps used (texmap_id). */
static bool hle_set_tev_order(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 stage = gx_arg(g, 0) & 15u;
    u32 coord = gx_arg(g, 1);
    const u32 map = gx_arg(g, 2), color = gx_arg(g, 3);
    gx.texmap_id[stage] = map;
    u32* tref = &gx.tref[(u32)((s32)stage / 2)];
    const u32 m = map & ~0x100u;
    const u32 tmap = m >= 8u ? 0u : m;
    if ((s32)coord >= 8) {
        gx.tev_tc_enab &= ~(1u << stage);
        coord = 0;
    } else {
        gx.tev_tc_enab |= 1u << stage;
    }
    const u32 rassel = color == 0xFFu ? 7u : mem_read32(&g->state, gx_tables.tev_order_c2r + color * 4u);
    const u32 enable = (map != 0xFFu && !(map & 0x100u)) ? 1u : 0u;
    if (stage & 1u) {
        gx_set(tref, tmap, 12, 3);
        gx_set(tref, coord, 15, 3);
        gx_set(tref, rassel, 19, 3);
        gx_set(tref, enable, 18, 1);
    } else {
        gx_set(tref, tmap, 0, 3);
        gx_set(tref, coord, 3, 3);
        gx_set(tref, rassel, 7, 3);
        gx_set(tref, enable, 6, 1);
    }
    gxw_bp(*tref);
    gx.bp_sent_not = 0;
    gx.dirty_state |= GX_DIRTY_SU_TEX;
    gx_return(g, 0);
    return true;
}

static bool hle_set_num_tev_stages(GcrtCpu* g, u32 address) {
    (void)address;
    gx_set(&gx.gen_mode, (gx_arg(g, 0) & 0xFFu) - 1u, 10, 4);
    gx.dirty_state |= GX_DIRTY_GEN_MODE;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Fog
 * ------------------------------------------------------------------------------------------- */

/* Float -> fog register format: 11 mantissa bits, 8 exponent bits, sign. */
static u32 fog_float(float f) {
    const u32 v = f2u(f);
    return ((v >> 12) & 0x7FFu) | (((v >> 23) & 0xFFu) << 11) | ((v >> 31) << 19);
}

/* GXSetFog(type, startz, endz, nearz, farz, color*): SDK arithmetic (single precision). With an orthographic
 * projection, the SDK writes two zero BP words instead of b_m and b_s: reproduced as is. */
static bool hle_set_fog(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 type = gx_arg(g, 0);
    const u32 color = mem_read32(&g->state, gx_arg(g, 1));
    const double startz = gx_farg(g, 0), endz = gx_farg(g, 1), nearz = gx_farg(g, 2), farz = gx_farg(g, 3);
    const u32 proj = (type >> 3) & 1u, fsel = type & 7u;
    u32 fog1 = 0, fog2 = 0;
    float a, c;
    if (proj) {
        if (farz == nearz || endz == startz) {
            a = 0.0f;
            c = 0.0f;
        } else {
            const float range = gx_fsubs(endz, startz);
            const float depth = gx_fsubs(farz, nearz);
            const float off = gx_fsubs(startz, nearz);
            const float inv = gx_fdivs(1.0f, range);
            a = gx_fmuls(inv, depth);
            c = gx_fmuls(inv, off);
        }
    } else {
        float A, B, C;
        if (farz == nearz || endz == startz) {
            A = 0.0f;
            B = 0.5f;
            C = 0.0f;
        } else {
            float depth = gx_fsubs(farz, nearz);
            const float range = gx_fsubs(endz, startz);
            const float fn = gx_fmuls(farz, nearz);
            B = gx_fdivs(farz, depth);
            depth = gx_fmuls(depth, range);
            C = gx_fdivs(startz, range);
            A = gx_fdivs(fn, depth);
        }
        s32 expo = 0;
        while ((double)B > 1.0) {
            B = gx_fmuls(B, 0.5f);
            expo++;
        }
        while ((double)B > 0.0f && (double)B < 0.5) {
            B = gx_fmuls(B, 2.0f);
            expo--;
        }
        const s32 b_s = expo + 1;
        const u32 sh = (u32)b_s & 0x3Fu;
        const u32 pow2 = sh >= 32u ? 0u : (1u << sh);
        const float denom = (float)(double)(s32)pow2;
        a = gx_fdivs(A, denom);
        const u32 b_m = gx_cvt_fp2unsigned(gx_fmuls(8388638.0f, B));
        gx_set(&fog1, b_m, 0, 24);
        gx_set(&fog2, (u32)b_s, 0, 5);
        gx_set(&fog1, 0xEF, 24, 8);
        gx_set(&fog2, 0xF0, 24, 8);
        c = C;
    }
    u32 fog3 = fog_float(c);
    gx_set(&fog3, proj, 20, 1);
    gx_set(&fog3, fsel, 21, 3);
    gx_set(&fog3, 0xF1, 24, 8);
    gxw_bp(fog_float(a) | 0xEE000000u);
    gxw_bp(fog1);
    gxw_bp(fog2);
    gxw_bp(fog3);
    gxw_bp(0xF2000000u | (color >> 8));
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* GXSetFogRangeAdj(enable, center, table*): BP 0xE9-0xED (table of 10 12-bit values), BP 0xE8 (center). */
static bool hle_set_fog_range_adj(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 enable = gx_arg(g, 0) & 0xFFu, center = gx_arg(g, 1) & 0xFFFFu, table = gx_arg(g, 2);
    if (enable) {
        for (u32 i = 0; i < 5; i++) {
            u32 reg = 0;
            gx_set(&reg, mem_read16(&g->state, table + i * 4u), 0, 12);
            gx_set(&reg, mem_read16(&g->state, table + i * 4u + 2u), 12, 12);
            gx_set(&reg, 0xE9u + i, 24, 8);
            gxw_bp(reg);
        }
    }
    u32 reg = 0;
    gx_set(&reg, center + 0x156u, 0, 10);
    gx_set(&reg, enable, 10, 1);
    gx_set(&reg, 0xE8, 24, 8);
    gxw_bp(reg);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Pixel operations
 * ------------------------------------------------------------------------------------------- */

static bool hle_set_blend_mode(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 type = gx_arg(g, 0);
    u32 r = gx.cmode0;
    gx_set(&r, type == 3u, 11, 1);
    gx_set(&r, type, 0, 1);
    gx_set(&r, type == 2u, 1, 1);
    gx_set(&r, gx_arg(g, 3), 12, 4);
    gx_set(&r, gx_arg(g, 1), 8, 3);
    gx_set(&r, gx_arg(g, 2), 5, 3);
    gxw_bp(r);
    gx.cmode0 = r;
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool cmode0_bit(GcrtCpu* g, unsigned bit) {
    gx_enter(g);
    gx_set(&gx.cmode0, gx_arg(g, 0), bit, 1);
    gxw_bp(gx.cmode0);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_color_update(GcrtCpu* g, u32 address) {
    (void)address;
    return cmode0_bit(g, 3);
}

static bool hle_set_alpha_update(GcrtCpu* g, u32 address) {
    (void)address;
    return cmode0_bit(g, 4);
}

static bool hle_set_dither(GcrtCpu* g, u32 address) {
    (void)address;
    return cmode0_bit(g, 2);
}

static bool hle_set_z_mode(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.zmode, gx_arg(g, 0), 0, 1);
    gx_set(&gx.zmode, gx_arg(g, 1), 1, 3);
    gx_set(&gx.zmode, gx_arg(g, 2), 4, 1);
    gxw_bp(gx.zmode);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_z_comp_loc(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.pe_ctrl, gx_arg(g, 0) & 0xFFu, 6, 1);
    gxw_bp(gx.pe_ctrl);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* GXSetPixelFmt(pix, z): PE format (SDK table), multisampling for RGB565_Z16, Y/U/V formats in
 * cmode1. */
static bool hle_set_pixel_fmt(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 pix = gx_arg(g, 0), z = gx_arg(g, 1);
    const u32 p2f = mem_read32(&g->state, gx_tables.pixel_fmt + pix * 4u);
    const u32 old = gx.pe_ctrl;
    gx_set(&gx.pe_ctrl, p2f, 0, 3);
    gx_set(&gx.pe_ctrl, z, 3, 3);
    if (old != gx.pe_ctrl) {
        gxw_bp(gx.pe_ctrl);
        gx_set(&gx.gen_mode, pix == 2u, 9, 1);
        gx.dirty_state |= GX_DIRTY_GEN_MODE;
    }
    if (p2f == 4u) {
        gx_set(&gx.cmode1, pix - 4u, 9, 2);
        gx_set(&gx.cmode1, 0x42, 24, 8);
        gxw_bp(gx.cmode1);
    }
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_dst_alpha(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32 r = gx.cmode1;
    gx_set(&r, gx_arg(g, 1) & 0xFFu, 0, 8);
    gx_set(&r, gx_arg(g, 0) & 0xFFu, 8, 1);
    gxw_bp(r);
    gx.cmode1 = r;
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_field_mask(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32 reg = 0;
    gx_set(&reg, gx_arg(g, 1) & 0xFFu, 0, 1);
    gx_set(&reg, gx_arg(g, 0), 1, 1);
    gx_set(&reg, 0x44, 24, 8);
    gxw_bp(reg);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_field_mode(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.lp_size, gx_arg(g, 1) & 0xFFu, 22, 1);
    gxw_bp(gx.lp_size);
    gx_flush_texture_state();
    gxw_bp(0x68000000u | (gx_arg(g, 0) & 0xFFu));
    gx_flush_texture_state();
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------------------------- */

void gcrt_native_gx_tev_install(GcrtCpu* g) {
    unsigned n = 0;
    s_default_tex_cb = gcrt_hle_find("__GXDefaultTexRegionCallback", &n);
    if (n != 1)
        s_default_tex_cb = 0;
    s_default_tlut_cb = gcrt_hle_find("__GXDefaultTlutRegionCallback", &n);
    if (n != 1)
        s_default_tlut_cb = 0;
    gx_replace(g, "GXLoadTexObjPreLoaded", hle_load_tex_obj_preloaded, NULL, 0, NULL);
    gx_replace(g, "GXLoadTexObj", hle_load_tex_obj, NULL, 0, NULL);
    gx_replace(g, "GXLoadTlut", hle_load_tlut, NULL, 0, NULL);
    gx_replace(g, "GXInvalidateTexAll", hle_invalidate_tex_all, NULL, 0, NULL);
    gx_replace(g, "GXSetTexRegionCallback", hle_set_tex_region_callback, NULL, 0, NULL);
    gx_replace(g, "GXSetTlutRegionCallback", hle_set_tlut_region_callback, NULL, 0, NULL);
    gx_replace(g, "__SetSURegs", hle_set_su_regs, NULL, 0, NULL);
    gx_replace(g, "__GXSetSUTexRegs", hle_set_su_tex_regs, NULL, 0, NULL);
    gx_replace(g, "__GXSetTmemConfig", hle_set_tmem_config, NULL, 0, NULL);
    gx_replace(g, "__GXFlushTextureState", hle_flush_texture_state, NULL, 0, NULL);
    gx_replace(g, "__GXSetIndirectMask", hle_set_indirect_mask, NULL, 0, NULL);
    /* (__GXUpdateBPMask: a lone `blr` in this SDK, kept recompiled; signature shared with any isolated `blr`.) */
    gx_replace(g, "GXSetTevIndirect", hle_set_tev_indirect, NULL, 0, NULL);
    gx_replace(g, "GXSetIndTexCoordScale", hle_set_ind_tex_coord_scale, NULL, 0, NULL);
    gx_replace(g, "GXSetNumIndStages", hle_set_num_ind_stages, NULL, 0, NULL);
    gx_replace(g, "GXSetTevOp", hle_set_tev_op, NULL, 0, NULL);
    gx_replace(g, "GXSetTevColorIn", hle_set_tev_color_in, NULL, 0, NULL);
    gx_replace(g, "GXSetTevAlphaIn", hle_set_tev_alpha_in, NULL, 0, NULL);
    gx_replace(g, "GXSetTevColorOp", hle_set_tev_color_op, NULL, 0, NULL);
    gx_replace(g, "GXSetTevAlphaOp", hle_set_tev_alpha_op, NULL, 0, NULL);
    gx_replace(g, "GXSetTevColor", hle_set_tev_color, NULL, 0, NULL);
    gx_replace(g, "GXSetTevColorS10", hle_set_tev_color_s10, NULL, 0, NULL);
    gx_replace(g, "GXSetTevKColor", hle_set_tev_k_color, NULL, 0, NULL);
    gx_replace(g, "GXSetTevKColorSel", hle_set_tev_k_color_sel, NULL, 0, NULL);
    gx_replace(g, "GXSetTevKAlphaSel", hle_set_tev_k_alpha_sel, NULL, 0, NULL);
    gx_replace(g, "GXSetTevSwapMode", hle_set_tev_swap_mode, NULL, 0, NULL);
    gx_replace(g, "GXSetTevSwapModeTable", hle_set_tev_swap_mode_table, NULL, 0, NULL);
    gx_replace(g, "GXSetAlphaCompare", hle_set_alpha_compare, NULL, 0, NULL);
    gx_replace(g, "GXSetZTexture", hle_set_z_texture, NULL, 0, NULL);
    gx_replace(g, "GXSetTevOrder", hle_set_tev_order, NULL, 0, NULL);
    gx_replace(g, "GXSetNumTevStages", hle_set_num_tev_stages, NULL, 0, NULL);
    gx_replace(g, "GXSetFog", hle_set_fog, NULL, 0, NULL);
    gx_replace(g, "GXSetFogRangeAdj", hle_set_fog_range_adj, NULL, 0, NULL);
    gx_replace(g, "GXSetBlendMode", hle_set_blend_mode, NULL, 0, NULL);
    gx_replace(g, "GXSetColorUpdate", hle_set_color_update, NULL, 0, NULL);
    gx_replace(g, "GXSetAlphaUpdate", hle_set_alpha_update, NULL, 0, NULL);
    gx_replace(g, "GXSetZMode", hle_set_z_mode, NULL, 0, NULL);
    gx_replace(g, "GXSetZCompLoc", hle_set_z_comp_loc, NULL, 0, NULL);
    gx_replace(g, "GXSetPixelFmt", hle_set_pixel_fmt, NULL, 0, NULL);
    gx_replace(g, "GXSetDither", hle_set_dither, NULL, 0, NULL);
    gx_replace(g, "GXSetDstAlpha", hle_set_dst_alpha, NULL, 0, NULL);
    gx_replace(g, "GXSetFieldMask", hle_set_field_mask, NULL, 0, NULL);
    gx_replace(g, "GXSetFieldMode", hle_set_field_mode, NULL, 0, NULL);
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native GX: default region callbacks %08X / %08X", s_default_tex_cb,
                      s_default_tlut_cb);
}

u64 gcrt_native_gx_guest_region_calls(void) { return s_guest_region_calls; }
