// HSD utilities (sysdolphin): product of two colours.
//
// Original unit: sysdolphin/baselib/util.c [hypothesis: name and unit from doldecomp/melee (names only), symbol
// supplement]. The code was written from the disassembly of both versions (identical in US and PAL). Structure:
// GXColor (lobj.h).
#include "lobj.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace hsd {

using gc::u32;

namespace {

// One component of the product: a * b / 255, rounded down. The original divides by multiplying with 0x80808081 and
// keeping the bits above 39, which is the exact unsigned division by 255 for every 32-bit value (here at most
// 255 * 255).
u8 multiply_component(u8 a, u8 b) {
    const u32 product = u32{a} * u32{b};
    return static_cast<u8>(product / 255u);
}

} // namespace

// `*result` = `*a` times `*b`, component by component (each component as a fraction of 255). The original reads both
// components then writes the result, one component at a time in the order red, green, blue, alpha: `result` may be
// one of the operands.
void HSD_MulColor(ptr<const GXColor> a, ptr<const GXColor> b, ptr<GXColor> result) {
    result->r = multiply_component(gc::load(a->r), gc::load(b->r));
    result->g = multiply_component(gc::load(a->g), gc::load(b->g));
    result->b = multiply_component(gc::load(a->b), gc::load(b->b));
    result->a = multiply_component(gc::load(a->a), gc::load(b->a));
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_mul_color, hsd::HSD_MulColor, gc::abi::flag_none);
