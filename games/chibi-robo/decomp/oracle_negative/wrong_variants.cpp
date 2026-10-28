// Negative test of the decompilation oracle: one deliberately WRONG variant of each function of the table, each with a
// different kind of error the oracle must report. Built only by tools/build.py --oracle-negative <this folder>, which
// registers <name>__wrong instead of <name>; never in another build (the folder is outside decomp/src).
//
//   state_cell_is_state_active   wrong result (r3 inverted)
//   hsd_archive_get_public       right result, callee-saved register r31 not preserved
//   mode_owner_is_mode_in_effect right result, condition field cr3 not preserved, and one word written at r3 (the
//                                object's first word): in its synthetic cases r3 = S-0x95D0 lies in the 64 KiB
//                                dead stack of a real call, so this store is seen only through the narrower
//                                window of synthetic cases (gc_oracle.cpp)
//   hsd_cobj_mtx_is_dirty        right result, two bytes written that the original does not write
//   vec3_copy                    one bit of a copied float different (least significant bit of y)
//
// The structures are the minimum each variant reads, in their own namespace (no clash with the real definitions).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace oracle_negative {

using gc::be;
using gc::f32;
using gc::ptr;
using gc::s32;
using gc::u16;
using gc::u32;
using gc::u8;

struct StateCell {
    be<s32> state;
};

u8 state_cell_wrong_result(ptr<const StateCell> cell) {
    const s32 state = gc::load(cell->state);
    const u8 right = state >= 1 && state < 6 ? 1 : 0;
    return right ^ 1u; // wrong on purpose
}

struct ArchiveHeader {
    be<u32> file_size, data_size, nb_reloc, nb_public, nb_extern;
    gc::array<be<u8>, 4> version;
    gc::array<be<u32>, 2> pad;
};
struct ArchivePublicInfo {
    be<u32> offset;
    be<u32> symbol;
};
struct Archive {
    ArchiveHeader header;
    ptr<void> data, reloc_info;
    ptr<ArchivePublicInfo> public_info;
    ptr<void> extern_info;
    ptr<char> symbols;
};

ptr<char> archive_wrong_preserved_register(ptr<Archive> archive, s32 index) {
    gc::abi::testing::clobber_gpr(31, 0xDEADBEEFu); // wrong on purpose: r31 must be preserved
    if (index < 0 || gc::load(archive->header.nb_public) <= static_cast<u32>(index))
        return nullptr;
    return archive->symbols + static_cast<std::ptrdiff_t>(gc::load(archive->public_info[index].symbol));
}

struct ModeOwner {
    be<u32> first_word;
    gc::array<be<u8>, 0x95CC> not_described_here;
    be<s32> current_mode;
    be<s32> pending_mode;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(ModeOwner, current_mode) == 0x95D0);
#endif

u8 mode_owner_wrong_condition_field(ptr<ModeOwner> owner, s32 mode) {
    owner->first_word = gc::load(owner->first_word) ^ 0x80u; // wrong on purpose: the original writes nothing
    s32 in_effect = gc::load(owner->pending_mode);
    if (in_effect < 0)
        in_effect = gc::load(owner->current_mode);
    gc::abi::testing::clobber_cr(0x000F0000u); // wrong on purpose: cr2..cr4 must be preserved (cr3 = 1111, others 0)
    return in_effect == mode ? 1 : 0;
}

struct Obj {
    ptr<void> class_info;
    be<u16> ref_count;
    be<u16> ref_count_individual;
};
struct WObj {
    Obj parent;
    be<u32> flags;
};
struct CObj {
    Obj parent;
    be<u32> flags;
    gc::array<be<u32>, 4> viewport;
    gc::array<be<u16>, 4> scissor;
    ptr<WObj> eyepos;
    ptr<WObj> interest;
};

u8 cobj_wrong_extra_write(ptr<CObj> cobj) {
    cobj->parent.ref_count += 1; // wrong on purpose: the original writes nothing
    if (gc::load(cobj->flags) & (1u << 30))
        return 1;
    const ptr<WObj> eyepos = cobj->eyepos;
    if (eyepos != nullptr && (gc::load(eyepos->flags) & 2u))
        return 1;
    const ptr<WObj> interest = cobj->interest;
    if (interest != nullptr && (gc::load(interest->flags) & 2u))
        return 1;
    return 0;
}

struct Vec3 {
    be<f32> x, y, z;
};

void vec3_wrong_float_bit(ptr<Vec3> destination, ptr<const Vec3> source) {
    const be<f32> x = source->x;
    const be<f32> y = source->y;
    destination->x = x;
    const be<f32> z = source->z;
    gc::store_raw(destination->y, gc::load_raw(y) ^ 1u); // wrong on purpose: last mantissa bit flipped
    destination->z = z;
}

} // namespace oracle_negative

GC_DECOMP_EXPORT(state_cell_is_state_active__wrong, oracle_negative::state_cell_wrong_result, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_archive_get_public__wrong, oracle_negative::archive_wrong_preserved_register,
                 gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(mode_owner_is_mode_in_effect__wrong, oracle_negative::mode_owner_wrong_condition_field,
                 gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_cobj_mtx_is_dirty__wrong, oracle_negative::cobj_wrong_extra_write, gc::abi::flag_none);
GC_DECOMP_EXPORT(vec3_copy__wrong, oracle_negative::vec3_wrong_float_bit, gc::abi::flag_none);
