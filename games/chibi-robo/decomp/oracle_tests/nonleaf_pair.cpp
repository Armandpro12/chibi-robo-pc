// Oracle test functions (oracle builds only: tools/build.py --oracle compiles games/<game>/decomp/oracle_tests; their
// rows of decomp/functions.tsv stay `disabled` and are built only by --decomp-enable). They exercise the comparison of
// a function that calls another (runtime/core/decomp/gc_oracle.cpp, "Calls") on two real functions of the game, a
// caller and its only callee, without floating point, indirect call or global: the callee is a leaf, the caller a
// non-leaf whose call the oracle records, runs once and replays. Not counted as decompiled progress (disabled rows).
//
// The code was read from the disassembly of both versions (identical code, symbols/GGTE01-to-GGTP01.tsv `unique`):
// the callee stores zero in 28 words of a structure, the last one through a one-iteration loop (CTR = 1) the
// compiler left; the caller saves its argument, calls the callee, then stores zero at offset 0x80. What the structure
// is, and so every name here, is not known [hypothesis].
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace chibi::oracle_test {

using gc::be;
using gc::u32;
using gc::u8;

// Words named after their offset: their role is not known.
struct ClearedRecord {
    be<u32> word_00;
    be<u32> word_04;
    gc::array<be<u8>, 4> untouched_08;
    be<u32> word_0c;
    gc::array<be<u8>, 8> untouched_10;
    gc::array<be<u32>, 25> words_18; // 0x18..0x78
    gc::array<be<u8>, 4> untouched_7c;
    be<u32> word_80; // written by the caller only
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(offsetof(ClearedRecord, word_0c) == 0xC && offsetof(ClearedRecord, words_18) == 0x18 &&
              offsetof(ClearedRecord, untouched_7c) == 0x7C && offsetof(ClearedRecord, word_80) == 0x80);
#endif

// Callee (leaf): zero into the words 0x00, 0x04, 0x0C, then 0x18..0x74 in order, then 0x78 (the loop's single
// iteration). Order kept from the original.
void clear_record(gc::ptr<ClearedRecord> record) {
    record->word_00 = 0u;
    record->word_04 = 0u;
    record->word_0c = 0u;
    for (unsigned i = 0; i < 24; i++)
        record->words_18[i] = 0u;
    record->words_18[24] = 0u;
}

// Caller (non-leaf): clears the record through its callee, then the word at 0x80.
void clear_record_and_tail(gc::ptr<ClearedRecord> record) {
    gc::abi::call<&clear_record>(record);
    record->word_80 = 0u;
}

} // namespace chibi::oracle_test

GC_DECOMP_EXPORT(oracle_test_clear_record, chibi::oracle_test::clear_record, gc::abi::flag_none);
GC_DECOMP_EXPORT(oracle_test_clear_record_and_tail, chibi::oracle_test::clear_record_and_tail, gc::abi::flag_none);
