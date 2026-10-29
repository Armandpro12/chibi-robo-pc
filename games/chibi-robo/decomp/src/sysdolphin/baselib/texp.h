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

// A TEV stage node: separate reference counts for its colour and alpha results.
struct HSD_TETev {
    be<HSD_TExpType> type;
    ptr<HSD_TExp> next;
    be<s32> c_ref;
    gc::array<be<u8>, 6> colour_settings; // c_dst, c_op, c_clamp, c_bias, c_scale, c_range
    be<s32> a_ref;
};

// A constant node.
struct HSD_TECnst {
    be<HSD_TExpType> type;
    ptr<HSD_TExp> next;
    ptr<void> val;
    gc::array<be<u8>, 4> settings; // comp, ctype, reg, idx
    be<u8> ref;
};

union HSD_TExp {
    be<HSD_TExpType> type;
    HSD_TETev tev;
    HSD_TECnst cnst;
};

// Which result of a TEV stage a reference is to (HSD_TE_RGB = 1 in cbr_decomp; any other value: alpha).
inline constexpr u8 texp_select_colour = 1;

// HSD_TEXP_TEX and HSD_TEXP_RAS are not nodes but the pointer values -1 and -2, which stand for the texture and the
// rasterised colour. True if `texp` is the marker of that value (marker_bits: 0xFFFFFFFF or 0xFFFFFFFE).
// [Proposal for the memory layer: a portable way to name such marker pointers; until then the two modes are written
// here, the guest one comparing the pointer's 32-bit value.]
inline bool is_texp_marker(ptr<const HSD_TExp> texp, u32 marker_bits) {
#if defined(GC_NATIVE_MEMORY)
    const auto marker = static_cast<std::intptr_t>(static_cast<s32>(marker_bits));
    return reinterpret_cast<std::intptr_t>(texp) == marker;
#else
    return gc::guest_address(texp) == marker_bits;
#endif
}
inline constexpr u32 texp_marker_texture = 0xFFFFFFFFu; // (HSD_TExp*) -1
inline constexpr u32 texp_marker_raster = 0xFFFFFFFEu;  // (HSD_TExp*) -2

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_TETev, c_ref) == 8 && offsetof(HSD_TETev, a_ref) == 0x14);
static_assert(offsetof(HSD_TECnst, ref) == 0x10);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_TEXP_H
