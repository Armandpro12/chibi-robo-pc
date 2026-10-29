// HSD object headers (sysdolphin): the class header and the reference-counted object header that every HSD object
// starts with.
//
// Names and layout from cbr_decomp's class.h and object.h (https://github.com/eavpsp/cbr_decomp, folder
// decomp_dtk_main/src/sysdolphin/baselib, CC0 1.0); doldecomp/melee's sysdolphin uses the same names (names only,
// see the project rules). Checked on this game's code only through the offsets that the decompiled functions use
// (static_asserts of each unit).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_OBJECT_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_OBJECT_H

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u16;

// Run-time class description of an HSD object (its name, allocator, virtual functions); not described here.
struct HSD_ClassInfo;

// Header of every HSD class object: a pointer to its class description.
struct HSD_Class {
    ptr<HSD_ClassInfo> class_info;
};

// Header of the reference-counted HSD objects (camera, light, joint, world objects...).
struct HSD_Obj {
    HSD_Class parent;
    be<u16> ref_count;
    be<u16> ref_count_individual;
};

#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(HSD_Class) == 4);
static_assert(sizeof(HSD_Obj) == 8 && offsetof(HSD_Obj, ref_count) == 4);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_OBJECT_H
