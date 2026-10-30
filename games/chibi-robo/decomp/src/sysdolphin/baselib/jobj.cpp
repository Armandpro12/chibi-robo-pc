// HSD joint objects (sysdolphin): HSD_JObj accessors and the ordering of a joint animation's curves.
//
// Original unit: sysdolphin/baselib/jobj.c [hypothesis: the functions sit next to that source-file string in both
// versions]. The code was written from the disassembly of both versions (identical in US and PAL). Cross-check
// [hypothesis on the identity]: JObjSortAnim and HSD_JObjGetDObj have the logic and the fields of the functions of
// the same names in cbr_decomp's jobj.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0);
// JObjSortAnim is also the name doldecomp/melee (names only) and TeamOrre/xd-decomp (CC0) give the function at this
// place of the unit. The symbol supplement proposed HSD_JObjPrependRObj for 801C7014 (US) by position only; its code
// is that of HSD_JObjGetDObj (a getter, no list). Structures: jobj.h, aobj.h. Names are this project's
// [hypothesis] (notes of the decompilation stage).
#include "aobj.h"
#include "jobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"
#include "decomp_globals.h"

namespace hsd {

// Moves the first curve of kind fobj_type_joint to the front of the animation's curve list; nothing if there is no
// animation, no curve, or no curve of that kind. The list is walked through the address of each link, as in the
// original (the head's link is the animation's fobj field). The loop's test is the original's one, at the bottom of
// its loop and reached first by a jump.
// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
void JObjSortAnim(ptr<HSD_AObj> aobj) {
    if (gc::path::taken(aobj == nullptr))
        return;
    if (gc::path::falls_through(aobj->fobj == nullptr))
        return;
    ptr<ptr<HSD_FObj>> link = gc::address_of(aobj->fobj);
    for (ptr<HSD_FObj> fobj = *link; gc::path::taken(fobj != nullptr); fobj = *link) {
        if (gc::path::falls_through(gc::load(fobj->obj_type) == fobj_type_joint)) {
            *link = fobj->next;      // unlink it
            fobj->next = aobj->fobj; // and put it in front
            aobj->fobj = fobj;
            return;
        }
        link = gc::address_of(fobj->next);
    }
}

// The joint's flags, 0 if there is no joint.
u32 HSD_JObjGetFlags(ptr<const HSD_JObj> jobj) {
    if (jobj == nullptr)
        return 0;
    return gc::load(jobj->flags);
}

// The joint's display objects, null if there is no joint or if its union holds particles or a spline. (Path marks:
// the original branches to its "null" on no joint, and to its "dobj" when neither flag is set: falls_through.)
ptr<HSD_DObj> HSD_JObjGetDObj(ptr<const HSD_JObj> jobj) {
    if (gc::path::taken(jobj == nullptr))
        return nullptr;
    if (gc::path::falls_through(gc::load(jobj->flags) & (jobj_particles | jobj_spline)))
        return nullptr;
    return jobj->u.dobj;
}

// current_jobj of jobj.c (cbr_decomp, CC0): the joint being drawn [hypothesis on the meaning: from the names; the
// game stores it in HSD_JObjSetCurrent of the symbol supplement, US 801C784C].
constexpr gc::global<ptr<HSD_JObj>, global_id::hsd_jobj_current> current_joint{};

// The current joint (HSD_JObjGetCurrent: name from doldecomp/melee, names only, and cbr_decomp's jobj.c, which
// returns current_jobj; [hypothesis on the identity]).
ptr<HSD_JObj> HSD_JObjGetCurrent() {
    return *current_joint;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_jobj_sort_anim, hsd::JObjSortAnim, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_jobj_get_flags, hsd::HSD_JObjGetFlags, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_jobj_get_dobj, hsd::HSD_JObjGetDObj, gc::abi::flag_reads_only | gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_jobj_get_current, hsd::HSD_JObjGetCurrent, gc::abi::flag_reads_only);
