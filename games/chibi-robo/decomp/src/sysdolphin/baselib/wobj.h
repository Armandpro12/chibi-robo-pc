// HSD world objects (sysdolphin): HSD_WObj, a point in the world (camera eye position or interest, light position).
//
// Names and layout from cbr_decomp's wobj.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). Offsets checked against this game's code for the fields decompiled functions use
// (static asserts below, oracle).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_WOBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_WOBJ_H

#include "object.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::f32;
using gc::ptr;
using gc::u32;

// A 3-component float vector (the SDK's Vec).
struct Vec3 {
    be<f32> x;
    be<f32> y;
    be<f32> z;
};

struct HSD_AObj;
struct HSD_RObj;

struct HSD_WObj {
    HSD_Obj parent;
    be<u32> flags;
    Vec3 pos;
    ptr<HSD_AObj> aobj;
    ptr<HSD_RObj> robj;
};

// Bits of a world object's flags. Bit 1: its position changed since the matrices that use it were computed
// [hypothesis on the meaning: HSD_CObjMtxIsDirty reads it, the position setters set it]. Bit 0: cleared by the same
// setters [hypothesis on the meaning: the position is no longer the one an animation or reference gave].
inline constexpr u32 wobj_position_changed = 1u << 1;
inline constexpr u32 wobj_position_from_reference = 1u << 0;

#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(Vec3) == 0xC);
static_assert(offsetof(HSD_WObj, flags) == 0x08 && offsetof(HSD_WObj, pos) == 0x0C);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_WOBJ_H
