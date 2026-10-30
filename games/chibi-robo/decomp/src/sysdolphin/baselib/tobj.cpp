// HSD texture objects (sysdolphin): search of the texture list by coordinate source.
//
// Original unit: sysdolphin/baselib/tobj.c [hypothesis: name and place in the unit from doldecomp/melee (names only),
// symbol supplement]. The code was written from the disassembly of both versions (identical in US and PAL, the address
// of the global aside). Cross-check [hypothesis on the identity]: it has the logic and the fields of
// _HSD_TObjGetCurrentByType in cbr_decomp's tobj.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src,
// CC0 1.0), whose global `tobj_head` (an extern there) gives the name of the list head. The C++ name drops the
// original's leading underscore (identifiers that start with an underscore and a capital are reserved in C++).
// Structure: tobj.h.
#include "tobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"
#include "decomp_globals.h"

// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
namespace hsd {

// tobj_head: first texture of the list being set up [hypothesis on the meaning: from the name; the game writes it in
// two functions of the polygon unit, US 801AEC78 and 801AF030].
constexpr gc::global<ptr<HSD_TObj>, global_id::hsd_tobj_head> texture_list_head{};

// The first texture after `from` (from the head of the list when `from` is null) whose coordinate source is `mapping`;
// null if none. The original tests the pointer at the bottom of its loop, before each turn.
ptr<HSD_TObj> HSD_TObjGetCurrentByType(ptr<const HSD_TObj> from, u32 mapping) {
    ptr<HSD_TObj> tobj;
    if (gc::path::taken(from != nullptr))
        tobj = from->next;
    else
        tobj = *texture_list_head;
    for (; gc::path::taken(tobj != nullptr); tobj = tobj->next) {
        if (gc::path::taken((gc::load(tobj->flags) & tobj_coord_mask) == mapping)) // original: a conditional return
            return tobj;
    }
    return nullptr;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_tobj_get_current_by_type, hsd::HSD_TObjGetCurrentByType,
                 gc::abi::flag_reads_only | gc::abi::flag_path_marked);
