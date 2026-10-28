// HSD polygon objects (sysdolphin): HSD_PObj and the shape set of a shape-animated polygon.
//
// Names and layout from cbr_decomp's pobj.h and forward.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). The offsets the decompiled functions use are checked against this game's code (static
// asserts below, oracle); the other fields keep cbr_decomp's names and types [hypothesis: not used by decompiled
// code yet].
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_POBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_POBJ_H

#include "object.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::f32;
using gc::ptr;
using gc::s32;
using gc::u16;
using gc::u8;

struct HSD_JObj;

// Shape set of a shape-animated polygon: its flags, then (at 0x1C) either one blend weight or a pointer to an array
// of them, depending on a flag.
struct HSD_ShapeSet {
    be<u16> flags;
    be<u16> nb_shape;
    be<s32> nb_vertex_index;
    ptr<void> vertex_desc;
    ptr<void> vertex_idx_list;
    be<s32> nb_normal_index;
    ptr<void> normal_desc;
    ptr<void> normal_idx_list;
    union Blend {
        ptr<be<f32>> weights; // shape_set_weight_array set
        be<f32> weight;       // otherwise
    } blend;
};

// Bit 1 of a shape set's flags: several weights (array) instead of one (SHAPESET_ADDITIVE in cbr_decomp).
inline constexpr u16 shape_set_weight_array = 1u << 1;

struct HSD_PObj {
    HSD_Class parent;
    ptr<HSD_PObj> next;
    ptr<void> verts;  // vertex descriptor list
    be<u16> flags;    // kind in bits 12-13, other flags below
    be<u16> n_display;
    ptr<be<u8>> display;
    // What the word at 0x14 points to depends on the kind (bits 12-13 of the flags).
    union Contents {
        ptr<HSD_JObj> jobj;
        ptr<HSD_ShapeSet> shape_set; // shape-animated polygon
        ptr<void> envelope_list;
    } u;
};

// Kind of polygon: bits 12-13 of its flags; 0x1000: shape-animated (POBJ_SHAPEANIM in cbr_decomp).
inline constexpr u16 pobj_kind_mask = 0x3000;
inline constexpr u16 pobj_kind_shape_animated = 0x1000;

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_ShapeSet, blend) == 0x1C);
static_assert(offsetof(HSD_PObj, flags) == 0xC && offsetof(HSD_PObj, u) == 0x14 && sizeof(HSD_PObj) == 0x18);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_POBJ_H
