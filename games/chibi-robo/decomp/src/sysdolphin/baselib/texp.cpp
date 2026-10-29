// HSD TEV expressions (sysdolphin): type of a node and reference counting.
//
// Original unit: sysdolphin/baselib/texp.c [hypothesis: names and place in the unit from doldecomp/melee (names only)
// and TeamOrre/xd-decomp (CC0), which put HSD_TExpGetType and HSD_TExpRef next to the same neighbours]. The code was
// written from the disassembly of both versions (identical in US and PAL). Cross-check [hypothesis on the identity]:
// both have the logic and the fields of the functions of the same names in cbr_decomp's texp.c
// (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0). Structure: texp.h.
#include "texp.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

// Kind of a node: zero for no node, texture or raster for the two marker pointers, otherwise the node's own type.
HSD_TExpType HSD_TExpGetType(ptr<const HSD_TExp> texp) {
    if (texp == nullptr)
        return HSD_TExpType::zero;
    if (is_texp_marker(texp, texp_marker_texture))
        return HSD_TExpType::texture;
    if (is_texp_marker(texp, texp_marker_raster))
        return HSD_TExpType::raster;
    return gc::load(texp->type);
}

// Counts one more reference to the node: to the colour or the alpha result of a TEV stage (`select`), or to a
// constant. Other kinds keep no count. The original has the call to HSD_TExpGetType inlined.
void HSD_TExpRef(ptr<HSD_TExp> texp, u8 select) {
    switch (HSD_TExpGetType(texp)) {
    case HSD_TExpType::tev:
        if (select == texp_select_colour)
            texp->tev.c_ref += 1;
        else
            texp->tev.a_ref += 1;
        break;
    case HSD_TExpType::constant:
        texp->cnst.ref += 1;
        break;
    default:
        break;
    }
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_texp_get_type, hsd::HSD_TExpGetType, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_texp_ref, hsd::HSD_TExpRef, gc::abi::flag_none);
