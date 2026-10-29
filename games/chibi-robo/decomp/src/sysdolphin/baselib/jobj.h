// HSD joint objects (sysdolphin): HSD_JObj, a node of a model's joint tree.
//
// Names and layout from cbr_decomp's jobj.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). Only the fields up to the union are described; the offsets decompiled functions use are
// checked against this game's code (static asserts below, oracle).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_JOBJ_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_JOBJ_H

#include "object.h"

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;

struct HSD_DObj;   // display object, not described yet
struct HSD_SList;  // generic singly linked list, not described yet
struct HSD_Spline; // spline, not described yet

struct HSD_JObj {
    HSD_Obj object;
    ptr<HSD_JObj> next;
    ptr<HSD_JObj> parent;
    ptr<HSD_JObj> child;
    be<u32> flags;
    // What the word at 0x18 points to depends on two flags (jobj_particles, jobj_spline).
    union Contents {
        ptr<HSD_SList> ptcl;
        ptr<HSD_DObj> dobj; // when neither flag is set
        ptr<HSD_Spline> spline;
    } u;
};

// Flags that give the meaning of the union (JOBJ_PTCL and JOBJ_SPLINE in cbr_decomp's jobj.h).
inline constexpr u32 jobj_particles = 1u << 5;
inline constexpr u32 jobj_spline = 1u << 14;

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_JObj, flags) == 0x14 && offsetof(HSD_JObj, u) == 0x18);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_JOBJ_H
