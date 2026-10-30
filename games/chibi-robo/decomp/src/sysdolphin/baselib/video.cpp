// HSD video (sysdolphin): whether the console's reset switch asked for a reset.
//
// Original unit: sysdolphin/baselib/video.c [hypothesis: the function sits among the functions that use the video
// state (US 8064C520, referenced from US 801CC0BC to 801CE074), the unit of HSD_VIData in cbr_decomp's video.c
// (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0)]. This game's video state does not have
// cbr_decomp's HSD_VIInfo layout (bytes at 0, 2, 3, 4 and a pointer at 8 in US 801CC0F8), so only the two bytes used
// here are described. The code was written from the disassembly of both versions (identical in US and PAL, the address
// of the global aside). The names are this project's [hypothesis]; the symbol supplement has none for this function.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::s32;
using gc::u8;

// The bytes of the video state used here. The function after the PAD read (US 801CC35C-801CC38C) sets
// reset_switch_held while OSGetResetSwitchState returns non-zero and, when it returns zero with reset_switch_held set,
// sets reset_requested and clears reset_switch_held [verified by reading the US disassembly; that function is not
// compared with the PAL one here].
struct HSD_VideoState {
    gc::array<be<u8>, 0x2A> not_described_here;
    be<u8> reset_switch_held;
    be<u8> reset_requested;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_VideoState, reset_requested) == 0x2B);
#endif

constexpr gc::global<HSD_VideoState, global_id::hsd_video_state> video_state{};

// 1 once the reset switch has been pressed and released, 0 before. (The original computes `byte != 0` without a
// branch.)
s32 HSD_VIIsResetRequested() {
    return gc::load(video_state->reset_requested) != 0 ? 1 : 0;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_vi_is_reset_requested, hsd::HSD_VIIsResetRequested, gc::abi::flag_reads_only);
