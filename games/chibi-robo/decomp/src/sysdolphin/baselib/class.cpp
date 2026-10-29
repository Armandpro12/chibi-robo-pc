// HSD class system (sysdolphin): the default virtual functions of the base class HSD_Class.
//
// Original unit: sysdolphin/baselib/class.c [hypothesis: names and place in the unit from doldecomp/melee (names only)
// and TeamOrre/xd-decomp (CC0), which put _hsdClassInit and _hsdClassRelease next to the same neighbours]. Both are
// reached only through function pointers (class descriptions). The code was written from the disassembly of both
// versions (identical in US and PAL). Cross-check [hypothesis on the identity]: same bodies as the functions of the
// same names in cbr_decomp's class.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0).
// Structure: object.h.
#include "object.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

using gc::s32;

// Default initialisation of a new object: nothing to do, success (0).
s32 _hsdClassInit(ptr<HSD_Class> /*object*/) {
    return 0;
}

// Default release of an object's resources: nothing to release.
void _hsdClassRelease(ptr<HSD_Class> /*object*/) {}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_class_init, hsd::_hsdClassInit, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_class_release, hsd::_hsdClassRelease, gc::abi::flag_reads_only);
