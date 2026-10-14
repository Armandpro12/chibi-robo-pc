/* Native OS, "system" part (milestone 10b): interrupts without a vector, reset, memory protection, IPL SRAM and
 * ROM, EXI bus, DSP bootstrap by the OS, system call. See the internal SDK notes § 13.0.1 and notes/runtime.md
 * (step 10b section). Generic: nothing specific to a game; static variables found in the recognized code.
 *
 * Interrupts: no more 0x500 vector nor guest ExternalInterruptHandler / __OSDispatchInterrupt. The OS masks stay in
 * low memory (0x800000C4 / 0x800000C8), maintained by native __OSMaskInterrupts / __OSUnmaskInterrupts with the same
 * arithmetic; what SetInterruptMask wrote into the registers (PI INTMSK, DSPCSR, AICR, MI, EXI) no longer has a
 * recipient: all sources are native since step 10d (native interrupts, native_irq.c, which read the OS masks
 * themselves). Until 10d, the host forwarded these values to the still-emulated hardware, whose line was dispatched
 * here like __OSDispatchInterrupt (bridge removed).
 *
 * EXI bus: no device reachable by the game (the memory card is native since milestone 7, the IPL SRAM and ROM are
 * served by the OS functions replaced here, port B and the serial port are empty). The functions of EXIBios.c are
 * replaced with the SDK semantics for empty slots (negative probes, selection and transfers refused, locks and
 * callbacks kept by the host). [hypothesis] for a game that would talk directly to an EXI device (network adapter,
 * microphone...): it would see it as absent. */
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <stdlib.h>
#include <string.h>

/* Low memory. */
#define LOW_MASK_PRIOR 0x800000C4u
#define LOW_MASK_CURRENT 0x800000C8u
#define LOW_CTX 0x800000D4u
#define LOW_PHYS_MEM 0x80000028u
#define LOW_SIM_MEM 0x800000F0u
#define LOW_GAME_CHOICE 0x800030E3u
#define HANDLER_TABLE 0x80003040u

/* OS masks (OS_INTERRUPTMASK_*, number n -> 0x80000000 >> n). */
#define M(n) (0x80000000u >> (n))
#define MASK_MEM (M(0) | M(1) | M(2) | M(3) | M(4))
#define MASK_DSP (M(5) | M(6) | M(7))
#define MASK_AI M(8)
#define MASK_EXI_0 (M(9) | M(10) | M(11))
#define MASK_EXI_1 (M(12) | M(13) | M(14))
#define MASK_EXI_2 (M(15) | M(16))
#define MASK_EXI (MASK_EXI_0 | MASK_EXI_1 | MASK_EXI_2)
#define MASK_PI (M(17) | M(18) | M(19) | M(20) | M(21) | M(22) | M(23) | M(24) | M(25) | M(26))

static struct {
    u32 sda;
    u32 a_handler_table_var; /* InterruptHandlerTable (pointer, r13) */
    u32 fn_set_handler, fn_set_exception_handler, fn_register_reset, fn_register_version, fn_get_arena_hi;
    u32 fn_get_gbs, fn_set_gbs;
    u32 scb;          /* SRAM control block (64-byte buffer + offset, enabled, lock, write done) */
    u32 a_reset_var;  /* OSGetResetCode: stored code (non-zero word) then value */
    u32 dsp_init_code;
    unsigned replaced;
    char missing[1024];
    u64 masks, unmasks, sram_writes, rom_reads, exi_probes, resets;
    u8 language;
    /* Reset button (OSGetResetButtonState): SDK state kept by the host; no reset button on PC. */
    bool rsw_down, rsw_last;
    u64 rsw_hold_up, rsw_hold_down;
    bool game_choice_warned;
    /* EXI: per-channel state kept by the host. */
    struct {
        u32 state; /* SDK bits: 0x08 locked (LOCKED), 0x04 selected (SELECTED), 0x10 attached (ATTACHED) */
        u32 dev, exi_callback, ext_callback;
        u32 queue_cb[3], queue_dev[3];
        unsigned items;
    } exi[3];
} s_sys;

#define EXI_STATE_SELECTED 0x04u
#define EXI_STATE_LOCKED 0x10u
#define EXI_STATE_ATTACHED 0x08u

static u32 R32(GcrtCpu* g, u32 a) { return mem_read32(&g->state, a); }
static void W32(GcrtCpu* g, u32 a, u32 v) { mem_write32(&g->state, a, v); }
static u8 R8(GcrtCpu* g, u32 a) { return mem_read8(&g->state, a); }
static void W8(GcrtCpu* g, u32 a, u8 v) { mem_write8(&g->state, a, v); }
static u16 R16(GcrtCpu* g, u32 a) { return mem_read16(&g->state, a); }
static void W16(GcrtCpu* g, u32 a, u16 v) { mem_write16(&g->state, a, v); }
static u32 arg(GcrtCpu* g, unsigned i) { return g->state.gpr[3 + i]; }
static bool entry_ee(GcrtCpu* g) { return (g->state.msr & 0x8000u) != 0; }

static void ret(GcrtCpu* g, u32 value) {
    g->state.gpr[3] = value;
    g->state.pc = g->state.lr;
}

static u32 cntlzw(u32 v) {
    u32 n = 0;
    if (!v)
        return 32u;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        n++;
    }
    return n;
}

static u32 fn_of(const char* name) {
    unsigned n = 0;
    const u32 a = gcrt_hle_find(name, &n);
    return n == 1 ? a : 0;
}

/* Call of a guest function that does not access the hardware (handler installation, banner...). */
static void call(GcrtCpu* g, u32 fn, const u32* args, unsigned n) {
    if (fn)
        gcrt_guest_call(g, fn, args, n, NULL);
}

/* --------------------------------------------------------------------------------------------- *
 * Interrupt masks (OSInterrupt.c)
 * --------------------------------------------------------------------------------------------- */

/* SetInterruptMask(mask, current): bits handled in one go by the SDK (one register per group: MI, DSPCSR, AICR,
 * EXI channels, PI). No register to write (all sources are native): only the OS loop is kept. */
static u32 set_interrupt_mask(u32 mask, u32 current) {
    (void)current;
    const u32 n = cntlzw(mask);
    if (n <= 4u)
        return mask & ~MASK_MEM;
    if (n <= 7u)
        return mask & ~MASK_DSP;
    if (n == 8u)
        return mask & ~MASK_AI;
    if (n <= 11u)
        return mask & ~MASK_EXI_0;
    if (n <= 14u)
        return mask & ~MASK_EXI_1;
    if (n <= 16u)
        return mask & ~MASK_EXI_2;
    if (n <= 26u)
        return mask & ~MASK_PI;
    return 0;
}

u32 gcrt_os_mask_interrupts(GcrtCpu* g, u32 global) {
    const u32 prev = R32(g, LOW_MASK_PRIOR), local = R32(g, LOW_MASK_CURRENT);
    u32 mask = ~(prev | local) & global;
    global |= prev;
    W32(g, LOW_MASK_PRIOR, global);
    while (mask)
        mask = set_interrupt_mask(mask, global | local);
    s_sys.masks++;
    return prev;
}

u32 gcrt_os_unmask_interrupts(GcrtCpu* g, u32 global) {
    const u32 prev = R32(g, LOW_MASK_PRIOR), local = R32(g, LOW_MASK_CURRENT);
    u32 mask = (prev | local) & global;
    global = prev & ~global;
    W32(g, LOW_MASK_PRIOR, global);
    while (mask)
        mask = set_interrupt_mask(mask, global | local);
    s_sys.unmasks++;
    return prev;
}

static bool hle_mask_interrupts(GcrtCpu* g, u32 address) {
    (void)address;
    ret(g, gcrt_os_mask_interrupts(g, arg(g, 0)));
    return true;
}

static bool hle_unmask_interrupts(GcrtCpu* g, u32 address) {
    (void)address;
    ret(g, gcrt_os_unmask_interrupts(g, arg(g, 0)));
    return true;
}

/* __OSInterruptInit: handler table, masks, PI INTMSK = 0xF0, everything masked, exception 4. */
static bool hle_interrupt_init(GcrtCpu* g, u32 address) {
    (void)address;
    if (s_sys.a_handler_table_var)
        W32(g, s_sys.a_handler_table_var, HANDLER_TABLE);
    for (u32 i = 0; i < 32u; i++)
        W32(g, HANDLER_TABLE + 4u * i, 0);
    W32(g, LOW_MASK_PRIOR, 0);
    W32(g, LOW_MASK_CURRENT, 0);
    gcrt_os_mask_interrupts(g, 0xFFFFFFE0u);
    static const char* const chain[] = {"__OSDispatchInterrupt"};
    const u32 external = gcrt_hle_resolve_following("ExternalInterruptHandler", chain, 1);
    if (external) {
        const u32 a[2] = {4, external};
        call(g, s_sys.fn_set_exception_handler, a, 2);
    }
    ret(g, g->state.gpr[3]);
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * Reset (OSReset.c, OSResetSW.c)
 * --------------------------------------------------------------------------------------------- */

/* OSGetResetButtonState: SDK debouncing, without a button (released). The demo mode (0x800030E3, automatic reset
 * after N minutes) is not reproduced: reported once if requested. */
static bool hle_reset_button_state(GcrtCpu* g, u32 address) {
    (void)address;
    const u64 now = g->state.timebase;
    const bool pressed = false;
    bool state;
    if (pressed) {
        if (!s_sys.rsw_down) {
            s_sys.rsw_down = true;
            state = s_sys.rsw_hold_up != 0;
            s_sys.rsw_hold_down = now;
        } else {
            state = s_sys.rsw_hold_up != 0 || (now - s_sys.rsw_hold_down) > GCRT_TB_HZ / 10000u;
        }
    } else if (s_sys.rsw_down) {
        s_sys.rsw_down = false;
        state = s_sys.rsw_last;
        s_sys.rsw_hold_up = state ? now : 0;
    } else if (s_sys.rsw_hold_up && now - s_sys.rsw_hold_up < GCRT_TB_HZ / 25u) {
        state = true;
    } else {
        state = false;
        s_sys.rsw_hold_up = 0;
    }
    s_sys.rsw_last = state;
    if ((R8(g, LOW_GAME_CHOICE) & 0x1Fu) && !s_sys.game_choice_warned) {
        s_sys.game_choice_warned = true;
        fprintf(stderr, "gcrt native OS: demo mode (automatic reset, 0x800030E3) not reproduced\n");
    }
    ret(g, state ? 1u : 0u);
    return true;
}

/* OSGetResetCode: code stored by a software restart, otherwise PI reset register = 0 (cold boot). */
static bool hle_reset_code(GcrtCpu* g, u32 address) {
    (void)address;
    u32 code = 0;
    if (s_sys.a_reset_var && R32(g, s_sys.a_reset_var))
        code = R32(g, s_sys.a_reset_var + 4u) | 0x80000000u;
    ret(g, code);
    return true;
}

/* OSResetSystem(reset, resetCode, forceMenu): on the console, back to the menu or restart of the game. Phase 1: the
 * game stops cleanly (no restart within the same process). */
static bool hle_reset_system(GcrtCpu* g, u32 address) {
    (void)address;
    s_sys.resets++;
    fprintf(stderr, "gcrt native OS: OSResetSystem(%u, %08X, %u): the game requests a restart; stopping the game\n",
            arg(g, 0), arg(g, 1), arg(g, 2));
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "OSResetSystem: restart requested by the game, stopping");
    ret(g, 0);
    gcrt_cpu_request_stop(g);
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * Memory protection (OSMemory.c)
 * --------------------------------------------------------------------------------------------- */

static bool hle_init_memory_protection(GcrtCpu* g, u32 address) {
    const u32 size = gcrt_hle_catalog_size("__OSInitMemoryProtection");
    gcrt_os_mask_interrupts(g, M(0) | M(1) | M(2) | M(3));
    const u32 handler = fn_of("MEMIntrruptHandler");
    for (u32 i = 0; i < 5u; i++) {
        const u32 a[2] = {i, handler};
        if (handler)
            call(g, s_sys.fn_set_handler, a, 2);
    }
    u32 info = 0;
    if (s_sys.fn_register_reset &&
        gcrt_hle_decode_reg_at_call(g, address, size, s_sys.fn_register_reset, 0, 3, s_sys.sda, 0, &info))
        call(g, s_sys.fn_register_reset, &info, 1);
    /* The MI register 0x4028 and the BAT reprogramming (Config24MB/48MB through RealMode) have no effect on the
     * runtime (no MMU and no memory protection). */
    gcrt_os_unmask_interrupts(g, M(4));
    ret(g, g->state.gpr[3]);
    return true;
}

/* OnReset@OSMemory(final): at the final reset, the MEM interrupts are masked (no more MI write). */
static bool hle_memory_on_reset(GcrtCpu* g, u32 address) {
    (void)address;
    if (arg(g, 0))
        gcrt_os_mask_interrupts(g, MASK_MEM);
    ret(g, 1);
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * IPL SRAM and ROM (OSRtc.c)
 * --------------------------------------------------------------------------------------------- */

#define SCB_OFFSET 0x40u
#define SCB_ENABLED 0x44u
#define SCB_LOCKED 0x48u
#define SCB_SYNC 0x4Cu

/* Default content, identical to the emulated SRAM of the reference (hw/exi, removed; Dolphin's model, Sram.cpp):
 * flags 0x2C (stereo, setup done), language (GCRT_LANGUAGE, 0 = English), slot identifiers "DOLPHINSLOTA" /
 * "DOLPHINSLOTB" and their sums, computed checksums. Not persisted (like the reference): a setting changed by the
 * game holds until the stop. */
static void sram_default(GcrtCpu* g, u32 scb) {
    for (u32 i = 0; i < 0x40u; i++)
        W8(g, scb + i, 0);
    W8(g, scb + 0x12, s_sys.language);
    W8(g, scb + 0x13, 0x2C);
    static const char a[] = "DOLPHINSLOTA", b[] = "DOLPHINSLOTB";
    for (u32 i = 0; i < 12u; i++) {
        W8(g, scb + 0x14 + i, (u8)a[i]);
        W8(g, scb + 0x20 + i, (u8)b[i]);
    }
    W8(g, scb + 0x3A, 0x6E);
    W8(g, scb + 0x3B, 0x6D);
    u16 sum = 0, inv = 0;
    for (u32 i = 0x0C; i < 0x14u; i += 2u) {
        const u16 v = R16(g, scb + i);
        sum = (u16)(sum + v);
        inv = (u16)(inv + (u16)~v);
    }
    W16(g, scb + 0, sum);
    W16(g, scb + 2, inv);
}

/* __OSInitSram: the SRAM (64 bytes) is read into the control block, then the GBS mode is validated again. */
static bool hle_init_sram(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 scb = s_sys.scb;
    W32(g, scb + SCB_ENABLED, 0);
    W32(g, scb + SCB_LOCKED, 0);
    sram_default(g, scb);
    W32(g, scb + SCB_SYNC, 1);
    W32(g, scb + SCB_OFFSET, 0x40);
    if (s_sys.fn_get_gbs && s_sys.fn_set_gbs) {
        u32 gbs = 0;
        gcrt_guest_call(g, s_sys.fn_get_gbs, NULL, 0, &gbs);
        call(g, s_sys.fn_set_gbs, &gbs, 1);
    }
    ret(g, g->state.gpr[3]);
    return true;
}

/* UnlockSram(commit, offset): checksums and "write" (into the host SRAM, not persisted). */
static bool hle_unlock_sram(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 scb = s_sys.scb;
    const u32 commit = arg(g, 0), offset = arg(g, 1);
    if (commit) {
        if (offset == 0) {
            const u8 flags = R8(g, scb + 0x13);
            if ((flags & 3u) > 2u)
                W8(g, scb + 0x13, (u8)(flags & ~3u));
            u16 sum = 0, inv = 0;
            for (u32 i = 0x0C; i < 0x14u; i += 2u) {
                const u16 v = R16(g, scb + i);
                sum = (u16)(sum + v);
                inv = (u16)(inv + (u16)~v);
            }
            W16(g, scb + 0, sum);
            W16(g, scb + 2, inv);
        }
        if (offset < R32(g, scb + SCB_OFFSET))
            W32(g, scb + SCB_OFFSET, offset);
        W32(g, scb + SCB_SYNC, 1); /* WriteSram: immediate success */
        W32(g, scb + SCB_OFFSET, 0x40);
        s_sys.sram_writes++;
        GCRT_TRACE(GCRT_TRACE_OS, "SRAM written (offset %u): language %u, flags %02X", offset, R8(g, scb + 0x12),
                   R8(g, scb + 0x13));
    }
    W32(g, scb + SCB_LOCKED, 0);
    g->state.msr = (g->state.msr & ~0x8000u) | (R32(g, scb + SCB_ENABLED) ? 0x8000u : 0u); /* OSRestoreInterrupts */
    ret(g, R32(g, scb + SCB_SYNC));
    return true;
}

/* __OSReadROM(buffer, length, offset): the IPL ROM (fonts) is not on the disc: zeros, like the emulated reference;
 * returns TRUE. [hypothesis] enough as long as the game does not display text with these fonts (OSFont). */
static bool hle_read_rom(GcrtCpu* g, u32 address) {
    (void)address;
    const u32 buffer = arg(g, 0), length = arg(g, 1);
    u8* p = gcrt_native_ram(g, buffer, length);
    if (p)
        memset(p, 0, length);
    s_sys.rom_reads++;
    GCRT_TRACE(GCRT_TRACE_OS, "__OSReadROM(%08X, %u, %08X): IPL font absent, zeros", buffer, length, arg(g, 2));
    ret(g, 1);
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * DSP bootstrap by the OS (OSAudioSystem.c)
 * --------------------------------------------------------------------------------------------- */

/* __OSInitAudioSystem: on the console, the small DSP bootstrap program is copied to 0x81000000 (previous content
 * saved below the top of the arena then restored), two 0x20-byte ARAM DMAs copy it to the start of the ARAM, the DSP
 * runs it (mail 0x4348) then is reset (ROM, HALT, INIT). The native DSP (HLE microcodes) already starts from that
 * final state; what remains are the effects visible in memory: copy below the top of the arena, first 0x20 bytes of
 * the ARAM (AR_SIZE, setting of the ARAM controller, no longer has a recipient). No waiting. */
static bool hle_init_audio_system(GcrtCpu* g, u32 address) {
    (void)address;
    u32 arena_hi = 0;
    if (s_sys.fn_get_arena_hi)
        gcrt_guest_call(g, s_sys.fn_get_arena_hi, NULL, 0, &arena_hi);
    u8* src = gcrt_native_ram(g, 0x81000000u, 0x80);
    u8* save = arena_hi ? gcrt_native_ram(g, arena_hi - 0x80u, 0x80) : NULL;
    if (src && save)
        memmove(save, src, 0x80);
    u32 aram_size = 0;
    u8* aram = gcrt_native_aram(&aram_size);
    const u8* code = s_sys.dsp_init_code ? gcrt_native_ram(g, s_sys.dsp_init_code, 0x20) : NULL;
    if (aram && aram_size >= 0x20u && code)
        memcpy(aram, code, 0x20);
    GCRT_TRACE(GCRT_TRACE_OS, "native __OSInitAudioSystem: top of arena %08X, bootstrap program %08X", arena_hi,
               s_sys.dsp_init_code);
    ret(g, g->state.gpr[3]);
    return true;
}

static bool hle_stop_audio_system(GcrtCpu* g, u32 address) {
    (void)address;
    ret(g, g->state.gpr[3]); /* stopping the DSP at reset: not applicable (the game stops, OSResetSystem) */
    return true;
}

/* PPCSync: `sc` (bus synchronization): no effect. */
static bool hle_ppc_sync(GcrtCpu* g, u32 address) {
    (void)address;
    g->state.pc = g->state.lr;
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * EXI bus without devices (EXIBios.c)
 * --------------------------------------------------------------------------------------------- */

static void exi_set_interrupt_mask(GcrtCpu* g, s32 chan) {
    switch (chan) {
    case 0:
        if ((!s_sys.exi[0].exi_callback && !s_sys.exi[2].exi_callback) || (s_sys.exi[0].state & EXI_STATE_LOCKED))
            gcrt_os_mask_interrupts(g, M(9) | M(15));
        else
            gcrt_os_unmask_interrupts(g, M(9) | M(15));
        break;
    case 1:
        if (!s_sys.exi[1].exi_callback || (s_sys.exi[1].state & EXI_STATE_LOCKED))
            gcrt_os_mask_interrupts(g, M(12));
        else
            gcrt_os_unmask_interrupts(g, M(12));
        break;
    case 2: {
        const u32 table = s_sys.a_handler_table_var ? R32(g, s_sys.a_handler_table_var) : HANDLER_TABLE;
        if (!R32(g, table + 4u * 25u) || (s_sys.exi[2].state & EXI_STATE_LOCKED))
            gcrt_os_mask_interrupts(g, M(25)); /* PI_DEBUG */
        else
            gcrt_os_unmask_interrupts(g, M(25));
        break;
    }
    default:
        break;
    }
}

static bool exi_chan_ok(s32 chan) { return chan >= 0 && chan < 3; }

static bool hle_exi_init(GcrtCpu* g, u32 address) {
    const u32 size = gcrt_hle_catalog_size("EXIInit");
    gcrt_os_mask_interrupts(g, MASK_EXI);
    static const char* const names[3] = {"EXIIntrruptHandler", "TCIntrruptHandler", "EXTIntrruptHandler"};
    for (u32 n = 9; n <= 16u; n++) {
        const u32 handler = fn_of(names[(n - 9u) % 3u]);
        const u32 a[2] = {n, handler};
        if (handler)
            call(g, s_sys.fn_set_handler, a, 2);
    }
    memset(s_sys.exi, 0, sizeof(s_sys.exi));
    /* EXIGetID(0, 2, &IDSerialPort1): empty serial port -> 0. */
    const u32 get_id = fn_of("EXIGetID");
    u32 id_var = 0;
    if (get_id && gcrt_hle_decode_reg_at_call(g, address, size, get_id, 0, 5, s_sys.sda, 0, &id_var) && id_var)
        W32(g, id_var, 0);
    /* "Barnacle" debugger: never present; library banner. */
    s16 d = 0;
    if (s_sys.fn_register_version && gcrt_hle_decode_d_offset(g, address, size, 32, 13, 1, &d)) {
        const u32 banner = R32(g, s_sys.sda + (u32)(s32)d);
        call(g, s_sys.fn_register_version, &banner, 1);
    }
    ret(g, g->state.gpr[3]);
    return true;
}

static bool hle_exi_probe_internal(GcrtCpu* g, u32 address) { /* __EXIProbe(chan) */
    (void)address;
    s_sys.exi_probes++;
    ret(g, arg(g, 0) == 2u ? 1u : 0u);
    return true;
}

static bool hle_exi_probe(GcrtCpu* g, u32 address) { /* EXIProbe(chan) */
    (void)address;
    s_sys.exi_probes++;
    ret(g, 0);
    return true;
}

static bool hle_exi_probe_ex(GcrtCpu* g, u32 address) { /* EXIProbeEx(chan): -1 = no device */
    (void)address;
    s_sys.exi_probes++;
    ret(g, 0xFFFFFFFFu);
    return true;
}

static bool hle_exi_get_id(GcrtCpu* g, u32 address) { /* EXIGetID(chan, dev, *id) */
    (void)address;
    const u32 id = arg(g, 2);
    if (id)
        W32(g, id, 0);
    ret(g, 0);
    return true;
}

static bool hle_exi_attach(GcrtCpu* g, u32 address) { /* EXIAttach(chan, cb): nothing to attach */
    (void)address;
    ret(g, 0);
    return true;
}

static bool hle_exi_detach(GcrtCpu* g, u32 address) { /* EXIDetach(chan): never attached -> TRUE */
    (void)address;
    ret(g, 1);
    return true;
}

static bool hle_exi_get_state(GcrtCpu* g, u32 address) {
    (void)address;
    const s32 chan = (s32)arg(g, 0);
    ret(g, exi_chan_ok(chan) ? s_sys.exi[chan].state : 0u);
    return true;
}

static bool hle_exi_set_callback(GcrtCpu* g, u32 address) { /* EXISetExiCallback(chan, cb) -> previous */
    (void)address;
    const s32 chan = (s32)arg(g, 0);
    if (!exi_chan_ok(chan)) {
        ret(g, 0);
        return true;
    }
    const u32 prev = s_sys.exi[chan].exi_callback;
    s_sys.exi[chan].exi_callback = arg(g, 1);
    exi_set_interrupt_mask(g, chan != 2 ? chan : 0);
    ret(g, prev);
    return true;
}

static bool hle_exi_lock(GcrtCpu* g, u32 address) { /* EXILock(chan, dev, unlockedCallback) */
    (void)address;
    const s32 chan = (s32)arg(g, 0);
    const u32 dev = arg(g, 1), cb = arg(g, 2);
    if (!exi_chan_ok(chan)) {
        ret(g, 0);
        return true;
    }
    if (s_sys.exi[chan].state & EXI_STATE_LOCKED) {
        if (cb) {
            for (unsigned i = 0; i < s_sys.exi[chan].items; i++)
                if (s_sys.exi[chan].queue_dev[i] == dev) {
                    ret(g, 0);
                    return true;
                }
            if (s_sys.exi[chan].items < 3u) {
                s_sys.exi[chan].queue_cb[s_sys.exi[chan].items] = cb;
                s_sys.exi[chan].queue_dev[s_sys.exi[chan].items] = dev;
                s_sys.exi[chan].items++;
            }
        }
        ret(g, 0);
        return true;
    }
    s_sys.exi[chan].state |= EXI_STATE_LOCKED;
    s_sys.exi[chan].dev = dev;
    exi_set_interrupt_mask(g, chan);
    ret(g, 1);
    return true;
}

static bool hle_exi_unlock(GcrtCpu* g, u32 address) { /* EXIUnlock(chan) */
    (void)address;
    const s32 chan = (s32)arg(g, 0);
    if (!exi_chan_ok(chan) || !(s_sys.exi[chan].state & EXI_STATE_LOCKED)) {
        ret(g, 0);
        return true;
    }
    s_sys.exi[chan].state &= ~EXI_STATE_LOCKED;
    exi_set_interrupt_mask(g, chan);
    if (s_sys.exi[chan].items) {
        const u32 cb = s_sys.exi[chan].queue_cb[0];
        s_sys.exi[chan].items--;
        memmove(&s_sys.exi[chan].queue_cb[0], &s_sys.exi[chan].queue_cb[1], sizeof(u32) * s_sys.exi[chan].items);
        memmove(&s_sys.exi[chan].queue_dev[0], &s_sys.exi[chan].queue_dev[1], sizeof(u32) * s_sys.exi[chan].items);
        const u32 a[2] = {(u32)chan, 0};
        gcrt_native_enter(g);
        gcrt_native_callback(g, cb, a, 2);
        gcrt_native_return(g, 1);
        return true;
    }
    ret(g, 1);
    return true;
}

/* Selection, transfers, end of transfer, deselection: no device -> refused (FALSE). */
static bool hle_exi_false(GcrtCpu* g, u32 address) {
    (void)address;
    ret(g, 0);
    return true;
}

static bool hle_exi_clear_interrupts(GcrtCpu* g, u32 address) {
    (void)address;
    ret(g, 1);
    return true;
}

/* Dead functions (EXI interrupt handlers, OS dispatch): a call to one of them reveals a forgotten path. */
static bool hle_dead(GcrtCpu* g, u32 address) {
    fprintf(stderr, "gcrt native OS: dead function called at %08X (lr %08X): ignored\n", address, g->state.lr);
    ret(g, 0);
    return true;
}

/* --------------------------------------------------------------------------------------------- *
 * Installation
 * --------------------------------------------------------------------------------------------- */

void gcrt_native_os_sys_install(GcrtCpu* g) {
    memset(&s_sys, 0, sizeof(s_sys));
    if (!gcrt_native_os_active())
        return;
    s_sys.sda = gcrt_native_os_sda();
    const char* lang = getenv("GCRT_LANGUAGE");
    s_sys.language = (u8)(lang && lang[0] ? strtoul(lang, NULL, 0) : 0u);
    s_sys.fn_set_handler = fn_of("__OSSetInterruptHandler");
    s_sys.fn_set_exception_handler = fn_of("__OSSetExceptionHandler");
    s_sys.fn_register_reset = fn_of("OSRegisterResetFunction");
    s_sys.fn_register_version = fn_of("OSRegisterVersion");
    s_sys.fn_get_gbs = fn_of("OSGetGbsMode");
    s_sys.fn_set_gbs = fn_of("OSSetGbsMode");
    char* m = s_sys.missing;
    const size_t ms = sizeof(s_sys.missing);
    unsigned* n = &s_sys.replaced;

    /* Interrupt handler table: pointer read by __OSSetInterruptHandler (lwz r3, d(r13)). */
    s16 d = 0;
    const u32 set_handler = s_sys.fn_set_handler;
    if (set_handler && gcrt_hle_decode_d_offset(g, set_handler, gcrt_hle_catalog_size("__OSSetInterruptHandler"), 32, 13,
                                                0, &d))
        s_sys.a_handler_table_var = s_sys.sda + (u32)(s32)d;

    /* Interrupts. */
    *n += gcrt_native_replace(g, "__OSInterruptInit", "OS", hle_interrupt_init, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "__OSMaskInterrupts", "OS", hle_mask_interrupts, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "__OSUnmaskInterrupts", "OS", hle_unmask_interrupts, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "SetInterruptMask@OSInterrupt", "OS", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__OSDispatchInterrupt", "OS", hle_dead, NULL, 0, NULL, m, ms);

    /* Reset. */
    const u32 reset_code = fn_of("OSGetResetCode");
    {
        /* lis r3, H ; addi r3, r3, L ; lwz r0, 0(r3): address of the variables of the stored code. */
        const u32 w0 = reset_code ? R32(g, reset_code) : 0, w1 = reset_code ? R32(g, reset_code + 4u) : 0;
        if ((w0 >> 26) == 15u && (w1 >> 26) == 14u)
            s_sys.a_reset_var = ((w0 & 0xFFFFu) << 16) + (u32)(s32)(s16)(w1 & 0xFFFFu);
        else
            s_sys.a_reset_var = 0;
    }
    *n += gcrt_native_replace(g, "OSGetResetCode", "OS", hle_reset_code, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "OSGetResetButtonState", "OS", hle_reset_button_state, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "OSResetSystem", "OS", hle_reset_system, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__OSDoHotReset", "OS", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__OSResetSWInterruptHandler", "OS", hle_dead, NULL, 0, NULL, m, ms);

    /* Memory protection. */
    *n += gcrt_native_replace(g, "__OSInitMemoryProtection", "OS", hle_init_memory_protection, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "OnReset@OSMemory", "OS", hle_memory_on_reset, NULL, 0, NULL, m, ms);

    /* IPL SRAM and ROM: control block = argument of the DCInvalidateRange of __OSInitSram. */
    const u32 init_sram = fn_of("__OSInitSram");
    const u32 dcinv = fn_of("DCInvalidateRange");
    if (init_sram && dcinv &&
        gcrt_hle_decode_reg_at_call(g, init_sram, gcrt_hle_catalog_size("__OSInitSram"), dcinv, 0, 3, s_sys.sda, 0,
                                    &s_sys.scb) && s_sys.scb) {
        *n += gcrt_native_replace(g, "__OSInitSram", "OS", hle_init_sram, NULL, 0, NULL, m, ms);
        *n += gcrt_native_replace(g, "UnlockSram@OSRtc", "OS", hle_unlock_sram, NULL, 0, NULL, m, ms);
        gcrt_native_replace(g, "WriteSram@OSRtc", "OS", hle_dead, NULL, 0, NULL, m, ms);
        gcrt_native_replace(g, "WriteSramCallback@OSRtc", "OS", hle_dead, NULL, 0, NULL, m, ms);
    } else {
        fprintf(stderr, "gcrt native OS: SRAM control block not found (__OSInitSram): SRAM not replaced\n");
    }
    *n += gcrt_native_replace(g, "__OSReadROM", "OS", hle_read_rom, NULL, 0, NULL, m, ms);

    /* DSP bootstrap: program copied by the second memcpy of __OSInitAudioSystem; top of the arena = first
     * function called. */
    const u32 audio = fn_of("__OSInitAudioSystem");
    const u32 memcpy_fn = fn_of("memcpy");
    if (audio && memcpy_fn) {
        const u32 asize = gcrt_hle_catalog_size("__OSInitAudioSystem");
        gcrt_hle_decode_reg_at_call(g, audio, asize, memcpy_fn, 1, 4, s_sys.sda, 0, &s_sys.dsp_init_code);
        for (u32 off = 0; off + 4u <= asize; off += 4u) {
            const u32 w = R32(g, audio + off);
            if ((w >> 26) == 18u && (w & 3u) == 1u) {
                u32 li = w & 0x03FFFFFCu;
                if (li & 0x02000000u)
                    li |= 0xFC000000u;
                s_sys.fn_get_arena_hi = audio + off + li;
                break;
            }
        }
    }
    *n += gcrt_native_replace(g, "__OSInitAudioSystem", "OS", hle_init_audio_system, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "__OSStopAudioSystem", "OS", hle_stop_audio_system, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "PPCSync", "PPC", hle_ppc_sync, NULL, 0, NULL, m, ms);

    /* EXI bus. */
    *n += gcrt_native_replace(g, "EXIInit", "EXI", hle_exi_init, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "__EXIProbe", "EXI", hle_exi_probe_internal, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIProbe", "EXI", hle_exi_probe, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIProbeEx", "EXI", hle_exi_probe_ex, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIGetID", "EXI", hle_exi_get_id, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIAttach", "EXI", hle_exi_attach, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIDetach", "EXI", hle_exi_detach, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIGetState", "EXI", hle_exi_get_state, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXISetExiCallback", "EXI", hle_exi_set_callback, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXILock", "EXI", hle_exi_lock, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIUnlock", "EXI", hle_exi_unlock, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXISelect", "EXI", hle_exi_false, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIDeselect", "EXI", hle_exi_false, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIImm", "EXI", hle_exi_false, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIImmEx", "EXI", hle_exi_false, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIDma", "EXI", hle_exi_false, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXISync", "EXI", hle_exi_false, NULL, 0, NULL, m, ms);
    *n += gcrt_native_replace(g, "EXIClearInterrupts", "EXI", hle_exi_clear_interrupts, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "EXIIntrruptHandler", "EXI", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "TCIntrruptHandler", "EXI", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "EXTIntrruptHandler", "EXI", hle_dead, NULL, 0, NULL, m, ms);
    gcrt_native_replace(g, "__OSEnableBarnacle", "EXI", hle_dead, NULL, 0, NULL, m, ms);

    gcrt_trace_printf(GCRT_TRACE_NATIVE,
                      "native OS (system): %u functions replaced, missing: %s; handler table %08X, SRAM "
                      "%08X, reset code %08X, DSP bootstrap %08X",
                      s_sys.replaced, s_sys.missing[0] ? s_sys.missing : "none", s_sys.a_handler_table_var,
                      s_sys.scb, s_sys.a_reset_var, s_sys.dsp_init_code);
}

void gcrt_native_os_sys_report(FILE* out) {
    if (!gcrt_native_os_active())
        return;
    fprintf(out, "native OS (system): masks %llu/%llu; SRAM written %llu times; ROM read %llu times; "
                 "EXI probes %llu; restarts requested %llu; %u functions replaced, missing: %s\n",
            (unsigned long long)s_sys.masks, (unsigned long long)s_sys.unmasks,
            (unsigned long long)s_sys.sram_writes, (unsigned long long)s_sys.rom_reads,
            (unsigned long long)s_sys.exi_probes, (unsigned long long)s_sys.resets, s_sys.replaced,
            s_sys.missing[0] ? s_sys.missing : "none");
}
