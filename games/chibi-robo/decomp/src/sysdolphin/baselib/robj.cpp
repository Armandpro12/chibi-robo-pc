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
#include "decomp/gc_path.h"

namespace hsd {

// The first reference object of the list that starts at `robj` whose type is `type` (already shifted into bits
// 28-30) and, unless `subtype` is 0, whose subtype is `subtype`; objects without a type are skipped. Null if none.
// The original tests the pointer at its entry, then jumps to the test at the bottom of its loop, so the first object
// is tested twice: the entry test below, then the loop's test (same sense) before each turn.
// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
ptr<HSD_RObj> HSD_RObjGetByType(ptr<HSD_RObj> robj, u32 type, u32 subtype) {
    if (gc::path::falls_through(robj == nullptr))
        return nullptr;
    for (ptr<HSD_RObj> current = robj; gc::path::taken(current != nullptr); current = current->next) {
        const u32 flags = gc::load(current->flags);
        if (gc::path::taken(!(flags & robj_has_type)) || gc::path::taken((flags & robj_type_mask) != type))
            continue;
        if (gc::path::taken(subtype == 0) || gc::path::falls_through(subtype == (flags & robj_subtype_mask)))
            return current;
    }
    return nullptr;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_robj_get_by_type, hsd::HSD_RObjGetByType, gc::abi::flag_reads_only | gc::abi::flag_path_marked);
