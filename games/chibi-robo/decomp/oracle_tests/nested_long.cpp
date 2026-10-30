// Oracle test function (oracle builds only, row of decomp/functions.tsv left `disabled`), mission D2-6a (anomaly A17):
// a real wrapper of the game whose only work is one call to a recompiled function that walks a whole tree of objects,
// so that the callee runs long enough, in the nested run of a decompiled caller, for an interrupt or an alarm to fall
// due during it. It measures how a nested call from decompiled code to recompiled code (gc_abi run_guest ->
// gcrt_guest_call_prepared: MSR[EE] = 0, thread switches refused) delays the delivery of events and the order of the
// threads, against the same function left recompiled.
// Code read from the disassembly of both versions: identical instruction for instruction, call target aside
// (GGTE01 801C4A78 -> 801C47B0, GGTP01 801C7374 -> 801C70AC): `li r4, 2047; bl callee` in a frame of 16 bytes, r3 and
// f1 passed through untouched. Names after the game's symbol map (HSD_JObjReqAnimAll calls HSD_JObjReqAnimAllByFlags
// with every flag) [hypothesis on the meaning]. Not counted as decompiled progress.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "test_addresses.h"

namespace chibi::oracle_test {

using gc::u32;

void request_animation_all(u32 joint);

namespace {
u32 request_animation_by_flags_address() {
    return callee_address(&request_animation_all, 0x801C4A78u, 0x801C7374u, 0x801C47B0u, 0x801C70ACu);
}
} // namespace

// Requests the animation of the joint tree rooted at `joint` for every animation kind (mask 0x7FF). The frame argument
// in f1 is passed through: the C++ signature does not declare it, so f1 is left as the caller set it.
void request_animation_all(u32 joint) {
    gc::abi::call_guest<void>(request_animation_by_flags_address(), joint, u32(0x7FF));
}

} // namespace chibi::oracle_test

GC_DECOMP_EXPORT(oracle_test_request_animation_all, chibi::oracle_test::request_animation_all, gc::abi::flag_none);
