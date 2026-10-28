// HSD particle system (sysdolphin): the particle and the shared position/rotation/scale record ("AppSRT") that
// particles and generators can be attached to.
//
// Names from doldecomp/melee's psstructs.h (names only, see the project rules): HSD_Particle, next, kind,
// HSD_psAppSRT, appsrt, usedCount. The layout is this game's, read from its code (static asserts below, oracle):
// `next` at 0, `kind` at 4, the AppSRT's use count at 0x32 and the particle's `appsrt` pointer at 0x90 are what the
// oracle checked in this game's code (the offsets are not taken from any other project).
#ifndef CHIBI_DECOMP_SYSDOLPHIN_BASELIB_PSSTRUCTS_H
#define CHIBI_DECOMP_SYSDOLPHIN_BASELIB_PSSTRUCTS_H

#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::be;
using gc::ptr;
using gc::u16;
using gc::u32;
using gc::u8;

struct HSD_psAppSRT {
    gc::array<be<u8>, 0x32> not_described_here;
    be<u16> used_count; // particles and generators attached to it
};

struct HSD_Particle {
    ptr<HSD_Particle> next; // next particle of the same list
    be<u32> kind;           // flags
    gc::array<be<u8>, 0x88> not_described_here;
    ptr<HSD_psAppSRT> appsrt; // AppSRT the particle follows, null if none
};

#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HSD_psAppSRT, used_count) == 0x32);
static_assert(offsetof(HSD_Particle, kind) == 4 && offsetof(HSD_Particle, appsrt) == 0x90);
#endif

} // namespace hsd

#endif // CHIBI_DECOMP_SYSDOLPHIN_BASELIB_PSSTRUCTS_H
