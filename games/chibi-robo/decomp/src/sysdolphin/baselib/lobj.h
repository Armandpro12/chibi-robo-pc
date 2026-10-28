// HSD light objects (sysdolphin): HSD_LObj, and the GX colour it holds.
//
// Names and layout from cbr_decomp's lobj.h and the Dolphin SDK header GXTypes.h it uses
// (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main, CC0 1.0); doldecomp/melee's sysdolphin uses the same
// names (names only, see the project rules). The fields up to `color` keep cbr_decomp's names and types; only
// `color` is used by decompiled code so far, its offset is checked against this game's code (static assert, oracle).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_LOBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_LOBJ_H

#include "object.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u16;
using gc::u8;

// A GX colour: red, green, blue, alpha, one byte each.
struct GXColor {
    be<u8> r;
    be<u8> g;
    be<u8> b;
    be<u8> a;
};

struct HSD_LObj {
    HSD_Obj parent;
    be<u16> flags;
    be<u16> priority;
    ptr<HSD_LObj> next;
    GXColor color;
};

#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(GXColor) == 4);
static_assert(offsetof(HSD_LObj, color) == 0x10);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_LOBJ_H
