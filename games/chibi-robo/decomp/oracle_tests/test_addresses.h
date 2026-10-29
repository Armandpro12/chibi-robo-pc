// Oracle test functions only (games/<game>/decomp/oracle_tests and their wrong variants in oracle_negative): guest
// address, in the version that runs, of a RECOMPILED function they call. The game has no per-version table of function
// addresses for decompiled code yet (stage 2, D2-2); until then a test function tells the version by the address the
// build gave to itself, a registered function whose address in the reference version is known. Never used by
// decompiled code of decomp/src: there, callees are reached through the table D2-2 will add.
#pragma once

#include "decomp/gc_abi.h"

#include <cstdio>
#include <cstdlib>

namespace chibi::oracle_test {

// `self`: the registered test function (its C++ function); `self_reference`, `self_other`: its address in the
// reference version (GGTE01) and in the other one (GGTP01); `reference`, `other`: the callee's two addresses.
template <class F>
gc::u32 callee_address(F* self, gc::u32 self_reference, gc::u32 self_other, gc::u32 reference, gc::u32 other) {
    const gc::u32 here = gc::abi::detail::address_of_function(reinterpret_cast<void (*)()>(self));
    if (here == self_reference)
        return reference;
    if (here == self_other)
        return other;
    std::fprintf(stderr, "oracle test: function registered at %08X, expected %08X or %08X\n",
                 static_cast<unsigned>(here), static_cast<unsigned>(self_reference), static_cast<unsigned>(self_other));
    std::abort();
}

} // namespace chibi::oracle_test
