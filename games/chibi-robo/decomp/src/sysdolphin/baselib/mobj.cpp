// HSD material objects (sysdolphin): HSD_MObj.
//
// Original unit: sysdolphin/baselib/mobj.c [hypothesis: the function sits between the tev.c and mobj.c source-file
// strings in both versions]. The code was written from the disassembly of both versions (identical in US and PAL).
// Cross-check [hypothesis on the identity]: it has the logic and the field of HSD_MObjClearFlags of cbr_decomp's
// mobj.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0), whose HSD_MObj has its render
// mode flags (rendermode) at 4, after the class pointer; doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). The names here are this project's [hypothesis] (notes of the decompilation stage).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;

// Only the field this function uses is named: the class pointer comes first.
struct MaterialObject {
    ptr<void> class_info;
    be<u32> flags; // render mode flags [hypothesis on the meaning]
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(MaterialObject, flags) == 4);
#endif

// Clears the flags of `mask` in the material's flags; nothing if there is no material.
void mobj_clear_flags(ptr<MaterialObject> material, u32 mask) {
    if (material == nullptr)
        return;
    material->flags &= ~mask;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_mobj_clear_flags, hsd::mobj_clear_flags, gc::abi::flag_none);
