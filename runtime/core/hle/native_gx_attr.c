/* Native GX drawing (milestone 10c): vertices (descriptors, formats, arrays, coordinate generation), geometry,
 * transforms, lighting and EFB copies. Each replacement redoes the recognized function of the SDK GX (disassembly read
 * function by function; original __GXData offsets in native_gx.h). See native_gx.c. Generic. */
#include "hle/native_gx.h"
#include "platform/trace.h"

/* ---------------------------------------------------------------------------------------------
 * Vertex descriptors
 * ------------------------------------------------------------------------------------------- */

/* VCD field of an attribute (GXAttr); NRM and NBT are handled separately. */
static void set_vtx_desc(u32 attr, u32 type) {
    switch (attr) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        gx_set(&gx.vcd_lo, type, attr, 1); /* matrix indices */
        break;
    case 9:
        gx_set(&gx.vcd_lo, type, 9, 2); /* position */
        break;
    case 10: /* normal */
        if (type) {
            gx.has_nrms = 1;
            gx.has_bi_nrms = 0;
            gx.nrm_type = type;
        } else {
            gx.has_nrms = 0;
        }
        break;
    case 25: /* normal, binormal, tangent */
        if (type) {
            gx.has_bi_nrms = 1;
            gx.has_nrms = 0;
            gx.nrm_type = type;
        } else {
            gx.has_bi_nrms = 0;
        }
        break;
    case 11:
        gx_set(&gx.vcd_lo, type, 13, 2);
        break;
    case 12:
        gx_set(&gx.vcd_lo, type, 15, 2);
        break;
    case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20:
        gx_set(&gx.vcd_hi, type, (attr - 13u) * 2u, 2);
        break;
    default:
        break;
    }
}

static void finish_vtx_desc(void) {
    if (gx.has_nrms || gx.has_bi_nrms)
        gx_set(&gx.vcd_lo, gx.nrm_type, 11, 2);
    else
        gx_set(&gx.vcd_lo, 0, 11, 2);
    gx.dirty_state |= GX_DIRTY_VCD;
}

static bool hle_set_vtx_desc(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 attr = gx_arg(g, 0);
    if (attr <= 0x19)
        set_vtx_desc(attr, gx_arg(g, 1));
    finish_vtx_desc();
    gx_return(g, 0);
    return true;
}

/* GXSetVtxDescv(list): 8-byte {attr, type} pairs up to GX_VA_NULL (0xFF). */
static bool hle_set_vtx_descv(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32 list = gx_arg(g, 0);
    for (unsigned guard = 0; guard < 64; guard++, list += 8) {
        const u32 attr = mem_read32(&g->state, list);
        if (attr == 0xFF)
            break;
        if (attr <= 0x19)
            set_vtx_desc(attr, mem_read32(&g->state, list + 4));
    }
    finish_vtx_desc();
    gx_return(g, 0);
    return true;
}

static u32 get_vtx_desc(u32 attr) {
    switch (attr) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: case 8:
        return gx_get(gx.vcd_lo, attr, 1);
    case 9:
        return gx_get(gx.vcd_lo, 9, 2);
    case 10:
        return gx.has_nrms ? gx_get(gx.vcd_lo, 11, 2) : 0;
    case 25:
        return gx.has_bi_nrms ? gx_get(gx.vcd_lo, 11, 2) : 0;
    case 11:
        return gx_get(gx.vcd_lo, 13, 2);
    case 12:
        return gx_get(gx.vcd_lo, 15, 2);
    case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20:
        return gx_get(gx.vcd_hi, (attr - 13u) * 2u, 2);
    default:
        return 0;
    }
}

static bool hle_get_vtx_desc(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    mem_write32(&g->state, gx_arg(g, 1), get_vtx_desc(gx_arg(g, 0)));
    gx_return(g, 0);
    return true;
}

/* GXGetVtxDescv(list): attributes 0 to 20, then NBT, then GX_VA_NULL. */
static bool hle_get_vtx_descv(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 list = gx_arg(g, 0);
    u32 i = 0;
    for (; i <= 20; i++) {
        mem_write32(&g->state, list + i * 8u, i);
        mem_write32(&g->state, list + i * 8u + 4u, get_vtx_desc(i));
    }
    mem_write32(&g->state, list + i * 8u, 25);
    mem_write32(&g->state, list + i * 8u + 4u, get_vtx_desc(25));
    i++;
    mem_write32(&g->state, list + i * 8u, 0xFF);
    gx_return(g, 0);
    return true;
}

static bool hle_clear_vtx_desc(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx.vcd_lo = 0;
    gx_set(&gx.vcd_lo, 1, 9, 2);
    gx.vcd_hi = 0;
    gx.has_nrms = 0;
    gx.has_bi_nrms = 0;
    gx.dirty_state |= GX_DIRTY_VCD;
    gx_return(g, 0);
    return true;
}

/* __GXSetVCD: CP 0x50 / 0x60, then XF 0x1008 (number of colors, of normals, of texture coordinates). */
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

void gx_set_vcd(void) {
    gxw_cp(0x50, gx.vcd_lo);
    gxw_cp(0x60, gx.vcd_hi);
    const u32 nrm = gx.has_bi_nrms ? 2u : (gx.has_nrms ? 1u : 0u);
    const u32 colors = (33u - cntlzw((gx.vcd_lo >> 13) & 0xFu)) >> 1;
    const u32 texs = (33u - cntlzw(gx.vcd_hi & 0xFFFFu)) >> 1;
    gxw_xf(0x1008, colors | (nrm << 2) | (texs << 4));
    gx.bp_sent_not = 1;
}

static bool hle_set_vcd(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set_vcd();
    gx_return(g, 0);
    return true;
}

/* __GXCalculateVLim: size of a vertex for the XF flush (only if vNum != 0). SDK tables: direct sizes of the colors
 * (4), of the coordinates (8), of the positions and normals (12); 1 and 2 for the indices. */
void gx_calculate_vlim(void) {
    static const u8 k_clr[4] = {0, 4, 1, 2}, k_tex[4] = {0, 8, 1, 2}, k_pn[4] = {0, 12, 1, 2};
    if (!gx.vnum)
        return;
    const u32 lo = gx.vcd_lo, hi = gx.vcd_hi;
    const u32 nbt = (gx.vat_a[0] >> 9) & 1u;
    u32 v = 0;
    for (u32 i = 0; i < 9; i++)
        v += (lo >> i) & 1u;
    v += k_pn[(lo >> 9) & 3u];
    v += k_pn[(lo >> 11) & 3u] * (nbt == 1 ? 3u : 1u);
    v += k_clr[(lo >> 13) & 3u];
    v += k_clr[(lo >> 15) & 3u];
    for (u32 t = 0; t < 8; t++)
        v += k_tex[(hi >> (t * 2u)) & 3u];
    gx.vlim = (u16)v;
}

static bool hle_calculate_vlim(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_calculate_vlim();
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Vertex formats (VAT)
 * ------------------------------------------------------------------------------------------- */

static void set_vat(u32 fmt, u32 attr, u32 cnt, u32 type, u32 frac) {
    u32* a = &gx.vat_a[fmt & 7u];
    u32* b = &gx.vat_b[fmt & 7u];
    u32* c = &gx.vat_c[fmt & 7u];
    switch (attr) {
    case 9:
        gx_set(a, cnt, 0, 1);
        gx_set(a, type, 1, 3);
        gx_set(a, frac, 4, 5);
        break;
    case 10:
    case 25:
        gx_set(a, type, 10, 3);
        if (cnt == 2) { /* GX_NRM_NBT3 */
            gx_set(a, 1, 9, 1);
            gx_set(a, 1, 31, 1);
        } else {
            gx_set(a, cnt, 9, 1);
            gx_set(a, 0, 31, 1);
        }
        break;
    case 11:
        gx_set(a, cnt, 13, 1);
        gx_set(a, type, 14, 3);
        break;
    case 12:
        gx_set(a, cnt, 17, 1);
        gx_set(a, type, 18, 3);
        break;
    case 13:
        gx_set(a, cnt, 21, 1);
        gx_set(a, type, 22, 3);
        gx_set(a, frac, 25, 5);
        break;
    case 14:
        gx_set(b, cnt, 0, 1);
        gx_set(b, type, 1, 3);
        gx_set(b, frac, 4, 5);
        break;
    case 15:
        gx_set(b, cnt, 9, 1);
        gx_set(b, type, 10, 3);
        gx_set(b, frac, 13, 5);
        break;
    case 16:
        gx_set(b, cnt, 18, 1);
        gx_set(b, type, 19, 3);
        gx_set(b, frac, 22, 5);
        break;
    case 17:
        gx_set(b, cnt, 27, 1);
        gx_set(b, type, 28, 3);
        gx_set(c, frac, 0, 5);
        break;
    case 18:
        gx_set(c, cnt, 5, 1);
        gx_set(c, type, 6, 3);
        gx_set(c, frac, 9, 5);
        break;
    case 19:
        gx_set(c, cnt, 14, 1);
        gx_set(c, type, 15, 3);
        gx_set(c, frac, 18, 5);
        break;
    case 20:
        gx_set(c, cnt, 23, 1);
        gx_set(c, type, 24, 3);
        gx_set(c, frac, 27, 5);
        break;
    default:
        break;
    }
}

static void mark_vat(u32 fmt) {
    gx.dirty_state |= GX_DIRTY_VAT;
    const u32 sh = fmt & 0xFFu;
    gx.dirty_vat |= (u8)(sh < 32u ? (1u << sh) : 0u);
}

static bool hle_set_vtx_attr_fmt(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 fmt = gx_arg(g, 0);
    set_vat(fmt, gx_arg(g, 1), gx_arg(g, 2), gx_arg(g, 3), gx_arg(g, 4) & 0xFFu);
    mark_vat(fmt);
    gx_return(g, 0);
    return true;
}

/* GXSetVtxAttrFmtv(fmt, list): 16-byte entries {attr, cnt, type, frac (byte)} up to GX_VA_NULL. */
static bool hle_set_vtx_attr_fmtv(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 fmt = gx_arg(g, 0);
    u32 list = gx_arg(g, 1);
    for (unsigned guard = 0; guard < 64; guard++, list += 16) {
        const u32 attr = mem_read32(&g->state, list);
        if (attr == 0xFF)
            break;
        set_vat(fmt, attr, mem_read32(&g->state, list + 4), mem_read32(&g->state, list + 8),
                mem_read8(&g->state, list + 12));
    }
    mark_vat(fmt);
    gx_return(g, 0);
    return true;
}

/* __GXSetVAT: CP 0x70/0x80/0x90 + n for each flagged format. */
void gx_set_vat(void) {
    u32 dirty = gx.dirty_vat;
    for (u32 i = 0; dirty; i++, dirty >>= 1) {
        if (dirty & 1u) {
            gxw_cp((u8)(0x70u | i), gx.vat_a[i]);
            gxw_cp((u8)(0x80u | i), gx.vat_b[i]);
            gxw_cp((u8)(0x90u | i), gx.vat_c[i]);
        }
    }
    gx.dirty_vat = 0;
}

static bool hle_set_vat(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set_vat();
    gx_return(g, 0);
    return true;
}

static void get_vat(u32 fmt, u32 attr, u32* cnt, u32* type, u8* frac) {
    const u32 a = gx.vat_a[fmt & 7u], b = gx.vat_b[fmt & 7u], c = gx.vat_c[fmt & 7u];
    switch (attr) {
    case 9:
        *cnt = a & 1u, *type = (a >> 1) & 7u, *frac = (u8)((a >> 4) & 0x1Fu);
        return;
    case 10:
    case 25:
        *cnt = (a >> 9) & 1u;
        if (*cnt == 1 && ((a >> 31) & 1u))
            *cnt = 2;
        *type = (a >> 10) & 7u;
        *frac = *type == 1 ? 6 : (*type == 3 ? 14 : 0);
        return;
    case 11:
        *cnt = (a >> 13) & 1u, *type = (a >> 14) & 7u, *frac = 0;
        return;
    case 12:
        *cnt = (a >> 17) & 1u, *type = (a >> 18) & 7u, *frac = 0;
        return;
    case 13:
        *cnt = (a >> 21) & 1u, *type = (a >> 22) & 7u, *frac = (u8)((a >> 25) & 0x1Fu);
        return;
    case 14:
        *cnt = b & 1u, *type = (b >> 1) & 7u, *frac = (u8)((b >> 4) & 0x1Fu);
        return;
    case 15:
        *cnt = (b >> 9) & 1u, *type = (b >> 10) & 7u, *frac = (u8)((b >> 13) & 0x1Fu);
        return;
    case 16:
        *cnt = (b >> 18) & 1u, *type = (b >> 19) & 7u, *frac = (u8)((b >> 22) & 0x1Fu);
        return;
    case 17:
        *cnt = (b >> 27) & 1u, *type = (b >> 28) & 7u, *frac = (u8)(c & 0x1Fu);
        return;
    case 18:
        *cnt = (c >> 5) & 1u, *type = (c >> 6) & 7u, *frac = (u8)((c >> 9) & 0x1Fu);
        return;
    case 19:
        *cnt = (c >> 14) & 1u, *type = (c >> 15) & 7u, *frac = (u8)((c >> 18) & 0x1Fu);
        return;
    case 20:
        *cnt = (c >> 23) & 1u, *type = (c >> 24) & 7u, *frac = (u8)((c >> 27) & 0x1Fu);
        return;
    default:
        *cnt = 1, *type = 0, *frac = 0;
        return;
    }
}

static bool hle_get_vtx_attr_fmt(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32 cnt, type;
    u8 frac;
    get_vat(gx_arg(g, 0), gx_arg(g, 1), &cnt, &type, &frac);
    /* The "position"... cases write the three outputs; so does the default case. */
    mem_write32(&g->state, gx_arg(g, 2), cnt);
    mem_write32(&g->state, gx_arg(g, 3), type);
    mem_write8(&g->state, gx_arg(g, 4), frac);
    gx_return(g, 0);
    return true;
}

static bool hle_get_vtx_attr_fmtv(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 fmt = gx_arg(g, 0);
    u32 list = gx_arg(g, 1);
    for (u32 attr = 9; attr <= 20; attr++, list += 16) {
        u32 cnt, type;
        u8 frac;
        mem_write32(&g->state, list, attr);
        get_vat(fmt, attr, &cnt, &type, &frac);
        mem_write32(&g->state, list + 4, cnt);
        mem_write32(&g->state, list + 8, type);
        mem_write8(&g->state, list + 12, frac);
    }
    mem_write32(&g->state, list, 0xFF);
    gx_return(g, 0);
    return true;
}

/* GXSetArray(attr, base, stride): CP 0xA0 + n = base & 0x3FFFFFFF, CP 0xB0 + n = stride (immediate). */
static bool hle_set_array(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    u32 attr = gx_arg(g, 0);
    if (attr == 25)
        attr = 10;
    const u32 n = attr - 9u;
    gxw_cp((u8)(n | 0xA0u), gx_arg(g, 1) & 0x3FFFFFFFu);
    gxw_cp((u8)(n | 0xB0u), gx_arg(g, 2) & 0xFFu);
    gx_return(g, 0);
    return true;
}

/* GXSetTexCoordGen2(coord, type, src, mtx, normalize, ptmtx): XF 0x1040 + n, XF 0x1050 + n immediate, matrix
 * index stored then __GXSetMatrixIndex(coord + 1). */
static bool hle_set_tex_coord_gen2(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 id = gx_arg(g, 0), func = gx_arg(g, 1), src = gx_arg(g, 2), mtx = gx_arg(g, 3);
    const u32 normalize = gx_arg(g, 4), ptmtx = gx_arg(g, 5);
    u32 row = 5, form = 0;
    switch (src) {
    case 0: row = 0, form = 1; break;   /* GX_TG_POS */
    case 1: row = 1, form = 1; break;   /* GX_TG_NRM */
    case 2: row = 3, form = 1; break;   /* GX_TG_BINRM */
    case 3: row = 4, form = 1; break;   /* GX_TG_TANGENT */
    case 19: case 20: row = 2; break;   /* GX_TG_COLOR0 / COLOR1 */
    case 4: case 5: case 6: case 7: case 8: case 9: case 10: case 11: row = 5u + (src - 4u); break; /* TEX0-7 */
    default: break;
    }
    u32 reg = 0;
    const s32 type = (s32)func;
    if (type == 1) { /* GX_TG_MTX2x4 */
        gx_set(&reg, 0, 1, 1);
        gx_set(&reg, form, 2, 1);
        gx_set(&reg, 0, 4, 3);
        gx_set(&reg, row, 7, 5);
    } else if (type == 0) { /* GX_TG_MTX3x4 */
        gx_set(&reg, 1, 1, 1);
        gx_set(&reg, form, 2, 1);
        gx_set(&reg, 0, 4, 3);
        gx_set(&reg, row, 7, 5);
    } else if (type >= 2 && type < 10) { /* GX_TG_BUMP0-7 */
        gx_set(&reg, 0, 1, 1);
        gx_set(&reg, form, 2, 1);
        gx_set(&reg, 1, 4, 3);
        gx_set(&reg, row, 7, 5);
        gx_set(&reg, src - 12u, 12, 3);
        gx_set(&reg, func - 2u, 15, 3);
    } else if (type == 10) { /* GX_TG_SRTG */
        gx_set(&reg, 0, 1, 1);
        gx_set(&reg, form, 2, 1);
        gx_set(&reg, src == 19 ? 2u : 3u, 4, 3);
        gx_set(&reg, 2, 7, 5);
    }
    gxw_xf(0x1040u + id, reg);
    u32 reg2 = 0;
    gx_set(&reg2, ptmtx - 0x40u, 0, 6);
    gx_set(&reg2, normalize & 0xFFu, 8, 1);
    gxw_xf(0x1050u + id, reg2);
    switch (id) {
    case 0: gx_set(&gx.mat_idx_a, mtx, 6, 6); break;
    case 1: gx_set(&gx.mat_idx_a, mtx, 12, 6); break;
    case 2: gx_set(&gx.mat_idx_a, mtx, 18, 6); break;
    case 3: gx_set(&gx.mat_idx_a, mtx, 24, 6); break;
    case 4: gx_set(&gx.mat_idx_b, mtx, 0, 6); break;
    case 5: gx_set(&gx.mat_idx_b, mtx, 6, 6); break;
    case 6: gx_set(&gx.mat_idx_b, mtx, 12, 6); break;
    default: gx_set(&gx.mat_idx_b, mtx, 18, 6); break;
    }
    gx_set_matrix_index(id + 1u);
    gx_return(g, 0);
    return true;
}

static bool hle_set_num_tex_gens(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 n = gx_arg(g, 0);
    gx_set(&gx.gen_mode, n, 0, 4);
    gxw_xf(0x103F, n & 0xFFu);
    gx.dirty_state |= GX_DIRTY_GEN_MODE;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Geometry
 * ------------------------------------------------------------------------------------------- */

static bool hle_set_line_width(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.lp_size, gx_arg(g, 0) & 0xFFu, 0, 8);
    gx_set(&gx.lp_size, gx_arg(g, 1), 16, 3);
    gxw_bp(gx.lp_size);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_point_size(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.lp_size, gx_arg(g, 0) & 0xFFu, 8, 8);
    gx_set(&gx.lp_size, gx_arg(g, 1), 19, 3);
    gxw_bp(gx.lp_size);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_enable_tex_offsets(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 coord = gx_arg(g, 0) & 7u;
    gx_set(&gx.su_ts0[coord], gx_arg(g, 1), 18, 1);
    gx_set(&gx.su_ts0[coord], gx_arg(g, 2) & 0xFFu, 19, 1);
    gxw_bp(gx.su_ts0[coord]);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_cull_mode(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 mode = gx_arg(g, 0);
    const u32 hw = ((mode >> 1) & 1u) | ((mode << 1) & 2u);
    gx_set(&gx.gen_mode, hw, 14, 2);
    gx.dirty_state |= GX_DIRTY_GEN_MODE;
    gx_return(g, 0);
    return true;
}

static bool hle_set_co_planar(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.gen_mode, gx_arg(g, 0), 19, 1);
    gxw_bp(0xFE080000u);
    gxw_bp(gx.gen_mode);
    gx_return(g, 0);
    return true;
}

static bool hle_set_gen_mode(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gxw_bp(gx.gen_mode);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Transforms
 * ------------------------------------------------------------------------------------------- */

void gx_set_matrix_index(u32 attr) {
    if ((s32)attr < 5) {
        gxw_cp(0x30, gx.mat_idx_a);
        gxw_xf(0x1018, gx.mat_idx_a);
    } else {
        gxw_cp(0x40, gx.mat_idx_b);
        gxw_xf(0x1019, gx.mat_idx_b);
    }
    gx.bp_sent_not = 1;
}

static bool hle_set_matrix_index(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set_matrix_index(gx_arg(g, 0));
    gx_return(g, 0);
    return true;
}

/* XF 0x1020, 6 words: projection parameters, then the type. */
static void emit_projection(void) {
    gxw_u8(0x10);
    gxw_u32(0x00061020u);
    for (int i = 0; i < 6; i++)
        gxw_u32(gx.proj_mtx[i]);
    gxw_u32(gx.proj_type);
    gx.bp_sent_not = 1;
}

/* GXSetProjection(mtx, type): m[0][0], m[1][1], m[2][2], m[2][3], and m[0][3]/m[1][3] (orthographic) or
 * m[0][2]/m[1][2] (perspective). */
static bool hle_set_projection(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 m = gx_arg(g, 0), type = gx_arg(g, 1);
    CPUState* c = &g->state;
    gx.proj_type = type;
    gx.proj_mtx[0] = mem_read32(c, m + 0x00);
    gx.proj_mtx[2] = mem_read32(c, m + 0x14);
    gx.proj_mtx[4] = mem_read32(c, m + 0x28);
    gx.proj_mtx[5] = mem_read32(c, m + 0x2C);
    if ((s32)type == 1) {
        gx.proj_mtx[1] = mem_read32(c, m + 0x0C);
        gx.proj_mtx[3] = mem_read32(c, m + 0x1C);
    } else {
        gx.proj_mtx[1] = mem_read32(c, m + 0x08);
        gx.proj_mtx[3] = mem_read32(c, m + 0x18);
    }
    emit_projection();
    gx_return(g, 0);
    return true;
}

/* GXSetProjectionv(v[7]): v[0] = 0.0 -> perspective, otherwise orthographic. */
static bool hle_set_projectionv(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 v = gx_arg(g, 0);
    CPUState* c = &g->state;
    gx.proj_type = (double)u2f(mem_read32(c, v)) == 0.0 ? 0u : 1u;
    for (int i = 0; i < 6; i++)
        gx.proj_mtx[i] = mem_read32(c, v + 4u + 4u * (u32)i);
    emit_projection();
    gx_return(g, 0);
    return true;
}

static bool hle_get_projectionv(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 v = gx_arg(g, 0);
    CPUState* c = &g->state;
    mem_write32(c, v, gx.proj_type ? f2u(1.0f) : f2u(0.0f));
    for (int i = 0; i < 6; i++)
        mem_write32(c, v + 4u + 4u * (u32)i, gx.proj_mtx[i]);
    gx_return(g, 0);
    return true;
}

/* Copies `count` words of guest memory into the stream (matrices: float bits unchanged). */
static void emit_words(GcrtCpu* g, u32 from, u32 count) {
    for (u32 i = 0; i < count; i++)
        gxw_u32(mem_read32(&g->state, from + 4u * i));
}

static bool hle_load_pos_mtx_imm(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 m = gx_arg(g, 0), id = gx_arg(g, 1);
    gxw_u8(0x10);
    gxw_u32((id * 4u) | 0x000B0000u);
    emit_words(g, m, 12);
    gx_return(g, 0);
    return true;
}

static bool hle_load_nrm_mtx_imm(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 m = gx_arg(g, 0), id = gx_arg(g, 1);
    gxw_u8(0x10);
    gxw_u32((id * 3u + 0x400u) | 0x00080000u);
    emit_words(g, m + 0x00, 3);
    emit_words(g, m + 0x10, 3);
    emit_words(g, m + 0x20, 3);
    gx_return(g, 0);
    return true;
}

static bool hle_set_current_mtx(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.mat_idx_a, gx_arg(g, 0), 0, 6);
    gx_set_matrix_index(0);
    gx_return(g, 0);
    return true;
}

/* GXLoadTexMtxImm(mtx, id, type): id >= 0x40 -> post-matrices (0x500); 2x4: 8 words, otherwise 12 (the header
 * follows the type GX_MTX2x4 = 1, the copy follows type != 0, like the SDK). */
static bool hle_load_tex_mtx_imm(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 m = gx_arg(g, 0), id = gx_arg(g, 1), type = gx_arg(g, 2);
    const u32 addr = id >= 0x40u ? 0x500u + (id - 0x40u) * 4u : id * 4u;
    const u32 count = (s32)type == 1 ? 8u : 12u;
    gxw_u8(0x10);
    gxw_u32(addr | ((count - 1u) << 16));
    emit_words(g, m, type == 0 ? 12u : 8u);
    gx_return(g, 0);
    return true;
}

/* __GXSetViewport: XF 0x101A, 6 floats (scales and origins, guard band of 342, depth * zScale). */
static void set_viewport(void) {
    const float half = 0.5f, guard = 342.0f;
    const float sx = gx_fmuls(gx.vp_wd, half);
    const float sy = gx_fmuls(-(double)gx.vp_ht, half);
    const float hy = gx_fmuls(gx.vp_ht, half);
    const float zmax = gx_fmuls(gx.vp_farz, gx.z_scale);
    const float zmin = gx_fmuls(gx.vp_nearz, gx.z_scale);
    const float sz = gx_fsubs(zmax, zmin);
    const float ox = gx_fadds(guard, gx_fadds(gx.vp_left, sx));
    const float oy = gx_fadds(guard, gx_fadds(gx.vp_top, hy));
    const float oz = gx_fadds(zmax, gx.z_offset);
    gxw_u8(0x10);
    gxw_u32(0x0005101Au);
    gxw_u32(f2u(sx));
    gxw_u32(f2u(sy));
    gxw_u32(f2u(sz));
    gxw_u32(f2u(ox));
    gxw_u32(f2u(oy));
    gxw_u32(f2u(oz));
}

static bool hle_set_viewport_internal(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    set_viewport();
    gx_return(g, 0);
    return true;
}

static void store_viewport(GcrtCpu* g, double top) {
    gx.vp_left = (float)gx_farg(g, 0);
    gx.vp_top = (float)top;
    gx.vp_wd = (float)gx_farg(g, 2);
    gx.vp_ht = (float)gx_farg(g, 3);
    gx.vp_nearz = (float)gx_farg(g, 4);
    gx.vp_farz = (float)gx_farg(g, 5);
}

static bool hle_set_viewport(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    store_viewport(g, gx_farg(g, 1));
    set_viewport();
    gx.bp_sent_not = 1;
    gx_return(g, 0);
    return true;
}

/* GXSetViewportJitter(..., field): even field (field == 0) -> Y origin shifted by 0.5. */
static bool hle_set_viewport_jitter(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    double top = gx_farg(g, 1);
    if (gx_arg(g, 0) == 0)
        top = gx_fsubs(top, 0.5f);
    store_viewport(g, top);
    set_viewport();
    gx.bp_sent_not = 1;
    gx_return(g, 0);
    return true;
}

static bool hle_get_viewportv(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 v = gx_arg(g, 0);
    const float f[6] = {gx.vp_left, gx.vp_top, gx.vp_wd, gx.vp_ht, gx.vp_nearz, gx.vp_farz};
    for (int i = 0; i < 6; i++)
        mem_write32(&g->state, v + 4u * (u32)i, f2u(f[i]));
    gx_return(g, 0);
    return true;
}

static bool hle_set_scissor(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 left = gx_arg(g, 0), top = gx_arg(g, 1), wd = gx_arg(g, 2), ht = gx_arg(g, 3);
    const u32 tp = top + 0x156u, lf = left + 0x156u;
    gx_set(&gx.su_scis0, tp, 0, 11);
    gx_set(&gx.su_scis0, lf, 12, 11);
    gx_set(&gx.su_scis1, tp + ht - 1u, 0, 11);
    gx_set(&gx.su_scis1, lf + wd - 1u, 12, 11);
    gxw_bp(gx.su_scis0);
    gxw_bp(gx.su_scis1);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_scissor_box_offset(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 x = gx_arg(g, 0) + 0x156u, y = gx_arg(g, 1) + 0x156u;
    u32 reg = 0;
    gx_set(&reg, x >> 1, 0, 10);
    gx_set(&reg, y >> 1, 10, 10);
    gx_set(&reg, 0x59, 24, 8);
    gxw_bp(reg);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_clip_mode(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gxw_xf(0x1005, gx_arg(g, 0));
    gx.bp_sent_not = 1;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Lighting
 * ------------------------------------------------------------------------------------------- */

/* GXLoadLightObjImm(obj, light): XF 0x600 + 16 * n, 16 words: 3 zeros, color, then the 12 floats of the object. */
static bool hle_load_light_obj_imm(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const u32 obj = gx_arg(g, 0), light = gx_arg(g, 1);
    const u32 idx = 31u - cntlzw(light);
    const u32 addr = 0x600u + ((idx << 4) & 0x70u);
    gxw_u8(0x10);
    gxw_u32(addr | 0x000F0000u);
    gxw_u32(0);
    gxw_u32(0);
    gxw_u32(0);
    gxw_u32(mem_read32(&g->state, obj + 0x0C));
    emit_words(g, obj + 0x10, 12);
    gx.bp_sent_not = 1;
    gx_return(g, 0);
    return true;
}

/* GXSetChanAmbColor / GXSetChanMatColor(chan, color*): XF 0x100A/0x100C + n. */
static void chan_color(GcrtCpu* g, u32* regs, u32 xf_base) {
    const s32 chan = (s32)gx_arg(g, 0);
    const u32 cp = gx_arg(g, 1);
    const u32 color = mem_read32(&g->state, cp);
    u32 n, v;
    switch (chan) {
    case 0: n = 0, v = (regs[0] & 0xFFu) | (color & 0xFFFFFF00u); break;
    case 1: n = 1, v = (regs[1] & 0xFFu) | (color & 0xFFFFFF00u); break;
    case 2: n = 0, v = (regs[0] & 0xFFFFFF00u) | mem_read8(&g->state, cp + 3); break;
    case 3: n = 1, v = (regs[1] & 0xFFFFFF00u) | mem_read8(&g->state, cp + 3); break;
    case 4: n = 0, v = color; break;
    case 5: n = 1, v = color; break;
    default: return;
    }
    gxw_xf(xf_base + n, v);
    gx.bp_sent_not = 1;
    regs[n] = v;
}

static bool hle_set_chan_amb_color(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    chan_color(g, gx.amb_color, 0x100A);
    gx_return(g, 0);
    return true;
}

static bool hle_set_chan_mat_color(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    chan_color(g, gx.mat_color, 0x100C);
    gx_return(g, 0);
    return true;
}

static bool hle_set_num_chans(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 n = gx_arg(g, 0);
    gx_set(&gx.gen_mode, n, 4, 3);
    gxw_xf(0x1009, n & 0xFFu);
    gx.dirty_state |= GX_DIRTY_GEN_MODE;
    gx_return(g, 0);
    return true;
}

/* GXSetChanCtrl(chan, enable, ambSrc, matSrc, lightMask, diffFn, attnFn). */
static bool hle_set_chan_ctrl(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 chan = gx_arg(g, 0), enable = gx_arg(g, 1) & 0xFFu, amb = gx_arg(g, 2), mat = gx_arg(g, 3);
    const u32 mask = gx_arg(g, 4);
    u32 diff = gx_arg(g, 5);
    const u32 attn = gx_arg(g, 6);
    u32 reg = 0;
    gx_set(&reg, enable, 1, 1);
    gx_set(&reg, mat, 0, 1);
    gx_set(&reg, amb, 6, 1);
    if (attn == 0)
        diff = 0;
    gx_set(&reg, diff, 7, 2);
    gx_set(&reg, attn != 2, 9, 1);
    gx_set(&reg, attn != 0, 10, 1);
    gx_set(&reg, mask, 2, 4);
    gx_set(&reg, mask >> 4, 11, 4);
    gxw_xf(0x100Eu + (chan & 3u), reg);
    if (chan == 4)
        gxw_xf(0x1010, reg);
    else if (chan == 5)
        gxw_xf(0x1011, reg);
    gx.bp_sent_not = 1;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * EFB copies (display and texture)
 * ------------------------------------------------------------------------------------------- */

static void copy_src(u32* src, u32* size, u32 left, u32 top, u32 wd, u32 ht) {
    *src = 0;
    gx_set(src, left & 0xFFFFu, 0, 10);
    gx_set(src, top & 0xFFFFu, 10, 10);
    gx_set(src, 0x49, 24, 8);
    *size = 0;
    gx_set(size, (wd & 0xFFFFu) - 1u, 0, 10);
    gx_set(size, (ht & 0xFFFFu) - 1u, 10, 10);
    gx_set(size, 0x4A, 24, 8);
}

static bool hle_set_disp_copy_src(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    copy_src(&gx.cp_disp_src, &gx.cp_disp_size, gx_arg(g, 0), gx_arg(g, 1), gx_arg(g, 2), gx_arg(g, 3));
    gx_return(g, 0);
    return true;
}

static bool hle_set_tex_copy_src(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    copy_src(&gx.cp_tex_src, &gx.cp_tex_size, gx_arg(g, 0), gx_arg(g, 1), gx_arg(g, 2), gx_arg(g, 3));
    gx_return(g, 0);
    return true;
}

static bool hle_set_disp_copy_dst(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 wd = gx_arg(g, 0);
    gx.cp_disp_stride = 0;
    gx_set(&gx.cp_disp_stride, (u32)((s32)((wd << 1) & 0xFFFEu) >> 5), 0, 10);
    gx_set(&gx.cp_disp_stride, 0x4D, 24, 8);
    gx_return(g, 0);
    return true;
}

/* __GetImageTileCount(fmt, wd, ht, &rows, &cols, &cmp): 8x8, 8x4 or 4x4 tiles depending on the format (table of the
 * SDK function). */
void gx_get_image_tile_count(u32 fmt, u32 wd, u32 ht, u32* row, u32* col, u32* cmp) {
    u32 tw = 0, th = 0;
    switch (fmt) {
    case 0x00: case 0x08: case 0x0E: case 0x20: case 0x30:
        tw = 3, th = 3;
        break;
    case 0x01: case 0x02: case 0x09: case 0x11: case 0x22: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x39:
    case 0x3A:
        tw = 3, th = 2;
        break;
    case 0x03: case 0x04: case 0x05: case 0x06: case 0x0A: case 0x13: case 0x16: case 0x23: case 0x2B: case 0x2C:
    case 0x3C:
        tw = 2, th = 2;
        break;
    default:
        break;
    }
    wd &= 0xFFFFu;
    ht &= 0xFFFFu;
    if (!wd)
        wd = 1;
    if (!ht)
        ht = 1;
    *row = (u32)((s32)(wd + (1u << tw) - 1u) >> tw);
    *col = (u32)((s32)(ht + (1u << th) - 1u) >> th);
    *cmp = (fmt == 0x06 || fmt == 0x16) ? 2u : 1u;
}

/* GXSetTexCopyDst(wd, ht, fmt, mipmap). */
static bool hle_set_tex_copy_dst(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 wd = gx_arg(g, 0), ht = gx_arg(g, 1), fmt = gx_arg(g, 2), mip = gx_arg(g, 3);
    gx.cp_tex_z = 0;
    u32 pe_fmt = fmt & 0xFu;
    if (fmt == 0x13)
        pe_fmt = 0xB;
    if ((s32)fmt == 0x26 || ((s32)fmt >= 0 && (s32)fmt < 4))
        gx_set(&gx.cp_tex, 3, 15, 2);
    else
        gx_set(&gx.cp_tex, 2, 15, 2);
    gx.cp_tex_z = (fmt & 0x10u) == 0x10u;
    gx_set(&gx.cp_tex, pe_fmt >> 3, 3, 1);
    pe_fmt &= 7u;
    u32 rows, cols, cmp;
    gx_get_image_tile_count(fmt, wd, ht, &rows, &cols, &cmp);
    gx.cp_tex_stride = 0;
    gx_set(&gx.cp_tex_stride, rows * cmp, 0, 10);
    gx_set(&gx.cp_tex_stride, 0x4D, 24, 8);
    gx_set(&gx.cp_tex, mip, 9, 1);
    gx_set(&gx.cp_tex, pe_fmt, 4, 3);
    gx_return(g, 0);
    return true;
}

static bool hle_set_disp_copy_frame2field(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.cp_disp, gx_arg(g, 0), 12, 2);
    gx_set(&gx.cp_tex, 0, 12, 2);
    gx_return(g, 0);
    return true;
}

/* GXSetCopyClamp(clamp) (catalog name: __GXGetNumXfbLines; the code is that of GXSetCopyClamp). */
static bool hle_set_copy_clamp(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 clamp = gx_arg(g, 0);
    const u32 top = (clamp & 1u) == 1u, bottom = (clamp & 2u) == 2u;
    gx_set(&gx.cp_disp, top, 0, 1);
    gx_set(&gx.cp_disp, bottom, 1, 1);
    gx_set(&gx.cp_tex, top, 0, 1);
    gx_set(&gx.cp_tex, bottom, 1, 1);
    gx_return(g, 0);
    return true;
}

/* GXSetDispCopyYScale(yscale): BP 0x4E = 256 / yscale (9 bits), returns the number of XFB lines. */
static bool hle_set_disp_copy_y_scale(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_fpu(g);
    const float q = gx_fdivs(256.0f, gx_farg(g, 0));
    const u32 scale = gx_cvt_fp2unsigned(q) & 0x1FFu;
    u32 reg = 0;
    gx_set(&reg, scale, 0, 9);
    gx_set(&reg, 0x4E, 24, 8);
    gxw_bp(reg);
    gx.bp_sent_not = 0;
    gx_set(&gx.cp_disp, scale != 0x100u, 10, 1);
    const u32 height = ((gx.cp_disp_size >> 10) & 0x3FFu) + 1u;
    u32 lines = scale ? (((height - 1u) << 8) / scale) + 1u : 0u;
    if (scale > 0x80u && scale < 0x100u) {
        u32 t = scale;
        while (!(t & 1u))
            t >>= 1;
        if (height % t == 0)
            lines++;
    }
    if (lines > 0x400u)
        lines = 0x400u;
    gx_return(g, lines);
    return true;
}

/* GXSetCopyClear(color*, z): BP 0x4F (alpha, red), 0x50 (green, blue), 0x51 (depth). */
static bool hle_set_copy_clear(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 cp = gx_arg(g, 0), z = gx_arg(g, 1);
    CPUState* c = &g->state;
    const u32 r = mem_read8(c, cp), gg = mem_read8(c, cp + 1), b = mem_read8(c, cp + 2), a = mem_read8(c, cp + 3);
    gxw_bp(0x4F000000u | (a << 8) | r);
    gxw_bp(0x50000000u | (gg << 8) | b);
    gxw_bp(0x51000000u | (z & 0xFFFFFFu));
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* GXSetCopyFilter(aa, samplePattern[12][2], vf, vfilter[7]): BP 0x01-0x04 (sample patterns), 0x53/0x54 (vertical
 * filter). */
static bool hle_set_copy_filter(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 aa = gx_arg(g, 0) & 0xFFu, sp = gx_arg(g, 1), vf = gx_arg(g, 2) & 0xFFu, vfl = gx_arg(g, 3);
    CPUState* c = &g->state;
    u32 ms[4];
    if (aa) {
        for (u32 k = 0; k < 4; k++) {
            u32 v = 0;
            for (u32 j = 0; j < 6; j++)
                gx_set(&v, mem_read8(c, sp + k * 6u + j), j * 4u, 4);
            gx_set(&v, k + 1u, 24, 8);
            ms[k] = v;
        }
    } else {
        ms[0] = 0x01666666u, ms[1] = 0x02666666u, ms[2] = 0x03666666u, ms[3] = 0x04666666u;
    }
    for (u32 k = 0; k < 4; k++)
        gxw_bp(ms[k]);
    u32 c0 = 0x53000000u, c1 = 0x54000000u;
    if (vf) {
        gx_set(&c0, mem_read8(c, vfl + 0), 0, 6);
        gx_set(&c1, mem_read8(c, vfl + 4), 0, 6);
        gx_set(&c0, mem_read8(c, vfl + 1), 6, 6);
        gx_set(&c0, mem_read8(c, vfl + 2), 12, 6);
        gx_set(&c1, mem_read8(c, vfl + 5), 6, 6);
        gx_set(&c0, mem_read8(c, vfl + 3), 18, 6);
        gx_set(&c1, mem_read8(c, vfl + 6), 12, 6);
    } else {
        gx_set(&c0, 0, 0, 6);
        gx_set(&c0, 0, 6, 6);
        gx_set(&c1, 0x15, 0, 6);
        gx_set(&c0, 0x15, 12, 6);
        gx_set(&c1, 0, 6, 6);
        gx_set(&c1, 0, 12, 6);
        gx_set(&c0, 0x16, 18, 6);
    }
    gxw_bp(c0);
    gxw_bp(c1);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_set_disp_copy_gamma(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gx_set(&gx.cp_disp, gx_arg(g, 0), 7, 2);
    gx_return(g, 0);
    return true;
}

/* Z and color modes forced during a copy that clears (Z always, write; no blending). */
static void copy_clear_modes(void) {
    u32 z = gx.zmode;
    gx_set(&z, 1, 0, 1);
    gx_set(&z, 7, 1, 3);
    gxw_bp(z);
    u32 c0 = gx.cmode0;
    gx_set(&c0, 0, 0, 1);
    gx_set(&c0, 0, 1, 1);
    gxw_bp(c0);
}

/* GXCopyDisp(dest, clear): EFB -> XFB copy (end of frame for the GX bridge). */
static bool hle_copy_disp(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 dest = gx_arg(g, 0), clear = gx_arg(g, 1) & 0xFFu;
    if (clear)
        copy_clear_modes();
    bool pe_changed = false;
    if (clear || (gx.pe_ctrl & 7u) == 3u) {
        if (((gx.pe_ctrl >> 6) & 1u) == 1u) {
            u32 pe = gx.pe_ctrl;
            gx_set(&pe, 0, 6, 1);
            gxw_bp(pe);
            pe_changed = true;
        }
    }
    gxw_bp(gx.cp_disp_src);
    gxw_bp(gx.cp_disp_size);
    gxw_bp(gx.cp_disp_stride);
    gxw_bp(0x4B000000u | ((dest >> 5) & 0x1FFFFFu));
    gx_set(&gx.cp_disp, clear, 11, 1);
    gx_set(&gx.cp_disp, 1, 14, 1);
    gx_set(&gx.cp_disp, 0x52, 24, 8);
    gxw_bp(gx.cp_disp);
    if (clear) {
        gxw_bp(gx.zmode);
        gxw_bp(gx.cmode0);
    }
    if (pe_changed)
        gxw_bp(gx.pe_ctrl);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* GXCopyTex(dest, clear): EFB -> texture copy (the destination is a guest address; Aurora keeps the copy). */
static bool hle_copy_tex(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    const u32 dest = gx_arg(g, 0), clear = gx_arg(g, 1) & 0xFFu;
    if (clear)
        copy_clear_modes();
    bool pe_changed = false;
    u32 pe = gx.pe_ctrl;
    if (gx.cp_tex_z && (pe & 7u) != 3u) {
        gx_set(&pe, 3, 0, 3);
        pe_changed = true;
    }
    if ((clear || (pe & 7u) == 3u) && ((pe >> 6) & 1u) == 1u) {
        gx_set(&pe, 0, 6, 1);
        pe_changed = true;
    }
    if (pe_changed)
        gxw_bp(pe);
    gxw_bp(gx.cp_tex_src);
    gxw_bp(gx.cp_tex_size);
    gxw_bp(gx.cp_tex_stride);
    gxw_bp(0x4B000000u | ((dest >> 5) & 0x1FFFFFu));
    gx_set(&gx.cp_tex, clear, 11, 1);
    gx_set(&gx.cp_tex, 0, 14, 1);
    gx_set(&gx.cp_tex, 0x52, 24, 8);
    gxw_bp(gx.cp_tex);
    if (clear) {
        gxw_bp(gx.zmode);
        gxw_bp(gx.cmode0);
    }
    if (pe_changed)
        gxw_bp(gx.pe_ctrl);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

static bool hle_clear_bounding_box(GcrtCpu* g, u32 address) {
    (void)address;
    gx_enter(g);
    gxw_bp(0x550003FFu);
    gxw_bp(0x560003FFu);
    gx.bp_sent_not = 0;
    gx_return(g, 0);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Installation
 * ------------------------------------------------------------------------------------------- */

void gcrt_native_gx_attr_install(GcrtCpu* g) {
    gx_replace(g, "GXSetVtxDesc", hle_set_vtx_desc, NULL, 0, NULL);
    gx_replace(g, "GXSetVtxDescv", hle_set_vtx_descv, NULL, 0, NULL);
    gx_replace(g, "GXGetVtxDesc", hle_get_vtx_desc, NULL, 0, NULL);
    gx_replace(g, "GXGetVtxDescv", hle_get_vtx_descv, NULL, 0, NULL);
    gx_replace(g, "GXClearVtxDesc", hle_clear_vtx_desc, NULL, 0, NULL);
    gx_replace(g, "__GXSetVCD", hle_set_vcd, NULL, 0, NULL);
    gx_replace(g, "__GXCalculateVLim", hle_calculate_vlim, NULL, 0, NULL);
    gx_replace(g, "GXSetVtxAttrFmt", hle_set_vtx_attr_fmt, NULL, 0, NULL);
    gx_replace(g, "GXSetVtxAttrFmtv", hle_set_vtx_attr_fmtv, NULL, 0, NULL);
    gx_replace(g, "__GXSetVAT", hle_set_vat, NULL, 0, NULL);
    gx_replace(g, "GXGetVtxAttrFmt", hle_get_vtx_attr_fmt, NULL, 0, NULL);
    gx_replace(g, "GXGetVtxAttrFmtv", hle_get_vtx_attr_fmtv, NULL, 0, NULL);
    gx_replace(g, "GXSetArray", hle_set_array, NULL, 0, NULL);
    gx_replace(g, "GXSetTexCoordGen2", hle_set_tex_coord_gen2, NULL, 0, NULL);
    gx_replace(g, "GXSetNumTexGens", hle_set_num_tex_gens, NULL, 0, NULL);
    gx_replace(g, "GXSetLineWidth", hle_set_line_width, NULL, 0, NULL);
    gx_replace(g, "GXSetPointSize", hle_set_point_size, NULL, 0, NULL);
    gx_replace(g, "GXEnableTexOffsets", hle_enable_tex_offsets, NULL, 0, NULL);
    gx_replace(g, "GXSetCullMode", hle_set_cull_mode, NULL, 0, NULL);
    gx_replace(g, "GXSetCoPlanar", hle_set_co_planar, NULL, 0, NULL);
    gx_replace(g, "__GXSetGenMode", hle_set_gen_mode, NULL, 0, NULL);
    gx_replace(g, "__GXSetMatrixIndex", hle_set_matrix_index, NULL, 0, NULL);
    gx_replace(g, "GXSetProjection", hle_set_projection, NULL, 0, NULL);
    gx_replace(g, "GXSetProjectionv", hle_set_projectionv, NULL, 0, NULL);
    gx_replace(g, "GXGetProjectionv", hle_get_projectionv, NULL, 0, NULL);
    gx_replace(g, "GXLoadPosMtxImm", hle_load_pos_mtx_imm, NULL, 0, NULL);
    gx_replace(g, "GXLoadNrmMtxImm", hle_load_nrm_mtx_imm, NULL, 0, NULL);
    gx_replace(g, "GXSetCurrentMtx", hle_set_current_mtx, NULL, 0, NULL);
    gx_replace(g, "GXLoadTexMtxImm", hle_load_tex_mtx_imm, NULL, 0, NULL);
    gx_replace(g, "__GXSetViewport", hle_set_viewport_internal, NULL, 0, NULL);
    gx_replace(g, "GXSetViewportJitter", hle_set_viewport_jitter, NULL, 0, NULL);
    gx_replace(g, "GXSetViewport", hle_set_viewport, NULL, 0, NULL);
    gx_replace(g, "GXGetViewportv", hle_get_viewportv, NULL, 0, NULL);
    gx_replace(g, "GXSetScissor", hle_set_scissor, NULL, 0, NULL);
    gx_replace(g, "GXSetScissorBoxOffset", hle_set_scissor_box_offset, NULL, 0, NULL);
    gx_replace(g, "GXSetClipMode", hle_set_clip_mode, NULL, 0, NULL);
    gx_replace(g, "GXLoadLightObjImm", hle_load_light_obj_imm, NULL, 0, NULL);
    gx_replace(g, "GXSetChanAmbColor", hle_set_chan_amb_color, NULL, 0, NULL);
    gx_replace(g, "GXSetChanMatColor", hle_set_chan_mat_color, NULL, 0, NULL);
    gx_replace(g, "GXSetNumChans", hle_set_num_chans, NULL, 0, NULL);
    gx_replace(g, "GXSetChanCtrl", hle_set_chan_ctrl, NULL, 0, NULL);
    gx_replace(g, "GXSetDispCopySrc", hle_set_disp_copy_src, NULL, 0, NULL);
    gx_replace(g, "GXSetTexCopySrc", hle_set_tex_copy_src, NULL, 0, NULL);
    gx_replace(g, "GXSetDispCopyDst", hle_set_disp_copy_dst, NULL, 0, NULL);
    gx_replace(g, "GXSetTexCopyDst", hle_set_tex_copy_dst, NULL, 0, NULL);
    gx_replace(g, "GXSetDispCopyFrame2Field", hle_set_disp_copy_frame2field, NULL, 0, NULL);
    gx_replace(g, "__GXGetNumXfbLines", hle_set_copy_clamp, NULL, 0, NULL);
    gx_replace(g, "GXSetDispCopyYScale", hle_set_disp_copy_y_scale, NULL, 0, NULL);
    gx_replace(g, "GXSetCopyClear", hle_set_copy_clear, NULL, 0, NULL);
    gx_replace(g, "GXSetCopyFilter", hle_set_copy_filter, NULL, 0, NULL);
    gx_replace(g, "GXSetDispCopyGamma", hle_set_disp_copy_gamma, NULL, 0, NULL);
    gx_replace(g, "GXCopyDisp", hle_copy_disp, NULL, 0, NULL);
    gx_replace(g, "GXCopyTex", hle_copy_tex, NULL, 0, NULL);
    gx_replace(g, "GXClearBoundingBox", hle_clear_bounding_box, NULL, 0, NULL);
}
