// State test reached through a function pointer stored in game data (no direct call in the code of either version).
//
// Original unit: not known yet (no split of cbr_decomp covers this function); the file is named after what it holds.
// Names are hypotheses (notes of the decompilation stage): the code was read from the disassembly of both versions,
// identical in US and PAL.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace chibi {

using gc::be;
using gc::s32;
using gc::u8;

// Object whose first word is a state number; nothing else is read here.
struct StateCell {
    be<s32> state;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(StateCell, state) == 0);
#endif

// States 1 to 5 are the "active" ones (signed comparison: negative states are not active).
constexpr s32 first_active_state = 1;
constexpr s32 end_of_active_states = 6;

// 1 if the cell is in an active state, 0 otherwise.
u8 is_state_active(gc::ptr<const StateCell> cell) {
    const s32 state = gc::load(cell->state);
    return state >= first_active_state && state < end_of_active_states ? 1 : 0;
}

} // namespace chibi

GC_DECOMP_EXPORT(state_cell_is_state_active, chibi::is_state_active, gc::abi::flag_reads_only);
