// Matrix helpers of sysdolphin.
//
// Original unit: not known [hypothesis: an HSD unit near mtx.c; the function lies between other HSD functions of
// both versions]. Names are hypotheses (notes of the decompilation stage); the code was written from the disassembly
// of both versions (identical in US and PAL).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::f32;
using gc::ptr;

// 3x4 matrix of single-precision floats, row after row (the GameCube's Mtx); the last column is the translation.
struct Matrix34 {
    gc::array<gc::array<be<f32>, 4>, 3> rows;
};
struct Vector3 {
    be<f32> x;
    be<f32> y;
    be<f32> z;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(Matrix34) == 0x30 && sizeof(Vector3) == 12);
#endif

// translation = the last column of `matrix`. Each float is moved as it is (the original's lfs/stfs keep its bits),
// read and written one after the other in the original's order.
void mtx_get_translation(ptr<const Matrix34> matrix, ptr<Vector3> translation) {
    translation->x = matrix->rows[0][3];
    translation->y = matrix->rows[1][3];
    translation->z = matrix->rows[2][3];
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_mtx_get_translation, hsd::mtx_get_translation, gc::abi::flag_none);
