// Entry of a table of 12-byte entries chosen by a 16-bit index read from memory.
//
// Original unit: not known yet (no split of cbr_decomp covers this function); the file is named after what it holds.
// The function is reached only through a function pointer (one call site, in a dispatch loop of the same area of the
// code). Names are hypotheses (notes of the decompilation stage): the code was read from the disassembly of both
// versions, identical in US and PAL.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

namespace chibi {

using gc::be;
using gc::ptr;
using gc::u16;
using gc::u8;

// An entry of the table: its 12 bytes are described by the units that use them.
struct TableEntry {
    gc::array<be<u8>, 12> not_described_here;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(TableEntry) == 12);
#endif

// &table[*index] (the index unsigned; the address wraps modulo 2^32 as the original's add).
ptr<TableEntry> table_entry_at(ptr<TableEntry> table, ptr<const be<u16>> index) {
    return table + static_cast<std::ptrdiff_t>(gc::load(*index));
}

} // namespace chibi

GC_DECOMP_EXPORT(table_entry_at, chibi::table_entry_at, gc::abi::flag_reads_only);
