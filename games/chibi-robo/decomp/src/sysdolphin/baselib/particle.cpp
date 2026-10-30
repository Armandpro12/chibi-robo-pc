// HSD particle system (sysdolphin): helpers of the particle command interpreter, and particle allocation.
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
#include "decomp_globals.h"

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

// --- Particle allocation (US 801E5BEC / PAL 801E84E8) ---
// Unit [hypothesis]: particle.c like the two helpers above; it sits after psAttachParticleAppSRT (psappsrt.cpp) and
// a single call site calls it (US 801DDC90). Its name and those of its globals are this project's
// [hypothesis]: the symbol supplement has none. The counts are 16-bit words also stored at US 801DD970 and 801DD980,
// and the function at US 801E5D24 gives a particle back to the free list and
// decrements the count in use [hypothesis: read in the US disassembly, not compared].
constexpr gc::global<ptr<HSD_Particle>, global_id::ps_free_particles> free_particles{};
constexpr gc::global<be<u16>, global_id::ps_particles_in_use> particles_in_use{};
constexpr gc::global<be<u16>, global_id::ps_particles_peak> particles_peak{};

// Particle lists by bank (16 heads) and, for each bank, a word set to 1 when a particle is added to it [hypothesis on
// the meaning of the banks].
inline constexpr std::size_t particle_banks = 16;
constexpr gc::global<gc::array<ptr<HSD_Particle>, particle_banks>, global_id::ps_particle_lists> particle_lists{};
constexpr gc::global<gc::array<be<u32>, particle_banks>, global_id::ps_particle_list_used> particle_list_used{};

// Takes a particle from the free list and links it into list `bank`: after `after` when it is not null, otherwise at
// the head of the list. Null when no particle is free. Counts the particle in use and keeps the peak count. Reads and
// writes in the original's order (the peak is read before the new count is stored).
ptr<HSD_Particle> psAllocParticle(ptr<HSD_Particle> after, u32 bank) {
    const ptr<HSD_Particle> particle = *free_particles;
    if (gc::path::falls_through(particle == nullptr))
        return nullptr;
    const u16 in_use = gc::load(*particles_in_use);
    const u16 peak = gc::load(*particles_peak);
    const u16 now_in_use = static_cast<u16>(in_use + 1);
    *particles_in_use = now_in_use;
    if (gc::path::falls_through(now_in_use > peak))
        *particles_peak = now_in_use;
    const ptr<HSD_Particle> next_free = particle->next;
    *free_particles = next_free;
    if (gc::path::taken(after != nullptr)) {
        particle->next = after->next;
        after->next = particle;
    } else {
        particle->next = (*particle_lists)[bank];
        (*particle_lists)[bank] = particle;
    }
    (*particle_list_used)[bank] = 1;
    return particle;
}

} // namespace hsd

GC_DECOMP_EXPORT(ps_alloc_particle, hsd::psAllocParticle, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(ps_read_count, hsd::psReadCount, gc::abi::flag_path_marked);
GC_DECOMP_EXPORT(ps_get_next_particle, hsd::psGetNextParticle, gc::abi::flag_reads_only);
