/* Native memory card (milestone 7): replacement of the SDK CARD functions by files on the PC.
 *
 * Principle (the internal stage-1 plan, milestone 7): no emulated card nor EXI bus any more. The public API of the
 * CARD library is replaced (functions recognized by signature, hle/sdk_signatures.tsv); its internal layers
 * (__CARDStart, __CARDRead/__CARDWrite, __CARDUnlock and its DSP task, EXI*, SRAM access) are no longer called.
 * For the game, **a card is always inserted and formatted in slot A** (like a console with a healthy card); slot B
 * is empty. The content of the card is a folder on the PC (hle/card_model.c: one .gci file per file of the card,
 * safe writes); folder: gcrt_save_dir() (platform/gcrt_user_dir.h).
 *
 * Reproduced semantics, function by function (return codes, CARDFileInfo and CARDStat fields written, order of the
 * checks) after the public decompilation of the library (consulted only) cross-checked with the disassembly of the
 * game by the reverse agent (the internal SDK notes, milestone 7 sections):
 *   - synchronous versions: the operation is done within the call; the calling thread does not sleep (the SDK put
 *     it to sleep in __CARDSync until the callback), the result is the same;
 *   - asynchronous versions: the operation is done within the call, then the callback `callback(chan, result)` is
 *     delivered later as a native "EXI transfer complete" interrupt of the channel (hle/native_irq.c), in a
 *     temporary exception context like the SDK handlers; the channel stays busy (BUSY) until then
 *     [hypothesis: path not exercised by the first ported game, which only calls the synchronous versions];
 *   - no "card busy" state visible for the synchronous versions: the wrapper of a game that loops on BUSY therefore
 *     never loops.
 * Accepted differences (notes/runtime.md § 16, milestone 7): the card is never damaged, never of another encoding,
 * never removed; CARDFormat moves the old files into a subfolder instead of erasing them; the work area (workArea)
 * passed to CARDMount is not filled with the card's file system [hypothesis: the game does not read it itself]; the
 * operations are instantaneous (the console took seconds).
 *
 * Generic: no reference to a game. The game identifier (access control of the files) is read at address
 * 0x80000000, as __CARDSetDiskID does in CARDInit.
 *
 * Settings: gcrt_native_card_set_size (game) or GCRT_CARD_MBIT (tests); GCRT_CARD=absent (diagnostic: slot A
 * empty, like the emulated reference, which had no card). "card" trace (GCRT_TRACE=card): every call. */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "hle/card_model.h"
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/gcrt_save_file.h"
#include "platform/gcrt_user_dir.h"
#include "platform/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHANS 2
#define SECTOR GCRT_CARD_SECTOR_SIZE
#define CARD_READ_SIZE 512u    /* reads in multiples of 512 bytes */
#define CARD_COMMENT_SIZE 64u
#define GAME_CHOICE 0x800030E3u /* OS byte ("GameChoice"): bit 0x80 = cards forbidden */
#define DISK_ID 0x80000000u     /* __CARDSetDiskID((DVDDiskID*)OSPhysicalToCached(0)) */
#define OS_BUS_CLOCK 0x800000F8u

/* CARDFileInfo (0x14 bytes). */
#define FI_CHAN 0x00u
#define FI_FILENO 0x04u
#define FI_OFFSET 0x08u
#define FI_LENGTH 0x0Cu
#define FI_IBLOCK 0x10u
/* CARDStat (0x6C useful bytes). */
#define ST_FILENAME 0x00u
#define ST_LENGTH 0x20u
#define ST_TIME 0x24u
#define ST_GAMENAME 0x28u
#define ST_COMPANY 0x2Cu
#define ST_BANNER_FORMAT 0x2Eu
#define ST_ICON_ADDR 0x30u
#define ST_ICON_FORMAT 0x34u
#define ST_ICON_SPEED 0x36u
#define ST_COMMENT_ADDR 0x38u
#define ST_OFFSET_BANNER 0x3Cu
#define ST_OFFSET_BANNER_TLUT 0x40u
#define ST_OFFSET_ICON 0x44u
#define ST_OFFSET_ICON_TLUT 0x64u
#define ST_OFFSET_DATA 0x68u

/* OS EXI interrupts (__OS_INTERRUPT_EXI_0_TC = 10, EXI_1_TC = 13). */
#define OS_IRQ_EXI_TC(chan) (10u + 3u * (unsigned)(chan))

enum {
    F_INIT, F_PROBE, F_PROBE_EX, F_MOUNT, F_UNMOUNT, F_CHECK, F_FORMAT, F_FREE_BLOCKS, F_SECTOR_SIZE, F_OPEN, F_CLOSE,
    F_CREATE, F_READ, F_WRITE, F_DELETE, F_GET_STATUS, F_SET_STATUS, F_COUNT
};
static const char* const k_fn_names[F_COUNT] = {
    "CARDInit", "CARDProbe", "CARDProbeEx", "CARDMount", "CARDUnmount", "CARDCheck", "CARDFormat", "CARDFreeBlocks",
    "CARDGetSectorSize", "CARDOpen", "CARDClose", "CARDCreate", "CARDRead", "CARDWrite", "CARDDelete",
    "CARDGetStatus", "CARDSetStatus"};

typedef struct {
    bool attached; /* mounted (CARDMount) */
    bool busy;     /* asynchronous operation whose callback has not been delivered yet */
    u32 work_area, detach_callback;
} Chan;

typedef struct {
    bool initialized; /* CARDInit called */
    bool slot_a_empty; /* GCRT_CARD=absent: slot A empty (diagnostic, comparison with the reference without a card) */
    bool card_ready;  /* card model prepared */
    unsigned size_mbit;
    GcrtCard card; /* slot A */
    Chan chan[CHANS];
    int last_probe[CHANS]; /* last result of CARDProbeEx (traced only when it changes) */
    /* Report */
    u64 calls[F_COUNT];
    u64 async_calls, bytes_read, bytes_written;
    unsigned installed;
    char missing[1024];
} NativeCard;

static NativeCard s_card = {.size_mbit = 16, .last_probe = {1, 1}};

static u32 rd32(GcrtCpu* g, u32 a) { return mem_read32(&g->state, a); }
static void wr32(GcrtCpu* g, u32 a, u32 v) { mem_write32(&g->state, a, v); }
static void wr16(GcrtCpu* g, u32 a, u16 v) { mem_write16(&g->state, a, v); }
static void wr8(GcrtCpu* g, u32 a, u8 v) { mem_write8(&g->state, a, v); }
static u32 arg(GcrtCpu* g, int i) { return g->state.gpr[3 + i]; }

/* ---------------------------------------------------------------------------------------------
 * Setting (game extension point) and helpers
 * ------------------------------------------------------------------------------------------- */

void gcrt_native_card_set_size(unsigned size_mbit) {
    switch (size_mbit) {
    case 4: case 8: case 16: case 32: case 64: case 128:
        s_card.size_mbit = size_mbit;
        
        break;
    default:
        fprintf(stderr, "gcrt: card size %u Mbit refused (4, 8, 16, 32, 64 or 128)\n", size_mbit);
        break;
    }
}

/* Game identifier (DVDDiskID at 0x80000000: gameName[4], company[2]). */
static void disk_id(GcrtCpu* g, u8 game[4], u8 company[2]) {
    for (u32 i = 0; i < 4; i++)
        game[i] = mem_read8(&g->state, DISK_ID + i);
    for (u32 i = 0; i < 2; i++)
        company[i] = mem_read8(&g->state, DISK_ID + 4u + i);
}

/* OSTicksToSeconds(OSGetTime()): game time base / (bus clock / 4). */
static u32 os_seconds(GcrtCpu* g) {
    u32 bus = rd32(g, OS_BUS_CLOCK);
    if (bus == 0)
        bus = GCRT_BUS_HZ;
    return (u32)(g->state.timebase / (bus / 4u));
}

static bool game_choice_forbids(GcrtCpu* g) { return (mem_read8(&g->state, GAME_CHOICE) & 0x80u) != 0; }

/* Card present in the slot? Slot A: always, except GCRT_CARD=absent (diagnostic); slot B: never. */
static bool card_in_slot(s32 chan) { return chan == 0 && !s_card.slot_a_empty; }

/* Guest name (at most 32 useful bytes; the 33rd is used by __CARDCompareFileName). */
static void guest_name(GcrtCpu* g, u32 address, char out[GCRT_CARD_FILENAME_MAX + 2]) {
    unsigned i = 0;
    for (; i < GCRT_CARD_FILENAME_MAX + 1u; i++) {
        out[i] = (char)mem_read8(&g->state, address + i);
        if (!out[i])
            break;
    }
    out[i < GCRT_CARD_FILENAME_MAX + 1u ? i : GCRT_CARD_FILENAME_MAX + 1u] = '\0';
}

static size_t guest_strlen(GcrtCpu* g, u32 address, size_t max) {
    size_t n = 0;
    while (n < max && mem_read8(&g->state, address + (u32)n))
        n++;
    return n;
}

/* Prepares the model (first mount). */
static void ensure_card(void) {
    if (s_card.card_ready)
        return;
    s_card.card_ready = true;
    const char* env = getenv("GCRT_CARD_MBIT");
    if (env && env[0])
        gcrt_native_card_set_size((unsigned)strtoul(env, NULL, 10));
    gcrt_card_init(&s_card.card, s_card.size_mbit);
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native CARD: %u Mbit card (%u blocks) always inserted in slot A, "
                                         "folder %s", s_card.size_mbit, (unsigned)(s_card.card.cblock - 5u),
                      gcrt_save_dir()[0] ? gcrt_save_dir() : "(unavailable)");
}

/* __CARDGetControlBlock: valid channel, CARDInit done, card mounted, no operation in progress. */
static int control_block(s32 chan) {
    if (chan < 0 || chan >= CHANS || !s_card.initialized)
        return GCRT_CARD_FATAL_ERROR;
    if (!s_card.chan[chan].attached)
        return GCRT_CARD_NOCARD;
    if (s_card.chan[chan].busy)
        return GCRT_CARD_BUSY;
    return GCRT_CARD_READY;
}

/* ---------------------------------------------------------------------------------------------
 * Callbacks of the asynchronous versions (native EXI interrupt)
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    s32 chan;
    s32 result;
    u32 callback;
} AsyncDone;

static AsyncDone s_async[CHANS];

static void os_mask_call(GcrtCpu* g, GcrtOsFn which, unsigned irq) {
    const u32 fn = gcrt_native_os_fn(which);
    if (fn) {
        const u32 mask = 0x80000000u >> irq;
        gcrt_guest_call(g, fn, &mask, 1, NULL);
    }
}

static void on_async_done(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    const s32 chan = (s32)(uintptr_t)user;
    AsyncDone d = s_async[chan];
    s_card.chan[chan].busy = false;
    /* Like the EXI "transfer complete" handler: the interrupt is masked again once served. */
    os_mask_call(g, GCRT_OSFN_MASK_INTERRUPTS, interrupt);
    GCRT_TRACE(GCRT_TRACE_CARD, "asynchronous callback %08X(%d, %d)", d.callback, d.chan, d.result);
    if (!d.callback)
        return;
    const u32 tmp = gcrt_native_exception_context_begin(g);
    const u32 args[2] = {(u32)d.chan, (u32)d.result};
    gcrt_native_callback(g, d.callback, args, 2);
    gcrt_native_exception_context_end(g, tmp, context);
}

/* End of an asynchronous version: the result goes to the callback; the call returns READY (operation started). */
static u32 finish_async(GcrtCpu* g, s32 chan, u32 callback, s32 result) {
    s_card.async_calls++;
    s_card.chan[chan].busy = true;
    s_async[chan] = (AsyncDone){chan, result, callback};
    const unsigned irq = OS_IRQ_EXI_TC(chan);
    os_mask_call(g, GCRT_OSFN_UNMASK_INTERRUPTS, irq); /* like EXIDma/EXIImm with a callback */
    if (!gcrt_nirq_post(g, irq, g->ticks + GCRT_TB_HZ / 10000u, on_async_done, (void*)(uintptr_t)chan)) {
        s_card.chan[chan].busy = false;
        return (u32)GCRT_CARD_FATAL_ERROR;
    }
    return GCRT_CARD_READY;
}

/* ---------------------------------------------------------------------------------------------
 * Functions
 * ------------------------------------------------------------------------------------------- */

static bool finish(GcrtCpu* g, int fn, s32 result) {
    if (fn != F_PROBE_EX)
        GCRT_TRACE(GCRT_TRACE_CARD, "%s -> %d", k_fn_names[fn], result);
    gcrt_native_return(g, (u32)result);
    return true;
}

/* CARDInit(): first call only (the SDK tests the diskID of both channels). Reproduced: OSGetFontEncode (native, see
 * native_vi.c: it sets the OS encoding and its ParseString pointer on the first call), DSPInit and OSInitAlarm, which
 * do not concern the card but that the original calls (same state of the OS, the DSP and the alarms as before). Not
 * reproduced: __CARDEncode (the encoding of the native card is always that of the console), OSRegisterVersion
 * (banner text), queues and alarms of the card, reset function (OnReset unmounts the cards: nothing to do, every
 * write is already on disk). */
static bool hle_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_INIT]++;
    if (!s_card.initialized) {
        s_card.initialized = true;
        static const char* const keep[] = {"OSGetFontEncode", "DSPInit", "OSInitAlarm"};
        for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
            const u32 fn = gcrt_native_sdk_function(keep[i]);
            if (fn)
                gcrt_guest_call(g, fn, NULL, 0, NULL);
        }
        GCRT_TRACE(GCRT_TRACE_CARD, "native CARDInit (game %.4s%.2s)", (const char*)gcrt_native_ram(g, DISK_ID, 6),
                   (const char*)gcrt_native_ram(g, DISK_ID + 4u, 2));
    }
    gcrt_native_return(g, 0);
    return true;
}

/* CARDProbe(chan) -> BOOL: card present? */
static bool hle_probe(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_PROBE]++;
    const s32 chan = (s32)arg(g, 0);
    gcrt_native_return(g, (card_in_slot(chan) && !game_choice_forbids(g)) ? 1u : 0u);
    return true;
}

/* CARDProbeEx(chan, s32* memSize, s32* sectorSize): slot A = card present (size in Mbit, 8 KiB sector); slot B = no
 * card. Called every frame by some games: traced only when it changes. */
static bool hle_probe_ex(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_PROBE_EX]++;
    const s32 chan = (s32)arg(g, 0);
    const u32 mem_size = arg(g, 1), sector_size = arg(g, 2);
    s32 result;
    if (chan < 0 || chan >= CHANS)
        result = GCRT_CARD_FATAL_ERROR;
    else if (game_choice_forbids(g) || !card_in_slot(chan))
        result = GCRT_CARD_NOCARD;
    else {
        if (mem_size)
            wr32(g, mem_size, s_card.size_mbit);
        if (sector_size)
            wr32(g, sector_size, SECTOR);
        result = GCRT_CARD_READY;
    }
    if (chan >= 0 && chan < CHANS && s_card.last_probe[chan] != result) {
        s_card.last_probe[chan] = result;
        GCRT_TRACE(GCRT_TRACE_CARD, "CARDProbeEx(%d) -> %d%s", chan, result,
                   result == 0 ? " (card present)" : (result == GCRT_CARD_NOCARD ? " (no card)" : ""));
    }
    return finish(g, F_PROBE_EX, result);
}

/* Common mount: channel A only; the card is read again from the folder (files added outside the game are seen). */
static s32 mount(GcrtCpu* g, s32 chan, u32 work_area, u32 detach_callback) {
    if (chan < 0 || chan >= CHANS)
        return GCRT_CARD_FATAL_ERROR;
    if (game_choice_forbids(g))
        return GCRT_CARD_NOCARD;
    if (s_card.chan[chan].busy)
        return GCRT_CARD_BUSY;
    if (!card_in_slot(chan))
        return GCRT_CARD_NOCARD; /* EXIAttach fails: nothing in the slot */
    ensure_card();
    u8 game[4], company[2];
    disk_id(g, game, company);
    const char* dir = gcrt_save_dir();
    const s32 result = dir[0] ? gcrt_card_load(&s_card.card, dir, game, company) : GCRT_CARD_IOERROR;
    if (result < 0) {
        s_card.chan[chan].attached = false; /* DoUnmount(chan, result) */
        return result;
    }
    s_card.chan[chan].attached = true;
    s_card.chan[chan].work_area = work_area;
    s_card.chan[chan].detach_callback = detach_callback;
    return GCRT_CARD_READY;
}

/* CARDMount(chan, workArea, detachCallback) */
static bool hle_mount(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_MOUNT]++;
    return finish(g, F_MOUNT, mount(g, (s32)arg(g, 0), arg(g, 1), arg(g, 2)));
}

/* CARDMountAsync(chan, workArea, detachCallback, attachCallback) */
static bool hle_mount_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_MOUNT]++;
    const s32 chan = (s32)arg(g, 0);
    if (chan < 0 || chan >= CHANS || game_choice_forbids(g) || s_card.chan[chan].busy || !card_in_slot(chan))
        return finish(g, F_MOUNT, mount(g, chan, arg(g, 1), arg(g, 2)));
    const s32 result = mount(g, chan, arg(g, 1), arg(g, 2));
    return finish(g, F_MOUNT, (s32)finish_async(g, chan, arg(g, 3), result));
}

/* CARDUnmount(chan) */
static bool hle_unmount(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_UNMOUNT]++;
    const s32 chan = (s32)arg(g, 0);
    s32 result = control_block(chan);
    if (result >= 0) {
        s_card.chan[chan].attached = false;
        s_card.chan[chan].work_area = 0;
        s_card.chan[chan].detach_callback = 0;
    }
    return finish(g, F_UNMOUNT, result);
}

/* CARDCheck(chan), CARDCheckEx(chan, s32* xferBytes): the native card is always consistent. */
static s32 check(GcrtCpu* g, s32 chan, u32 xfer_bytes) {
    const s32 result = control_block(chan);
    if (result >= 0 && xfer_bytes)
        wr32(g, xfer_bytes, 0);
    return result;
}

static bool hle_check(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_CHECK]++;
    return finish(g, F_CHECK, check(g, (s32)arg(g, 0), 0));
}

static bool hle_check_ex(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_CHECK]++;
    return finish(g, F_CHECK, check(g, (s32)arg(g, 0), arg(g, 1)));
}

/* CARDCheckExAsync(chan, xferBytes, callback) / CARDCheckAsync(chan, callback) */
static bool check_async(GcrtCpu* g, s32 chan, u32 xfer_bytes, u32 callback) {
    s_card.calls[F_CHECK]++;
    const s32 result = check(g, chan, xfer_bytes);
    if (result < 0)
        return finish(g, F_CHECK, result);
    return finish(g, F_CHECK, (s32)finish_async(g, chan, callback, GCRT_CARD_READY));
}

static bool hle_check_ex_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    return check_async(g, (s32)arg(g, 0), arg(g, 1), arg(g, 2));
}

static bool hle_check_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    return check_async(g, (s32)arg(g, 0), 0, arg(g, 1));
}

/* CARDFormat(chan); __CARDFormatRegionAsync(chan, encode, callback); CARDFormatAsync(chan, callback). */
static s32 format(s32 chan) {
    s32 result = control_block(chan);
    if (result >= 0)
        result = gcrt_card_format(&s_card.card);
    return result;
}

static bool hle_format(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_FORMAT]++;
    return finish(g, F_FORMAT, format((s32)arg(g, 0)));
}

static bool format_async(GcrtCpu* g, s32 chan, u32 callback) {
    s_card.calls[F_FORMAT]++;
    s32 result = control_block(chan);
    if (result < 0)
        return finish(g, F_FORMAT, result);
    result = gcrt_card_format(&s_card.card);
    return finish(g, F_FORMAT, (s32)finish_async(g, chan, callback, result));
}

static bool hle_format_region_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    return format_async(g, (s32)arg(g, 0), arg(g, 2));
}

static bool hle_format_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    return format_async(g, (s32)arg(g, 0), arg(g, 1));
}

/* CARDFreeBlocks(chan, s32* bytesNotUsed, s32* filesNotUsed) */
static bool hle_free_blocks(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_FREE_BLOCKS]++;
    const s32 chan = (s32)arg(g, 0);
    const s32 result = control_block(chan);
    if (result >= 0) {
        s32 bytes = 0, files = 0;
        gcrt_card_free_space(&s_card.card, &bytes, &files);
        if (arg(g, 1))
            wr32(g, arg(g, 1), (u32)bytes);
        if (arg(g, 2))
            wr32(g, arg(g, 2), (u32)files);
        GCRT_TRACE(GCRT_TRACE_CARD, "CARDFreeBlocks: %d bytes (%d blocks), %d free entries", bytes,
                   bytes / (s32)SECTOR, files);
    }
    return finish(g, F_FREE_BLOCKS, result);
}

/* CARDGetSectorSize(chan, u32* size) */
static bool hle_get_sector_size(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_SECTOR_SIZE]++;
    const s32 result = control_block((s32)arg(g, 0));
    if (result >= 0 && arg(g, 1))
        wr32(g, arg(g, 1), SECTOR);
    return finish(g, F_SECTOR_SIZE, result);
}

/* CARDOpen(chan, fileName, CARDFileInfo*) */
static bool hle_open(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_OPEN]++;
    const s32 chan = (s32)arg(g, 0);
    const u32 fi = arg(g, 2);
    char name[GCRT_CARD_FILENAME_MAX + 2];
    guest_name(g, arg(g, 1), name);
    wr32(g, fi + FI_CHAN, (u32)-1);
    s32 result = control_block(chan);
    if (result >= 0) {
        u8 game[4], company[2];
        disk_id(g, game, company);
        int no = -1;
        result = gcrt_card_find(&s_card.card, name, game, company, &no);
        if (result >= 0) {
            const GcrtCardFile* f = gcrt_card_file(&s_card.card, no);
            const u16 start = gcrt_card_be16(f->ent + GCRT_DIR_START_BLOCK);
            if (!gcrt_card_valid_block(&s_card.card, start)) {
                result = GCRT_CARD_BROKEN;
            } else {
                wr32(g, fi + FI_CHAN, (u32)chan);
                wr32(g, fi + FI_FILENO, (u32)no);
                wr32(g, fi + FI_OFFSET, 0);
                wr16(g, fi + FI_IBLOCK, start);
            }
        }
        GCRT_TRACE(GCRT_TRACE_CARD, "CARDOpen(%d, \"%s\"): entry %d", chan, name, result >= 0 ? no : -1);
    }
    return finish(g, F_OPEN, result);
}

/* CARDClose(CARDFileInfo*) */
static bool hle_close(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_CLOSE]++;
    const u32 fi = arg(g, 0);
    const s32 result = control_block((s32)rd32(g, fi + FI_CHAN));
    if (result >= 0)
        wr32(g, fi + FI_CHAN, (u32)-1);
    return finish(g, F_CLOSE, result);
}

/* CARDCreate(chan, fileName, size, CARDFileInfo*) and CARDCreateAsync(..., callback): returns READY if the file is
 * created (sync) or if the creation is started (async, *launched*). */
static s32 create(GcrtCpu* g, s32 chan, u32 name_addr, u32 size, u32 fi, bool* launched) {
    *launched = false;
    if (guest_strlen(g, name_addr, GCRT_CARD_FILENAME_MAX + 1u) > GCRT_CARD_FILENAME_MAX)
        return GCRT_CARD_NAMETOOLONG;
    s32 result = control_block(chan);
    if (result < 0)
        return result;
    if (size == 0 || size % SECTOR)
        return GCRT_CARD_FATAL_ERROR;
    char name[GCRT_CARD_FILENAME_MAX + 2];
    guest_name(g, name_addr, name);
    u8 game[4], company[2];
    disk_id(g, game, company);
    int no = -1;
    result = gcrt_card_create(&s_card.card, name, size, game, company, os_seconds(g), &no);
    GCRT_TRACE(GCRT_TRACE_CARD, "CARDCreate(%d, \"%s\", 0x%X): %d (entry %d)", chan, name, size, result, no);
    if (result == GCRT_CARD_EXIST || result == GCRT_CARD_NOENT || result == GCRT_CARD_INSSPACE)
        return result; /* refused before the blocks are allocated */
    *launched = true;
    if (result >= 0) {
        const GcrtCardFile* f = gcrt_card_file(&s_card.card, no);
        wr32(g, fi + FI_CHAN, (u32)chan);
        wr32(g, fi + FI_FILENO, (u32)no);
        wr32(g, fi + FI_OFFSET, 0);
        wr16(g, fi + FI_IBLOCK, gcrt_card_be16(f->ent + GCRT_DIR_START_BLOCK));
    }
    return result;
}

static bool hle_create(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_CREATE]++;
    bool launched;
    return finish(g, F_CREATE, create(g, (s32)arg(g, 0), arg(g, 1), arg(g, 2), arg(g, 3), &launched));
}

static bool hle_create_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_CREATE]++;
    bool launched;
    const s32 chan = (s32)arg(g, 0);
    const s32 result = create(g, chan, arg(g, 1), arg(g, 2), arg(g, 3), &launched);
    if (!launched)
        return finish(g, F_CREATE, result);
    return finish(g, F_CREATE, (s32)finish_async(g, chan, arg(g, 4), result));
}

/* __CARDSeek: checks and positioning of CARDFileInfo (fields written like the SDK). */
static s32 seek(GcrtCpu* g, u32 fi, s32 length, s32 offset, s32* chan_out, int* file_no_out) {
    const s32 chan = (s32)rd32(g, fi + FI_CHAN);
    s32 result = control_block(chan);
    if (result < 0)
        return result;
    GcrtCard* card = &s_card.card;
    const s32 file_no = (s32)rd32(g, fi + FI_FILENO);
    u16 iblock = mem_read16(&g->state, fi + FI_IBLOCK);
    s32 fi_offset = (s32)rd32(g, fi + FI_OFFSET);
    if (!gcrt_card_valid_block(card, iblock) || (s32)(card->cblock * SECTOR) <= fi_offset)
        return GCRT_CARD_FATAL_ERROR;
    const GcrtCardFile* f = gcrt_card_file(card, file_no);
    if (!f)
        return GCRT_CARD_FATAL_ERROR; /* [hypothesis] the SDK would read outside the directory */
    const s32 file_size = (s32)(gcrt_card_be16(f->ent + GCRT_DIR_LENGTH) * SECTOR);
    if (file_size <= offset || file_size < offset + length)
        return GCRT_CARD_LIMIT;
    wr32(g, fi + FI_LENGTH, (u32)length);
    if (offset < fi_offset) {
        fi_offset = 0;
        iblock = gcrt_card_be16(f->ent + GCRT_DIR_START_BLOCK);
        wr32(g, fi + FI_OFFSET, 0);
        wr16(g, fi + FI_IBLOCK, iblock);
        if (!gcrt_card_valid_block(card, iblock))
            return GCRT_CARD_BROKEN;
    }
    while ((u32)fi_offset < ((u32)offset & ~(SECTOR - 1u))) {
        fi_offset += (s32)SECTOR;
        iblock = gcrt_card_next_block(card, iblock);
        wr32(g, fi + FI_OFFSET, (u32)fi_offset);
        wr16(g, fi + FI_IBLOCK, iblock);
        if (!gcrt_card_valid_block(card, iblock))
            return GCRT_CARD_BROKEN;
    }
    wr32(g, fi + FI_OFFSET, (u32)offset);
    *chan_out = chan;
    *file_no_out = file_no;
    return GCRT_CARD_READY;
}

/* CARDFileInfo fields at the end of a read (ReadCallback); BROKEN if the block chain breaks. */
static s32 read_epilogue(GcrtCpu* g, u32 fi) {
    s32 length = (s32)rd32(g, fi + FI_LENGTH);
    s32 offset = (s32)rd32(g, fi + FI_OFFSET);
    u16 iblock = mem_read16(&g->state, fi + FI_IBLOCK);
    for (;;) {
        const s32 chunk = (s32)(((u32)offset + SECTOR) & ~(SECTOR - 1u)) - offset;
        length -= chunk;
        wr32(g, fi + FI_LENGTH, (u32)length);
        if (length <= 0)
            return GCRT_CARD_READY;
        offset += chunk;
        iblock = gcrt_card_next_block(&s_card.card, iblock);
        wr32(g, fi + FI_OFFSET, (u32)offset);
        wr16(g, fi + FI_IBLOCK, iblock);
        if (!gcrt_card_valid_block(&s_card.card, iblock))
            return GCRT_CARD_BROKEN;
    }
}

/* CARDRead(fileInfo, buf, length, offset) / CARDReadAsync(..., callback) */
static s32 card_read(GcrtCpu* g, u32 fi, u32 buf, s32 length, s32 offset, bool* launched, s32* chan_out) {
    *launched = false;
    if (((u32)offset & (CARD_READ_SIZE - 1u)) || ((u32)length & (CARD_READ_SIZE - 1u)))
        return GCRT_CARD_FATAL_ERROR;
    s32 chan = -1;
    int no = -1;
    s32 result = seek(g, fi, length, offset, &chan, &no);
    if (result < 0)
        return result;
    *chan_out = chan;
    u8 game[4], company[2];
    disk_id(g, game, company);
    result = gcrt_card_readable(gcrt_card_file(&s_card.card, no), game, company);
    if (result < 0)
        return result;
    *launched = true;
    u8* dst = length > 0 ? gcrt_native_ram(g, buf, (u32)length) : NULL;
    if (length > 0 && !dst)
        return GCRT_CARD_FATAL_ERROR;
    if (length > 0)
        result = gcrt_card_read(&s_card.card, no, (u32)offset, dst, (u32)length);
    if (result >= 0) {
        s_card.bytes_read += (u64)(length > 0 ? length : 0);
        result = read_epilogue(g, fi);
    }
    GCRT_TRACE(GCRT_TRACE_CARD, "CARDRead(entry %d, 0x%X bytes at 0x%X -> %08X): %d", no, (u32)length,
               (u32)offset, buf, result);
    return result;
}

static bool hle_read(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_READ]++;
    bool launched;
    s32 chan;
    return finish(g, F_READ, card_read(g, arg(g, 0), arg(g, 1), (s32)arg(g, 2), (s32)arg(g, 3), &launched, &chan));
}

static bool hle_read_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_READ]++;
    bool launched;
    s32 chan = -1;
    const s32 result = card_read(g, arg(g, 0), arg(g, 1), (s32)arg(g, 2), (s32)arg(g, 3), &launched, &chan);
    if (!launched)
        return finish(g, F_READ, result);
    return finish(g, F_READ, (s32)finish_async(g, chan, arg(g, 4), result));
}

/* CARDFileInfo fields at the end of a write (WriteCallback). */
static s32 write_epilogue(GcrtCpu* g, u32 fi) {
    s32 length = (s32)rd32(g, fi + FI_LENGTH);
    s32 offset = (s32)rd32(g, fi + FI_OFFSET);
    u16 iblock = mem_read16(&g->state, fi + FI_IBLOCK);
    for (;;) {
        length -= (s32)SECTOR;
        wr32(g, fi + FI_LENGTH, (u32)length);
        if (length <= 0)
            return GCRT_CARD_READY;
        offset += (s32)SECTOR;
        iblock = gcrt_card_next_block(&s_card.card, iblock);
        wr32(g, fi + FI_OFFSET, (u32)offset);
        wr16(g, fi + FI_IBLOCK, iblock);
        if (!gcrt_card_valid_block(&s_card.card, iblock))
            return GCRT_CARD_BROKEN;
    }
}

/* CARDWrite(fileInfo, buf, length, offset) / CARDWriteAsync(..., callback). Writes the file on the PC immediately
 * (safe write) and updates the time of the entry, like WriteCallback. */
static s32 card_write(GcrtCpu* g, u32 fi, u32 buf, s32 length, s32 offset, bool* launched, s32* chan_out) {
    *launched = false;
    s32 chan = -1;
    int no = -1;
    s32 result = seek(g, fi, length, offset, &chan, &no);
    if (result < 0)
        return result;
    *chan_out = chan;
    if (((u32)offset & (SECTOR - 1u)) || ((u32)length & (SECTOR - 1u)))
        return GCRT_CARD_FATAL_ERROR;
    u8 game[4], company[2];
    disk_id(g, game, company);
    result = gcrt_card_writable(gcrt_card_file(&s_card.card, no), game, company);
    if (result < 0)
        return result;
    *launched = true;
    /* The SDK always erases then writes at least the first sector, even for a zero length. */
    const u32 bytes = length > 0 ? (u32)length : SECTOR;
    const u8* src = gcrt_native_ram(g, buf, bytes);
    if (!src)
        return GCRT_CARD_FATAL_ERROR;
    result = gcrt_card_write(&s_card.card, no, (u32)offset, src, bytes, os_seconds(g));
    if (result >= 0) {
        s_card.bytes_written += bytes;
        result = write_epilogue(g, fi);
    }
    GCRT_TRACE(GCRT_TRACE_CARD, "CARDWrite(entry %d, 0x%X bytes at 0x%X <- %08X): %d", no, (u32)length,
               (u32)offset, buf, result);
    return result;
}

static bool hle_write(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_WRITE]++;
    bool launched;
    s32 chan;
    return finish(g, F_WRITE, card_write(g, arg(g, 0), arg(g, 1), (s32)arg(g, 2), (s32)arg(g, 3), &launched, &chan));
}

static bool hle_write_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_WRITE]++;
    bool launched;
    s32 chan = -1;
    const s32 result = card_write(g, arg(g, 0), arg(g, 1), (s32)arg(g, 2), (s32)arg(g, 3), &launched, &chan);
    if (!launched)
        return finish(g, F_WRITE, result);
    return finish(g, F_WRITE, (s32)finish_async(g, chan, arg(g, 4), result));
}

/* CARDDelete(chan, fileName) / CARDDeleteAsync(chan, fileName, callback) */
static s32 card_delete(GcrtCpu* g, s32 chan, u32 name_addr, bool* launched) {
    *launched = false;
    s32 result = control_block(chan);
    if (result < 0)
        return result;
    char name[GCRT_CARD_FILENAME_MAX + 2];
    guest_name(g, name_addr, name);
    u8 game[4], company[2];
    disk_id(g, game, company);
    int no = -1;
    result = gcrt_card_find(&s_card.card, name, game, company, &no);
    if (result >= 0) {
        *launched = true;
        result = gcrt_card_delete(&s_card.card, no);
    }
    GCRT_TRACE(GCRT_TRACE_CARD, "CARDDelete(%d, \"%s\"): %d (entry %d)", chan, name, result, no);
    return result;
}

static bool hle_delete(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_DELETE]++;
    bool launched;
    return finish(g, F_DELETE, card_delete(g, (s32)arg(g, 0), arg(g, 1), &launched));
}

static bool hle_delete_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_DELETE]++;
    bool launched;
    const s32 chan = (s32)arg(g, 0);
    const s32 result = card_delete(g, chan, arg(g, 1), &launched);
    if (!launched)
        return finish(g, F_DELETE, result);
    return finish(g, F_DELETE, (s32)finish_async(g, chan, arg(g, 2), result));
}

/* UpdateIconOffsets(entry, stat): offsets of the banner, the icons and the data, written into the game's CARDStat;
 * if the entry has no icon, bannerFormat/iconFormat/iconSpeed of the CARDStat are reset to 0. */
static void update_icon_offsets(GcrtCpu* g, const u8* ent, u32 stat) {
    u32 offset = gcrt_card_be32(ent + GCRT_DIR_ICON_ADDR);
    if (offset == 0xFFFFFFFFu) {
        wr8(g, stat + ST_BANNER_FORMAT, 0);
        wr16(g, stat + ST_ICON_FORMAT, 0);
        wr16(g, stat + ST_ICON_SPEED, 0);
        offset = 0;
    }
    bool icon_tlut = false;
    switch (ent[GCRT_DIR_BANNER_FORMAT] & 3u) {
    case 1: /* C8 */
        wr32(g, stat + ST_OFFSET_BANNER, offset);
        offset += 96u * 32u;
        wr32(g, stat + ST_OFFSET_BANNER_TLUT, offset);
        offset += 2u * 256u;
        break;
    case 2: /* RGB5A3 */
        wr32(g, stat + ST_OFFSET_BANNER, offset);
        offset += 2u * 96u * 32u;
        wr32(g, stat + ST_OFFSET_BANNER_TLUT, 0xFFFFFFFFu);
        break;
    default:
        wr32(g, stat + ST_OFFSET_BANNER, 0xFFFFFFFFu);
        wr32(g, stat + ST_OFFSET_BANNER_TLUT, 0xFFFFFFFFu);
        break;
    }
    const u16 icon_format = gcrt_card_be16(ent + GCRT_DIR_ICON_FORMAT);
    for (u32 i = 0; i < 8u; i++) {
        switch ((icon_format >> (2u * i)) & 3u) {
        case 1: /* C8 */
            wr32(g, stat + ST_OFFSET_ICON + 4u * i, offset);
            offset += 32u * 32u;
            icon_tlut = true;
            break;
        case 2: /* RGB5A3 */
            wr32(g, stat + ST_OFFSET_ICON + 4u * i, offset);
            offset += 2u * 32u * 32u;
            break;
        default:
            wr32(g, stat + ST_OFFSET_ICON + 4u * i, 0xFFFFFFFFu);
            break;
        }
    }
    if (icon_tlut) {
        wr32(g, stat + ST_OFFSET_ICON_TLUT, offset);
        offset += 2u * 256u;
    } else {
        wr32(g, stat + ST_OFFSET_ICON_TLUT, 0xFFFFFFFFu);
    }
    wr32(g, stat + ST_OFFSET_DATA, offset);
}

/* CARDGetStatus(chan, fileNo, CARDStat*) */
static bool hle_get_status(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_GET_STATUS]++;
    const s32 chan = (s32)arg(g, 0), no = (s32)arg(g, 1);
    const u32 stat = arg(g, 2);
    if (no < 0 || no >= GCRT_CARD_MAX_FILE)
        return finish(g, F_GET_STATUS, GCRT_CARD_FATAL_ERROR);
    s32 result = control_block(chan);
    if (result >= 0) {
        u8 game[4], company[2];
        disk_id(g, game, company);
        const GcrtCardFile* f = gcrt_card_file(&s_card.card, no);
        result = gcrt_card_readable(f, game, company);
        if (result >= 0) {
            const u8* ent = f->ent;
            for (u32 i = 0; i < 4u; i++)
                wr8(g, stat + ST_GAMENAME + i, ent[GCRT_DIR_GAMENAME + i]);
            for (u32 i = 0; i < 2u; i++)
                wr8(g, stat + ST_COMPANY + i, ent[GCRT_DIR_COMPANY + i]);
            wr32(g, stat + ST_LENGTH, (u32)gcrt_card_be16(ent + GCRT_DIR_LENGTH) * SECTOR);
            for (u32 i = 0; i < GCRT_CARD_FILENAME_MAX; i++)
                wr8(g, stat + ST_FILENAME + i, ent[GCRT_DIR_FILENAME + i]);
            wr32(g, stat + ST_TIME, gcrt_card_be32(ent + GCRT_DIR_TIME));
            wr8(g, stat + ST_BANNER_FORMAT, ent[GCRT_DIR_BANNER_FORMAT]);
            wr32(g, stat + ST_ICON_ADDR, gcrt_card_be32(ent + GCRT_DIR_ICON_ADDR));
            wr16(g, stat + ST_ICON_FORMAT, gcrt_card_be16(ent + GCRT_DIR_ICON_FORMAT));
            wr16(g, stat + ST_ICON_SPEED, gcrt_card_be16(ent + GCRT_DIR_ICON_SPEED));
            wr32(g, stat + ST_COMMENT_ADDR, gcrt_card_be32(ent + GCRT_DIR_COMMENT_ADDR));
            update_icon_offsets(g, ent, stat);
        }
    }
    if (result != GCRT_CARD_NOFILE)
        GCRT_TRACE(GCRT_TRACE_CARD, "CARDGetStatus(%d, entry %d): %d", chan, no, result);
    return finish(g, F_GET_STATUS, result);
}

/* CARDSetStatus(chan, fileNo, CARDStat*) / CARDSetStatusAsync(..., callback) */
static s32 set_status(GcrtCpu* g, s32 chan, s32 no, u32 stat, bool* launched) {
    *launched = false;
    const u32 icon_addr = rd32(g, stat + ST_ICON_ADDR);
    const u32 comment_addr = rd32(g, stat + ST_COMMENT_ADDR);
    if (no < 0 || no >= GCRT_CARD_MAX_FILE || (icon_addr != 0xFFFFFFFFu && icon_addr >= CARD_READ_SIZE) ||
        (comment_addr != 0xFFFFFFFFu && comment_addr % SECTOR > SECTOR - CARD_COMMENT_SIZE))
        return GCRT_CARD_FATAL_ERROR;
    s32 result = control_block(chan);
    if (result < 0)
        return result;
    u8 game[4], company[2];
    disk_id(g, game, company);
    const GcrtCardFile* f = gcrt_card_file(&s_card.card, no);
    result = gcrt_card_writable(f, game, company);
    if (result < 0)
        return result;
    *launched = true;
    u8 ent[GCRT_CARD_DIRENT_SIZE];
    memcpy(ent, f->ent, sizeof(ent));
    ent[GCRT_DIR_BANNER_FORMAT] = mem_read8(&g->state, stat + ST_BANNER_FORMAT);
    gcrt_card_put32(ent + GCRT_DIR_ICON_ADDR, icon_addr);
    gcrt_card_put16(ent + GCRT_DIR_ICON_FORMAT, mem_read16(&g->state, stat + ST_ICON_FORMAT));
    gcrt_card_put16(ent + GCRT_DIR_ICON_SPEED, mem_read16(&g->state, stat + ST_ICON_SPEED));
    gcrt_card_put32(ent + GCRT_DIR_COMMENT_ADDR, comment_addr);
    update_icon_offsets(g, ent, stat);
    if (icon_addr == 0xFFFFFFFFu) /* CARDSetIconSpeed(ent, 0, CARD_STAT_SPEED_FAST) */
        gcrt_card_put16(ent + GCRT_DIR_ICON_SPEED, (u16)((gcrt_card_be16(ent + GCRT_DIR_ICON_SPEED) & ~3u) | 1u));
    gcrt_card_put32(ent + GCRT_DIR_TIME, os_seconds(g));
    result = gcrt_card_set_entry(&s_card.card, no, ent);
    GCRT_TRACE(GCRT_TRACE_CARD, "CARDSetStatus(%d, entry %d: banner %u, icon %08X/%04X/%04X, comment %08X): %d",
               chan, no, ent[GCRT_DIR_BANNER_FORMAT], icon_addr, gcrt_card_be16(ent + GCRT_DIR_ICON_FORMAT),
               gcrt_card_be16(ent + GCRT_DIR_ICON_SPEED), comment_addr, result);
    return result;
}

static bool hle_set_status(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_SET_STATUS]++;
    bool launched;
    return finish(g, F_SET_STATUS, set_status(g, (s32)arg(g, 0), (s32)arg(g, 1), arg(g, 2), &launched));
}

static bool hle_set_status_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_card.calls[F_SET_STATUS]++;
    bool launched;
    const s32 chan = (s32)arg(g, 0);
    const s32 result = set_status(g, chan, (s32)arg(g, 1), arg(g, 2), &launched);
    if (!launched)
        return finish(g, F_SET_STATUS, result);
    return finish(g, F_SET_STATUS, (s32)finish_async(g, chan, arg(g, 3), result));
}

/* ---------------------------------------------------------------------------------------------
 * Installation and report
 * ------------------------------------------------------------------------------------------- */

typedef struct {
    const char* name;
    GcrtHleFn fn;
    const char* anchor; /* shared signature: copy that immediately follows this function in its source file */
} Replacement;

/* The 0x48-byte synchronous wrappers have the same masked code in pairs (CARDRead/CARDWrite,
 * CARDMount/CARDSetStatus...): each one follows its asynchronous version in the SDK source file. */
static const Replacement k_replacements[] = {
    {"CARDInit", hle_init, NULL},
    {"CARDProbe", hle_probe, NULL},
    {"CARDProbeEx", hle_probe_ex, NULL},
    {"CARDMountAsync", hle_mount_async, NULL},
    {"CARDMount", hle_mount, "CARDMountAsync"},
    {"CARDUnmount", hle_unmount, NULL},
    {"CARDCheckExAsync", hle_check_ex_async, NULL},
    {"CARDCheckAsync", hle_check_async, NULL},
    {"CARDCheckEx", hle_check_ex, NULL},
    {"CARDCheck", hle_check, NULL},
    {"__CARDFormatRegionAsync", hle_format_region_async, NULL},
    {"CARDFormatAsync", hle_format_async, NULL},
    {"CARDFormat", hle_format, "__CARDFormatRegionAsync"},
    {"CARDFreeBlocks", hle_free_blocks, NULL},
    {"CARDGetSectorSize", hle_get_sector_size, NULL},
    {"CARDOpen", hle_open, NULL},
    {"CARDClose", hle_close, NULL},
    {"CARDCreateAsync", hle_create_async, NULL},
    {"CARDCreate", hle_create, "CARDCreateAsync"},
    {"CARDReadAsync", hle_read_async, NULL},
    {"CARDRead", hle_read, "CARDReadAsync"},
    {"CARDWriteAsync", hle_write_async, NULL},
    {"CARDWrite", hle_write, "CARDWriteAsync"},
    {"CARDDeleteAsync", hle_delete_async, NULL},
    {"CARDDelete", hle_delete, "CARDDeleteAsync"},
    {"CARDGetStatus", hle_get_status, NULL},
    {"CARDSetStatusAsync", hle_set_status_async, NULL},
    {"CARDSetStatus", hle_set_status, "CARDSetStatusAsync"},
};

void gcrt_native_card_install(GcrtCpu* g) {
    const char* slot = getenv("GCRT_CARD");
    s_card.slot_a_empty = slot && !strcmp(slot, "absent");
    if (slot && slot[0] && !s_card.slot_a_empty)
        fprintf(stderr, "gcrt: GCRT_CARD=%s ignored (only value: absent)\n", slot);
    if (s_card.slot_a_empty)
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "WARNING: GCRT_CARD=absent: no card in slot A (diagnostic: "
                                             "comparison with the emulated reference, which had no card)");
    s_card.installed = 0;
    s_card.missing[0] = '\0';
    for (size_t i = 0; i < sizeof(k_replacements) / sizeof(k_replacements[0]); i++) {
        const Replacement* r = &k_replacements[i];
        const char* chain[1] = {r->anchor};
        if (gcrt_native_replace(g, r->name, "CARD", r->fn, r->anchor ? chain : NULL, r->anchor ? 1u : 0u, NULL,
                                s_card.missing, sizeof(s_card.missing)))
            s_card.installed++;
    }
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native CARD: %u functions replaced, missing: %s", s_card.installed,
                      s_card.missing[0] ? s_card.missing : "none");
}

void gcrt_native_card_report(FILE* out) {
    uint64_t ok = 0, failed = 0;
    gcrt_sf_stats(&ok, &failed);
    const GcrtCardStats* st = &s_card.card.stats;
    fprintf(out, "native CARD: probes %llu, mounts %llu, opens %llu, creates %u, reads %llu (%llu B), "
                 "writes %llu (%llu B), status writes %u, deletes %u, formats %u; safe writes %llu (failed %llu), "
                 "orphans removed %u, ignored files %u, I/O errors %u, async calls %llu; %u functions "
                 "replaced, missing: %s\n",
            (unsigned long long)s_card.calls[F_PROBE_EX], (unsigned long long)s_card.calls[F_MOUNT],
            (unsigned long long)s_card.calls[F_OPEN], st->creates, (unsigned long long)s_card.calls[F_READ],
            (unsigned long long)s_card.bytes_read, (unsigned long long)s_card.calls[F_WRITE],
            (unsigned long long)s_card.bytes_written, st->status_writes, st->deletes, st->formats,
            (unsigned long long)ok, (unsigned long long)failed, st->orphans_removed, st->ignored, st->io_errors,
            (unsigned long long)s_card.async_calls, s_card.installed, s_card.missing[0] ? s_card.missing : "none");
}
