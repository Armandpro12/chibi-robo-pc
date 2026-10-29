// HSD particle system (sysdolphin): helpers of the particle command interpreter.
//
// Original unit: sysdolphin/baselib/particle.c [hypothesis: both functions are called only by the particle command
// interpreter (US 801DF0C8), the large function between them, which reads particle commands from a
// byte stream; doldecomp/melee's particle.c (names only) has that interpreter but inlines both helpers]. The code
// was written from the disassembly of both versions (identical in US and PAL). Both names are this project's
// [hypothesis]; the symbol supplement proposed psSetUserFunc for 801E2354 (US) by position only, but that function
// of Melee stores a generator's callback, while this one only reads a word. Structure: psstructs.h.
#include "psstructs.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"

namespace hsd {

// Reads a count from the command stream at `stream` into `*count` and returns the position after it: one byte, or,
// when that byte has its top bit set, two bytes holding a 15-bit big-endian value (the top bit of the first byte is
// dropped). The first byte is written to `*count` and read back from it before the second one is read, as in the
// original (the same bytes if `count` overlaps the stream).
// (Path mark: runtime/core/decomp/gc_path.h; the original returns early by a conditional return, taken on one byte.)
ptr<const be<u8>> psReadCount(ptr<const be<u8>> stream, ptr<be<u16>> count) {
    *count = gc::load(*stream);
    stream += 1;
    const u16 first = gc::load(*count);
    if (gc::path::taken(!(first & 0x80)))
        return stream;
    const u8 second = gc::load(*stream);
    stream += 1;
    *count = static_cast<u16>(((first << 8) & 0x7F00) + second);
    return stream;
}

// The particle after `particle` in its list (the role is a [hypothesis]: not checked against its callers).
ptr<HSD_Particle> psGetNextParticle(ptr<const HSD_Particle> particle) {
    return particle->next;
}

} // namespace hsd

GC_DECOMP_EXPORT(ps_read_count, hsd::psReadCount, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(ps_get_next_particle, hsd::psGetNextParticle, gc::abi::flag_reads_only);
