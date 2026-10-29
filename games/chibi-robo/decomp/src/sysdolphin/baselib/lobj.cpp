// HSD light objects (sysdolphin): HSD_LObj.
//
// Original unit: sysdolphin/baselib/lobj.c [hypothesis: name and place in the unit from doldecomp/melee (names only)
// and TeamOrre/xd-decomp (CC0), which put HSD_LObjSetColor next to the same neighbour]. The code was written from the
// disassembly of both versions (identical in US and PAL). Cross-check [hypothesis on the identity]: it has the logic
// and the field of HSD_LObjSetColor in cbr_decomp's lobj.c (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src, CC0 1.0). Structure: lobj.h.
#include "lobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

// Sets the light's colour. The colour is passed by value in C, which the PowerPC EABI turns into a pointer to a copy
// (r4); there is no test of a missing light (the original has none). The original moves the four bytes with one
// word load and one word store, so the colour is first copied into a local: the result is the original's even when
// the source overlaps the light's own colour (anomaly A19).
void HSD_LObjSetColor(ptr<HSD_LObj> lobj, ptr<const GXColor> color) {
    const GXColor value = *color;
    lobj->color = value;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_lobj_set_color, hsd::HSD_LObjSetColor, gc::abi::flag_none);
