// HSD camera objects (sysdolphin): HSD_CObj accessors.
//
// Original unit: sysdolphin/baselib/cobj.c [hypothesis: HSD_CObjMtxIsDirty has the name, the fields and the logic of
// that function in that unit of cbr_decomp (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src,
// CC0 1.0), whose split does not place it at this address; the other names come from doldecomp/melee (names only)
// by their order in the unit, and cbr_decomp's cobj.c has functions of the same names with the same logic and
// fields: HSD_CObjSetScissor, HSD_CObjSetViewportf, HSD_CObjGetProjectionType, HSD_CObjSetFlags,
// HSD_CObjClearFlags]. The code was written from the disassembly of both versions (identical in US and PAL).
// Structures: cobj.h, wobj.h, object.h. Names are this project's [hypothesis] (notes of the decompilation stage).
#include "cobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

// 1 if the camera's viewing matrix is out of date: its own flag, or its eye position or interest moved.
u8 HSD_CObjMtxIsDirty(ptr<const HSD_CObj> cobj) {
    if (gc::load(cobj->flags) & cobj_mtx_dirty)
        return 1;
    const ptr<const HSD_WObj> eyepos = cobj->eyepos;
    if (eyepos != nullptr && (gc::load(eyepos->flags) & wobj_position_changed))
        return 1;
    const ptr<const HSD_WObj> interest = cobj->interest;
    if (interest != nullptr && (gc::load(interest->flags) & wobj_position_changed))
        return 1;
    return 0;
}

// Sets the camera's scissor box (in pixels) from `*scissor`; nothing if there is no camera. The original loads the
// whole box (two words) before storing it, so the box is first copied into a local: the result is the original's
// even when the source overlaps the camera's own box (anomaly A19; in the game the source is always a copy on the
// caller's stack).
void HSD_CObjSetScissor(ptr<HSD_CObj> cobj, ptr<const HSD_CObj::Scissor> scissor) {
    if (cobj == nullptr)
        return;
    const HSD_CObj::Scissor box = *scissor;
    cobj->scissor = box;
}

// Sets the camera's viewport from `*viewport`; nothing if there is no camera. The four floats are copied as they are
// (the original moves their bits with integer loads and stores), in the original's order: left and right are both
// loaded, then stored, then top and bottom; so the result is the original's even when the source overlaps the
// camera's own viewport (anomaly A19; in the game the source is always a copy on the caller's stack).
void HSD_CObjSetViewportf(ptr<HSD_CObj> cobj, ptr<const HSD_CObj::Viewport> viewport) {
    if (cobj == nullptr)
        return;
    const be<f32> left = viewport->left;
    const be<f32> right = viewport->right;
    cobj->viewport.left = left;
    cobj->viewport.right = right;
    const be<f32> top = viewport->top;
    const be<f32> bottom = viewport->bottom;
    cobj->viewport.top = top;
    cobj->viewport.bottom = bottom;
}

// The camera's projection type; perspective if there is no camera.
u8 HSD_CObjGetProjectionType(ptr<const HSD_CObj> cobj) {
    if (cobj == nullptr)
        return projection_perspective;
    return gc::load(cobj->projection_type);
}

// Sets the flags of `mask` in the camera's flags; nothing if there is no camera.
void HSD_CObjSetFlags(ptr<HSD_CObj> cobj, u32 mask) {
    if (cobj == nullptr)
        return;
    cobj->flags |= mask;
}

// Clears the flags of `mask` in the camera's flags; nothing if there is no camera.
void HSD_CObjClearFlags(ptr<HSD_CObj> cobj, u32 mask) {
    if (cobj == nullptr)
        return;
    cobj->flags &= ~mask;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_cobj_mtx_is_dirty, hsd::HSD_CObjMtxIsDirty, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_cobj_set_scissor, hsd::HSD_CObjSetScissor, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_cobj_set_viewportf, hsd::HSD_CObjSetViewportf, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_cobj_get_projection_type, hsd::HSD_CObjGetProjectionType, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_cobj_set_flags, hsd::HSD_CObjSetFlags, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_cobj_clear_flags, hsd::HSD_CObjClearFlags, gc::abi::flag_none);
