// HSD identifier table (sysdolphin): data registered under a 32-bit identifier, in a hash table of lists.
//
// Original unit: sysdolphin/baselib/id.c [hypothesis: name and unit from doldecomp/melee (names only), symbol
// supplement]. The code was written from the disassembly of both versions (identical in US and PAL, the address of the
// global aside). Cross-check [hypothesis on the identity]: it has the logic and the fields of HSD_IDGetDataFromTable in
// cbr_decomp's id.c (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0), whose id.h gives the
// names and the layout of IDEntry and HSD_IDTable (101 lists, hash = identifier modulo 101: the original's 0x65) and
// whose id.c names the table used when none is given, default_table.
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"
#include "decomp/gc_path.h"
#include "decomp_globals.h"

#include <cstddef>

// Cost by path (runtime/core/decomp/gc_path.h): gc::path::taken / falls_through mark, in the original's order, the
// outcome of each of its conditional branches (identical in US and PAL), so that each call is charged the cycles of the
// original's path; they return their condition unchanged.
namespace hsd {

using gc::be;
using gc::ptr;
using gc::s32;
using gc::u32;

struct IDEntry {
    ptr<IDEntry> next;
    be<u32> id;
    ptr<void> data;
};

inline constexpr u32 id_table_lists = 101;
struct HSD_IDTable {
    gc::array<ptr<IDEntry>, id_table_lists> table;
};

#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(IDEntry) == 0xC && offsetof(IDEntry, data) == 8);
static_assert(sizeof(HSD_IDTable) == 101 * 4);
#endif

constexpr gc::global<HSD_IDTable, global_id::hsd_id_default_table> default_table{};

// The data registered under `id` in `table` (the default table when `table` is null), null if none. When `success` is
// not null, `*success` is set to 1 if the identifier was found, 0 otherwise.
ptr<void> HSD_IDGetDataFromTable(ptr<HSD_IDTable> table, u32 id, ptr<be<s32>> success) {
    if (gc::path::falls_through(table == nullptr))
        table = default_table.address();
    // The original tests the entry at the bottom of its loop, before each turn.
    for (ptr<IDEntry> entry = table->table[id % id_table_lists]; gc::path::taken(entry != nullptr);
         entry = entry->next) {
        if (gc::path::taken(gc::load(entry->id) != id))
            continue;
        if (gc::path::falls_through(success != nullptr))
            *success = 1;
        return entry->data;
    }
    if (gc::path::falls_through(success != nullptr))
        *success = 0;
    return nullptr;
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_id_get_data_from_table, hsd::HSD_IDGetDataFromTable, gc::abi::flag_path_marked);
