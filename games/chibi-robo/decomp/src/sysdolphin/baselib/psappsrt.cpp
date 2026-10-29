// HSD particle system (sysdolphin): attaching a particle to a shared AppSRT record.
//
// Original unit: sysdolphin/baselib/psappsrt.c [hypothesis: the name psAttachParticleAppSRT comes from
// doldecomp/melee (names only), where it is followed by psAttachGeneratorAppSRT, a function of the same size; here the
// function is followed by one of the same size too (US 801E57CC). Its callers in this game pass a new particle and
// the AppSRT of its generator]. The code was written from the disassembly of both versions (identical in US and PAL).
// Structure: psstructs.h.
#include "psstructs.h"

#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"

namespace hsd {

using gc::s32;

// Attaches `particle` to `appsrt` and returns the AppSRT's new use count; -1 (nothing done) if either is null or if
// the particle already follows an AppSRT. (Path marks: runtime/core/decomp/gc_path.h; the original tests the AppSRT,
// then the particle, then branches to the attachment when the particle has no AppSRT: falls_through there.)
s32 psAttachParticleAppSRT(ptr<HSD_Particle> particle, ptr<HSD_psAppSRT> appsrt) {
    if (gc::path::taken(appsrt == nullptr) || gc::path::taken(particle == nullptr))
        return -1;
    if (gc::path::falls_through(particle->appsrt != nullptr))
        return -1;
    particle->appsrt = appsrt;
    const u16 used_count = static_cast<u16>(gc::load(appsrt->used_count) + 1);
    appsrt->used_count = used_count;
    return used_count;
}

} // namespace hsd

GC_DECOMP_EXPORT(ps_attach_particle_app_srt, hsd::psAttachParticleAppSRT, gc::abi::flag_path_marked);
