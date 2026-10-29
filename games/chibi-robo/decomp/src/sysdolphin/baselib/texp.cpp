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
#include "decomp/gc_path.h"

namespace hsd {

// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
// Kind of a node: zero for no node, texture or raster for the two marker pointers, otherwise the node's own type. (The
// original branches away on the "other" side of each test: falls_through.)
HSD_TExpType HSD_TExpGetType(ptr<const HSD_TExp> texp) {
    if (gc::path::falls_through(texp == nullptr))
        return HSD_TExpType::zero;
    if (gc::path::falls_through(is_texp_marker(texp, texp_marker_texture)))
        return HSD_TExpType::texture;
    if (gc::path::falls_through(is_texp_marker(texp, texp_marker_raster)))
        return HSD_TExpType::raster;
    return gc::load(texp->type);
}

// Counts one more reference to the node: to the colour or the alpha result of a TEV stage (`select`), or to a
// constant. Other kinds keep no count. The original has the call to HSD_TExpGetType inlined (its path marks count
// for this function then) and tests the kind in this order: constant, above constant (signed), tev.
void HSD_TExpRef(ptr<HSD_TExp> texp, u8 select) {
    const HSD_TExpType type = HSD_TExpGetType(texp);
    if (gc::path::taken(type == HSD_TExpType::constant)) {
        texp->cnst.ref += 1;
        return;
    }
    if (gc::path::taken(type > HSD_TExpType::constant))
        return; // a kind above constant: no count
    if (gc::path::taken(type != HSD_TExpType::tev))
        return; // zero, texture or raster: no count
    if (gc::path::falls_through(select == texp_select_colour))
        texp->tev.c_ref += 1;
    else
        texp->tev.a_ref += 1;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_texp_get_type, hsd::HSD_TExpGetType, gc::abi::flag_reads_only | gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(hsd_texp_ref, hsd::HSD_TExpRef, gc::abi::flag_path_marked);
