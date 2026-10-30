// HSD sound synthesis (sysdolphin): whether a stream is playing.
//
// Original unit: sysdolphin/baselib/synth.c [hypothesis: the function sits among the stream functions of synth.c of
// the symbol supplement (HSD_SynthPStreamStart at US 801F253C, names from doldecomp/melee, names only)]. The code was
// written from the disassembly of both versions (identical in US and PAL, the address of the global aside). The names
// are this project's [hypothesis]: the symbol supplement has none for this function.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"

namespace hsd {

using gc::ptr;
using gc::s32;

// Head of the list of the streams started [hypothesis on the meaning: the stream start (US 801F26A0-801F26AC) pushes
// a record on it, its first word linking to the previous head; two functions (US 801F11C0, 801F1628) search it].
// The record is not described here.
struct HSD_SynthStream;
constexpr gc::global<ptr<HSD_SynthStream>, global_id::hsd_synth_stream_list> started_streams{};

// 1 if the list of started streams is not empty, 0 otherwise. (The original computes `head != 0` without a branch.)
s32 HSD_SynthIsStreamStarted() {
    return *started_streams != nullptr ? 1 : 0;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_synth_is_stream_started, hsd::HSD_SynthIsStreamStarted, gc::abi::flag_reads_only);
