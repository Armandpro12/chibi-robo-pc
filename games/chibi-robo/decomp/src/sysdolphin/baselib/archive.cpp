// HSD archives (sysdolphin): a file loaded in memory with its tables of public and external symbols.
//
// Original unit: sysdolphin/baselib/archive.c, the unit of that name in cbr_decomp's sources
// (https://github.com/eavpsp/cbr_decomp, folder decomp_dtk_main/src, CC0 1.0); cbr_decomp's split does not place it
// at this address. The function that follows this one (read in the US version; identical code in PAL) is, instruction for instruction,
// cbr_decomp's HSD_ArchiveGetPublicAddress (same fields, same loop). The names HSD_Archive, HSD_ArchiveHeader,
// HSD_ArchivePublicInfo, HSD_ArchiveExternInfo, their fields and the layout come from cbr_decomp's archive.h; only
// names and layout are taken (doldecomp/melee's sysdolphin uses the same names: names only, see the project rules).
// The function below is not in cbr_decomp: it was written from the disassembly of both versions (identical code in
// US and PAL). It is the public-symbol counterpart of cbr_decomp's HSD_ArchiveGetExtern; its name is a hypothesis
// given by that symmetry (notes of the decompilation stage).
#include "decomp/gc_abi.h"
#include "decomp/gc_mem.h"

#include <cstddef>

namespace hsd {

using gc::array;
using gc::be;
using gc::ptr;
using gc::s32;
using gc::u32;
using gc::u8;

struct HSD_ArchiveHeader {
    be<u32> file_size;
    be<u32> data_size;
    be<u32> nb_reloc;
    be<u32> nb_public;
    be<u32> nb_extern;
    array<be<u8>, 4> version;
    array<be<u32>, 2> pad;
};

struct HSD_ArchiveRelocationInfo {
    be<u32> offset;
};

// One public symbol: where its data is (offset in `data`) and where its name is (offset in `symbols`).
struct HSD_ArchivePublicInfo {
    be<u32> offset;
    be<u32> symbol;
};

struct HSD_ArchiveExternInfo {
    be<u32> offset;
    be<u32> symbol;
};

struct HSD_Archive {
    HSD_ArchiveHeader header;
    ptr<be<u8>> data;
    ptr<HSD_ArchiveRelocationInfo> reloc_info;
    ptr<HSD_ArchivePublicInfo> public_info;
    ptr<HSD_ArchiveExternInfo> extern_info;
    ptr<char> symbols; // names of the symbols, one after the other
    ptr<HSD_Archive> next;
    ptr<char> name;
    be<u32> flags;
    ptr<void> top_ptr;
};
#if !defined(GC_NATIVE_MEMORY)
static_assert(sizeof(HSD_ArchiveHeader) == 0x20);
static_assert(offsetof(HSD_Archive, header) + offsetof(HSD_ArchiveHeader, nb_public) == 0x0C);
static_assert(offsetof(HSD_Archive, data) == 0x20);
static_assert(offsetof(HSD_Archive, public_info) == 0x28);
static_assert(offsetof(HSD_Archive, symbols) == 0x30);
static_assert(sizeof(HSD_Archive) == 0x44);
static_assert(sizeof(HSD_ArchivePublicInfo) == 8);
#endif

// Name of the public symbol number `index`, or null if there is no such symbol.
ptr<char> HSD_ArchiveGetPublic(ptr<HSD_Archive> archive, s32 index) {
    // A negative index is refused before the unsigned comparison with the count (both versions test both).
    if (index < 0 || gc::load(archive->header.nb_public) <= static_cast<u32>(index))
        return nullptr;
    return archive->symbols + static_cast<std::ptrdiff_t>(gc::load(archive->public_info[index].symbol));
}

} // namespace hsd

GC_DECOMP_EXPORT(hsd_archive_get_public, hsd::HSD_ArchiveGetPublic, gc::abi::flag_reads_only);
