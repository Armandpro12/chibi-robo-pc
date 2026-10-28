// HSD material objects (sysdolphin): the current material.
//
// Original unit: sysdolphin/baselib/mobj.c [hypothesis: the function sits between the tev.c and mobj.c source-file
// strings in both versions, next to HSD_MObjClearFlags]. This file holds the functions of that unit that use a game
// global; it is to be merged with mobj.cpp. The code was written from the disassembly of both versions (identical in
// US and PAL, the address of the global aside). Names from cbr_decomp's mobj.c (https://github.com/eavpsp/cbr_decomp,
// folder decomp_dtk_main/src, CC0 1.0): HSD_MObjSetCurrent stores its argument in the static current_mobj;
// doldecomp/melee's sysdolphin uses the same function name (names only, see the project rules).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

using gc::ptr;

// HSD_MObj (mobj.cpp describes the fields it uses); only its address is handled here.
struct MaterialObject;

// current_mobj of mobj.c: the material being drawn [hypothesis on the meaning: from the names and the callers of
// cbr_decomp's dobj.c, which set it before drawing a display object and clear it after].
constexpr gc::global<ptr<MaterialObject>, global_id::hsd_mobj_current> current_material{};

// Makes `material` the current material (null clears it).
void mobj_set_current(ptr<MaterialObject> material) {
    *current_material = material;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_mobj_set_current, hsd::mobj_set_current, gc::abi::flag_none);
