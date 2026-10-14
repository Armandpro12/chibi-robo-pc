/* Loading of a DOL executable (GameCube/Wii format) into guest RAM. */
#include "boot/gcrt_boot.h"

#include <string.h>

enum {
    DOL_TEXT_COUNT = 7,
    DOL_DATA_COUNT = 11,
    DOL_SECTION_COUNT = DOL_TEXT_COUNT + DOL_DATA_COUNT,
    DOL_HEADER_SIZE = 0x100,
    /* Header tables: file offsets (0x00), addresses (0x48), sizes (0x90); text then data. */
    DOL_OFFSETS = 0x00,
    DOL_ADDRESSES = 0x48,
    DOL_SIZES = 0x90,
    DOL_BSS_ADDRESS = 0xD8,
    DOL_BSS_SIZE = 0xDC,
    DOL_ENTRY = 0xE0,
};

u64 gcrt_fnv1a64(const u8* data, size_t size) {
    u64 h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; i++) {
        h ^= data[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

/* Host pointer for [address, address + size) in guest RAM (0x8xxxxxxx area), or NULL. */
static u8* guest_range(CPUState* cpu, u32 address, u32 size) {
    if (address < GC_RAM_BASE)
        return NULL;
    u32 offset = address - GC_RAM_BASE;
    if (offset > cpu->ram_size || size > cpu->ram_size - offset)
        return NULL;
    return cpu->ram + offset;
}

bool gcrt_dol_load(CPUState* cpu, const u8* data, size_t size, GcrtDolInfo* info) {
    if (size < DOL_HEADER_SIZE) {
        fprintf(stderr, "gcrt: DOL too small (%zu bytes)\n", size);
        return false;
    }
    memset(info, 0, sizeof(*info));
    info->entry_point = read_be32(data + DOL_ENTRY);
    info->bss_address = read_be32(data + DOL_BSS_ADDRESS);
    info->bss_size = read_be32(data + DOL_BSS_SIZE);
    info->lowest = 0xFFFFFFFFu;

    /* The declared BSS encloses data sections (.sdata, .sdata2): it is zeroed first,
     * then the sections are loaded over it. */
    if (info->bss_size) {
        u8* bss = guest_range(cpu, info->bss_address, info->bss_size);
        if (!bss) {
            fprintf(stderr, "gcrt: DOL BSS outside RAM (%08X + %X)\n", info->bss_address, info->bss_size);
            return false;
        }
        memset(bss, 0, info->bss_size);
        info->lowest = info->bss_address;
        info->highest = info->bss_address + info->bss_size;
    }

    for (u32 i = 0; i < DOL_SECTION_COUNT; i++) {
        u32 offset = read_be32(data + DOL_OFFSETS + i * 4u);
        u32 address = read_be32(data + DOL_ADDRESSES + i * 4u);
        u32 length = read_be32(data + DOL_SIZES + i * 4u);
        if (length == 0)
            continue;
        if (offset > size || length > size - offset) {
            fprintf(stderr, "gcrt: DOL section %u outside the file\n", i);
            return false;
        }
        u8* dest = guest_range(cpu, address, length);
        if (!dest) {
            fprintf(stderr, "gcrt: DOL section %u outside RAM (%08X + %X)\n", i, address, length);
            return false;
        }
        memcpy(dest, data + offset, length);
        if (i < DOL_TEXT_COUNT) {
            info->text_address[i] = address;
            info->text_size[i] = length;
        }
        if (address < info->lowest)
            info->lowest = address;
        if (address + length > info->highest)
            info->highest = address + length;
    }
    return true;
}
