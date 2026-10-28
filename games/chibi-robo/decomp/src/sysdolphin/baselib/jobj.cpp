// HSD joint objects (sysdolphin): HSD_JObj.
//
// Original unit: sysdolphin/baselib/jobj.c [hypothesis: the function sits next to that source-file string in both
// versions]. Names are hypotheses (notes of the decompilation stage); the code was written from the disassembly of
// both versions (identical in US and PAL).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;
using gc::u8;

// Only the field this function reads is named; the bytes in front of it are described by other units later.
struct JointObject {
    gc::array<be<u8>, 0x14> not_described_here;
    be<u32> flags;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(JointObject, flags) == 0x14);
#endif

// The joint's flags, 0 if there is no joint.
u32 jobj_get_flags(ptr<const JointObject> joint) {
    if (joint == nullptr)
        return 0;
    return gc::load(joint->flags);
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_jobj_get_flags, hsd::jobj_get_flags, gc::abi::flag_reads_only);
