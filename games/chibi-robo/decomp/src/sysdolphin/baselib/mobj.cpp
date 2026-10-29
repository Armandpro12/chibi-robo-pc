// HSD material objects (sysdolphin): the current material, HSD_MObj flags and texture accessors.
//
// Original unit: sysdolphin/baselib/mobj.c [hypothesis: the functions sit between the tev.c and mobj.c source-file
// strings in both versions]. The code was written from the disassembly of both versions (identical in US and PAL,
// the address of the global aside).
// Cross-check [hypothesis on the identity]: HSD_MObjSetFlags, HSD_MObjClearFlags and HSD_MObjGetTObj have the logic
// and the fields of the functions of the same names in cbr_decomp's mobj.c (https://github.com/eavpsp/cbr_decomp,
// folder decomp_dtk_main/src, CC0 1.0); HSD_MObjGetFlags is the name TeamOrre/xd-decomp (CC0) gives the function
// between HSD_MObjSetCurrent and HSD_MObjSetFlags. Names are this project's [hypothesis] (notes of the decompilation
// stage); structure: mobj.h. HSD_MObjSetCurrent stores its argument in the static current_mobj (names from the same
// mobj.c; doldecomp/melee's sysdolphin uses the same function name, names only, see the project rules).
#include "mobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"
#include "decomp_globals.h"

// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
namespace hsd {

// current_mobj of mobj.c: the material being drawn [hypothesis on the meaning: from the names and the callers of
// cbr_decomp's dobj.c, which set it before drawing a display object and clear it after].
constexpr gc::global<ptr<HSD_MObj>, global_id::hsd_mobj_current> current_material{};

// Makes `mobj` the current material (null clears it).
void HSD_MObjSetCurrent(ptr<HSD_MObj> mobj) {
    *current_material = mobj;
}

// The material's render mode flags, 0 if there is no material.
u32 HSD_MObjGetFlags(ptr<const HSD_MObj> mobj) {
    if (mobj == nullptr)
        return 0;
    return gc::load(mobj->rendermode);
}

// Sets the flags of `mask` in the material's render mode; nothing if there is no material.
void HSD_MObjSetFlags(ptr<HSD_MObj> mobj, u32 mask) {
    if (gc::path::taken(mobj == nullptr)) // original: a conditional return
        return;
    mobj->rendermode |= mask;
}

// Clears the flags of `mask` in the material's render mode; nothing if there is no material.
void HSD_MObjClearFlags(ptr<HSD_MObj> mobj, u32 mask) {
    if (gc::path::taken(mobj == nullptr)) // original: a conditional return
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

GC_DECOMP_EXPORT(hsd_mobj_set_current, hsd::HSD_MObjSetCurrent, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_mobj_get_flags, hsd::HSD_MObjGetFlags, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_mobj_set_flags, hsd::HSD_MObjSetFlags, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_mobj_clear_flags, hsd::HSD_MObjClearFlags, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_mobj_get_tobj, hsd::HSD_MObjGetTObj, gc::abi::flag_reads_only);
