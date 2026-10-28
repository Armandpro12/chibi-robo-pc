// Negative test of the decompilation oracle, the ten leaves of D1 mission 8: one deliberately WRONG variant each
// (built only by tools/build.py --oracle-negative, see wrong_variants.cpp).
//
//   hsd_hermite_interpolate   plain host float arithmetic: `float` products without the 25-bit rounding of frC and
//                             separate multiply and add instead of fused fmadds (wrong only on the states where the
//                             PowerPC rounding changes the result: the share is measured, not 100 % by design)
//   flag_set_test             low bit of the 64-bit result (r4) flipped
//   flag_set_clear            the high word is not written back (the original always writes it)
//   table_entry_at            address 4 bytes too far
//   hsd_obj_alloc_free        the used count is not decremented
//   hsd_mobj_clear_flags      the class pointer word written back unchanged (a store the original does not make)
//   hsd_jobj_get_flags        lowest bit of the result flipped
//   hsd_mtx_get_translation   lowest bit of z flipped
//   hsd_pobj_set_shape_blend  sign of the stored weight flipped
//   xaction_reset             the last word is not cleared
//
// The structures are the minimum each variant uses, in their own namespace (no clash with the real definitions).
#include "decomp/gc_abi.h"
#include "decomp/gc_float.h"
#include "decomp/gc_mem.h"

#include <cmath>
#include <cstddef>

namespace oracle_negative_leaves {

using gc::be;
using gc::f32;
using gc::f64;
using gc::ptr;
using gc::s32;
using gc::u16;
using gc::u32;
using gc::u64;
using gc::u8;

f32 hermite_plain_float(f64 inverse_duration, f64 time, f64 start_value, f64 end_value, f64 start_tangent,
                        f64 end_tangent) {
    // wrong on purpose: host float arithmetic, no 25-bit rounding of frC, multiply and add rounded separately
    const float a = static_cast<float>(inverse_duration), t = static_cast<float>(time);
    const float t2 = t * t, a2 = a * a, t3 = t2 * t;
    const float three_a2_t2 = 3.0f * t2 * a2, a_t2 = t2 * a, a2_t3 = a2 * t3;
    const float two_a3_t3 = 2.0f * a2_t3 * a;
    const float end_tangent_weight = a2_t3 - a_t2;
    const float start_tangent_weight = t + (end_tangent_weight - a_t2);
    const float end_value_weight = -two_a3_t3 + three_a2_t2;
    const float start_value_weight = 1.0f + (two_a3_t3 - three_a2_t2);
    float sum = static_cast<float>(end_value) * end_value_weight;
    sum = static_cast<float>(start_value) * start_value_weight + sum;
    sum = static_cast<float>(start_tangent) * start_tangent_weight + sum;
    return static_cast<float>(end_tangent) * end_tangent_weight + sum;
}

struct Flags64 {
    be<u32> high, low;
};
struct FlagSetOwner {
    gc::array<be<u8>, 0x228> before;
    Flags64 flags;
};

u64 flag_set_test_wrong_low_bit(ptr<const FlagSetOwner> owner, u64 mask) {
    const u64 flags = (static_cast<u64>(gc::load(owner->flags.high)) << 32) | gc::load(owner->flags.low);
    return (flags & mask) ^ 1u; // wrong on purpose
}

void flag_set_clear_wrong_missing_store(ptr<FlagSetOwner> owner, u64 mask) {
    owner->flags.low = gc::load(owner->flags.low) & ~static_cast<u32>(mask);
    // wrong on purpose: the high word is not written back
}

struct TableEntry {
    gc::array<be<u8>, 12> bytes;
};
ptr<TableEntry> table_entry_wrong_address(ptr<TableEntry> table, ptr<const be<u16>> index) {
    const ptr<TableEntry> right = table + static_cast<std::ptrdiff_t>(gc::load(*index));
    return gc::ptr_from_guest_address<TableEntry>(gc::guest_address(right) + 4u); // wrong on purpose
}

struct FreeBlock {
    ptr<FreeBlock> next;
};
struct ObjAllocData {
    gc::array<be<u8>, 4> before;
    ptr<FreeBlock> free_head;
    be<u32> used_count;
    be<u32> free_count;
};
void obj_alloc_free_wrong_count(ptr<ObjAllocData> pool, ptr<FreeBlock> block) {
    block->next = pool->free_head;
    pool->free_head = block;
    pool->free_count += 1u;
    // wrong on purpose: used_count not decremented
}

struct MaterialObject {
    ptr<void> class_info;
    be<u32> flags;
};
void mobj_clear_flags_wrong_extra_store(ptr<MaterialObject> material, u32 mask) {
    if (material == nullptr)
        return;
    material->class_info = material->class_info; // wrong on purpose: a store the original does not make
    material->flags &= ~mask;
}

struct JointObject {
    gc::array<be<u8>, 0x14> before;
    be<u32> flags;
};
u32 jobj_get_flags_wrong_bit(ptr<const JointObject> joint) {
    return (joint == nullptr ? 0u : gc::load(joint->flags)) ^ 1u; // wrong on purpose
}

struct Matrix34 {
    gc::array<gc::array<be<f32>, 4>, 3> rows;
};
struct Vector3 {
    be<f32> x, y, z;
};
void mtx_get_translation_wrong_bit(ptr<const Matrix34> matrix, ptr<Vector3> translation) {
    translation->x = matrix->rows[0][3];
    translation->y = matrix->rows[1][3];
    gc::store_raw(translation->z, gc::load_raw(matrix->rows[2][3]) ^ 1u); // wrong on purpose
}

struct ShapeSet {
    be<u16> flags;
    gc::array<be<u8>, 0x1A> before;
    ptr<be<f32>> weights; // or one weight, by the flag
};
struct PolygonObject {
    gc::array<be<u8>, 0xC> before;
    be<u16> flags;
    gc::array<be<u8>, 6> between;
    ptr<ShapeSet> shape_set;
};
void pobj_set_shape_blend_wrong_sign(ptr<PolygonObject> polygon, s32 track, ptr<const be<f32>> value) {
    if (polygon == nullptr || (gc::load(polygon->flags) & 0x3000u) != 0x1000u)
        return;
    const ptr<ShapeSet> shape_set = polygon->shape_set;
    const u32 bits = gc::load_raw(*value) ^ 0x80000000u; // wrong on purpose
    if (gc::load(shape_set->flags) & 2u) {
        const ptr<be<f32>> weights = shape_set->weights;
        gc::store_raw(weights[static_cast<std::ptrdiff_t>(track) - 2], bits);
    } else {
        gc::store_raw(*gc::ptr_cast<be<u32>>(gc::address_of(shape_set->weights)), bits);
    }
}

struct XAction {
    gc::array<be<u8>, 4> before;
    be<u32> state_a;
    gc::array<be<u8>, 4> between;
    be<u32> state_b;
    gc::array<be<u8>, 0x10> between_too;
    be<u32> state_c;
    be<u32> state_d;
};
void xaction_reset_wrong_missing_store(ptr<XAction> action) {
    action->state_a = 0u;
    action->state_b = 0u;
    action->state_c = 0u;
    // wrong on purpose: state_d not cleared
}

} // namespace oracle_negative_leaves

GC_DECOMP_EXPORT(hsd_hermite_interpolate__wrong, oracle_negative_leaves::hermite_plain_float,
                 gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(flag_set_test__wrong, oracle_negative_leaves::flag_set_test_wrong_low_bit, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(flag_set_clear__wrong, oracle_negative_leaves::flag_set_clear_wrong_missing_store,
                 gc::abi::flag_none);
GC_DECOMP_EXPORT(table_entry_at__wrong, oracle_negative_leaves::table_entry_wrong_address, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_obj_alloc_free__wrong, oracle_negative_leaves::obj_alloc_free_wrong_count, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_mobj_clear_flags__wrong, oracle_negative_leaves::mobj_clear_flags_wrong_extra_store,
                 gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_jobj_get_flags__wrong, oracle_negative_leaves::jobj_get_flags_wrong_bit,
                 gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_mtx_get_translation__wrong, oracle_negative_leaves::mtx_get_translation_wrong_bit,
                 gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_pobj_set_shape_blend__wrong, oracle_negative_leaves::pobj_set_shape_blend_wrong_sign,
                 gc::abi::flag_none);
GC_DECOMP_EXPORT(xaction_reset__wrong, oracle_negative_leaves::xaction_reset_wrong_missing_store, gc::abi::flag_none);
