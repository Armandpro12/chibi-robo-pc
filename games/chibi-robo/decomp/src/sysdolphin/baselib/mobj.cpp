// HSD material objects (sysdolphin): HSD_MObj flags and texture accessors.
//
// Original unit: sysdolphin/baselib/mobj.c [hypothesis: the functions sit between the tev.c and mobj.c source-file
// strings in both versions]. The code was written from the disassembly of both versions (identical in US and PAL).
// Cross-check [hypothesis on the identity]: HSD_MObjSetFlags, HSD_MObjClearFlags and HSD_MObjGetTObj have the logic
// and the fields of the functions of the same names in cbr_decomp's mobj.c (https://github.com/eavpsp/cbr_decomp,
// folder decomp_dtk_main/src, CC0 1.0); HSD_MObjGetFlags is the name TeamOrre/xd-decomp (CC0) gives the function
// between HSD_MObjSetCurrent and HSD_MObjSetFlags. Names are this project's [hypothesis] (notes of the decompilation
// stage); structure: mobj.h.
#include "mobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

// The material's render mode flags, 0 if there is no material.
u32 HSD_MObjGetFlags(ptr<const HSD_MObj> mobj) {
    if (mobj == nullptr)
        return 0;
    return gc::load(mobj->rendermode);
}

// Sets the flags of `mask` in the material's render mode; nothing if there is no material.
void HSD_MObjSetFlags(ptr<HSD_MObj> mobj, u32 mask) {
    if (mobj == nullptr)
        return;
    mobj->rendermode |= mask;
}

// Clears the flags of `mask` in the material's render mode; nothing if there is no material.
void HSD_MObjClearFlags(ptr<HSD_MObj> mobj, u32 mask) {
    if (mobj == nullptr)
        return;
    mobj->rendermode &= ~mask;
}

// The material's first texture, null if there is no material.
ptr<HSD_TObj> HSD_MObjGetTObj(ptr<const HSD_MObj> mobj) {
    if (mobj == nullptr)
        return nullptr;
    return mobj->tobj;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_mobj_get_flags, hsd::HSD_MObjGetFlags, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_mobj_set_flags, hsd::HSD_MObjSetFlags, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_mobj_clear_flags, hsd::HSD_MObjClearFlags, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_mobj_get_tobj, hsd::HSD_MObjGetTObj, gc::abi::flag_reads_only);
