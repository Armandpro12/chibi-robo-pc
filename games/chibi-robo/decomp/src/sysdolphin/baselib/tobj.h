// HSD texture objects (sysdolphin): HSD_TObj, the textures of a material in a singly linked list.
//
// Names from cbr_decomp's tobj.h (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src/sysdolphin/baselib,
// CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only, see the project rules). Only the fields
// decompiled code uses are described; their offsets are this game's, read from its code (static asserts below, oracle).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_TOBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_TOBJ_H

#include "object.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;
using gc::u8;

struct HSD_TObj {
    HSD_Obj parent;
    ptr<HSD_TObj> next; // next texture of the same material
    gc::array<be<u8>, 0x40> not_described_here;
    be<u32> flags; // bits 0-3: texture coordinate source (tobj_coord of cbr_decomp's tobj.h)
};

// Bits of HSD_TObj::flags that hold the texture coordinate source (TEX_COORD_MASK in cbr_decomp's tobj.h).
inline constexpr u32 tobj_coord_mask = 0xF;

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_TObj, next) == 8 && offsetof(HSD_TObj, flags) == 0x4C);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_TOBJ_H
