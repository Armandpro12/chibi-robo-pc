// HSD render state (sysdolphin): TEV stages in use, TEV colour registers and texture coordinate generators.
//
// Original unit: sysdolphin/baselib/tev.c [hypothesis: the functions sit after the tev.c source-file string in both
// versions]. The code was written from the disassembly of both versions (identical in US and PAL, the addresses of the
// globals aside). Names from cbr_decomp's tev.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src,
// CC0 1.0): HSD_StateInitTev clears the static current_tev, which HSD_StateGetNumTevStages returns;
// _HSD_StateInvalidateTevStage, _HSD_StateInvalidateTevRegister and _HSD_StateInvalidateTexCoordGen clear
// current_tev, the `c` word of the four TevReg records and num_tex_gens there, which is what these three do here
// [hypothesis on the identity]. doldecomp/melee's sysdolphin uses the same function names (names only, see the
// project rules). The C++ names drop the leading underscore of the last three (identifiers that start with an
// underscore and a capital are reserved in C++).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

using gc::be;
using gc::s16;
using gc::s32;

// current_tev of tev.c: number of TEV stages assigned since the state was last reset [hypothesis on the meaning: from
// the names].
constexpr gc::global<be<s32>, global_id::hsd_state_current_tev> current_tev_stage{};

// num_tex_gens of tev.c: number of texture coordinate generators set [hypothesis on the meaning: from the names].
constexpr gc::global<be<s32>, global_id::hsd_state_num_tex_gens> texture_coordinate_generator_count{};

// One record of TevReg in tev.c: the value last set in a TEV colour register (a GXColorS10, four signed 16-bit
// components) and a word `c` that says whether it is valid [hypothesis on the meaning: from cbr_decomp's tev.c, which
// sets the register again only when `c` is non-zero]. 0xC bytes: the original clears the word at +8 of four records
// 0xC apart.
struct TevRegisterState {
    gc::array<be<s16>, 4> colour;
    be<s32> valid;
};
constexpr gc::global<gc::array<TevRegisterState, 4>, global_id::hsd_state_tev_registers> tev_registers{};

// Resets the TEV stage count: no stage assigned.
void HSD_StateInitTev() {
    *current_tev_stage = 0;
}

// Number of TEV stages assigned since the last reset.
s32 HSD_StateGetNumTevStages() {
    return *current_tev_stage;
}

// Forgets the TEV stages assigned (the same store as HSD_StateInitTev, a separate function in the original).
void HSD_StateInvalidateTevStage() {
    *current_tev_stage = 0;
}

// Marks the four TEV colour registers as not set, in the original's order (registers 0 to 3).
void HSD_StateInvalidateTevRegister() {
    for (int index = 0; index < 4; ++index)
        (*tev_registers)[static_cast<std::size_t>(index)].valid = 0;
}

// Forgets the texture coordinate generators set.
void HSD_StateInvalidateTexCoordGen() {
    *texture_coordinate_generator_count = 0;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_state_init_tev, hsd::HSD_StateInitTev, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_state_get_num_tev_stages, hsd::HSD_StateGetNumTevStages, gc::abi::flag_reads_only);
GC_DECOMP_EXPORT(hsd_state_invalidate_tev_stage, hsd::HSD_StateInvalidateTevStage, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_state_invalidate_tev_register, hsd::HSD_StateInvalidateTevRegister, gc::abi::flag_none);
GC_DECOMP_EXPORT(hsd_state_invalidate_tex_coord_gen, hsd::HSD_StateInvalidateTexCoordGen, gc::abi::flag_none);
