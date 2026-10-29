// HSD polygon objects (sysdolphin): the matrix marks.
//
// Original unit: sysdolphin/baselib/pobj.c [hypothesis: the function sits in the pobj.c range of both versions]. This
// file holds the functions of that unit that use a game global; it is to be merged with pobj.cpp. The code was
// written from the disassembly of both versions (identical in US and PAL, the address of the global aside). Names
// and layout from cbr_decomp's pobj.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0):
// HSD_PObjClearMtxMark, mtx_mark[2] of {obj, mark}; doldecomp/melee's sysdolphin uses the same function name (names
// only, see the project rules). The role of the marks is this project's [hypothesis].
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u32;

// One matrix mark: the object whose matrix was last loaded, and a mark of how [hypothesis on the meaning].
struct MatrixMark {
    ptr<void> object;
    be<u32> mark;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(MatrixMark) == 8 && offsetof(MatrixMark, mark) == 4);
#endif

// mtx_mark of pobj.c: two marks (the original writes both at once here).
constexpr gc::global<gc::array<MatrixMark, 2>, global_id::hsd_pobj_mtx_mark> matrix_marks{};

// Sets both matrix marks to (`object`, `mark`). The original stores the words in the order object 0, mark 0,
// object 1, mark 1.
void pobj_clear_mtx_mark(ptr<void> object, u32 mark) {
    for (auto& entry : *matrix_marks) {
        entry.object = object;
        entry.mark = mark;
    }
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_pobj_clear_mtx_mark, hsd::pobj_clear_mtx_mark, gc::abi::flag_none);
