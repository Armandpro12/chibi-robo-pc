// Oracle test function (oracle builds only, row of decomp/functions.tsv left `disabled`, see nonleaf_pair.cpp): a real
// function of the game with two assertion blocks (anomaly A4, notes of the stage). Its original tests a pointer, calls
// __assert ([decomp.no_return] of game.toml) when it is null, and, because the compiler did not know that __assert
// never returns, goes on to read through that null pointer. A synthetic case enters the first assertion block: the
// call to __assert is declared without return (gc::abi::call_no_return), which ends the case on both sides at that
// call, so neither the original's read of low memory nor a null gc::ptr in the decompiled version is ever reached.
// No floating point. The code was read from the disassembly of both versions (GGTE01 801C01E4, GGTP01 801C2AE0):
// identical instruction for instruction, the small-data displacements of the two __assert arguments and the call
// targets aside. What the structure is, and so every name here, is not known [hypothesis]. Not counted as decompiled
// progress.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp_globals.h"
#include "test_addresses.h"

#include <cstddef>

namespace chibi::oracle_test {

using gc::be;
using gc::ptr;
using gc::s32;
using gc::u32;
using gc::u8;

struct ForwardOwner {
    gc::array<be<u8>, 0x28> unknown_00;
    be<u32> target; // 0x28: the only field the function reads, passed on as the first argument
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(ForwardOwner, target) == 0x28);
#endif

// The two __assert arguments in small data (globals table): the source file name and the asserted expression
// [hypothesis on the roles: the order of __assert(file, line, expression) of the CodeWarrior runtime].
constexpr gc::global<gc::array<be<u8>, 7>, global_id::oracle_test_assert_file> assert_file{};
constexpr gc::global<gc::array<be<u8>, 5>, global_id::oracle_test_assert_expression> assert_expression{};

u32 forward_checked(ptr<ForwardOwner> owner, u32 value);

namespace {
u32 assert_address() {
    return callee_address(&forward_checked, 0x801C01E4u, 0x801C2AE0u, 0x801DD248u, 0x801DFB44u);
}
u32 forward_address() {
    return callee_address(&forward_checked, 0x801C01E4u, 0x801C2AE0u, 0x801D3580u, 0x801D5E7Cu);
}
} // namespace

// Checks `owner` (twice: the second check, with its own line, is presumably an inlined accessor's), then passes its
// target and `value` to the recompiled callee and returns what that callee left in r3.
u32 forward_checked(ptr<ForwardOwner> owner, u32 value) {
    if (!owner)
        gc::abi::call_no_return(assert_address(), assert_file.address(), s32{0x2EC}, assert_expression.address());
    if (!owner)
        gc::abi::call_no_return(assert_address(), assert_file.address(), s32{0x2BC}, assert_expression.address());
    return gc::abi::call_guest<u32>(forward_address(), gc::load(owner->target), value);
}

} // namespace chibi::oracle_test

GC_DECOMP_EXPORT(oracle_test_forward_checked, chibi::oracle_test::forward_checked, gc::abi::flag_none);
