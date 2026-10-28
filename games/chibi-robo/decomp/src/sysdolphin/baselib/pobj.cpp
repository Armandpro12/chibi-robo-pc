// HSD polygon objects (sysdolphin): HSD_PObj and the shape blending of a shape-animated one.
//
// Original unit: sysdolphin/baselib/pobj.c [hypothesis: the function sits between the pobj.c and tobj.c source-file
// strings in both versions; it is called only through a function pointer, by the animation code]. Names are
// hypotheses (notes of the decompilation stage); the code was written from the disassembly of both versions
// (identical in US and PAL).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::f32;
using gc::ptr;
using gc::s32;
using gc::u16;
using gc::u8;

// Shape set of a shape-animated polygon: its flags, then (at 0x1C) either one blend weight or a pointer to an array
// of them, depending on a flag.
struct ShapeSet {
    be<u16> flags;
    gc::array<be<u8>, 0x1A> not_described_here;
    union Blend {
        ptr<be<f32>> weights; // shape_set_weight_array set
        be<f32> weight;       // otherwise
    } blend;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(ShapeSet, blend) == 0x1C);
#endif

struct PolygonObject {
    gc::array<be<u8>, 0xC> not_described_here;
    be<u16> flags;
    gc::array<be<u8>, 6> not_described_here_too;
    ptr<ShapeSet> shape_set; // meaning of this word for a shape-animated polygon [hypothesis for the other kinds]
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(PolygonObject, flags) == 0xC && offsetof(PolygonObject, shape_set) == 0x14);
#endif

// Kind of polygon: bits 12-13 of its flags; 0x1000: shape-animated [hypothesis on the meaning: the only kind this
// function handles].
constexpr u16 pobj_kind_mask = 0x3000;
constexpr u16 pobj_kind_shape_animated = 0x1000;
// Bit 1 of a shape set's flags: several weights (array) instead of one.
constexpr u16 shape_set_weight_array = 1u << 1;

// Animation callback: stores the animated value `*value` as the shape blend weight of `polygon`, the weight number
// `track - 2` of the array when the shape set has one, its single weight otherwise. Nothing for a missing polygon
// or one that is not shape-animated. The float is moved as it is (lfs/stfs keep its bits).
void pobj_set_shape_blend(ptr<PolygonObject> polygon, s32 track, ptr<const be<f32>> value) {
    if (polygon == nullptr)
        return;
    if ((gc::load(polygon->flags) & pobj_kind_mask) != pobj_kind_shape_animated)
        return;
    const ptr<ShapeSet> shape_set = polygon->shape_set;
    if (gc::load(shape_set->flags) & shape_set_weight_array) {
        // weights + 4 * track - 8, wrapping modulo 2^32 as the original's add (no overflow in host arithmetic).
        const ptr<be<f32>> weights = shape_set->blend.weights;
        weights[static_cast<std::ptrdiff_t>(track) - 2] = *value;
    } else {
        shape_set->blend.weight = *value;
    }
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_pobj_set_shape_blend, hsd::pobj_set_shape_blend, gc::abi::flag_none);
