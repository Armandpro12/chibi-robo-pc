/* Generic HLE: dispatcher of the replacements, scan by signature, "magic" returns.
 * See gcrt_hle.h and notes/hle.md. */
#include "hle/gcrt_hle.h"

#include "boot/gcrt_boot.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------------------
 * Table of replacements (open addressing, guest address -> callback)
 * ------------------------------------------------------------------------------------------- */

#define REPL_SLOTS 1024u /* power of 2 */
typedef struct {
    u32 address; /* 0: free */
    GcrtHleFn fn;
    const char* name;
    u64 calls;
} Replacement;

static Replacement s_repl[REPL_SLOTS];
static unsigned s_repl_count;
static bool s_trace;
static FILE* s_guest_log;

/* The same entry point can be offered twice in a row for the same block: by the chassis before entering
 * the block, then by the hook compiled at the label. A replacement that declined (let the original code
 * run) must not be called again for that same pass. */
static u32 s_declined_pc;
static u64 s_declined_blocks = ~0ull;

static Replacement* repl_slot(u32 address, bool insert) {
    u32 h = ((address >> 2) * 2654435761u) & (REPL_SLOTS - 1u);
    for (u32 i = 0; i < REPL_SLOTS; i++) {
        Replacement* r = &s_repl[(h + i) & (REPL_SLOTS - 1u)];
        if (r->address == address)
            return r;
        if (r->address == 0)
            return insert ? r : NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------
 * "Magic" returns: addresses outside any guest memory, intercepted by the dispatcher
 * ------------------------------------------------------------------------------------------- */

#define RETURN_BASE 0xF0000000u
#define RETURN_SLOTS 64u

typedef struct {
    bool used;
    u32 original_return;
    GcrtHleReturnFn fn;
    u8 data[64];
} ReturnSlot;

static ReturnSlot s_returns[RETURN_SLOTS];

bool gcrt_hle_is_return_address(u32 address) {
    return (address & ~(RETURN_SLOTS * 4u - 1u)) == RETURN_BASE;
}

u32 gcrt_hle_push_return(GcrtCpu* g, u32 original_return, GcrtHleReturnFn fn, const void* data, size_t size) {
    for (u32 k = 0; k < RETURN_SLOTS; k++) {
        if (!s_returns[k].used) {
            s_returns[k].used = true;
            s_returns[k].original_return = original_return;
            s_returns[k].fn = fn;
            memset(s_returns[k].data, 0, sizeof(s_returns[k].data));
            if (data)
                memcpy(s_returns[k].data, data, size < sizeof(s_returns[k].data) ? size : sizeof(s_returns[k].data));
            return RETURN_BASE + 4u * k;
        }
    }
    fprintf(stderr, "gcrt hle: no return slot left (%u in use)\n", RETURN_SLOTS);
    gcrt_cpu_request_stop(g);
    return original_return;
}

static bool handle_return(GcrtCpu* g, u32 address) {
    u32 k = (address - RETURN_BASE) / 4u;
    if (k >= RETURN_SLOTS || !s_returns[k].used) {
        fprintf(stderr, "gcrt hle: return to %08X without a call in progress\n", address);
        gcrt_cpu_request_stop(g);
        return true;
    }
    ReturnSlot slot = s_returns[k];
    s_returns[k].used = false;
    slot.fn(g, slot.original_return, slot.data);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Dispatcher (host_call callback of the chassis and of the compiled hooks)
 * ------------------------------------------------------------------------------------------- */

/* Diagnostic (GCRT_HLE_STACK_FILL=BYTE[:SIZE], hexadecimal byte, SIZE in bytes, default 256): on entry into every
 * replacement, the SIZE bytes of free guest stack just below r1 receive BYTE. The original SDK functions wrote their
 * stack frames there; the native replacements do not, so game code that later reads uninitialized locals in that
 * area sees what earlier code left. Two runs with different bytes show whether such reads change the output (stream
 * and captures). Below r1 is free stack by the PowerPC EABI (no red zone), so the fill never changes a live value. */
static u32 s_stack_fill_word;
static u32 s_stack_fill_size;

static void stack_fill(GcrtCpu* g) {
    const u32 sp = g->state.gpr[1];
    if (sp < s_stack_fill_size + 0x80000000u)
        return;
    for (u32 a = (sp - s_stack_fill_size) & ~3u; a < (sp & ~3u); a += 4)
        mem_write32(&g->state, a, s_stack_fill_word);
}

static bool dispatch(CPUState* cpu, u32 address) {
    GcrtCpu* g = gcrt_from_state(cpu);
    if (gcrt_hle_is_return_address(address))
        return handle_return(g, address);
    if (!s_repl_count)
        return false;
    Replacement* r = repl_slot(address, false);
    if (!r)
        return false;
    if (address == s_declined_pc && g->blocks == s_declined_blocks)
        return false;
    if (s_stack_fill_size)
        stack_fill(g);
    bool done = r->fn(g, address);
    r->calls++;
    if (!done) {
        s_declined_pc = address;
        s_declined_blocks = g->blocks;
    }
    return done;
}

void gcrt_hle_init(GcrtCpu* g, bool trace) {
    memset(s_repl, 0, sizeof(s_repl));
    memset(s_returns, 0, sizeof(s_returns));
    s_repl_count = 0;
    s_trace = trace;
    s_stack_fill_size = 0;
    const char* fill = getenv("GCRT_HLE_STACK_FILL");
    if (fill && fill[0]) {
        char* end = NULL;
        const unsigned long byte = strtoul(fill, &end, 16);
        unsigned long size = 256;
        if (end && *end == ':')
            size = strtoul(end + 1, NULL, 0);
        if (size > 4096)
            size = 4096;
        s_stack_fill_word = (u32)(byte & 0xFFu) * 0x01010101u;
        s_stack_fill_size = (u32)size & ~3u;
        fprintf(stderr, "gcrt hle: GCRT_HLE_STACK_FILL: %u bytes below r1 set to 0x%02X on entry into each replacement\n",
                (unsigned)s_stack_fill_size, (unsigned)(byte & 0xFFu));
    }
    g->host_call = dispatch;       /* chassis */
    g->state.host_call = dispatch; /* compiled hooks (ppc_host_call) */
}

bool gcrt_hle_trace_enabled(void) {
    return s_trace;
}

static bool compiled_with_hook(u32 address) {
    const GcrtBuildInfo* info = &gcrt_build_info;
    u32 lo = 0, hi = info->hle_hook_count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (info->hle_hooks[mid] == address)
            return true;
        if (info->hle_hooks[mid] < address)
            lo = mid + 1u;
        else
            hi = mid;
    }
    return false;
}

bool gcrt_hle_register(GcrtCpu* g, u32 address, const char* name, GcrtHleFn fn) {
    Replacement* r = repl_slot(address, true);
    if (!r) {
        fprintf(stderr, "gcrt hle: replacement table full\n");
        return false;
    }
    if (r->address == 0)
        s_repl_count++;
    r->address = address;
    r->fn = fn;
    r->name = name;
    r->calls = 0;
    if (g)
        gcrt_cpu_mark_host_call(g, address); /* chassis filter (cpu/chassis.c, host_call_filter) */
    /* The "magic" addresses (0xF0000000 and above, outside guest memory) are only reached through the chassis
     * (returns of calls prepared by the host): no compiled hook to expect. */
    if (address < RETURN_BASE && !compiled_with_hook(address))
        fprintf(stderr, "gcrt hle: %s (%08X) was not compiled with a hook: only the calls that go through "
                        "the chassis will be replaced (add the address to game.toml, [hle.hooks])\n",
                name, address);
    if (s_trace)
        fprintf(stderr, "gcrt hle: replacement %s at %08X\n", name, address);
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * Scan by signature (same algorithm as tools/sdksig.py find)
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    u16 sig;
    u32 address;
} Match;

#define MAX_MATCHES 4096u
static Match s_matches[MAX_MATCHES];
static unsigned s_match_count;
static u32 s_crc_table[256];

static void crc_init(void) {
    if (s_crc_table[1])
        return;
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[i] = c;
    }
}

/* CRC-32 (the zlib one) of the masked words of a function: port of tools/symbols.py:keep_masks.
 * Masked: displacement of b/bl; bc whose target leaves the function; immediate of lis (rA = 0, r2, r13
 * or a register loaded by lis); displacement of D-form accesses relative to r2/r13 or to a register loaded
 * by lis; immediate of ori/oris whose source was loaded by lis; 12-bit displacement of psq. */
static u32 masked_crc(const u8* code, u32 start, u32 size) {
    u32 crc = 0xFFFFFFFFu;
    u32 tainted = 0;
    const u32 sda = (1u << 2) | (1u << 13);
    for (u32 i = 0; i < size / 4u; i++) {
        u32 w = read_be32(code + 4u * i);
        u32 op = w >> 26, rd = (w >> 21) & 31u, ra = (w >> 16) & 31u;
        u32 m = 0xFFFFFFFFu;
        if (op == 18) {
            m = 0xFC000003u;
        } else if (op == 16) {
            s32 bd = (s16)(w & 0xFFFCu);
            u32 target = start + 4u * i + (u32)bd;
            if (!(w & 2u) && !(target >= start && target < start + size))
                m = 0xFFFF0003u;
        } else if (op == 15) {
            if (ra == 0 || ((sda | tainted) >> ra) & 1u)
                m = 0xFFFF0000u;
            if (ra == 0)
                tainted |= 1u << rd;
        } else if (op == 14 || (op >= 32 && op <= 55)) {
            if (ra != 0 && (((sda | tainted) >> ra) & 1u))
                m = 0xFFFF0000u;
        } else if (op == 24 || op == 25) {
            if ((tainted >> rd) & 1u)
                m = 0xFFFF0000u;
        } else if (op == 56 || op == 57 || op == 60 || op == 61) {
            if (ra != 0 && (((sda | tainted) >> ra) & 1u))
                m = 0xFFFFF000u;
        }
        u32 v = w & m;
        u8 bytes[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v};
        for (int b = 0; b < 4; b++)
            crc = s_crc_table[(crc ^ bytes[b]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

typedef struct {
    u32 start;
    u32 size;
    const u8* code;
    u8* candidate; /* one byte per word */
} ScanRange;

static ScanRange* range_of(ScanRange* ranges, unsigned count, u32 address) {
    for (unsigned i = 0; i < count; i++)
        if (address >= ranges[i].start && address - ranges[i].start < ranges[i].size)
            return &ranges[i];
    return NULL;
}

static unsigned scan_ranges(GcrtCpu* g, ScanRange* ranges, unsigned count, const char* origin) {
    crc_init();
    CPUState* cpu = &g->state;
    for (unsigned r = 0; r < count; r++) {
        ScanRange* sr = &ranges[r];
        if (sr->start < GC_RAM_BASE || sr->start - GC_RAM_BASE + sr->size > cpu->ram_size) {
            sr->size = 0;
            continue;
        }
        sr->code = cpu->ram + (sr->start - GC_RAM_BASE);
        sr->candidate = (u8*)calloc(sr->size / 4u + 1u, 1);
    }
    /* Candidate starts: non-zero word following a blr, target of a bl within the scanned ranges. */
    for (unsigned r = 0; r < count; r++) {
        ScanRange* sr = &ranges[r];
        if (!sr->candidate)
            continue;
        bool prev_blr = false;
        for (u32 i = 0; i < sr->size / 4u; i++) {
            u32 w = read_be32(sr->code + 4u * i);
            if (prev_blr && w != 0)
                sr->candidate[i] = 1;
            prev_blr = (w == 0x4E800020u);
            if ((w >> 26) == 18u && (w & 3u) == 1u) {
                u32 li = w & 0x03FFFFFCu;
                if (li & 0x02000000u)
                    li |= 0xFC000000u;
                u32 t = sr->start + 4u * i + li;
                ScanRange* tr = range_of(ranges, count, t);
                if (tr && tr->candidate && ((t - tr->start) & 3u) == 0)
                    tr->candidate[(t - tr->start) / 4u] = 1;
            }
        }
    }
    unsigned found = 0;
    for (unsigned r = 0; r < count; r++) {
        ScanRange* sr = &ranges[r];
        if (!sr->candidate)
            continue;
        for (u32 i = 0; i < sr->size / 4u; i++) {
            if (!sr->candidate[i])
                continue;
            u32 w0 = read_be32(sr->code + 4u * i);
            u32 address = sr->start + 4u * i;
            for (unsigned s = 0; s < gcrt_sdk_catalog_count; s++) {
                const GcrtSdkSignature* sig = &gcrt_sdk_catalog[s];
                if ((w0 & sig->key_mask) != sig->key || sig->size > sr->size - 4u * i)
                    continue;
                if (masked_crc(sr->code + 4u * i, address, sig->size) != sig->crc32)
                    continue;
                if (s_match_count < MAX_MATCHES) {
                    s_matches[s_match_count].sig = (u16)s;
                    s_matches[s_match_count].address = address;
                    s_match_count++;
                }
                found++;
                if (s_trace)
                    fprintf(stderr, "gcrt hle: %s: %s at %08X\n", origin, sig->name, address);
            }
        }
        free(sr->candidate);
        sr->candidate = NULL;
    }
    return found;
}

unsigned gcrt_hle_scan(GcrtCpu* g, u32 start, u32 size, const char* origin) {
    ScanRange range = {start, size & ~3u, NULL, NULL};
    return scan_ranges(g, &range, 1, origin);
}

unsigned gcrt_hle_scan_image(GcrtCpu* g, const u32* starts, const u32* sizes, unsigned count, const char* origin) {
    ScanRange ranges[16];
    unsigned n = 0;
    for (unsigned i = 0; i < count && n < 16; i++) {
        if (!sizes[i])
            continue;
        ranges[n].start = starts[i];
        ranges[n].size = sizes[i] & ~3u;
        ranges[n].code = NULL;
        ranges[n].candidate = NULL;
        n++;
    }
    return scan_ranges(g, ranges, n, origin);
}

u32 gcrt_hle_find(const char* name, unsigned* count) {
    u32 first = 0;
    unsigned n = 0;
    for (unsigned i = 0; i < s_match_count; i++) {
        if (strcmp(gcrt_sdk_catalog[s_matches[i].sig].name, name) == 0) {
            if (!n || s_matches[i].address < first)
                first = s_matches[i].address;
            n++;
        }
    }
    if (count)
        *count = n;
    return first;
}

const char* gcrt_hle_name_at(u32 address, u32* start) {
    for (unsigned i = 0; i < s_match_count; i++) {
        const GcrtSdkSignature* sig = &gcrt_sdk_catalog[s_matches[i].sig];
        if (address >= s_matches[i].address && address - s_matches[i].address < sig->size) {
            if (start)
                *start = s_matches[i].address;
            return sig->name;
        }
    }
    return NULL;
}

unsigned gcrt_hle_find_all(const char* name, u32* out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < s_match_count; i++) {
        if (strcmp(gcrt_sdk_catalog[s_matches[i].sig].name, name) == 0) {
            if (n < max)
                out[n] = s_matches[i].address;
            n++;
        }
    }
    return n;
}

/* Does the name belong to library `prefix` ("DVD": DVDRead..., __DVDInterruptHandler...)? */
static bool in_library(const char* name, const char* prefix) {
    size_t len = strlen(prefix);
    while (*name == '_')
        name++;
    return strncmp(name, prefix, len) == 0;
}

bool gcrt_hle_library_range(const char* prefix, u32* lo, u32* hi) {
    bool any = false;
    for (unsigned i = 0; i < s_match_count; i++) {
        const char* name = gcrt_sdk_catalog[s_matches[i].sig].name;
        if (!in_library(name, prefix))
            continue;
        unsigned count = 0;
        gcrt_hle_find(name, &count);
        if (count != 1)
            continue; /* only unambiguous matches delimit the library */
        u32 end = s_matches[i].address + gcrt_sdk_catalog[s_matches[i].sig].size;
        if (!any || s_matches[i].address < *lo)
            *lo = s_matches[i].address;
        if (!any || end > *hi)
            *hi = end;
        any = true;
    }
    return any;
}

unsigned gcrt_hle_replace_in_library(GcrtCpu* g, const char* sdk_name, const char* prefix, GcrtHleFn fn) {
    u32 addresses[16];
    unsigned n = gcrt_hle_find_all(sdk_name, addresses, 16);
    if (n == 1)
        return gcrt_hle_register(g, addresses[0], sdk_name, fn) ? 1u : 0u;
    if (n == 0)
        return 0;
    /* Several functions have the same masked code (small accessor functions, e.g. OSSetCurrentHeap and
     * DVDSetAutoInvalidation): the only copy located in the range of its library is kept (the SDK
     * libraries are linked in one piece), otherwise the replacement is refused. */
    u32 lo = 0, hi = 0;
    if (!gcrt_hle_library_range(prefix, &lo, &hi)) {
        fprintf(stderr, "gcrt hle: %s recognized %u times and library %s not found: no replacement\n",
                sdk_name, n, prefix);
        return 0;
    }
    int chosen = -1;
    for (unsigned k = 0; k < n && k < 16; k++) {
        if (addresses[k] >= lo && addresses[k] < hi) {
            if (chosen >= 0) {
                fprintf(stderr, "gcrt hle: %s: several copies in library %s (%08X-%08X): no "
                                "replacement\n", sdk_name, prefix, lo, hi);
                return 0;
            }
            chosen = (int)k;
        }
    }
    if (chosen < 0) {
        fprintf(stderr, "gcrt hle: %s: none of the %u copies in library %s (%08X-%08X): no "
                        "replacement\n", sdk_name, n, prefix, lo, hi);
        return 0;
    }
    if (s_trace)
        fprintf(stderr, "gcrt hle: %s recognized %u times, copy %08X kept (library %s %08X-%08X)\n", sdk_name,
                n, addresses[chosen], prefix, lo, hi);
    return gcrt_hle_register(g, addresses[chosen], sdk_name, fn) ? 1u : 0u;
}

/* Address and size of a function recognized exactly once (false otherwise). */
static bool unique_function(const char* name, u32* address, u32* size) {
    unsigned n = 0;
    u32 a = 0, s = 0;
    for (unsigned i = 0; i < s_match_count; i++) {
        const GcrtSdkSignature* sig = &gcrt_sdk_catalog[s_matches[i].sig];
        if (strcmp(sig->name, name) == 0) {
            a = s_matches[i].address;
            s = sig->size;
            n++;
        }
    }
    if (n != 1)
        return false;
    *address = a;
    *size = s;
    return true;
}

static u32 catalog_size(const char* name) {
    for (unsigned s = 0; s < gcrt_sdk_catalog_count; s++)
        if (strcmp(gcrt_sdk_catalog[s].name, name) == 0)
            return gcrt_sdk_catalog[s].size;
    return 0;
}

u32 gcrt_hle_catalog_size(const char* name) {
    return catalog_size(name);
}

/* ---------------------------------------------------------------------------------------------
 * Reading recognized code: linear simulation of constants (see gcrt_hle.h)
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    u32 val[32];
    u32 known; /* one bit per GPR */
} ConstRegs;

static void cr_set(ConstRegs* r, unsigned reg, u32 v) {
    r->val[reg] = v;
    r->known |= 1u << reg;
}
static void cr_forget(ConstRegs* r, unsigned reg) { r->known &= ~(1u << reg); }
static bool cr_get(const ConstRegs* r, unsigned reg, u32* v) {
    if (!((r->known >> reg) & 1u))
        return false;
    *v = r->val[reg];
    return true;
}

/* Effect of an instruction on the known constants. */
static void cr_step(ConstRegs* r, u32 w) {
    const u32 op = w >> 26, rd = (w >> 21) & 31u, ra = (w >> 16) & 31u, rb = (w >> 11) & 31u;
    const u32 uimm = w & 0xFFFFu;
    const s32 simm = (s16)uimm;
    u32 a;
    switch (op) {
    case 14: /* addi (li) */
    case 15: /* addis (lis) */ {
        const u32 k = op == 15 ? (uimm << 16) : (u32)simm;
        if (ra == 0)
            cr_set(r, rd, k);
        else if (cr_get(r, ra, &a))
            cr_set(r, rd, a + k);
        else
            cr_forget(r, rd);
        return;
    }
    case 24: /* ori: rA = rS | imm */
    case 25: /* oris */
        if (cr_get(r, rd, &a))
            cr_set(r, ra, a | (op == 25 ? (uimm << 16) : uimm));
        else
            cr_forget(r, ra);
        return;
    case 31: {
        const u32 xo = (w >> 1) & 0x3FFu;
        if (xo == 444u && rd == rb) { /* or rA, rS, rS (mr) */
            if (cr_get(r, rd, &a))
                cr_set(r, ra, a);
            else
                cr_forget(r, ra);
            return;
        }
        switch (xo) {
        /* Logical and shift operations (X form): destination rA, rS = rD field is a source. */
        case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476: case 24: case 536: case 792:
        case 824: case 26: case 922: case 954:
            cr_forget(r, ra);
            return;
        /* No integer destination: compares, trap, mt*, indexed stores, cache, synchronization. */
        case 0: case 32: case 4: case 144: case 146: case 467: case 210: case 242: case 151: case 215: case 407:
        case 662: case 918: case 663: case 727: case 983: case 54: case 86: case 470: case 982: case 1014: case 278:
        case 246: case 598: case 854: case 150:
            return;
        /* Indexed stores with update: rA only. */
        case 183: case 247: case 439: case 695: case 759:
            cr_forget(r, ra);
            return;
        /* Indexed loads with update: rD and rA. */
        case 55: case 119: case 311: case 375: case 567: case 631:
            cr_forget(r, rd);
            cr_forget(r, ra);
            return;
        default:
            /* Arithmetic (XO form), indexed loads, mfspr, mfcr, mfmsr, mftb...: destination rD. */
            cr_forget(r, rd);
            return;
        }
    }
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: /* loads (D form, and with update) */
        cr_forget(r, rd);
        if (op & 1u)
            cr_forget(r, ra);
        return;
    case 46: /* lmw */
        for (u32 k = rd; k < 32u; k++)
            cr_forget(r, k);
        return;
    case 37: case 39: case 45: /* stwu, stbu, sthu: rA updated */
        cr_forget(r, ra);
        return;
    case 36: case 38: case 44: case 47: /* stores: no destination */
        return;
    case 20: case 21: case 23: case 26: case 27: case 28: case 29: /* rlw*, xori, xoris, andi., andis.: rA */
        cr_forget(r, ra);
        return;
    case 7: case 8: case 12: case 13: /* mulli, subfic, addic: rD */
        cr_forget(r, rd);
        return;
    case 10: case 11: case 16: case 17: case 18: case 19: /* compares, branches, sc */
        return;
    default:
        /* Floating point, paired singles: no GPR written, except the rA update ("u" forms); be careful. */
        if (op == 49u || op == 51u || op == 53u || op == 55u || op == 57u || op == 61u)
            cr_forget(r, ra);
        else if (op == 4u) /* indexed paired singles: forms that update rA */
            cr_forget(r, ra);
        return;
    }
}

static bool fn_word(GcrtCpu* g, u32 address, u32* w) {
    const CPUState* c = &g->state;
    if (address < GC_RAM_BASE || address - GC_RAM_BASE > c->ram_size - 4u)
        return false;
    *w = read_be32(c->ram + (address - GC_RAM_BASE));
    return true;
}

bool gcrt_hle_decode_reg_at_call(GcrtCpu* g, u32 fn, u32 size, u32 target, unsigned nth, unsigned reg, u32 sda,
                                 u32 sda2, u32* out) {
    ConstRegs r;
    memset(&r, 0, sizeof(r));
    if (sda)
        cr_set(&r, 13, sda);
    if (sda2)
        cr_set(&r, 2, sda2);
    unsigned seen = 0;
    for (u32 off = 0; off + 4u <= size; off += 4u) {
        u32 w;
        if (!fn_word(g, fn + off, &w))
            return false;
        if ((w >> 26) == 18u && (w & 3u) == 1u) { /* bl */
            u32 li = w & 0x03FFFFFCu;
            if (li & 0x02000000u)
                li |= 0xFC000000u;
            if (fn + off + li == target) {
                if (seen++ == nth)
                    return reg < 32u && cr_get(&r, reg, out);
            }
            /* Volatile registers are lost after a call. */
            cr_forget(&r, 0);
            for (unsigned k = 3; k <= 12; k++)
                cr_forget(&r, k);
            continue;
        }
        cr_step(&r, w);
    }
    return false;
}

bool gcrt_hle_decode_reg_at_blr(GcrtCpu* g, u32 fn, u32 size, unsigned reg, u32* out) {
    ConstRegs r;
    memset(&r, 0, sizeof(r));
    for (u32 off = 0; off + 4u <= size; off += 4u) {
        u32 w;
        if (!fn_word(g, fn + off, &w))
            return false;
        if (w == 0x4E800020u)
            return reg < 32u && cr_get(&r, reg, out);
        cr_step(&r, w);
    }
    return false;
}

bool gcrt_hle_decode_d_offset(GcrtCpu* g, u32 fn, u32 size, unsigned opcode, unsigned base, unsigned nth, s16* out) {
    unsigned seen = 0;
    for (u32 off = 0; off + 4u <= size; off += 4u) {
        u32 w;
        if (!fn_word(g, fn + off, &w))
            return false;
        if ((w >> 26) == opcode && ((w >> 16) & 31u) == base) {
            if (seen++ == nth) {
                *out = (s16)(w & 0xFFFFu);
                return true;
            }
        }
    }
    return false;
}

u32 gcrt_hle_resolve_following(const char* name, const char* const* chain, unsigned chain_count) {
    if (!chain_count)
        return 0;
    u32 address = 0, size = 0;
    if (!unique_function(chain[0], &address, &size))
        return 0;
    u32 expected = address + size;
    for (unsigned k = 1; k < chain_count; k++) {
        u32 s = catalog_size(chain[k]);
        if (!s)
            return 0;
        expected += s;
    }
    u32 candidates[16];
    unsigned n = gcrt_hle_find_all(name, candidates, 16);
    for (unsigned k = 0; k < n && k < 16; k++)
        if (candidates[k] == expected)
            return expected;
    return 0;
}

u32 gcrt_hle_resolve_preceding(const char* name, const char* anchor) {
    u32 address = 0, size = 0;
    if (!unique_function(anchor, &address, &size))
        return 0;
    u32 own = catalog_size(name);
    u32 candidates[16];
    unsigned n = gcrt_hle_find_all(name, candidates, 16);
    for (unsigned k = 0; k < n && k < 16; k++)
        if (candidates[k] + own == address)
            return candidates[k];
    return 0;
}

unsigned gcrt_hle_replace(GcrtCpu* g, const char* sdk_name, GcrtHleFn fn) {
    unsigned n = 0;
    for (unsigned i = 0; i < s_match_count; i++) {
        if (strcmp(gcrt_sdk_catalog[s_matches[i].sig].name, sdk_name) == 0) {
            if (gcrt_hle_register(g, s_matches[i].address, sdk_name, fn))
                n++;
        }
    }
    if (!n && s_trace)
        fprintf(stderr, "gcrt hle: %s absent (no replacement)\n", sdk_name);
    return n;
}

void gcrt_hle_report(FILE* out) {
    unsigned unique = 0, multiple = 0, absent = 0;
    for (unsigned s = 0; s < gcrt_sdk_catalog_count; s++) {
        unsigned n = 0;
        for (unsigned i = 0; i < s_match_count; i++)
            n += s_matches[i].sig == s;
        if (n == 0)
            absent++;
        else if (n == 1)
            unique++;
        else
            multiple++;
    }
    fprintf(out, "HLE: %u catalog functions recognized once, %u several times, %u absent "
                 "(%u in total); %u active replacements\n",
            unique, multiple, absent, gcrt_sdk_catalog_count, s_repl_count);
    if (!s_trace)
        return;
    for (u32 k = 0; k < REPL_SLOTS; k++)
        if (s_repl[k].address)
            fprintf(out, "  replacement %-10s %08X: %llu calls\n", s_repl[k].name, s_repl[k].address,
                    (unsigned long long)s_repl[k].calls);
}

/* ---------------------------------------------------------------------------------------------
 * Call of a guest function from the host
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    bool returned;
    u32 result;
} CallState;

static CallState* s_call;

static void call_returned(GcrtCpu* g, u32 original_return, void* data) {
    (void)original_return;
    (void)data;
    if (s_call) {
        s_call->returned = true;
        s_call->result = g->state.gpr[3];
    }
    gcrt_cpu_request_stop(g);
}

bool gcrt_hle_call_guest(GcrtCpu* g, u32 function, const u32* args, unsigned arg_count, u64 max_blocks, u32* result) {
    CPUState* cpu = &g->state;
    u32 gpr[32], pc = cpu->pc, lr = cpu->lr, ctr = cpu->ctr, cr = cpu->cr, xer = cpu->xer, msr = cpu->msr;
    u32 srr0 = cpu->srr0, srr1 = cpu->srr1;
    memcpy(gpr, cpu->gpr, sizeof(gpr));
    CallState state = {false, 0};
    CallState* previous = s_call;
    s_call = &state;

    for (unsigned i = 0; i < arg_count && i < 8; i++)
        cpu->gpr[3 + i] = args[i];
    cpu->lr = gcrt_hle_push_return(g, 0, call_returned, NULL, 0);
    cpu->pc = function;
    cpu->gpr[1] = (cpu->gpr[1] - 0x100u) & ~0xFu; /* margin below the current stack */
    /* Relative budget: the limit of gcrt_cpu_run applies to the cumulative block counter (fix of
     * 2026-10-10: an absolute limit stopped the call immediately once the game was running). */
    GcrtRunLimits limits = {g->blocks + max_blocks, 0.0};
    GcrtStopReason reason = gcrt_cpu_run(g, &limits);
    if (!state.returned)
        fprintf(stderr, "gcrt hle: the call to %08X did not return (%s)\n", function, gcrt_stop_reason_name(reason));

    memcpy(cpu->gpr, gpr, sizeof(gpr));
    cpu->pc = pc;
    cpu->lr = lr;
    cpu->ctr = ctr;
    cpu->cr = cr;
    cpu->xer = xer;
    cpu->msr = msr;
    cpu->srr0 = srr0;
    cpu->srr1 = srr1;
    s_call = previous;
    g->stop = GCRT_STOP_NONE;
    if (result)
        *result = state.result;
    return state.returned;
}

/* ---------------------------------------------------------------------------------------------
 * Game output
 * ------------------------------------------------------------------------------------------- */

void gcrt_hle_set_guest_log(FILE* file) {
    s_guest_log = file;
}

void gcrt_hle_guest_output(const char* text, size_t length) {
    fwrite(text, 1, length, stdout);
    if (memchr(text, '\n', length))
        fflush(stdout);
    if (s_guest_log) {
        fwrite(text, 1, length, s_guest_log);
        fflush(s_guest_log);
    }
}

u32 gcrt_hle_crc32(const void* data, size_t size) {
    crc_init();
    const u8* p = (const u8*)data;
    u32 crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; i++)
        crc = s_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
