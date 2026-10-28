// A 64-bit set of flags inside a large game object: test and clear by mask.
//
// Original unit: not known yet (no split of cbr_decomp covers these functions); the file is named after what it holds.
// Names are hypotheses (notes of the decompilation stage): the code was read from the disassembly of both versions,
// identical in US and PAL. Both functions use the same field, and their only callers are in one function of the same
// unit (in both versions).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace chibi {

using gc::be;
using gc::u32;
using gc::u64;
using gc::u8;

// The console's 64-bit integer as the original reaches it: two words, the high one first. The original reads and
// writes it word by word (lwz/stw), which this layout keeps: the same stores, in the same order, so a lwarx
// reservation of either 32-byte block is cleared as the original clears it even if the object is only 4-aligned.
struct Flags64 {
    be<u32> high;
    be<u32> low;
};

// Only the field these functions use is named; the bytes in front of it are described by other units later.
struct FlagSetOwner {
    gc::array<be<u8>, 0x228> not_described_here;
    Flags64 flags;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(FlagSetOwner, flags) == 0x228 && sizeof(Flags64) == 8);
#endif

// The flags of `mask` that are set (flags & mask).
u64 flag_set_test(gc::ptr<const FlagSetOwner> owner, u64 mask) {
    const u32 low = gc::load(owner->flags.low);
    const u32 high = gc::load(owner->flags.high);
    return ((static_cast<u64>(high) << 32) | low) & mask;
}

// Clears the flags of `mask` (flags &= ~mask): both words read, then the low word written, then the high word, as in
// the original.
void flag_set_clear(gc::ptr<FlagSetOwner> owner, u64 mask) {
    const u32 low = gc::load(owner->flags.low);
    const u32 high = gc::load(owner->flags.high);
    owner->flags.low = low & ~static_cast<u32>(mask);
    owner->flags.high = high & ~static_cast<u32>(mask >> 32);
}

} // namespace chibi

GC_DECOMP_EXPORT(flag_set_test, chibi::flag_set_test, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(flag_set_clear, chibi::flag_set_clear, gc::abi::flag_none);
