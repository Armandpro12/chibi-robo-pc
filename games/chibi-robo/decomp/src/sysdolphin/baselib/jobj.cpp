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

namespace hsd {

// Moves the first curve of kind fobj_type_joint to the front of the animation's curve list; nothing if there is no
// animation, no curve, or no curve of that kind. The list is walked through the address of each link, as in the
// original (the head's link is the animation's fobj field).
void JObjSortAnim(ptr<HSD_AObj> aobj) {
    if (aobj == nullptr)
        return;
    if (aobj->fobj == nullptr)
        return;
    ptr<ptr<HSD_FObj>> link = gc::address_of(aobj->fobj);
    for (ptr<HSD_FObj> fobj = *link; fobj != nullptr; fobj = *link) {
        if (gc::load(fobj->obj_type) == fobj_type_joint) {
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

// The joint's display objects, null if there is no joint or if its union holds particles or a spline.
ptr<HSD_DObj> HSD_JObjGetDObj(ptr<const HSD_JObj> jobj) {
    if (jobj == nullptr)
        return nullptr;
    if (gc::load(jobj->flags) & (jobj_particles | jobj_spline))
        return nullptr;
    return jobj->u.dobj;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_jobj_sort_anim, hsd::JObjSortAnim, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_jobj_get_flags, hsd::HSD_JObjGetFlags, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_jobj_get_dobj, hsd::HSD_JObjGetDObj, gc::abi::flag_reads_only);
