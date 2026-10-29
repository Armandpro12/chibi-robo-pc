// HSD polygon objects (sysdolphin): HSD_PObj flags, the matrix marks and the shape blending of a shape-animated one.
//
// Original unit: sysdolphin/baselib/pobj.c [hypothesis: the functions sit between the pobj.c and tobj.c source-file
// strings in both versions]. The code was written from the disassembly of both versions (identical in US and PAL,
// the address of the global aside).
// Cross-check of pobj_set_shape_blend [hypothesis on the identity]: it has the logic and the fields of
// PObjUpdateFunc, with ShapeSetSetAnimResult inlined, in cbr_decomp's pobj.c (https://github.com/eavpsp/cbr_decomp,
// folder decomp_dtk_main/src, CC0 1.0); the first weight track 2 is HSD_A_S_W0 there. Structure names and layout:
// pobj.h. HSD_PObjSetFlags and HSD_PObjClearFlags are this project's names [hypothesis], after the HSD_MObj and
// HSD_CObj pairs of the same shape (notes of the decompilation stage). HSD_PObjClearMtxMark and mtx_mark[2] of
// {obj, mark}: names and layout from the same pobj.c; doldecomp/melee's sysdolphin uses the same function name (names
// only, see the project rules). The role of the marks is this project's [hypothesis].
#include "pobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"
#include "decomp_globals.h"

#include <cstddef>

// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
namespace hsd {

using gc::u32;

// One matrix mark: the object whose matrix was last loaded, and a mark of how [hypothesis on the meaning].
struct MatrixMark {
    ptr<void> object;
    be<u32> mark;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(MatrixMark) == 8 && offsetof(MatrixMark, mark) == 4);
#endif

// mtx_mark of pobj.c: two marks (HSD_PObjClearMtxMark writes both at once).
constexpr gc::global<gc::array<MatrixMark, 2>, global_id::hsd_pobj_mtx_mark> matrix_marks{};

// Sets both matrix marks to (`object`, `mark`). The original stores the words in the order object 0, mark 0,
// object 1, mark 1.
void HSD_PObjClearMtxMark(ptr<void> object, u32 mark) {
    for (auto& entry : *matrix_marks) {
        entry.object = object;
        entry.mark = mark;
    }
}

// Clears the flags of `mask` in the polygon's 16-bit flags; nothing if there is no polygon. Only the low 16 bits of
// `mask` matter (the original stores a halfword).
void HSD_PObjClearFlags(ptr<HSD_PObj> pobj, u32 mask) {
    if (gc::path::taken(pobj == nullptr)) // original: a conditional return
        return;
    pobj->flags = static_cast<u16>(gc::load(pobj->flags) & ~mask);
}

// Sets the flags of `mask` in the polygon's 16-bit flags; nothing if there is no polygon.
void HSD_PObjSetFlags(ptr<HSD_PObj> pobj, u32 mask) {
    if (gc::path::taken(pobj == nullptr)) // original: a conditional return
        return;
    pobj->flags = static_cast<u16>(gc::load(pobj->flags) | mask);
}

// Animation callback: stores the animated value `*value` as the shape blend weight of `polygon`, the weight number
// `track - 2` of the array when the shape set has one, its single weight otherwise. Nothing for a missing polygon
// or one that is not shape-animated. The float is moved as it is (lfs/stfs keep its bits). (The original returns by
// conditional returns on the first two tests and branches to the single weight: falls_through there.)
void pobj_set_shape_blend(ptr<HSD_PObj> polygon, s32 track, ptr<const be<f32>> value) {
    if (gc::path::taken(polygon == nullptr))
        return;
    if (gc::path::taken((gc::load(polygon->flags) & pobj_kind_mask) != pobj_kind_shape_animated))
        return;
    const ptr<HSD_ShapeSet> shape_set = polygon->u.shape_set;
    if (gc::path::falls_through(gc::load(shape_set->flags) & shape_set_weight_array)) {
        // weights + 4 * track - 8, wrapping modulo 2^32 as the original's add (no overflow in host arithmetic).
        const ptr<be<f32>> weights = shape_set->blend.weights;
        weights[static_cast<std::ptrdiff_t>(track) - 2] = *value;
    } else {
        shape_set->blend.weight = *value;
    }
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_pobj_clear_mtx_mark, hsd::HSD_PObjClearMtxMark, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_pobj_clear_flags, hsd::HSD_PObjClearFlags, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_pobj_set_flags, hsd::HSD_PObjSetFlags, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_pobj_set_shape_blend, hsd::pobj_set_shape_blend, gc::abi::flag_path_marked);
