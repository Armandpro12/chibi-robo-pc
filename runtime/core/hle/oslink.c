/* REL modules: replacement of OSLink / OSUnlink and diagnostic test.
 *
 * Problem: DolRecomp compiles each REL for a fixed address (base + position in the file, BSS at
 * align(base + fixSize)), whereas the game loads it into a buffer from its heap, at an arbitrary address, then
 * calls OSLink(module, bss), which relocates it in place. The recompiled code, for its part, references the fixed
 * addresses.
 *
 * Solution (see notes/hle.md and notes/decisions.md): on entry to OSLink, the host
 *   1. checks that the loaded module is indeed the recompiled file (CRC-32);
 *   2. copies the image of the file to its fixed address; the end of the file (imports and relocations, beyond
 *      fixSize) is stored after the BSS, because DolRecomp places the BSS over it and OSUnlink will need it;
 *      the corresponding offsets of the header and of the imports are corrected;
 *   3. lets the original OSLink (recompiled SDK code) run on the fixed copy, with the fixed BSS: it is the one
 *      that maintains the OS module list, converts the header, applies the relocations, zeroes the BSS and
 *      calls OSNotifyLink, exactly as on the console;
 *   4. on return, copies the linked header back into the game's buffer: the game then reads prolog/epilog/sections
 *      through its own pointer and gets the fixed addresses.
 * OSUnlink(game buffer) is redirected to the fixed copy in the same way.
 * No game address here: the locations come from gcrt_build_info (computed at build time). */
#include "hle/gcrt_hle.h"

#include "boot/gcrt_boot.h"

#include <stdlib.h>
#include <string.h>

enum {
    H_ID = 0x00,
    H_NEXT = 0x04,
    H_NUM_SECTIONS = 0x0C,
    H_SECTION_INFO = 0x10,
    H_VERSION = 0x1C,
    H_BSS_SIZE = 0x20,
    H_REL_OFFSET = 0x24,
    H_IMP_OFFSET = 0x28,
    H_IMP_SIZE = 0x2C,
    H_PROLOG_SECTION = 0x30,
    H_PROLOG = 0x34,
    H_ALIGN = 0x40,
    H_HEADER_SIZE_V3 = 0x4C,
    OS_MODULE_LIST_HEAD = 0x800030C8, /* low OS global: list of linked modules (head, tail) */
};

#define MAX_RELS 64

typedef struct {
    bool in_use;
    u32 heap; /* game buffer */
} Slot;

static Slot s_slots[MAX_RELS];

typedef struct {
    u32 heap;
    u32 index;
} LinkCall;

static bool guest_range(GcrtCpu* g, u32 address, u32 size) {
    CPUState* cpu = &g->state;
    if (address < GC_RAM_BASE)
        return false;
    u32 offset = address - GC_RAM_BASE;
    return offset <= cpu->ram_size && size <= cpu->ram_size - offset;
}

static u8* host(GcrtCpu* g, u32 address) {
    return g->state.ram + (address - GC_RAM_BASE);
}

static const GcrtRelLayout* layout_for_id(u32 id, u32* index) {
    for (u32 i = 0; i < gcrt_build_info.rel_count && i < MAX_RELS; i++) {
        if (gcrt_build_info.rels[i].id == id) {
            if (index)
                *index = i;
            return &gcrt_build_info.rels[i];
        }
    }
    return NULL;
}

static bool tail_moved(const GcrtRelLayout* L) {
    return L->tail_addr != L->base + L->fix_size;
}

/* Copies the file `src` (in guest RAM) to the fixed address of the module and stores its end after the BSS. */
static void place_module(GcrtCpu* g, const GcrtRelLayout* L, u32 src) {
    memmove(host(g, L->base), host(g, src), L->file_size);
    if (!tail_moved(L))
        return;
    u32 tail_size = L->file_size - L->fix_size;
    memmove(host(g, L->tail_addr), host(g, src + L->fix_size), tail_size);
    u32 delta = L->tail_addr - (L->base + L->fix_size);
    CPUState* cpu = &g->state;
    /* Offsets (still relative to the start of the file, before linking) that point into the moved part. */
    u32 fields[2] = {H_REL_OFFSET, H_IMP_OFFSET};
    for (int i = 0; i < 2; i++) {
        u32 v = mem_read32(cpu, L->base + fields[i]);
        if (v >= L->fix_size)
            mem_write32(cpu, L->base + fields[i], v + delta);
    }
    u32 imp = mem_read32(cpu, L->base + H_IMP_OFFSET);
    u32 imp_size = mem_read32(cpu, L->base + H_IMP_SIZE);
    for (u32 o = 0; o + 8u <= imp_size; o += 8u) {
        u32 at = L->base + imp + o + 4u;
        u32 v = mem_read32(cpu, at);
        if (v >= L->fix_size)
            mem_write32(cpu, at, v + delta);
    }
}

static void scan_linked_module(GcrtCpu* g, const GcrtRelLayout* L) {
    CPUState* cpu = &g->state;
    u32 count = mem_read32(cpu, L->base + H_NUM_SECTIONS);
    u32 info = mem_read32(cpu, L->base + H_SECTION_INFO); /* absolute after linking */
    for (u32 i = 0; i < count && i < 64; i++) {
        u32 raw = mem_read32(cpu, info + 8u * i);
        u32 size = mem_read32(cpu, info + 8u * i + 4u);
        if ((raw & 1u) && size)
            gcrt_hle_scan(g, raw & ~1u, size, L->name);
    }
}

static void link_done(GcrtCpu* g, u32 original_return, void* data) {
    CPUState* cpu = &g->state;
    LinkCall call;
    memcpy(&call, data, sizeof(call));
    const GcrtRelLayout* L = &gcrt_build_info.rels[call.index];
    if (cpu->gpr[3]) {
        /* The game reads prolog, epilog, the section table... from its buffer: the linked header is copied there. */
        memmove(host(g, call.heap), host(g, L->base), H_HEADER_SIZE_V3);
        s_slots[call.index].in_use = true;
        s_slots[call.index].heap = call.heap;
        scan_linked_module(g, L);
        if (gcrt_hle_trace_enabled())
            fprintf(stderr, "gcrt hle: OSLink module %u (%s): buffer %08X -> %08X, BSS %08X, prolog %08X\n", L->id,
                    L->name, call.heap, L->base, L->bss_addr, mem_read32(cpu, L->base + H_PROLOG));
    } else {
        fprintf(stderr, "gcrt hle: OSLink of module %u (%s) failed\n", L->id, L->name);
    }
    cpu->lr = original_return;
    cpu->pc = original_return;
}

static bool fail_link(GcrtCpu* g) {
    g->state.gpr[3] = 0; /* OSLink returns FALSE */
    g->state.pc = g->state.lr;
    return true;
}

bool gcrt_hle_oslink(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* cpu = &g->state;
    u32 module = cpu->gpr[3];
    u32 bss = cpu->gpr[4];
    if (!guest_range(g, module, H_HEADER_SIZE_V3))
        return false;
    u32 index;
    const GcrtRelLayout* L = layout_for_id(mem_read32(cpu, module + H_ID), &index);
    if (!L) {
        fprintf(stderr, "gcrt hle: OSLink of module %u, which was not recompiled: original linking "
                        "(its code will not be able to run)\n", mem_read32(cpu, module + H_ID));
        return false;
    }
    if (module == L->base)
        return false; /* already at its fixed address */
    if (!guest_range(g, module, L->file_size)) {
        fprintf(stderr, "gcrt hle: OSLink: module %u outside RAM (%08X)\n", L->id, module);
        return fail_link(g);
    }
    u32 crc = gcrt_hle_crc32(host(g, module), L->file_size);
    if (crc != L->crc32) {
        fprintf(stderr, "gcrt hle: OSLink: module %u (%s) loaded at %08X is not the recompiled file "
                        "(CRC-32 %08X instead of %08X)\n", L->id, L->name, module, crc, L->crc32);
        g->stop = GCRT_STOP_REQUESTED;
        return fail_link(g);
    }
    if (s_slots[index].in_use) {
        fprintf(stderr, "gcrt hle: OSLink: module %u (%s) is already linked (buffer %08X); with fixed addresses, a "
                        "module can only be linked once at a time\n", L->id, L->name, s_slots[index].heap);
        return fail_link(g);
    }
    place_module(g, L, module);
    LinkCall call = {module, index};
    cpu->gpr[3] = L->base;
    cpu->gpr[4] = L->bss_size ? L->bss_addr : bss;
    cpu->lr = gcrt_hle_push_return(g, cpu->lr, link_done, &call, sizeof(call));
    return false; /* the original OSLink runs on the fixed copy */
}

static void unlink_done(GcrtCpu* g, u32 original_return, void* data) {
    CPUState* cpu = &g->state;
    u32 index;
    memcpy(&index, data, sizeof(index));
    if (cpu->gpr[3])
        s_slots[index].in_use = false;
    else
        fprintf(stderr, "gcrt hle: OSUnlink of module %s failed\n", gcrt_build_info.rels[index].name);
    if (gcrt_hle_trace_enabled())
        fprintf(stderr, "gcrt hle: OSUnlink module %s -> %u\n", gcrt_build_info.rels[index].name, cpu->gpr[3]);
    cpu->lr = original_return;
    cpu->pc = original_return;
}

bool gcrt_hle_osunlink(GcrtCpu* g, u32 address) {
    (void)address;
    CPUState* cpu = &g->state;
    u32 module = cpu->gpr[3];
    for (u32 i = 0; i < gcrt_build_info.rel_count && i < MAX_RELS; i++) {
        const GcrtRelLayout* L = &gcrt_build_info.rels[i];
        if (s_slots[i].in_use && (s_slots[i].heap == module || L->base == module)) {
            cpu->gpr[3] = L->base;
            cpu->lr = gcrt_hle_push_return(g, cpu->lr, unlink_done, &i, sizeof(i));
            return false;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------------------------
 * Diagnostic test: linking an extracted REL, as a game would
 * ------------------------------------------------------------------------------------------- */

/* Independent relocation (without the game's code) of the file `image`, placed at the fixed address of L: serves as
 * the reference to check the result of OSLink. Relocation types: SDK REL format (R_PPC_* of the PowerPC ELF ABI,
 * plus R_DOLPHIN_NOP/SECTION/END). Only the imports of the module itself and of the DOL (module 0) are applied, like
 * OSLink when no other module is linked. */
static bool relocate_reference(u8* image, u32 size, const GcrtRelLayout* L, char* error, size_t error_size) {
    u32 count = read_be32(image + H_NUM_SECTIONS);
    u32 info = read_be32(image + H_SECTION_INFO);
    u32 imp = read_be32(image + H_IMP_OFFSET);
    u32 imp_size = read_be32(image + H_IMP_SIZE);
    u32 id = read_be32(image + H_ID);
    if (count > 64 || info + 8u * count > size || imp + imp_size > size) {
        snprintf(error, error_size, "inconsistent header");
        return false;
    }
    u32 sec_addr[64], sec_off[64], sec_size[64];
    for (u32 i = 0; i < count; i++) {
        u32 raw = read_be32(image + info + 8u * i);
        sec_size[i] = read_be32(image + info + 8u * i + 4u);
        sec_off[i] = raw & ~1u;
        sec_addr[i] = sec_size[i] ? (sec_off[i] ? L->base + sec_off[i] : L->bss_addr) : 0;
    }
    for (u32 o = 0; o + 8u <= imp_size; o += 8u) {
        u32 module = read_be32(image + imp + o);
        u32 cursor = read_be32(image + imp + o + 4u);
        if (module != 0 && module != id)
            continue;
        u32 section = 0, offset = 0;
        for (; cursor + 8u <= size; cursor += 8u) {
            const u8* e = image + cursor;
            u16 delta = read_be16(e);
            u8 type = e[2], sec = e[3];
            u32 addend = read_be32(e + 4);
            if (type == 202) { /* R_DOLPHIN_SECTION */
                section = sec;
                offset = 0;
                continue;
            }
            if (type == 203) /* R_DOLPHIN_END */
                break;
            offset += delta;
            if (type == 201 || type == 0) /* R_DOLPHIN_NOP, R_PPC_NONE */
                continue;
            u32 patch_size = (type >= 3 && type <= 6) ? 2u : 4u;
            if (section >= count || !sec_off[section] || offset + patch_size > sec_size[section]) {
                snprintf(error, error_size, "relocation outside its section (%u+%X)", section, offset);
                return false;
            }
            u32 target = module == 0 ? addend : (sec < count ? sec_addr[sec] : 0) + addend;
            u32 where = sec_addr[section] + offset;
            u8* p = image + sec_off[section] + offset;
            switch (type) {
            case 1: write_be32(p, target); break;                                                        /* ADDR32 */
            case 2: write_be32(p, (read_be32(p) & 0xFC000003u) | (target & 0x03FFFFFCu)); break;          /* ADDR24 */
            case 3: case 4: write_be16(p, (u16)target); break;                                           /* ADDR16, _LO */
            case 5: write_be16(p, (u16)(target >> 16)); break;                                            /* ADDR16_HI */
            case 6: write_be16(p, (u16)((target + 0x8000u) >> 16)); break;                                /* ADDR16_HA */
            case 7: case 8: case 9: write_be32(p, (read_be32(p) & 0xFFFF0003u) | (target & 0xFFFCu)); break; /* ADDR14 */
            case 10: write_be32(p, (read_be32(p) & 0xFC000003u) | ((target - where) & 0x03FFFFFCu)); break; /* REL24 */
            case 11: write_be32(p, (read_be32(p) & 0xFFFF0003u) | ((target - where) & 0xFFFCu)); break;   /* REL14 */
            default:
                snprintf(error, error_size, "unsupported relocation type %u", type);
                return false;
            }
        }
    }
    return true;
}

static int check(bool ok, const char* what, int* failures) {
    printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
    if (!ok)
        (*failures)++;
    return ok;
}

static bool module_in_os_list(GcrtCpu* g, u32 module) {
    CPUState* cpu = &g->state;
    u32 m = mem_read32(cpu, OS_MODULE_LIST_HEAD);
    for (int i = 0; i < 256 && m; i++) {
        if (m == module)
            return true;
        if (!guest_range(g, m, 8))
            return false;
        m = mem_read32(cpu, m + H_NEXT);
    }
    return false;
}

int gcrt_hle_rel_selftest(GcrtCpu* g, const char* path) {
    CPUState* cpu = &g->state;
    char what[256], error[128];
    int failures = 0;
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "gcrt: REL test: file not found %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* file = (u8*)malloc((size_t)(size > 0 ? size : 1));
    bool read_ok = file && size >= H_HEADER_SIZE_V3 && fread(file, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!read_ok) {
        fprintf(stderr, "gcrt: REL test: cannot read %s\n", path);
        free(file);
        return 1;
    }
    u32 index;
    const GcrtRelLayout* L = layout_for_id(read_be32(file + H_ID), &index);
    printf("REL test: %s (module %u)\n", path, read_be32(file + H_ID));
    if (!check(L != NULL, "module present among the recompiled RELs", &failures) ||
        !check((u32)size == L->file_size && gcrt_hle_crc32(file, (size_t)size) == L->crc32,
               "file identical to the one that was recompiled (size, CRC-32)", &failures)) {
        free(file);
        return 1;
    }
    unsigned n_link = 0, n_unlink = 0;
    u32 link = gcrt_hle_find("OSLink", &n_link), unlink = gcrt_hle_find("OSUnlink", &n_unlink);
    snprintf(what, sizeof(what), "OSLink (%08X) and OSUnlink (%08X) recognized by signature", link, unlink);
    if (!check(n_link == 1 && n_unlink == 1, what, &failures)) {
        free(file);
        return 1;
    }

    /* Buffers below the top of the arena, like an allocation from the game's heap. */
    u32 arena_hi = mem_read32(cpu, 0x80000034u);
    if (arena_hi < 0x80800000u || arena_hi > GC_RAM_BASE + GCRT_MEM1_SIZE)
        arena_hi = 0x81700000u;
    u32 align = read_be32(file + H_VERSION) >= 2 ? read_be32(file + H_ALIGN) : 32u;
    if (align < 64u)
        align = 64u;
    u32 bss_buf = (arena_hi - ((L->bss_size + 63u) & ~63u) - 0x100u) & ~63u;
    u32 module_buf = (bss_buf - (((u32)size + 63u) & ~63u) - 0x100u) & ~(align - 1u);
    memcpy(host(g, module_buf), file, (size_t)size);
    memset(host(g, bss_buf), 0xCD, L->bss_size); /* the fixed BSS must be zeroed by OSLink */
    if (L->bss_size)
        memset(host(g, L->bss_addr), 0xCD, L->bss_size);
    printf("  \"game\" buffer %08X, BSS %08X; fixed address %08X, fixed BSS %08X, end of file at %08X\n",
           module_buf, bss_buf, L->base, L->bss_addr, L->tail_addr);

    u32 args[2] = {module_buf, bss_buf}, result = 0;
    bool returned = gcrt_hle_call_guest(g, link, args, 2, 2000000u, &result);
    check(returned && result == 1, "game's OSLink run, returns TRUE", &failures);
    check(module_in_os_list(g, L->base), "fixed copy registered in the OS module list", &failures);
    check(s_slots[index].in_use && s_slots[index].heap == module_buf, "module recorded by the replacement", &failures);

    /* Sections: absolute addresses expected in the linked table. */
    u32 count = read_be32(file + H_NUM_SECTIONS), info = read_be32(file + H_SECTION_INFO);
    bool sections_ok = count <= 64;
    for (u32 i = 0; sections_ok && i < count; i++) {
        u32 raw = read_be32(file + info + 8u * i), sz = read_be32(file + info + 8u * i + 4u);
        if (!sz)
            continue;
        u32 expected = (raw & ~1u) ? L->base + (raw & ~1u) : L->bss_addr;
        u32 actual = mem_read32(cpu, L->base + info + 8u * i) & ~1u;
        if (actual != expected) {
            printf("    section %u: %08X instead of %08X\n", i, actual, expected);
            sections_ok = false;
        }
    }
    check(sections_ok, "section addresses = addresses assumed by DolRecomp (BSS included)", &failures);

    u8 psec = file[H_PROLOG_SECTION];
    u32 prolog_expected = psec && psec < count
                              ? L->base + (read_be32(file + info + 8u * psec) & ~1u) + read_be32(file + H_PROLOG) : 0;
    u32 prolog = mem_read32(cpu, L->base + H_PROLOG);
    snprintf(what, sizeof(what), "absolute prolog %08X (expected %08X), present in the recompiled code", prolog,
             prolog_expected);
    check(prolog == prolog_expected && gcrt_cpu_lookup(g, prolog, NULL) != NULL, what, &failures);
    check(mem_read32(cpu, module_buf + H_PROLOG) == prolog, "linked header copied back into the game's buffer",
          &failures);

    /* Relocations: comparison with an independent relocation of the file. */
    u8* reference = (u8*)malloc((size_t)size);
    memcpy(reference, file, (size_t)size);
    bool ref_ok = relocate_reference(reference, (u32)size, L, error, sizeof(error));
    u32 differences = 0, first_diff = 0;
    for (u32 i = 0; ref_ok && i < count; i++) {
        u32 raw = read_be32(file + info + 8u * i), sz = read_be32(file + info + 8u * i + 4u);
        if (!sz || !(raw & ~1u))
            continue;
        const u8* expected = reference + (raw & ~1u);
        const u8* actual = host(g, L->base + (raw & ~1u));
        for (u32 b = 0; b < sz; b++) {
            if (expected[b] != actual[b]) {
                if (!differences)
                    first_diff = L->base + (raw & ~1u) + b;
                differences++;
            }
        }
    }
    if (!ref_ok)
        snprintf(what, sizeof(what), "reference relocation impossible: %s", error);
    else if (differences)
        snprintf(what, sizeof(what), "relocated sections identical to the reference (%u bytes differ, from %08X)",
                 differences, first_diff);
    else
        snprintf(what, sizeof(what), "relocated sections identical to the reference relocation (code and data)");
    check(ref_ok && !differences, what, &failures);
    free(reference);

    bool bss_zero = true;
    for (u32 i = 0; i < L->bss_size; i++)
        bss_zero &= host(g, L->bss_addr)[i] == 0;
    check(bss_zero, "fixed BSS zeroed", &failures);

    u32 unlink_args[1] = {module_buf};
    returned = gcrt_hle_call_guest(g, unlink, unlink_args, 1, 2000000u, &result);
    check(returned && result == 1, "game's OSUnlink (on the game's buffer) returns TRUE", &failures);
    check(!module_in_os_list(g, L->base) && !s_slots[index].in_use, "module removed from the list and freed",
          &failures);

    printf("REL test: %s\n", failures ? "FAIL" : "passed");
    free(file);
    return failures ? 1 : 0;
}
