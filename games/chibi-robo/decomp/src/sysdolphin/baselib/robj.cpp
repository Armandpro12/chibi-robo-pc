// HSD reference objects (sysdolphin): HSD_RObj lookup.
//
// Original unit: sysdolphin/baselib/robj.c [hypothesis: name and place in the unit from doldecomp/melee (names only)
// and TeamOrre/xd-decomp (CC0), which put HSD_RObjGetByType next to the same neighbour]. The code was written from the
// disassembly of both versions (identical in US and PAL). Cross-check [hypothesis on the identity]: it has the logic
// and the fields of HSD_RObjGetByType in cbr_decomp's robj.c (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src, CC0 1.0). Structure: robj.h.
#include "robj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

// The first reference object of the list that starts at `robj` whose type is `type` (already shifted into bits
// 28-30) and, unless `subtype` is 0, whose subtype is `subtype`; objects without a type are skipped. Null if none.
ptr<HSD_RObj> HSD_RObjGetByType(ptr<HSD_RObj> robj, u32 type, u32 subtype) {
    for (ptr<HSD_RObj> current = robj; current != nullptr; current = current->next) {
        const u32 flags = gc::load(current->flags);
        if (!(flags & robj_has_type) || (flags & robj_type_mask) != type)
            continue;
        if (subtype == 0 || subtype == (flags & robj_subtype_mask))
            return current;
    }
    return nullptr;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_robj_get_by_type, hsd::HSD_RObjGetByType, gc::abi::flag_reads_only);
