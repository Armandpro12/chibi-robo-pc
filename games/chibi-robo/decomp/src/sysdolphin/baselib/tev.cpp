// HSD render state (sysdolphin): TEV stages in use.
//
// Original unit: sysdolphin/baselib/tev.c [hypothesis: the function sits after the tev.c source-file string in both
// versions]. The code was written from the disassembly of both versions (identical in US and PAL, the address of the
// global aside). Names from cbr_decomp's tev.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src,
// CC0 1.0): HSD_StateInitTev clears the static current_tev, which HSD_StateGetNumTevStages returns;
// doldecomp/melee's sysdolphin uses the same function names (names only, see the project rules).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

using gc::be;
using gc::s32;

// current_tev of tev.c: number of TEV stages assigned since the state was last reset [hypothesis on the meaning: from
// the names].
constexpr gc::global<be<s32>, global_id::hsd_state_current_tev> current_tev_stage{};

// Resets the TEV stage count: no stage assigned.
void HSD_StateInitTev() {
    *current_tev_stage = 0;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_state_init_tev, hsd::HSD_StateInitTev, gc::abi::flag_none);
