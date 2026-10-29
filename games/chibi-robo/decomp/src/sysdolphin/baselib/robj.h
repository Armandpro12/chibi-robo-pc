// HSD reference objects (sysdolphin): HSD_RObj, a constraint or reference attached to a joint or world object.
//
// Names and layout from cbr_decomp's robj.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). Only the two fields decompiled code uses are described; their offsets are checked against
// this game's code (static assert, oracle).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_ROBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_ROBJ_H

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;

struct HSD_RObj {
    ptr<HSD_RObj> next;
    be<u32> flags; // bit 31: has a type; bits 28-30: type; bits 0-27: subtype
};

// Fields of a reference object's flags (the mask TYPE_MASK and the type values REFTYPE_* are in cbr_decomp's robj.h;
// the names of the other two masks are this project's [hypothesis on the meaning of bit 31]).
inline constexpr u32 robj_has_type = 0x80000000u;
inline constexpr u32 robj_type_mask = 0x70000000u;
inline constexpr u32 robj_subtype_mask = 0x0FFFFFFFu;

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_RObj, flags) == 4);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_ROBJ_H
