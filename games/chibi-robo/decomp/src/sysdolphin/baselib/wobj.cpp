// HSD world objects (sysdolphin): HSD_WObj.
//
// Original unit: sysdolphin/baselib/wobj.c [hypothesis: name and place in the unit from doldecomp/melee (names only)
// and TeamOrre/xd-decomp (CC0), which put HSD_WObjSetPosition next to the same neighbour]. The code was written from
// the disassembly of both versions (identical in US and PAL). Cross-check [hypothesis on the identity]: it has the
// logic and the fields of HSD_WObjSetPosition in cbr_decomp's wobj.c (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src, CC0 1.0). Structure: wobj.h.
#include "wobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"

namespace hsd {

// Moves the world object to `*pos` and marks its position as changed. Nothing if either pointer is null. The floats
// are copied as they are (the original moves their bits with integer loads and stores). The flags are written twice,
// as in the original (set, then clear). (Path marks: runtime/core/decomp/gc_path.h; the original returns by a
// conditional return on a null object, then branches over its return when the position is not null.)
void HSD_WObjSetPosition(ptr<HSD_WObj> wobj, ptr<const Vec3> pos) {
    if (gc::path::taken(wobj == nullptr))
        return;
    if (gc::path::falls_through(pos == nullptr))
        return;
    wobj->pos = *pos;
    wobj->flags |= wobj_position_changed;
    wobj->flags &= ~wobj_position_from_reference;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_wobj_set_position, hsd::HSD_WObjSetPosition, gc::abi::flag_path_marked);
