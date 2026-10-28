// Negative test of the oracle's comparison of functions that call others (D2-1): deliberately WRONG variants of the
// oracle test functions of games/<game>/decomp/oracle_tests (built only by tools/build.py --oracle-negative, see
// wrong_variants.cpp, with --decomp-enable naming those disabled rows).
//
//   oracle_test_clear_record            (leaf callee) word 0x0C not cleared; with GCRT_ORACLE_TEST_VARIANT=
//                                       leaf_lr_word instead: every word cleared, and a store of 0 into the LR save
//                                       word of its caller's frame, 4(r1) at entry (a leaf's original never writes
//                                       there: the LR save word rule of gc_oracle_compare.h must not hide it)
//   oracle_test_clear_record_and_tail   one of seven errors of a caller, chosen by GCRT_ORACLE_TEST_VARIANT (one run
//                                       per variant: tools/decomp_oracle.py run --env GCRT_ORACLE_TEST_VARIANT=<name>):
//                                         missing_store      the store at +0x80 is not made
//                                         wrong_argument     the callee gets record + 4
//                                         extra_call         the callee is called twice
//                                         missing_call       the callee is not called
//                                         store_before_call  the store at +0x80 is made before the call
//                                         wrong_target       the call goes to the callee's address + 4 (never run:
//                                                            under comparison a call is replayed, not made)
//                                         lr_word_other_value  after the call, 0 is stored into the LR save word,
//                                                            4(r1) at entry, where the original saved its entry LR
//                                                            (another value there must stay a difference)
//                                         leaf_lr_word       the caller is right (the leaf's variant above is wrong)
//                                       unset or unknown: the run stops at the first call (no silent default)
//   oracle_test_free_chain              calls in the wrong order: the walked nodes released from the head to the
//                                       deepest (and the deepest one before the recursive call)
//   oracle_test_run_handler_if_ready    result inverted (1 when not ready, 0 after the handler)
//
// The structures are the minimum each variant uses, in their own namespace (no clash with the real definitions).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "../oracle_tests/test_addresses.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oracle_negative_nonleaf {

using chibi::oracle_test::callee_address;
using gc::be;
using gc::ptr;
using gc::u32;
using gc::u8;

struct ClearedRecord {
    be<u32> word_00;
    be<u32> word_04;
    gc::array<be<u8>, 4> untouched_08;
    be<u32> word_0c;
    gc::array<be<u8>, 8> untouched_10;
    gc::array<be<u32>, 25> words_18;
    gc::array<be<u8>, 4> untouched_7c;
    be<u32> word_80;
};

enum class caller_variant {
    missing_store,
    wrong_argument,
    extra_call,
    missing_call,
    store_before_call,
    wrong_target,
    lr_word_other_value,
    leaf_lr_word,
};
caller_variant selected_variant();

// The leaf's variant: read without stopping when the variable is unset (the offline replay of its cases, tools/
// decomp_oracle.py replay --negative, runs it without one).
bool leaf_stores_lr_word() {
    static const bool chosen = [] {
        const char* name = std::getenv("GCRT_ORACLE_TEST_VARIANT");
        return name && std::strcmp(name, "leaf_lr_word") == 0;
    }();
    return chosen;
}

void clear_record_wrong_missing_word(ptr<ClearedRecord> record) {
    record->word_00 = 0u;
    record->word_04 = 0u;
    if (leaf_stores_lr_word()) {
        record->word_0c = 0u;
        for (unsigned i = 0; i < 25; i++)
            record->words_18[i] = 0u;
        gc::abi::testing::store_word_at_entry_sp(4u, 0u); // wrong on purpose: into the caller's LR save word
        return;
    }
    // wrong on purpose: word_0c not cleared
    for (unsigned i = 0; i < 25; i++)
        record->words_18[i] = 0u;
}

caller_variant selected_variant() {
    static const caller_variant chosen = [] {
        const char* name = std::getenv("GCRT_ORACLE_TEST_VARIANT");
        static const struct {
            const char* name;
            caller_variant variant;
        } known[] = {{"missing_store", caller_variant::missing_store},
                     {"wrong_argument", caller_variant::wrong_argument},
                     {"extra_call", caller_variant::extra_call},
                     {"missing_call", caller_variant::missing_call},
                     {"store_before_call", caller_variant::store_before_call},
                     {"wrong_target", caller_variant::wrong_target},
                     {"lr_word_other_value", caller_variant::lr_word_other_value},
                     {"leaf_lr_word", caller_variant::leaf_lr_word}};
        for (const auto& k : known)
            if (name && std::strcmp(name, k.name) == 0) {
                std::fprintf(stderr, "oracle negative: oracle_test_clear_record_and_tail variant %s\n", k.name);
                return k.variant;
            }
        std::fprintf(stderr, "oracle negative: GCRT_ORACLE_TEST_VARIANT=%s is not a variant of "
                             "oracle_test_clear_record_and_tail\n", name ? name : "(unset)");
        std::abort();
    }();
    return chosen;
}

void clear_record_and_tail_wrong(ptr<ClearedRecord> record) {
    switch (selected_variant()) {
    case caller_variant::missing_store:
        gc::abi::call<&clear_record_wrong_missing_word>(record);
        break;
    case caller_variant::wrong_argument:
        gc::abi::call<&clear_record_wrong_missing_word>(
            gc::ptr_from_guest_address<ClearedRecord>(gc::guest_address(record) + 4u));
        record->word_80 = 0u;
        break;
    case caller_variant::extra_call:
        gc::abi::call<&clear_record_wrong_missing_word>(record);
        gc::abi::call<&clear_record_wrong_missing_word>(record);
        record->word_80 = 0u;
        break;
    case caller_variant::missing_call:
        record->word_80 = 0u;
        break;
    case caller_variant::store_before_call:
        record->word_80 = 0u;
        gc::abi::call<&clear_record_wrong_missing_word>(record);
        break;
    case caller_variant::wrong_target: {
        const u32 callee = gc::abi::detail::address_of_function(
            reinterpret_cast<void (*)()>(&clear_record_wrong_missing_word));
        gc::abi::call_guest<void>(callee + 4u, record);
        record->word_80 = 0u;
        break;
    }
    case caller_variant::lr_word_other_value:
        gc::abi::call<&clear_record_wrong_missing_word>(record);
        record->word_80 = 0u;
        gc::abi::testing::store_word_at_entry_sp(4u, 0u); // wrong on purpose: not the entry LR
        break;
    case caller_variant::leaf_lr_word:
        gc::abi::call<&clear_record_wrong_missing_word>(record);
        record->word_80 = 0u;
        break;
    }
}

struct ChainNode {
    ptr<ChainNode> next;
};

void free_chain_wrong_order(ptr<ChainNode> head);

u32 release_node_address() {
    return callee_address(&free_chain_wrong_order, 0x801C1E28u, 0x801C4724u, 0x801C3B20u, 0x801C641Cu);
}
u32 release_last_walked_address() {
    return callee_address(&free_chain_wrong_order, 0x801C1E28u, 0x801C4724u, 0x801C1E00u, 0x801C46FCu);
}

void free_chain_wrong_order(ptr<ChainNode> head) {
    ptr<ChainNode> walked[9];
    unsigned count = 0;
    for (ptr<ChainNode> node = head; node; node = node->next) {
        walked[count++] = node;
        if (count == 9)
            break;
    }
    // wrong on purpose: released from the head to the deepest, the deepest before the recursive call
    const unsigned released = count == 9 ? 8 : count;
    for (unsigned i = 0; i < released; i++)
        gc::abi::call_guest<void>(release_node_address(), walked[i]);
    if (count == 9) {
        gc::abi::call_guest<void>(release_last_walked_address(), walked[8]);
        gc::abi::call<&free_chain_wrong_order>(ptr<ChainNode>(walked[8]->next));
    }
}

struct HandlerTable {
    gc::array<be<u8>, 0x18> unknown_00;
    be<u32> handler;
};
struct HandlerContext {
    gc::array<be<u8>, 0x190> unknown;
};
struct HandlerOwner {
    gc::array<be<u8>, 0x70> unknown_00;
    ptr<be<u32>> value_source;
    gc::array<be<u8>, 0x90C> unknown_74;
    HandlerContext context;
    ptr<HandlerTable> handlers;
};

u32 run_handler_wrong_result(ptr<HandlerOwner> owner);

u32 is_ready_address() {
    return callee_address(&run_handler_wrong_result, 0x80092D34u, 0x80094DE0u, 0x80091C80u, 0x80093D2Cu);
}

u32 run_handler_wrong_result(ptr<HandlerOwner> owner) {
    if (gc::abi::call_guest<u32>(is_ready_address(), owner) == 0)
        return 1; // wrong on purpose
    const u32 value = gc::load(*ptr<be<u32>>(owner->value_source));
    const ptr<HandlerTable> handlers = owner->handlers;
    gc::abi::call_address<void>(gc::load(handlers->handler), gc::address_of(owner->context), value);
    return 0; // wrong on purpose
}

} // namespace oracle_negative_nonleaf

GC_DECOMP_EXPORT(oracle_test_clear_record__wrong, oracle_negative_nonleaf::clear_record_wrong_missing_word,
                 gc::abi::flag_none);
GC_DECOMP_EXPORT(oracle_test_clear_record_and_tail__wrong, oracle_negative_nonleaf::clear_record_and_tail_wrong,
                 gc::abi::flag_none);
GC_DECOMP_EXPORT(oracle_test_free_chain__wrong, oracle_negative_nonleaf::free_chain_wrong_order, gc::abi::flag_none);
GC_DECOMP_EXPORT(oracle_test_run_handler_if_ready__wrong, oracle_negative_nonleaf::run_handler_wrong_result,
                 gc::abi::flag_none);
