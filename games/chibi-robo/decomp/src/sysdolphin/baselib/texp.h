// HSD TEV expressions (sysdolphin): HSD_TExp, the nodes from which the library compiles a material's TEV stages.
//
// Names, values and layout from cbr_decomp's texp.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). Only the fields decompiled code uses are described; their offsets are checked against this
// game's code (static asserts below, oracle). In this game the enumerations stored in the constant node (input and
// type, before `reg`) take one byte each: the reference count is the byte at 0x10.
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_TEXP_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_TEXP_H

#include "decomp/gc_mem.h"

#include <cstddef>
#include <cstdint>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::s32;
using gc::u32;
using gc::u8;

// Kind of a TEV expression node (HSD_TExpType). Stored as a 32-bit word in the node.
enum class HSD_TExpType : s32 {
    zero = 0,     // HSD_TE_ZERO: no node (null pointer)
    tev = 1,      // HSD_TE_TEV: a TEV stage
    texture = 2,  // HSD_TE_TEX: the texture colour (marker pointer, see below)
    raster = 3,   // HSD_TE_RAS: the rasterised colour (marker pointer)
    constant = 4, // HSD_TE_CNST: a constant
    immediate = 5,
};

union HSD_TExp;

// One input of a TEV stage node (HSD_TEArg): how it is fed (`type`, an HSD_TExpType value), the GX argument chosen
// for it (`arg`) and the node it comes from.
struct HSD_TEArg {
    be<u8> type;
    be<u8> sel;
    be<u8> arg;
    ptr<HSD_TExp> exp;
};

// A TEV stage node: separate reference counts for its colour and alpha results, the constant colour and alpha
// selections (kcsel, kasel) and its four colour and four alpha inputs. In this game kcsel is at 0x20, one byte after
// cbr_decomp's layout: the eight bytes after a_ref are taken to be a_dst, a_op, a_clamp, a_bias, a_scale, a_range,
// tex_swap, ras_swap (Melee's sysdolphin has an a_range there; names only) [hypothesis; only kcsel, kasel and the
// inputs are used by decompiled code, at the offsets of this game's code].
struct HSD_TETev {
    be<HSD_TExpType> type;
    ptr<HSD_TExp> next;
    be<s32> c_ref;
    gc::array<be<u8>, 6> colour_settings; // c_dst, c_op, c_clamp, c_bias, c_scale, c_range
    be<s32> a_ref;
    gc::array<be<u8>, 8> alpha_settings; // a_dst, a_op, a_clamp, a_bias, a_scale, a_range, tex_swap, ras_swap
    be<u8> kcsel;                        // GX constant colour selection of the stage
    be<u8> kasel;                        // GX constant alpha selection of the stage
    gc::array<HSD_TEArg, 4> c_in;
    gc::array<HSD_TEArg, 4> a_in;
};

// A constant node.
struct HSD_TECnst {
    be<HSD_TExpType> type;
    ptr<HSD_TExp> next;
    ptr<void> val;
    be<u8> comp;  // component (HSD_TEInput): texp_input_x for one component of a register, otherwise a colour
    be<u8> ctype; // type of the value (HSD_TEType)
    be<u8> reg;   // register the constant is assigned to, texp_register_undefined while none is
    be<u8> idx;   // component of that register
    be<u8> ref;
};

union HSD_TExp {
    be<HSD_TExpType> type;
    HSD_TETev tev;
    HSD_TECnst cnst;
};

// Allocation state of a TEV expression compilation (HSD_TExpRes): for each of the eight registers (0-3 the constant
// colours, 4-7 the colour registers), which colour components and whether the alpha are used.
struct HSD_TExpRes {
    be<s32> failed;
    be<s32> texmap;
    be<s32> cnst_remain;
    struct Register {
        be<u8> color; // number of colour components used (3: the whole colour)
        be<u8> alpha; // 1 once the alpha is used
    };
    gc::array<Register, 8> reg;
};

// Values from cbr_decomp's texp.h and the Dolphin SDK's GXEnum.h (names in comments).
inline constexpr u8 texp_register_undefined = 0xFF; // HSD_TE_UNDEF
inline constexpr u8 texp_input_x = 6;               // HSD_TE_X: one component of a register
inline constexpr u8 texp_arg_immediate = 5;         // HSD_TE_IMM: input from a colour register
inline constexpr u8 texp_arg_konst = 6;             // HSD_TE_KONST: input from a constant (konst) selection
inline constexpr u8 gx_cc_konst = 0xE;              // GX_CC_KONST
inline constexpr u8 gx_ca_konst = 6;                // GX_CA_KONST

// Which result of a TEV stage a reference is to (HSD_TE_RGB = 1 in cbr_decomp; any other value: alpha).
inline constexpr u8 texp_select_colour = 1;

// HSD_TEXP_TEX and HSD_TEXP_RAS are not nodes but the marker pointer values -1 and -2 (gc::marker_ptr), which stand
// for the texture and the rasterised colour.
inline constexpr s32 texp_marker_texture = -1; // (HSD_TExp*) -1
inline constexpr s32 texp_marker_raster = -2;  // (HSD_TExp*) -2
inline bool is_texp_marker(ptr<const HSD_TExp> texp, s32 marker) {
    return gc::is_marker_ptr(texp, marker);
}

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_TETev, c_ref) == 8 && offsetof(HSD_TETev, a_ref) == 0x14);
static_assert(offsetof(HSD_TETev, kcsel) == 0x20 && offsetof(HSD_TETev, kasel) == 0x21);
static_assert(offsetof(HSD_TETev, c_in) == 0x24 && offsetof(HSD_TETev, a_in) == 0x44 && sizeof(HSD_TEArg) == 8);
static_assert(offsetof(HSD_TEArg, arg) == 2 && offsetof(HSD_TEArg, exp) == 4);
static_assert(offsetof(HSD_TECnst, comp) == 0xC && offsetof(HSD_TECnst, reg) == 0xE);
static_assert(offsetof(HSD_TECnst, idx) == 0xF && offsetof(HSD_TECnst, ref) == 0x10);
static_assert(offsetof(HSD_TExpRes, reg) == 0xC && sizeof(HSD_TExpRes::Register) == 2);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_TEXP_H
