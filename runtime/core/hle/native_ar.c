/* Native audio memory (milestone 10a): replacement of the SDK AR functions (ar.c).
 *
 * Boundary (the internal SDK notes § 3.3, reverse agent): ARInit, ARStartDMA, ARRegisterDMACallback, ARGetDMAStatus,
 * ARAlloc, ARFree, ARGetSize. ARQ (request queue, pure software) stays recompiled: it only calls ARStartDMA and
 * ARRegisterDMACallback. __ARHandler, __ARChecksize, __ARClearInterrupt, __ARGetInterruptStatus are no longer
 * called: no access to the ARAM registers (0xCC005010-0x502F).
 *
 * Semantics (ar.c of the reference decompilation, consulted only; reference emulation hw/dsp.cpp, removed in
 * step 10d, kept outside this repository's public history):
 *   - ARInit(stack, n): first call only (afterwards returns 0x4000); null callback, DSP_ARAM interrupt unmasked
 *     in the OS (like the original), allocation stack at 0x4000, n blocks, array of lengths = `stack` (guest
 *     array), size 16 MiB (result of __ARChecksize on the reference, also written at 0x800000D0);
 *   - ARAlloc / ARFree: allocation stack, guest array maintained like the SDK does;
 *   - ARStartDMA(type, ram, aram, length): type 0 RAM -> ARAM, 1 ARAM -> RAM; addresses and length taken like
 *     the DMA registers (physical bits, multiples of 32); immediate copy (like the reference), then completion
 *     delivered like the DSP_ARAM interrupt (#6) after 246 cycles per 32-byte block (Dolphin measurement, same
 *     model as the reference): __AR_Callback() is called in a temporary exception context, like __ARHandler;
 *   - ARGetDMAStatus: 0x200 as long as the transfer has not finished.
 * The audio memory is a 16 MiB host buffer allocated here (gcrt_native_aram), also read by the HLE DSP microcodes
 * (step 10d: no more emulated DSP nor register-driven ARAM DMA). Generic: no reference to a game. */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <stdlib.h>
#include <string.h>

#define AR_USER_BASE 0x4000u      /* __AR_ARAM_USR_BASE_ADDR */
#define AR_INTERNAL_SIZE 0x1000000u /* 16 MiB */
#define AR_CYCLES_PER_BLOCK 246u  /* per 32-byte block (Dolphin, DSP.cpp; like the emulated reference) */
#define LOW_ARAM_SIZE 0x800000D0u

typedef struct {
    u8* aram;
    u32 aram_size;
    bool initialized;
    u32 callback;
    u32 size;
    u32 stack_pointer;
    u32 free_blocks;
    u32 block_length; /* guest address of the array of lengths */
    u64 busy_until;   /* ticks: end of the transfer in progress */
    /* Report */
    u64 dmas, to_aram, to_ram, bytes, callbacks;
    unsigned installed;
    char missing[512];
} NativeAr;

static NativeAr s_ar;

static u32 arg(GcrtCpu* g, int i) { return g->state.gpr[3 + i]; }

u8* gcrt_native_aram(u32* size) {
    if (size)
        *size = s_ar.aram_size;
    return s_ar.aram;
}

/* ARInit(u32* stack_index_addr, u32 num_entries) -> u32 */
static bool hle_ar_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    if (!s_ar.initialized) {
        s_ar.callback = 0;
        u32 unmask = gcrt_native_os_fn(GCRT_OSFN_UNMASK_INTERRUPTS);
        if (unmask) {
            const u32 mask = 0x80000000u >> GCRT_OS_IRQ_DSP_ARAM;
            gcrt_guest_call(g, unmask, &mask, 1, NULL);
        }
        s_ar.stack_pointer = AR_USER_BASE;
        s_ar.free_blocks = arg(g, 1);
        s_ar.block_length = arg(g, 0);
        /* __ARChecksize: no expansion memory (like the reference), size also stored in low memory. */
        s_ar.size = AR_INTERNAL_SIZE;
        mem_write32(&g->state, LOW_ARAM_SIZE, s_ar.size);
        s_ar.initialized = true;
        GCRT_TRACE(GCRT_TRACE_AR, "native ARInit: %u blocks, array %08X, ARAM %u MiB%s", s_ar.free_blocks,
                   s_ar.block_length, s_ar.size >> 20, s_ar.aram ? "" : " (WARNING: no buffer provided)");
    }
    gcrt_native_return(g, s_ar.initialized ? AR_USER_BASE : 0);
    return true;
}

/* ARAlloc(u32 length) -> u32 */
static bool hle_ar_alloc(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 length = arg(g, 0);
    const u32 old = s_ar.stack_pointer;
    s_ar.stack_pointer += length;
    mem_write32(&g->state, s_ar.block_length, length);
    s_ar.block_length += 4u;
    s_ar.free_blocks--;
    GCRT_TRACE(GCRT_TRACE_AR, "ARAlloc(%u) -> 0x%08X", length, old);
    gcrt_native_return(g, old);
    return true;
}

/* ARFree(u32* length) -> u32 */
static bool hle_ar_free(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 out = arg(g, 0);
    s_ar.block_length -= 4u;
    const u32 length = mem_read32(&g->state, s_ar.block_length);
    if (out)
        mem_write32(&g->state, out, length);
    s_ar.stack_pointer -= length;
    s_ar.free_blocks++;
    GCRT_TRACE(GCRT_TRACE_AR, "ARFree -> %u bytes, stack 0x%08X", length, s_ar.stack_pointer);
    gcrt_native_return(g, s_ar.stack_pointer);
    return true;
}

static bool hle_ar_get_size(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, s_ar.size);
    return true;
}

/* ARRegisterDMACallback(cb) -> previous callback */
static bool hle_ar_register_dma_callback(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 old = s_ar.callback;
    s_ar.callback = arg(g, 0);
    gcrt_native_return(g, old);
    return true;
}

static bool hle_ar_get_dma_status(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, g->ticks < s_ar.busy_until ? 0x200u : 0u);
    return true;
}

/* Native DSP_ARAM interrupt: like __ARHandler (temporary exception context, then __AR_Callback()). */
static void on_dma_done(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    (void)interrupt;
    (void)user;
    const u32 tmp = gcrt_native_exception_context_begin(g);
    if (s_ar.callback) {
        s_ar.callbacks++;
        gcrt_native_callback(g, s_ar.callback, NULL, 0);
    }
    gcrt_native_exception_context_end(g, tmp, context);
}

/* ARStartDMA(u32 type, u32 mainmem, u32 aram, u32 length) */
static bool hle_ar_start_dma(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const bool to_ram = (arg(g, 0) & 1u) != 0;
    /* Like the DMA registers: 10 high bits, 16 low bits that are multiples of 32. */
    u32 mm = ((arg(g, 1) >> 16) & 0x3FFu) << 16 | (arg(g, 1) & 0xFFE0u);
    u32 ar = ((arg(g, 2) >> 16) & 0x3FFu) << 16 | (arg(g, 2) & 0xFFE0u);
    u32 count = ((arg(g, 3) >> 16) & 0x3FFu) << 16 | (arg(g, 3) & 0xFFE0u);
    const u32 total = count;
    CPUState* c = &g->state;
    const u32 mem1 = c->ram_size < GCRT_MEM1_SIZE ? c->ram_size : GCRT_MEM1_SIZE;
    /* In 8-byte words, with the same bounds as the reference (beyond the ARAM: zeros to RAM, nothing to the
     * ARAM). */
    while (count >= 8u) {
        if (mm < mem1 && mem1 - mm >= 8u) {
            if (to_ram) {
                if (s_ar.aram && ar < s_ar.aram_size)
                    memcpy(c->ram + mm, s_ar.aram + ar, 8);
                else
                    memset(c->ram + mm, 0, 8);
            } else if (s_ar.aram && ar < s_ar.aram_size) {
                memcpy(s_ar.aram + ar, c->ram + mm, 8);
            }
        }
        mm += 8u;
        ar += 8u;
        count -= 8u;
    }
    s_ar.dmas++;
    s_ar.bytes += total;
    if (to_ram)
        s_ar.to_ram++;
    else
        s_ar.to_aram++;
    const u64 cycles = (u64)(total / 32u) * AR_CYCLES_PER_BLOCK;
    const u64 delay = (cycles + GCRT_CYCLES_PER_TB_TICK - 1u) / GCRT_CYCLES_PER_TB_TICK;
    s_ar.busy_until = g->ticks + delay;
    GCRT_TRACE(GCRT_TRACE_AR, "DMA %s: %u bytes, RAM 0x%08X, ARAM 0x%08X", to_ram ? "ARAM->RAM" : "RAM->ARAM", total,
               arg(g, 1), arg(g, 2));
    gcrt_nirq_post(g, GCRT_OS_IRQ_DSP_ARAM, s_ar.busy_until, on_dma_done, NULL);
    gcrt_native_return(g, 0);
    return true;
}

void gcrt_native_ar_install(GcrtCpu* g) {
    static const char* const after_init[] = {"ARInit"};
    if (!s_ar.aram) {
        /* Audio memory of the console (16 MiB), empty at power-on; kept until the end of the process. */
        s_ar.aram = (u8*)calloc(1, AR_INTERNAL_SIZE);
        s_ar.aram_size = s_ar.aram ? AR_INTERNAL_SIZE : 0u;
        if (!s_ar.aram)
            fprintf(stderr, "gcrt native: cannot allocate the audio memory (%u bytes)\n", AR_INTERNAL_SIZE);
    }
    s_ar.installed = 0;
    s_ar.missing[0] = '\0';
    char* m = s_ar.missing;
    const size_t ms = sizeof(s_ar.missing);
    s_ar.installed += gcrt_native_replace(g, "ARInit", "AR", hle_ar_init, NULL, 0, NULL, m, ms);
    s_ar.installed += gcrt_native_replace(g, "ARStartDMA", "AR", hle_ar_start_dma, NULL, 0, NULL, m, ms);
    s_ar.installed += gcrt_native_replace(g, "ARAlloc", "AR", hle_ar_alloc, NULL, 0, NULL, m, ms);
    s_ar.installed += gcrt_native_replace(g, "ARFree", "AR", hle_ar_free, NULL, 0, NULL, m, ms);
    s_ar.installed += gcrt_native_replace(g, "ARGetDMAStatus", "AR", hle_ar_get_dma_status, NULL, 0, NULL, m, ms);
    /* Shared signatures (small accessors): anchored by the order of the functions in ar.c. */
    s_ar.installed += gcrt_native_replace(g, "ARGetSize", "AR", hle_ar_get_size, after_init, 1, NULL, m, ms);
    s_ar.installed += gcrt_native_replace(g, "ARRegisterDMACallback", "AR", hle_ar_register_dma_callback, NULL, 0,
                                          "ARGetDMAStatus", m, ms);
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native ARAM: %u functions replaced, missing: %s", s_ar.installed,
                      s_ar.missing[0] ? s_ar.missing : "none");
}

void gcrt_native_ar_report(FILE* out) {
    fprintf(out, "native ARAM: %llu transfers (%llu to ARAM, %llu to RAM), %.1f MiB, %llu callbacks; stack 0x%08X; "
                 "%u functions replaced, missing: %s\n",
            (unsigned long long)s_ar.dmas, (unsigned long long)s_ar.to_aram, (unsigned long long)s_ar.to_ram,
            (double)s_ar.bytes / (1024.0 * 1024.0), (unsigned long long)s_ar.callbacks, s_ar.stack_pointer,
            s_ar.installed, s_ar.missing[0] ? s_ar.missing : "none");
}
