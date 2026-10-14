/* Boot of the generic GameCube runtime: DOL loading, HLE boot, entry point.
 *
 * A game's executable provides:
 *   - `main()` (in games/<game>/), which calls gcrt_main() with the game description;
 *   - `gcrt_recompiled_modules` and `gcrt_build_info`, produced by CMake (cpu/module_registry.c.in).
 */
#ifndef GCRT_BOOT_H
#define GCRT_BOOT_H

#include "cpu/gcrt_cpu.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed slot of a recompiled REL module (computed by tools/build.py, same rules as DolRecomp).
 * The module's code was compiled for these addresses; the OSLink replacement copies the module loaded
 * by the game there (see hle/oslink.c). */
typedef struct GcrtRelLayout {
    u32 id;        /* module number (header +0x00) */
    u32 base;      /* address of the header and sections (base + offset in the file) */
    u32 file_size;
    u32 fix_size;  /* header +0x48: start of the import and relocation table */
    u32 bss_addr;  /* BSS address assumed by DolRecomp (0 if no BSS) */
    u32 bss_size;
    u32 tail_addr; /* where to store [fix_size, file_size) when the BSS covers it (= base + fix_size otherwise) */
    u32 slot_end;  /* end of the slot reserved for the module */
    u32 crc32;     /* CRC-32 of the recompiled .rel file */
    const char* name;
} GcrtRelLayout;

/* Parameters fixed at build time (tools/build.py -> CMake). */
typedef struct GcrtBuildInfo {
    const char* game;      /* folder games/<game> */
    const char* version;   /* disc identifier (6 characters: game, region, publisher) */
    const char* disc_root; /* root of the extracted disc (contains sys/main.dol) */
    u64 dol_fnv64;         /* FNV-1a 64 hash of the recompiled DOL (0: no check) */
    u32 ram_size;          /* size of the host RAM (>= 24 MiB, covers the virtual area of the RELs) */
    const GcrtRelLayout* rels; /* recompiled REL modules */
    unsigned rel_count;
    const u32* hle_hooks;  /* entry points compiled with an HLE hook (--hle-hooks), sorted */
    unsigned hle_hook_count;
} GcrtBuildInfo;

extern const GcrtBuildInfo gcrt_build_info;
extern const GcrtModuleTable gcrt_recompiled_modules;

/* Game-specific extension points (all optional). */
typedef struct GcrtGameDesc {
    const char* title;
    /* Called after loading the DOL, the HLE boot and the installation of the generic replacements
     * (hle/), before the first instruction: install the game-specific replacements here
     * (gcrt_hle_register, address to be declared in game.toml [hle.hooks] too) and its patches. */
    void (*install)(GcrtCpu* g);
} GcrtGameDesc;

/* DOL header. */
typedef struct GcrtDolInfo {
    u32 entry_point;
    u32 bss_address;
    u32 bss_size;
    u32 lowest;  /* lowest loaded address */
    u32 highest; /* end of the highest section or of the BSS */
    u32 text_address[7]; /* code sections (size 0: absent) */
    u32 text_size[7];
} GcrtDolInfo;

/* Loads the DOL sections into guest RAM (BSS zeroed first). */
bool gcrt_dol_load(CPUState* cpu, const u8* data, size_t size, GcrtDolInfo* info);
u64 gcrt_fnv1a64(const u8* data, size_t size);

/* HLE boot like Dolphin's: processor and low globals prepared as by the IPL, then what the disc's apploader
 * would have set (static boot, without running the apploader). `disc_root`: root of the extracted disc
 * (sys/boot.bin, bi2.bin, fst.bin). */
bool gcrt_boot_hle(GcrtCpu* g, const char* disc_root, const GcrtDolInfo* dol);

int gcrt_main(int argc, char** argv, const GcrtGameDesc* game);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_BOOT_H */
