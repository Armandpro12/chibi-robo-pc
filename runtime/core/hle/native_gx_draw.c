/* Shapes of the SDK GX library (GXDraw.c), native (milestone 10c): GXDrawCylinder.
 *
 * In the SDK, GXDrawCylinder writes its vertices (positions and normals as f32) to the write-gather pipe after
 * GXBegin: it is SDK code that touches the hardware. The replacement redoes the function instruction by instruction
 * on the guest processor state (stack, floating-point registers, FPSCR, calls of the same functions) and appends the
 * vertices directly to the stream (gx_emit_vertex_u32) instead of writing them to the pipe. The constants are read
 * where the recognized code reads them (r2), the functions called are those of its `bl`: nothing is copied from the
 * game.
 *
 * GXDrawSphere, GXDrawCube and GXDrawCubeFace stay recompiled (never run in the bench scenarios: an unverified
 * replacement would be riskier than their writes to the pipe, which stay within an open draw and are therefore
 * counted as inline vertices, not as accesses "outside a draw"). Generic: no reference to a game. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "hle/native_gx.h"
#include "platform/trace.h"

#define MSR_FP 0x00002000u

/* Floating-point conversions of the processor's single-precision memory accesses (lfs/stfs), after the PowerPC
 * programming manual (load: extended exponent, denormals normalized; store: truncation, denormalization between 874
 * and 896). Identical to those of the recompiled code. */
static double lfs_value(u32 w) {
    const u32 exp = (w >> 23) & 0xFFu, frac = w & 0x007FFFFFu;
    u64 r;
    if (exp > 0 && exp < 255) {
        const u64 y = ((w >> 30) & 1u) ? 0u : 1u;
        r = ((u64)(w & 0xC0000000u) << 32) | (y << 61) | (y << 60) | (y << 59) | ((u64)(w & 0x3FFFFFFFu) << 29);
    } else if (exp == 0 && frac != 0) {
        s32 e = -126;
        u32 m = frac;
        while (!(m & 0x00800000u)) {
            m <<= 1;
            e--;
        }
        r = ((u64)(w & 0x80000000u) << 32) | ((u64)(e + 1023) << 52) | ((u64)(m & 0x007FFFFFu) << 29);
    } else {
        const u64 y = (w >> 30) & 1u;
        r = ((u64)(w & 0xC0000000u) << 32) | (y << 61) | (y << 60) | (y << 59) | ((u64)(w & 0x3FFFFFFFu) << 29);
    }
    double d;
    memcpy(&d, &r, 8);
    return d;
}

static u32 stfs_bits(double d) {
    u64 b;
    memcpy(&b, &d, 8);
    const u32 exp = (u32)((b >> 52) & 0x7FFu);
    if (exp <= 896u && exp >= 874u && (b & 0x7FFFFFFFFFFFFFFFull) != 0) {
        const u64 mant = (1ull << 52) | (b & 0x000FFFFFFFFFFFFFull);
        return (u32)((b >> 32) & 0x80000000u) | (u32)((mant >> (926u - exp)) & 0x007FFFFFu);
    }
    return (u32)(((b >> 32) & 0xC0000000u) | ((b >> 29) & 0x3FFFFFFFu));
}

static u64 f64_bits(double d) {
    u64 b;
    memcpy(&b, &d, 8);
    return b;
}

static double f64_value(u64 b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}

/* lfs fD, ea: ps0 and ps1. lfd: ps0 only. fneg: ps0 only. */
static void lfs(CPUState* c, unsigned d, u32 ea) {
    const double v = lfs_value(mem_read32(c, ea));
    c->fpr[d] = v;
    c->ps1[d] = v;
}
static void lfd(CPUState* c, unsigned d, u32 ea) { c->fpr[d] = f64_value(mem_read64(c, ea)); }
static void fneg(CPUState* c, unsigned d, unsigned b) { c->fpr[d] = f64_value(f64_bits(c->fpr[b]) ^ 0x8000000000000000ull); }
/* stfs to the pipe: vertex appended to the stream. */
static void put(CPUState* c, unsigned s) { gx_emit_vertex_u32(stfs_bits(c->fpr[s])); }

/* ---------------------------------------------------------------------------------------------
 * Reading the recognized code: relocated fields (addresses, r2 offsets, bl targets), checked instruction by instruction
 * ------------------------------------------------------------------------------------------- */

static bool dec_d(GcrtCpu* g, u32 fn, u32 off, u32 opcd, u32 rt, u32 ra, s16* d) {
    const u32 w = mem_read32(&g->state, fn + off);
    if ((w >> 26) != opcd || ((w >> 21) & 31u) != rt || ((w >> 16) & 31u) != ra)
        return false;
    *d = (s16)(w & 0xFFFFu);
    return true;
}

static bool dec_bl(GcrtCpu* g, u32 fn, u32 off, u32* target) {
    const u32 w = mem_read32(&g->state, fn + off);
    if ((w & 0xFC000003u) != 0x48000001u)
        return false;
    s32 li = (s32)(w & 0x03FFFFFCu);
    if (li & 0x02000000)
        li -= 0x04000000;
    *target = fn + off + (u32)li;
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * GXDrawCylinder(u8 numEdges)
 * ------------------------------------------------------------------------------------------- */

static struct {
    bool ok;
    u32 hi;                 /* lis r4 (+0x04) */
    u32 save;               /* r31: saved vertex state (GXGetVtxDescv / GXGetVtxAttrFmtv) */
    s16 fmt_off;            /* addi r4, r31, d (+0x48) */
    s16 k_top, k_a, k_b, k_zero, k_bot; /* lfs (r2) */
    s16 k_i2f, k_u2f;       /* lfd (r2): integer -> floating-point conversions */
    u32 get_desc, get_fmt, clear_desc, set_desc, set_fmt, fx, fy, begin, set_descv, set_fmtv;
    u64 calls, fpu_claims, failed_calls;
} cyl;

static bool call(GcrtCpu* g, u32 fn, const u32* args, unsigned n) {
    if (gcrt_guest_call(g, fn, args, n, NULL))
        return true;
    cyl.failed_calls++;
    return false;
}

static bool hle_draw_cylinder(GcrtCpu* g, u32 address) {
    CPUState* c = &g->state;
    const u32 fn = address;
    const u32 sp0 = c->gpr[1], lr = c->lr;
    const u32 sda2 = c->gpr[2];
    cyl.calls++;
    /* mflr r0 ; lis r4 ; stw r0, 4(r1) ; stwu r1, -0x3C8(r1) */
    c->gpr[0] = lr;
    c->gpr[4] = cyl.hi;
    mem_write32(c, sp0 + 4u, lr);
    const u32 fr = sp0 - 0x3C8u;
    mem_write32(c, fr, sp0);
    c->gpr[1] = fr;
    /* stfd f31: first floating-point instruction (lazy FPU of the OS). */
    if (!(c->msr & MSR_FP)) {
        cyl.fpu_claims++;
        gcrt_native_os_fpu_claim(g, fn + 0x10u);
    }
    for (unsigned k = 0; k < 6; k++)
        mem_write64(c, fr + 0x3C0u - 8u * k, f64_bits(c->fpr[31 - k]));
    for (unsigned r = 24; r < 32; r++) /* stmw r24, 0x378(r1) */
        mem_write32(c, fr + 0x378u + 4u * (r - 24u), c->gpr[r]);
    const u32 save = cyl.save;
    const u32 n = c->gpr[3] & 0xFFu; /* r30 */
    bool ok = true;

    lfs(c, 0, sda2 + (u32)(s32)cyl.k_top);
    fneg(c, 31, 0);
    /* Vertex format: saved, then direct position and normal as f32 (format 3). */
    {
        const u32 a0[1] = {save};
        const u32 a1[2] = {3u, save + (u32)(s32)cyl.fmt_off};
        const u32 a3[2] = {9u, 1u};
        const u32 a4[2] = {10u, 1u};
        const u32 a5[5] = {3u, 9u, 1u, 4u, 0u};
        const u32 a6[5] = {3u, 10u, 0u, 4u, 0u};
        ok = ok && call(g, cyl.get_desc, a0, 1) && call(g, cyl.get_fmt, a1, 2) && call(g, cyl.clear_desc, NULL, 0) &&
             call(g, cyl.set_desc, a3, 2) && call(g, cyl.set_desc, a4, 2) && call(g, cyl.set_fmt, a5, 5) &&
             call(g, cyl.set_fmt, a6, 5);
    }
    lfs(c, 27, sda2 + (u32)(s32)cyl.k_a);
    lfs(c, 28, sda2 + (u32)(s32)cyl.k_b);
    lfd(c, 29, sda2 + (u32)(s32)cyl.k_i2f);
    lfd(c, 30, sda2 + (u32)(s32)cyl.k_u2f);
    /* Tables x[i] (fr + 0x1D4) and y[i] (fr + 0x44), i = 0..numEdges: angle = a * (b * i) / numEdges. */
    const u32 xs = fr + 0x1D4u, ys = fr + 0x44u;
    for (u32 i = 0; ok && (s32)i <= (s32)n; i++) {
        mem_write32(c, fr + 0x36Cu, n);
        mem_write32(c, fr + 0x374u, i ^ 0x80000000u);
        mem_write32(c, fr + 0x370u, 0x43300000u);
        lfd(c, 0, fr + 0x370u);
        mem_write32(c, fr + 0x368u, 0x43300000u);
        ppc_fsubs(c, 1, 0, 29);
        lfd(c, 0, fr + 0x368u);
        ppc_fmuls(c, 1, 28, 1);
        ppc_fsubs(c, 0, 0, 30);
        ppc_fmuls(c, 1, 27, 1);
        ppc_fdivs(c, 26, 1, 0);
        c->fpr[1] = c->fpr[26];
        ok = call(g, cyl.fx, NULL, 0);
        mem_write32(c, xs + 4u * i, stfs_bits(c->fpr[1]));
        c->fpr[1] = c->fpr[26];
        ok = ok && call(g, cyl.fy, NULL, 0);
        mem_write32(c, ys + 4u * i, stfs_bits(c->fpr[1]));
    }
    /* Side: triangle strip, (numEdges + 1) * 2 vertices: (x, y, -h) n(x, y, 0); (x, y, h) n(x, y, 0). */
    if (ok) {
        const u32 b[3] = {0x98u, 3u, ((n + 1u) << 1) & 0xFFFEu};
        ok = call(g, cyl.begin, b, 3);
    }
    if (ok) {
        lfs(c, 1, sda2 + (u32)(s32)cyl.k_zero);
        lfs(c, 0, sda2 + (u32)(s32)cyl.k_top);
        for (u32 i = 0; (s32)i <= (s32)n; i++) {
            lfs(c, 3, xs + 4u * i);
            lfs(c, 2, ys + 4u * i);
            put(c, 3), put(c, 2), put(c, 31), put(c, 3), put(c, 2), put(c, 1);
            put(c, 3), put(c, 2), put(c, 0), put(c, 3), put(c, 2), put(c, 1);
        }
        /* Top: fan, numEdges + 2 vertices: center (0, 0, h) n(0, 0, h), then (x, -y, h) n(0, 0, h). */
        const u32 b[3] = {0xA0u, 3u, (n + 2u) & 0xFFFFu};
        ok = call(g, cyl.begin, b, 3);
    }
    if (ok) {
        lfs(c, 2, sda2 + (u32)(s32)cyl.k_zero);
        lfs(c, 1, sda2 + (u32)(s32)cyl.k_top);
        put(c, 2), put(c, 2), put(c, 1), put(c, 2), put(c, 2), put(c, 1);
        for (u32 i = 0; (s32)i <= (s32)n; i++) {
            lfs(c, 0, xs + 4u * i);
            put(c, 0);
            lfs(c, 0, ys + 4u * i);
            fneg(c, 0, 0);
            put(c, 0), put(c, 1), put(c, 2), put(c, 2), put(c, 1);
        }
        /* Bottom: fan, center (0, 0, -h) n(0, 0, k), then (x, y, -h) n(0, 0, k). */
        const u32 b[3] = {0xA0u, 3u, (n + 2u) & 0xFFFFu};
        ok = call(g, cyl.begin, b, 3);
    }
    if (ok) {
        lfs(c, 2, sda2 + (u32)(s32)cyl.k_zero);
        lfs(c, 1, sda2 + (u32)(s32)cyl.k_bot);
        put(c, 2), put(c, 2), put(c, 31), put(c, 2), put(c, 2), put(c, 1);
        for (u32 i = 0; (s32)i <= (s32)n; i++) {
            lfs(c, 0, xs + 4u * i);
            put(c, 0);
            lfs(c, 0, ys + 4u * i);
            put(c, 0), put(c, 31), put(c, 2), put(c, 2), put(c, 1);
        }
        /* Vertex format restored. */
        const u32 a0[1] = {save};
        const u32 a1[2] = {3u, save + (u32)(s32)cyl.fmt_off};
        ok = call(g, cyl.set_descv, a0, 1) && call(g, cyl.set_fmtv, a1, 2);
    }
    if (!ok && g->stop == GCRT_STOP_NONE)
        fprintf(stderr, "gcrt native GX: GXDrawCylinder interrupted (nested call failed)\n");
    /* Epilogue: lmw r24 (unchanged), lwz r0, 0x3CC(r1), lfd f31..f26, addi r1, r1, 0x3C8, blr. */
    c->gpr[0] = mem_read32(c, fr + 0x3CCu);
    for (unsigned k = 0; k < 6; k++)
        lfd(c, 31 - k, fr + 0x3C0u - 8u * k);
    c->gpr[1] = sp0;
    c->lr = c->gpr[0];
    gx_return(g, c->gpr[3]);
    return true;
}

static bool find_cylinder(GcrtCpu* g, u32 fn) {
    s16 hi = 0, lo = 0, top2 = 0;
    bool ok = gcrt_hle_catalog_size("GXDrawCylinder") == 0x6CCu;
    ok = ok && dec_d(g, fn, 0x04, 15, 4, 0, &hi) && dec_d(g, fn, 0x2C, 14, 31, 4, &lo) &&
         dec_d(g, fn, 0x48, 14, 4, 31, &cyl.fmt_off);
    ok = ok && dec_d(g, fn, 0x38, 48, 0, 2, &cyl.k_top) && dec_d(g, fn, 0xA0, 48, 27, 2, &cyl.k_a) &&
         dec_d(g, fn, 0xA8, 48, 28, 2, &cyl.k_b) && dec_d(g, fn, 0xAC, 50, 29, 2, &cyl.k_i2f) &&
         dec_d(g, fn, 0xB4, 50, 30, 2, &cyl.k_u2f) && dec_d(g, fn, 0x140, 48, 1, 2, &cyl.k_zero) &&
         dec_d(g, fn, 0x144, 48, 0, 2, &top2) && top2 == cyl.k_top && dec_d(g, fn, 0x4AC, 48, 1, 2, &cyl.k_bot);
    /* The other loads of the same constants (fans, remainder loops) must target the same addresses. */
    s16 z1 = 0, t1 = 0, t2 = 0, z2 = 0, z3 = 0, z4 = 0, b2 = 0;
    ok = ok && dec_d(g, fn, 0x2B4, 48, 2, 2, &z1) && dec_d(g, fn, 0x2BC, 48, 1, 2, &t1) &&
         dec_d(g, fn, 0x448, 48, 1, 2, &t2) && dec_d(g, fn, 0x450, 48, 0, 2, &z2) &&
         dec_d(g, fn, 0x4A4, 48, 2, 2, &z3) && dec_d(g, fn, 0x610, 48, 1, 2, &z4) &&
         dec_d(g, fn, 0x618, 48, 0, 2, &b2);
    ok = ok && z1 == cyl.k_zero && z2 == cyl.k_zero && z3 == cyl.k_zero && z4 == cyl.k_zero && t1 == cyl.k_top &&
         t2 == cyl.k_top && b2 == cyl.k_bot;
    u32 set_desc2 = 0, set_fmt2 = 0, begin2 = 0, begin3 = 0;
    ok = ok && dec_bl(g, fn, 0x40, &cyl.get_desc) && dec_bl(g, fn, 0x4C, &cyl.get_fmt) &&
         dec_bl(g, fn, 0x50, &cyl.clear_desc) && dec_bl(g, fn, 0x5C, &cyl.set_desc) && dec_bl(g, fn, 0x68, &set_desc2) &&
         dec_bl(g, fn, 0x80, &cyl.set_fmt) && dec_bl(g, fn, 0x98, &set_fmt2) && dec_bl(g, fn, 0x100, &cyl.fx) &&
         dec_bl(g, fn, 0x10C, &cyl.fy) && dec_bl(g, fn, 0x138, &cyl.begin) && dec_bl(g, fn, 0x2B0, &begin2) &&
         dec_bl(g, fn, 0x4A0, &begin3) && dec_bl(g, fn, 0x65C, &cyl.set_descv) && dec_bl(g, fn, 0x668, &cyl.set_fmtv);
    ok = ok && set_desc2 == cyl.set_desc && set_fmt2 == cyl.set_fmt && begin2 == cyl.begin && begin3 == cyl.begin;
    /* The calls must target the recognized SDK functions (the shape was recognized together with them). */
    unsigned count = 0;
    ok = ok && gcrt_hle_find("GXBegin", &count) == cyl.begin && count == 1;
    if (ok) {
        cyl.hi = (u32)(u16)hi << 16;
        cyl.save = cyl.hi + (u32)(s32)lo;
    }
    return ok;
}

void gcrt_native_gx_draw_install(GcrtCpu* g) {
    memset(&cyl, 0, sizeof(cyl));
    unsigned count = 0;
    const u32 fn = gcrt_hle_find("GXDrawCylinder", &count);
    if (!fn || count != 1)
        return; /* absent (or ambiguous): nothing to replace */
    if (!gcrt_native_os_active()) {
        /* Without the native OS, the lazy FPU cannot be served from the host: the shape stays recompiled. */
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "native GX: GXDrawCylinder stays recompiled (native OS absent)");
        return;
    }
    if (!find_cylinder(g, fn)) {
        fprintf(stderr, "gcrt native GX: GXDrawCylinder: unexpected code, it stays recompiled (its vertices will go "
                        "through the pipe)\n");
        return;
    }
    cyl.ok = true;
    gx_replace(g, "GXDrawCylinder", hle_draw_cylinder, NULL, 0, NULL);
    gcrt_trace_printf(GCRT_TRACE_NATIVE,
                      "native GX: GXDrawCylinder: state buffer %08X (+%d), constants r2%+d/%+d/%+d/%+d/%+d/%+d/%+d, "
                      "functions x %08X y %08X",
                      cyl.save, cyl.fmt_off, cyl.k_top, cyl.k_a, cyl.k_b, cyl.k_zero, cyl.k_bot, cyl.k_i2f, cyl.k_u2f,
                      cyl.fx, cyl.fy);
}

void gcrt_native_gx_draw_report(FILE* out) {
    if (!cyl.ok)
        return;
    fprintf(out, "native GX, SDK shapes: GXDrawCylinder %llu calls (%llu FPU claims, %llu failed nested "
                 "calls)\n",
            (unsigned long long)cyl.calls, (unsigned long long)cyl.fpu_claims, (unsigned long long)cyl.failed_calls);
}
