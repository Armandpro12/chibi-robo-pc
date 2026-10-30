// HSD particle display (sysdolphin): the TEV set-up of particle drawing.
//
// Original unit: sysdolphin/baselib/psdisptev.c [hypothesis: name and unit from doldecomp/melee (names only), symbol
// supplement; cbr_decomp's psdisptev.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0)
// declares psSetupTevInvalidState there without a body]. The code was written from the disassembly of both versions
// (identical in US and PAL, the address of the global aside).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

using gc::be;
using gc::s32;

// The TEV set-up last made for particles, -1 when none is known [hypothesis on the meaning: the particle TEV set-up
// (US 801E4F54) compares its argument with this word and stores it]. The name is this project's.
constexpr gc::global<be<s32>, global_id::ps_current_tev_setup> current_particle_tev_setup{};

// Forgets the particle TEV set-up, so that the next one is made again.
void psSetupTevInvalidState() {
    *current_particle_tev_setup = -1;
}

} // namespace hsd

GC_DECOMP_EXPORT(ps_setup_tev_invalid_state, hsd::psSetupTevInvalidState, gc::abi::flag_none);
