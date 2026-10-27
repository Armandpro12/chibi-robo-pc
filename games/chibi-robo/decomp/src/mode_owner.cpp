// Mode of a large game object: a current mode and a pending one that, when set, takes its place.
//
// Original unit: not known yet (no split of cbr_decomp covers this function); the file is named after what it holds.
// Names are hypotheses (notes of the decompilation stage): the code was read from the disassembly of both versions,
// identical in US and PAL; callers test the result against the modes 1, 2 and 3 in a row.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace chibi {

using gc::be;
using gc::s32;
using gc::u8;

// Only the two fields this unit reads are named; the bytes in front of them are described by other units later.
struct ModeOwner {
    gc::array<be<u8>, 0x95D0> not_described_here;
    be<s32> current_mode; // used when pending_mode is negative
    be<s32> pending_mode; // negative: no pending mode
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(ModeOwner, current_mode) == 0x95D0);
static_assert(offsetof(ModeOwner, pending_mode) == 0x95D4);
#endif

// 1 if `mode` is the mode in effect (the pending one if set, otherwise the current one), 0 otherwise.
u8 is_mode_in_effect(gc::ptr<const ModeOwner> owner, s32 mode) {
    s32 in_effect = gc::load(owner->pending_mode);
    if (in_effect < 0)
        in_effect = gc::load(owner->current_mode);
    return in_effect == mode ? 1 : 0;
}

} // namespace chibi

GC_DECOMP_EXPORT(mode_owner_is_mode_in_effect, chibi::is_mode_in_effect, gc::abi::flag_reads_only);
