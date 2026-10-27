// HSD camera objects (sysdolphin): HSD_CObj, a camera with its eye-position and interest world objects.
//
// Original unit: sysdolphin/baselib/cobj.c [hypothesis: the function below has the name, the fields and the logic of
// HSD_CObjMtxIsDirty in that unit of cbr_decomp (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src,
// CC0 1.0); cbr_decomp's split does not place it at this address]. Only names and the structure layout are taken from cbr_decomp's cobj.h,
// wobj.h and object.h (names that doldecomp/melee's sysdolphin also uses: names only, see the project rules); the
// code was written from the disassembly of both versions (identical in US and PAL).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::f32;
using gc::ptr;
using gc::u16;
using gc::u32;
using gc::u8;

// Header of every HSD object: its class, then its reference counts.
struct HSD_Obj {
    ptr<void> class_info;
    be<u16> ref_count;
    be<u16> ref_count_individual;
};

// World object (a point the camera uses); only the header and the flags are described here.
struct HSD_WObj {
    HSD_Obj parent;
    be<u32> flags;
};

struct HSD_CObj {
    HSD_Obj parent;
    be<u32> flags;
    struct Viewport {
        be<f32> left;
        be<f32> right;
        be<f32> top;
        be<f32> bottom;
    } viewport;
    struct Scissor {
        be<u16> left;
        be<u16> right;
        be<u16> top;
        be<u16> bottom;
    } scissor;
    ptr<HSD_WObj> eyepos;
    ptr<HSD_WObj> interest;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_WObj, flags) == 0x08);
static_assert(offsetof(HSD_CObj, flags) == 0x08);
static_assert(offsetof(HSD_CObj, eyepos) == 0x24);
static_assert(offsetof(HSD_CObj, interest) == 0x28);
#endif

// Bit 30 of the camera's flags: its matrix must be computed again [hypothesis on the meaning: name of the bit's
// setter in cbr_decomp, HSD_CObjSetMtxDirty]. Bit 1 of a world object's flags: its position changed [hypothesis on
// the meaning: the position setters of cbr_decomp's wobj.c, HSD_WObjSetPosition and the like, set it].
constexpr u32 cobj_mtx_dirty = 1u << 30;
constexpr u32 wobj_position_changed = 1u << 1;

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

} // namespace hsd

GC_DECOMP_EXPORT(hsd_cobj_mtx_is_dirty, hsd::HSD_CObjMtxIsDirty, gc::abi::flag_reads_only);
