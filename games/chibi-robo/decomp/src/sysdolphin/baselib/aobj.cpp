// HSD animation objects (sysdolphin): the counts that decide when the end-of-animation callbacks run.
//
// Original unit: sysdolphin/baselib/aobj.c [hypothesis: name and place in the unit from doldecomp/melee (names only),
// symbol supplement]. The code was written from the disassembly of both versions (identical in US and PAL, the
// addresses of the globals aside). Cross-check [hypothesis on the identity]: cbr_decomp's aobj.c
// (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0) has HSD_AObjInitEndCallBack, which
// clears two counters that its HSD_AObjInvokeCallBacks tests (callbacks only when the first is non-zero and the
// second zero) and that its animation step increments (the first for an animation that has ended, the second
// otherwise); this game's next function (US 801BA83C) tests the two globals cleared here in that way, the first one
// cleared first. Their names are this project's [hypothesis]: cbr_decomp only has address-based names for them.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

using gc::be;
using gc::s32;

// Animations stepped since the last reset that have reached their end, and those still running [hypothesis on the
// meaning, see above].
constexpr gc::global<be<s32>, global_id::hsd_aobj_ended_count> ended_animation_count{};
constexpr gc::global<be<s32>, global_id::hsd_aobj_running_count> running_animation_count{};

// Resets both counts, in the original's order.
void HSD_AObjInitEndCallBack() {
    *ended_animation_count = 0;
    *running_animation_count = 0;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_aobj_init_end_callback, hsd::HSD_AObjInitEndCallBack, gc::abi::flag_none);
