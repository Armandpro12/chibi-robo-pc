// Oracle test functions (oracle builds only, rows of decomp/functions.tsv left `disabled`, see nonleaf_pair.cpp): two
// real functions of the game whose calls exercise what the non-leaf comparison of runtime/core/decomp/gc_oracle.cpp
// ("Calls") had only read so far:
//   - free_chain: a call to itself (recursion), calls in a fixed order to two other functions, and the register
//     save/restore helpers of the EABI (_savegpr_23 / _restgpr_23, `bl` the oracle runs inline, not as calls);
//   - run_handler_if_ready: a direct call whose result decides an indirect call (`bctrl`) through a function pointer
//     read from memory. NOT enabled in the oracle builds (mission D2-1b, notes of the stage): in GGTE01 new-game its
//     first call never returned, because its callee (GGTE01 80091C80) ran past the block budget of the nested run in
//     which the oracle runs the original (interrupts masked, no thread switch); the game stopped there. Kept as the
//     test of that limit, to be enabled again once the oracle handles callees that wait.
// Neither has floating point nor a global. The code was read from the disassembly of both versions: identical
// instruction for instruction, call targets aside (the callees' addresses below, read in each version's code). What
// the structures are, and so every name here, is not known [hypothesis]. Not counted as decompiled progress.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "test_addresses.h"

#include <cstddef>

namespace chibi::oracle_test {

using gc::be;
using gc::ptr;
using gc::u32;
using gc::u8;

// --- free_chain (GGTE01 801C1E28, GGTP01 801C4724) ------------------------------------------------------------------

struct ChainNode {
    ptr<ChainNode> next; // the only field the function reads
};

void free_chain(ptr<ChainNode> head);

namespace {
// Callees (recompiled): the release of a node, and the one the deepest node walked goes through (it calls the
// first one).
u32 release_node_address() {
    return callee_address(&free_chain, 0x801C1E28u, 0x801C4724u, 0x801C3B20u, 0x801C641Cu);
}
u32 release_last_walked_address() {
    return callee_address(&free_chain, 0x801C1E28u, 0x801C4724u, 0x801C1E00u, 0x801C46FCu);
}
} // namespace

// Frees a chain of nodes from its last node back to its head. The compiler inlined the recursion eight levels deep:
// up to nine nodes are walked (the ninth through its own release function), the rest of the chain beyond the ninth
// goes through a real recursive call made first, then the walked nodes are released from the deepest to the head.
void free_chain(ptr<ChainNode> head) {
    constexpr unsigned walked_max = 9;
    ptr<ChainNode> walked[walked_max];
    unsigned count = 0;
    for (ptr<ChainNode> node = head; node; node = node->next) {
        walked[count++] = node;
        if (count == walked_max)
            break;
    }
    if (count == walked_max) {
        const ptr<ChainNode> deepest = walked[walked_max - 1];
        gc::abi::call<&free_chain>(ptr<ChainNode>(deepest->next));
        gc::abi::call_guest<void>(release_last_walked_address(), deepest);
        count--;
    }
    while (count > 0)
        gc::abi::call_guest<void>(release_node_address(), walked[--count]);
}

// --- run_handler_if_ready (GGTE01 80092D34, GGTP01 80094DE0) --------------------------------------------------------

struct HandlerTable {
    gc::array<be<u8>, 0x18> unknown_00;
    be<u32> handler; // guest address of a function (context, value)
};
struct HandlerContext {
    gc::array<be<u8>, 0x190> unknown; // 0x980..0xB10 of the owner; only its address is used here
};
struct HandlerOwner {
    gc::array<be<u8>, 0x70> unknown_00;
    ptr<be<u32>> value_source; // 0x70
    gc::array<be<u8>, 0x90C> unknown_74;
    HandlerContext context; // 0x980
    ptr<HandlerTable> handlers; // 0xB10
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(HandlerOwner, value_source) == 0x70 && offsetof(HandlerOwner, context) == 0x980 &&
              offsetof(HandlerOwner, handlers) == 0xB10 && offsetof(HandlerTable, handler) == 0x18);
#endif

u32 run_handler_if_ready(ptr<HandlerOwner> owner);

namespace {
u32 is_ready_address() {
    return callee_address(&run_handler_if_ready, 0x80092D34u, 0x80094DE0u, 0x80091C80u, 0x80093D2Cu);
}
} // namespace

// If the owner is ready (recompiled test), calls its handler with its context and the current value: 1; else 0.
u32 run_handler_if_ready(ptr<HandlerOwner> owner) {
    if (gc::abi::call_guest<u32>(is_ready_address(), owner) == 0)
        return 0;
    const u32 value = gc::load(*ptr<be<u32>>(owner->value_source));
    const ptr<HandlerTable> handlers = owner->handlers;
    gc::abi::call_address<void>(gc::load(handlers->handler), gc::address_of(owner->context), value);
    return 1;
}

} // namespace chibi::oracle_test

GC_DECOMP_EXPORT(oracle_test_free_chain, chibi::oracle_test::free_chain, gc::abi::flag_none);
GC_DECOMP_EXPORT(oracle_test_run_handler_if_ready, chibi::oracle_test::run_handler_if_ready, gc::abi::flag_none);
