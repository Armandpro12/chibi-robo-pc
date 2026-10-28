// HSD material objects (sysdolphin): HSD_MObj.
//
// Names and layout from cbr_decomp's mobj.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). Only the fields that decompiled functions use are described; their offsets are checked
// against this game's code (static asserts below, oracle).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_MOBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_MOBJ_H

#include "object.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;

struct HSD_TObj; // texture object, not described yet

struct HSD_MObj {
    HSD_Class parent;
    be<u32> rendermode; // render mode flags [hypothesis on the meaning of each bit]
    ptr<HSD_TObj> tobj; // first texture of the material
};

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_MObj, rendermode) == 4 && offsetof(HSD_MObj, tobj) == 8);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_MOBJ_H
