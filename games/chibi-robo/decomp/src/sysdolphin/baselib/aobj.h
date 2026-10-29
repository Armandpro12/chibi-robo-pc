// HSD animation objects (sysdolphin): HSD_AObj, and the per-track animation curves HSD_FObj it holds.
//
// Names and layout from cbr_decomp's aobj.h and fobj.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). The offsets that decompiled functions use are checked against this game's code (static
// asserts below, oracle); the other fields keep cbr_decomp's names and types [hypothesis].
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_AOBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_AOBJ_H

#include "object.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::f32;
using gc::ptr;
using gc::u32;
using gc::u8;

// One animation curve: the encoded keys of one track of an animated object, in a singly linked list.
struct HSD_FObj {
    ptr<HSD_FObj> next;
    ptr<be<u8>> ad;      // read position in the keys
    ptr<be<u8>> ad_head; // first key
    be<u32> length;
    be<u8> flags;
    be<u8> op;
    be<u8> op_intrp;
    be<u8> obj_type; // which property the curve animates: a track number of the animated object's kind
};

// Curve kind 12, which JObjSortAnim moves to the front of a joint animation's list (TYPE_JOBJ = 12 in cbr_decomp's
// fobj.h) [hypothesis on the meaning].
inline constexpr u8 fobj_type_joint = 12;

struct HSD_AObj {
    be<u32> flags;
    be<f32> curr_frame;
    be<f32> rewind_frame;
    be<f32> end_frame;
    be<f32> framerate;
    ptr<HSD_FObj> fobj; // first curve
    ptr<HSD_Obj> hsd_obj; // object the animation is attached to
};

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_FObj, obj_type) == 0x13);
static_assert(offsetof(HSD_AObj, fobj) == 0x14);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_AOBJ_H
