// XAction, base class of the player's actions (XActLift, XActHang, XActJump, XActLadder, XActRope, XActPush,
// XActTumble, XActCopter, XActPopper, XActRadar...: class names read in the RTTI objects of their vtables).
//
// Original unit: not known yet (no split of cbr_decomp covers this function); the file is named after the class.
// The function is the second virtual function of XAction's vtable (the first word after the RTTI pointer and the
// offset word is another function), shared by the vtables of the derived classes listed above. Its name and the
// meaning of the four fields are hypotheses (notes of the decompilation stage); the code was read from the
// disassembly of both versions, identical in US and PAL.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace chibi {

using gc::be;
using gc::u32;
using gc::u8;

// Only the four words this function writes are named, after their place; their role is not known yet. Where the
// vtable pointer of the class lies is not known either (CVec keeps it after its members, notes/pal-addresses.md).
struct XAction {
    gc::array<be<u8>, 4> not_described_here;
    be<u32> state_a;
    gc::array<be<u8>, 4> not_described_here_too;
    be<u32> state_b;
    gc::array<be<u8>, 0x10> not_described_here_either;
    be<u32> state_c;
    be<u32> state_d;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(XAction, state_a) == 0x4 && offsetof(XAction, state_b) == 0xC &&
              offsetof(XAction, state_c) == 0x20 && offsetof(XAction, state_d) == 0x24);
#endif

// Resets the action: the four words to zero, in the original's order.
void xaction_reset(gc::ptr<XAction> action) {
    action->state_a = 0u;
    action->state_b = 0u;
    action->state_c = 0u;
    action->state_d = 0u;
}

} // namespace chibi

GC_DECOMP_EXPORT(xaction_reset, chibi::xaction_reset, gc::abi::flag_none);
