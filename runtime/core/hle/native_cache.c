/* Native cache operations (milestone 10a): replacement of the SDK cache functions (OSCache.c).
 *
 * The recompiled code has no cache to maintain: flushes and invalidations have no effect. But each
 * `dcbf/dcbi/dcbst/icbi` is delegated by DolRecomp (block exit) and DCFlushRange / DCStoreRange end with an `sc`
 * (exception; before milestone 10b, vector 0xC00 run by the mini-interpreter, handled by the host since then):
 * replacing these functions removes that cost (the internal SDK notes § 5, reverse agent; measurement in
 * notes/runtime.md).
 *   - no effect: DCInvalidateRange, DCFlushRange, DCStoreRange, DCFlushRangeNoSync, ICInvalidateRange;
 *   - ICFlashInvalidate: HID0 |= ICFI, like the original code (the special-purpose register stays consistent);
 *   - with a memory effect: DCZeroRange (zeroes the 32-byte lines covered, like `dcbz`),
 *     LCStoreData / LCStoreBlocks (copy from the locked cache, 16 KiB at 0xE0000000, to RAM, in 32-byte lines;
 *     LCStoreData returns the number of 128-line transfers), LCQueueWait (DMA queue always empty: immediate
 *     return).
 * Kept recompiled (special-purpose registers only, once at startup or reset, no gain): DCEnable, ICEnable,
 * L2GlobalInvalidate, __OSCacheInit, LCEnable (zeroes the locked cache with dcbz_l), LCDisable.
 * Generic: no reference to a game. */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <string.h>

#define SPR_HID0 1008u
#define HID0_ICFI 0x00000800u
#define LC_LINE 32u
#define LC_BLOCK_LINES 128u

typedef struct {
    u64 calls[8];
    u64 bytes_zeroed, bytes_stored;
    unsigned installed;
    char missing[512];
} NativeCache;

enum { C_DCINV = 0, C_DCFLUSH, C_DCSTORE, C_DCFLUSH_NOSYNC, C_ICINV, C_ICFLASH, C_DCZERO, C_LCSTORE };

static NativeCache s_cache;

static u32 arg(GcrtCpu* g, int i) { return g->state.gpr[3 + i]; }

static bool no_effect(GcrtCpu* g, int kind) {
    gcrt_native_enter(g);
    s_cache.calls[kind]++;
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_dc_invalidate_range(GcrtCpu* g, u32 a) { (void)a; return no_effect(g, C_DCINV); }
static bool hle_dc_flush_range(GcrtCpu* g, u32 a) { (void)a; return no_effect(g, C_DCFLUSH); }
static bool hle_dc_store_range(GcrtCpu* g, u32 a) { (void)a; return no_effect(g, C_DCSTORE); }
static bool hle_dc_flush_range_no_sync(GcrtCpu* g, u32 a) { (void)a; return no_effect(g, C_DCFLUSH_NOSYNC); }
static bool hle_ic_invalidate_range(GcrtCpu* g, u32 a) { (void)a; return no_effect(g, C_ICINV); }

static bool hle_ic_flash_invalidate(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_cache.calls[C_ICFLASH]++;
    g->spr[SPR_HID0] |= HID0_ICFI;
    gcrt_native_return(g, 0);
    return true;
}

/* DCZeroRange(void* addr, u32 nBytes): lines [addr & ~31, addr + n) rounded to 32 bytes (SDK code:
 * (n + (addr & 31) + 31) >> 5 lines, nothing if n == 0). */
static bool hle_dc_zero_range(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_cache.calls[C_DCZERO]++;
    const u32 addr = arg(g, 0), n = arg(g, 1);
    if (n) {
        const u32 lines = (n + (addr & 31u) + 31u) >> 5;
        const u32 start = addr & ~31u;
        u8* p = gcrt_native_ram(g, start, lines * LC_LINE);
        if (p) {
            memset(p, 0, (size_t)lines * LC_LINE);
        } else {
            for (u32 i = 0; i < lines * LC_LINE; i += 4u)
                mem_write32(&g->state, start + i, 0);
        }
        s_cache.bytes_zeroed += (u64)lines * LC_LINE;
    }
    gcrt_native_return(g, 0);
    return true;
}

/* Copies `lines` lines from the locked cache (src) to RAM (dst), like the DMA triggered by DMAL. */
static void lc_store(GcrtCpu* g, u32 dst, u32 src, u32 lines) {
    dst &= ~31u;
    src &= ~31u;
    const u32 bytes = lines * LC_LINE;
    if (src < GCRT_LOCKED_CACHE_BASE || src - GCRT_LOCKED_CACHE_BASE > GCRT_LOCKED_CACHE_SIZE - bytes) {
        GCRT_TRACE(GCRT_TRACE_UNHANDLED, "native cache: LCStore outside the locked cache (%08X, %u bytes)", src, bytes);
        return;
    }
    const u8* from = g->locked_cache + (src - GCRT_LOCKED_CACHE_BASE);
    u8* to = gcrt_native_ram(g, dst, bytes);
    if (to)
        memcpy(to, from, bytes);
    else
        for (u32 i = 0; i < bytes; i += 4u)
            mem_write32(&g->state, dst + i, read_be32(from + i));
    s_cache.bytes_stored += bytes;
}

/* LCStoreBlocks(void* dst, void* src, u32 nBlocks): 0 = 128 lines. */
static bool hle_lc_store_blocks(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_cache.calls[C_LCSTORE]++;
    const u32 n = arg(g, 2) & (LC_BLOCK_LINES - 1u);
    lc_store(g, arg(g, 0), arg(g, 1), n ? n : LC_BLOCK_LINES);
    gcrt_native_return(g, 0);
    return true;
}

/* LCStoreData(void* dst, void* src, u32 nBytes) -> number of transfers (blocks of 128 lines). */
static bool hle_lc_store_data(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_cache.calls[C_LCSTORE]++;
    u32 dst = arg(g, 0), src = arg(g, 1);
    u32 lines = (arg(g, 2) + LC_LINE - 1u) / LC_LINE;
    const u32 transactions = (lines + LC_BLOCK_LINES - 1u) / LC_BLOCK_LINES;
    while (lines > 0) {
        const u32 now = lines < LC_BLOCK_LINES ? lines : LC_BLOCK_LINES;
        lc_store(g, dst, src, now);
        lines -= now;
        dst += LC_BLOCK_LINES * LC_LINE;
        src += LC_BLOCK_LINES * LC_LINE;
    }
    gcrt_native_return(g, transactions);
    return true;
}

/* LCQueueWait(u32 len): waits until the locked-cache DMA queue holds no more than `len`: always empty. */
static bool hle_lc_queue_wait(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, 0);
    return true;
}

void gcrt_native_cache_install(GcrtCpu* g) {
    static const struct {
        const char* name;
        GcrtHleFn fn;
    } k[] = {
        {"DCInvalidateRange", hle_dc_invalidate_range},
        {"DCFlushRange", hle_dc_flush_range},
        {"DCStoreRange", hle_dc_store_range},
        {"DCFlushRangeNoSync", hle_dc_flush_range_no_sync},
        {"ICInvalidateRange", hle_ic_invalidate_range},
        {"ICFlashInvalidate", hle_ic_flash_invalidate},
        {"DCZeroRange", hle_dc_zero_range},
        {"LCStoreBlocks", hle_lc_store_blocks},
        {"LCStoreData", hle_lc_store_data},
        {"LCQueueWait", hle_lc_queue_wait},
    };
    s_cache.installed = 0;
    s_cache.missing[0] = '\0';
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        s_cache.installed += gcrt_native_replace(g, k[i].name, "DC", k[i].fn, NULL, 0, NULL, s_cache.missing,
                                                 sizeof(s_cache.missing));
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native cache: %u functions replaced, missing: %s", s_cache.installed,
                      s_cache.missing[0] ? s_cache.missing : "none");
}

void gcrt_native_cache_report(FILE* out) {
    fprintf(out, "native cache: DCInvalidateRange %llu, DCFlushRange %llu, DCStoreRange %llu, DCFlushRangeNoSync %llu, "
                 "ICInvalidateRange %llu, ICFlashInvalidate %llu, DCZeroRange %llu (%llu bytes), LCStore %llu (%llu "
                 "bytes); %u functions replaced, missing: %s\n",
            (unsigned long long)s_cache.calls[C_DCINV], (unsigned long long)s_cache.calls[C_DCFLUSH],
            (unsigned long long)s_cache.calls[C_DCSTORE], (unsigned long long)s_cache.calls[C_DCFLUSH_NOSYNC],
            (unsigned long long)s_cache.calls[C_ICINV], (unsigned long long)s_cache.calls[C_ICFLASH],
            (unsigned long long)s_cache.calls[C_DCZERO], (unsigned long long)s_cache.bytes_zeroed,
            (unsigned long long)s_cache.calls[C_LCSTORE], (unsigned long long)s_cache.bytes_stored, s_cache.installed,
            s_cache.missing[0] ? s_cache.missing : "none");
}
