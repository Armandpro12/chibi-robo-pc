// HSD library start-up and rendering passes (sysdolphin): the end of a rendering pass.
//
// Original unit: sysdolphin/baselib/initialize.c [hypothesis: the function follows the start of a rendering pass
// (US 801CAF20, which stores the pass and calls GXSetPixelFmt, GXSetFieldMode, GXInvalidateVtxCache and
// GXInvalidateTexAll), in the order of the functions of initialize.c in cbr_decomp (https://github.com/eavpsp/
// cbr_decomp, folder decomp_dtk_main/src, CC0 1.0) and doldecomp/melee (names only), where the function after
// HSD_StartRender does nothing useful]. The code was written from the disassembly of both versions (identical in US
// and PAL). The name HSD_EndRender is this project's [hypothesis]: the reference projects only have an address-based
// name for it.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

// End of a rendering pass: nothing to do in this game (an empty function; two functions of the game call it, US
// 8000DFA8 and 80193F3C, from the call graph of the US symbols).
void HSD_EndRender() {}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_end_render, hsd::HSD_EndRender, gc::abi::flag_reads_only);
