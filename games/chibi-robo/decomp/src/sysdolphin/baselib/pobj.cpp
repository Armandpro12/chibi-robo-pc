// HSD polygon objects (sysdolphin): HSD_PObj flags and the shape blending of a shape-animated one.
//
// Original unit: sysdolphin/baselib/pobj.c [hypothesis: the functions sit between the pobj.c and tobj.c source-file
// strings in both versions]. The code was written from the disassembly of both versions (identical in US and PAL).
// Cross-check of pobj_set_shape_blend [hypothesis on the identity]: it has the logic and the fields of
// PObjUpdateFunc, with ShapeSetSetAnimResult inlined, in cbr_decomp's pobj.c (https://github.com/eavpsp/cbr_decomp,
// folder decomp_dtk_main/src, CC0 1.0); the first weight track 2 is HSD_A_S_W0 there. Structure names and layout:
// pobj.h. HSD_PObjSetFlags and HSD_PObjClearFlags are this project's names [hypothesis], after the HSD_MObj and
// HSD_CObj pairs of the same shape (notes of the decompilation stage).
#include "pobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::u32;

// Clears the flags of `mask` in the polygon's 16-bit flags; nothing if there is no polygon. Only the low 16 bits of
// `mask` matter (the original stores a halfword).
void HSD_PObjClearFlags(ptr<HSD_PObj> pobj, u32 mask) {
    if (pobj == nullptr)
        return;
    pobj->flags = static_cast<u16>(gc::load(pobj->flags) & ~mask);
}

// Sets the flags of `mask` in the polygon's 16-bit flags; nothing if there is no polygon.
void HSD_PObjSetFlags(ptr<HSD_PObj> pobj, u32 mask) {
    if (pobj == nullptr)
        return;
    pobj->flags = static_cast<u16>(gc::load(pobj->flags) | mask);
}

// Animation callback: stores the animated value `*value` as the shape blend weight of `polygon`, the weight number
// `track - 2` of the array when the shape set has one, its single weight otherwise. Nothing for a missing polygon
// or one that is not shape-animated. The float is moved as it is (lfs/stfs keep its bits).
void pobj_set_shape_blend(ptr<HSD_PObj> polygon, s32 track, ptr<const be<f32>> value) {
    if (polygon == nullptr)
        return;
    if ((gc::load(polygon->flags) & pobj_kind_mask) != pobj_kind_shape_animated)
        return;
    const ptr<HSD_ShapeSet> shape_set = polygon->u.shape_set;
    if (gc::load(shape_set->flags) & shape_set_weight_array) {
        // weights + 4 * track - 8, wrapping modulo 2^32 as the original's add (no overflow in host arithmetic).
        const ptr<be<f32>> weights = shape_set->blend.weights;
        weights[static_cast<std::ptrdiff_t>(track) - 2] = *value;
    } else {
        shape_set->blend.weight = *value;
    }
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_pobj_clear_flags, hsd::HSD_PObjClearFlags, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_pobj_set_flags, hsd::HSD_PObjSetFlags, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_pobj_set_shape_blend, hsd::pobj_set_shape_blend, gc::abi::flag_none);
