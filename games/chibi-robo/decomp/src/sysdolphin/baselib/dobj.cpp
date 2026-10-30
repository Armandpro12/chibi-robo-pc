// HSD display objects (sysdolphin): the current display object.
//
// Original unit: sysdolphin/baselib/dobj.c [hypothesis: the function stores its argument in a global and is called by
// the joint display function (US 801CA1C0, HSD_JObjDispSub of the symbol supplement) with each display object of a
// joint before drawing it, then with null after the list; cbr_decomp's dobj.c (https://github.com/eavpsp/cbr_decomp,
// folder decomp_dtk_main/src, CC0 1.0) has HSD_DObjSetCurrent, which stores its argument in the static current_dobj,
// and doldecomp/melee's sysdolphin uses the same function name (names only, see the project rules)]. The symbol
// supplement leaves this function without a name. The code was written from the disassembly of both versions
// (identical in US and PAL, the address of the global aside). The game never reads the global with the same
// small-data form (one store, no load, scan of the US DOL) [hypothesis: read through another form or not at all].
#include "jobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

// current_dobj of dobj.c: the display object being drawn [hypothesis on the meaning: from the names and the caller].
constexpr gc::global<ptr<HSD_DObj>, global_id::hsd_dobj_current> current_display_object{};

// Makes `dobj` the current display object (null clears it).
void HSD_DObjSetCurrent(ptr<HSD_DObj> dobj) {
    *current_display_object = dobj;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_dobj_set_current, hsd::HSD_DObjSetCurrent, gc::abi::flag_none);
