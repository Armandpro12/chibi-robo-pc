// Copy of a vector of three single-precision floats (x, y, z), from one object to another.
//
// Original unit: not known yet (no split of cbr_decomp covers this function); the file is named after what it holds.
// The name is a hypothesis (notes of the decompilation stage): the code was read from the disassembly of both
// versions, identical in US and PAL. Disabled in the game's table: it is the floating-point leaf of the oracle's
// tests (tools/build.py --oracle --decomp-enable vec3_copy), not a replacement of the game yet.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace chibi {

using gc::be;
using gc::f32;
using gc::ptr;

struct Vec3 {
    be<f32> x;
    be<f32> y;
    be<f32> z;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(Vec3) == 12 && offsetof(Vec3, z) == 8);
#endif

// destination = source. The original moves each float with a single-precision load and store (lfs/stfs), which keep
// the bits; copying the guest values keeps them too (no conversion through a host float). Same order as the original:
// x and y are read before x is written, z before y is written, so an overlapping copy gives the same bytes.
void vec3_copy(ptr<Vec3> destination, ptr<const Vec3> source) {
    const be<f32> x = source->x;
    const be<f32> y = source->y;
    destination->x = x;
    const be<f32> z = source->z;
    destination->y = y;
    destination->z = z;
}

} // namespace chibi

GC_DECOMP_EXPORT(vec3_copy, chibi::vec3_copy, gc::abi::flag_none);
