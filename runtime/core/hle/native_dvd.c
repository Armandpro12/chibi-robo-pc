/* Native disc drive (milestone 10a): replacement of the SDK DVD functions that drive the drive.
 *
 * Replacement boundary. The SDK DVD library has three layers:
 *   - dvdfs.c (DVDOpen, DVDFastOpen, DVDConvertPathToEntrynum, DVDReadAsyncPrio, DVDReadPrio...): file system over
 *     the FST in RAM and synchronous wrappers (wait on __DVDThreadQueue). No register: KEPT as is (recompiled
 *     code);
 *   - dvd.c: command queue by priority, state machine (stateReady, stateBusy, cbForStateBusy...), cancellation,
 *     drive status. It reads and writes DI registers itself (DICVR in stateBusy, DILENGTH and DIIMMBUF in
 *     cbForStateBusy, DISR/DICVR in DVDInit, DVDReset, DVDCheckDisk): REPLACED here, entirely;
 *   - dvdlow.c (DVDLowRead, __DVDInterruptHandler...): DI registers, DI interrupt. No longer called by the replaced
 *     layer; if some other code called it, the hardware access counter would show it.
 * The native engine reproduces the behavior of dvd.c on the DVDCommandBlock fields that the game sees (state,
 * transferred size, callback), the order of the callbacks and the return codes; the data come from the disc image
 * (same bytes as the reference emulation) and the end of each transfer is delivered as a native DI interrupt
 * (hle/native_irq.c), after the same latency as the emulation (0.1 ms + size / rate, GCRT_DVD_RATE, 3 MiB/s by
 * default; 0 = instantaneous). Behavior reference: public decompilation of the SDK (dvd.c, dvdlow.c, consulted only;
 * sources in the project notes); reference emulation hw/di.cpp, removed in step 10a (kept outside this
 * repository's public history).
 *
 * Choice of the data source: the disc image (.iso of the manifest, GCRT_DISC_IMAGE) rather than the extracted
 * files: the SDK DVD API works with absolute offsets on the disc (DVDReadAbsAsyncPrio, disc identifier, BB2, reads
 * that overflow a file), the image gives exactly those bytes, the same as the reference emulation. See
 * notes/decisions.md (2026-10-10, runtime agent).
 *
 * No reference to a game: only the SDK conventions (DVDCommandBlock, command and state codes). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "hle/gcrt_hle.h"
#include "hle/gcrt_native_modules.h"
#include "platform/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DVDCommandBlock (SDK, 0x30 bytes). */
#define BLK_NEXT 0x00u
#define BLK_PREV 0x04u
#define BLK_COMMAND 0x08u
#define BLK_STATE 0x0Cu
#define BLK_OFFSET 0x10u
#define BLK_LENGTH 0x14u
#define BLK_ADDR 0x18u
#define BLK_CURR_TRANSFER 0x1Cu
#define BLK_TRANSFERRED 0x20u
#define BLK_ID 0x24u
#define BLK_CALLBACK 0x28u
/* DVDFileInfo: DVDCommandBlock then startAddr, length, callback. */
#define FI_START 0x30u

/* Commands (DVD_COMMAND_*). */
enum {
    CMD_READ = 1, CMD_SEEK = 2, CMD_CHANGE_DISK = 3, CMD_BSREAD = 4, CMD_READID = 5, CMD_INITSTREAM = 6,
    CMD_CANCELSTREAM = 7, CMD_STOP_STREAM_AT_END = 8, CMD_REQUEST_AUDIO_ERROR = 9, CMD_REQUEST_PLAY_ADDR = 10,
    CMD_REQUEST_START_ADDR = 11, CMD_REQUEST_LENGTH = 12, CMD_AUDIO_BUFFER_CONFIG = 13, CMD_INQUIRY = 14,
    CMD_BS_CHANGE_DISK = 15, CMD_STOP_MOTOR = 16,
};
/* States (DVD_STATE_*). */
enum {
    ST_FATAL_ERROR = -1, ST_END = 0, ST_BUSY = 1, ST_WAITING = 2, ST_COVER_CLOSED = 3, ST_NO_DISK = 4,
    ST_COVER_OPEN = 5, ST_WRONG_DISK = 6, ST_MOTOR_STOPPED = 7, ST_PAUSING = 8, ST_IGNORED = 9, ST_CANCELED = 10,
    ST_RETRY = 11,
};
/* Results passed to the callbacks (DVD_RESULT_*). */
#define RESULT_CANCELED ((u32)-3)
#define RESULT_IGNORED ((u32)-2)

/* Command completion causes seen by the state machine (cbForStateBusy): 1 = transfer complete, 8 = requested
 * break (DVDLowBreak). */
#define CAUSE_TC 1u
#define CAUSE_BREAK 8u

#define DUMMY_BLOCK 1u        /* "executing == &DummyCommandBlock" */
#define MAX_TRANSFER 0x80000u /* stateBusy: transfers in 512 KiB chunks */
#define QUEUE_LEVELS 4u
#define QUEUE_MAX 256u

/* Fixed latency of a command (same assumption as the reference emulation): 0.1 ms. */
#define COMMAND_LATENCY_TICKS (GCRT_TB_HZ / 10000u)

typedef struct {
    FILE* image;
    char image_path[1024];
    u64 image_size;
    u32 rate; /* bytes per second, 0 = instantaneous */

    bool initialized;
    u32 executing;      /* guest block in progress, 0 = none, DUMMY_BLOCK */
    u32 queue[QUEUE_LEVELS][QUEUE_MAX];
    unsigned queue_count[QUEUE_LEVELS];
    bool pause_flag, pausing_flag, fatal_error_flag;
    bool canceling;
    u32 cancel_callback;
    u32 resume_from_here;
    bool reset_required;
    bool auto_invalidation;
    u32 curr_command;
    bool auto_finishing;
    bool break_requested; /* DVDLowBreak: stop at the next completed transfer */
    u32 event_id;         /* pending end of transfer (native interrupt) */
    u32 event_block;

    /* Report */
    u64 commands, bytes, callbacks, cancels;
    u64 by_command[17];
    unsigned installed;
    char missing[512];
} NativeDvd;

static NativeDvd s_dvd = {.auto_invalidation = true, .rate = 3u * 1024u * 1024u};

static u32 rd32(GcrtCpu* g, u32 a) { return mem_read32(&g->state, a); }
static void wr32(GcrtCpu* g, u32 a, u32 v) { mem_write32(&g->state, a, v); }
static u32 arg(GcrtCpu* g, int i) { return g->state.gpr[3 + i]; }

/* ---------------------------------------------------------------------------------------------
 * Disc image
 * ------------------------------------------------------------------------------------------- */

static bool image_open(const char* path) {
    s_dvd.image = fopen(path, "rb");
    if (!s_dvd.image)
        return false;
    snprintf(s_dvd.image_path, sizeof(s_dvd.image_path), "%s", path);
#if defined(_WIN32)
    _fseeki64(s_dvd.image, 0, SEEK_END);
    s_dvd.image_size = (u64)_ftelli64(s_dvd.image);
#else
    fseeko(s_dvd.image, 0, SEEK_END);
    s_dvd.image_size = (u64)ftello(s_dvd.image);
#endif
    return true;
}

/* Reads `length` bytes at `offset` into `dst`; pads with zeros beyond the end (like the reference). */
static void image_read(u64 offset, u8* dst, u32 length) {
    memset(dst, 0, length);
    if (!s_dvd.image || offset >= s_dvd.image_size)
        return;
    u64 avail = s_dvd.image_size - offset;
    u32 n = avail < length ? (u32)avail : length;
#if defined(_WIN32)
    _fseeki64(s_dvd.image, (long long)offset, SEEK_SET);
#else
    fseeko(s_dvd.image, (off_t)offset, SEEK_SET);
#endif
    if (fread(dst, 1, n, s_dvd.image) != n)
        GCRT_TRACE(GCRT_TRACE_UNHANDLED, "native DVD: cannot read the image (offset 0x%09llX, %u bytes)",
                   (unsigned long long)offset, n);
}

/* ---------------------------------------------------------------------------------------------
 * Command queue (dvdqueue.c: 4 priority levels, first come first served)
 * ------------------------------------------------------------------------------------------- */

static bool queue_push(u32 prio, u32 block) {
    if (prio >= QUEUE_LEVELS)
        prio = QUEUE_LEVELS - 1u; /* [hypothesis] the SDK trusts the caller (priority 0-3) */
    if (s_dvd.queue_count[prio] >= QUEUE_MAX) {
        fprintf(stderr, "gcrt native: DVD queue full\n");
        return false;
    }
    s_dvd.queue[prio][s_dvd.queue_count[prio]++] = block;
    return true;
}

static u32 queue_pop(void) {
    for (unsigned p = 0; p < QUEUE_LEVELS; p++) {
        if (s_dvd.queue_count[p]) {
            u32 block = s_dvd.queue[p][0];
            memmove(&s_dvd.queue[p][0], &s_dvd.queue[p][1], (s_dvd.queue_count[p] - 1u) * sizeof(u32));
            s_dvd.queue_count[p]--;
            return block;
        }
    }
    return 0;
}

static bool queue_remove(u32 block) {
    for (unsigned p = 0; p < QUEUE_LEVELS; p++) {
        for (unsigned i = 0; i < s_dvd.queue_count[p]; i++) {
            if (s_dvd.queue[p][i] == block) {
                memmove(&s_dvd.queue[p][i], &s_dvd.queue[p][i + 1], (s_dvd.queue_count[p] - i - 1u) * sizeof(u32));
                s_dvd.queue_count[p]--;
                return true;
            }
        }
    }
    return false;
}

static bool queue_empty(void) {
    for (unsigned p = 0; p < QUEUE_LEVELS; p++)
        if (s_dvd.queue_count[p])
            return false;
    return true;
}

/* ---------------------------------------------------------------------------------------------
 * State machine (dvd.c)
 * ------------------------------------------------------------------------------------------- */

static void state_ready(GcrtCpu* g);
static void state_busy(GcrtCpu* g, u32 block);

static void call_block_callback(GcrtCpu* g, u32 fn, u32 result, u32 block) {
    if (!fn)
        return;
    s_dvd.callbacks++;
    const u32 args[2] = {result, block};
    GCRT_TRACE(GCRT_TRACE_DVD, "callback %08X(%d, block %08X)", fn, (int)result, block);
    gcrt_native_callback(g, fn, args, 2);
}

/* End of a command: executing = &DummyCommandBlock, state, callback, then next command. */
static void finish_command(GcrtCpu* g, s32 state, u32 result) {
    const u32 finished = s_dvd.executing;
    s_dvd.executing = DUMMY_BLOCK;
    wr32(g, finished + BLK_STATE, (u32)state);
    call_block_callback(g, rd32(g, finished + BLK_CALLBACK), result, finished);
    state_ready(g);
}

/* CheckCancel(resume): cancellation requested during the current command. */
static bool check_cancel(GcrtCpu* g, u32 resume) {
    if (!s_dvd.canceling)
        return false;
    s_dvd.resume_from_here = resume;
    s_dvd.canceling = false;
    const u32 finished = s_dvd.executing;
    s_dvd.executing = DUMMY_BLOCK;
    wr32(g, finished + BLK_STATE, (u32)ST_CANCELED);
    s_dvd.cancels++;
    call_block_callback(g, rd32(g, finished + BLK_CALLBACK), RESULT_CANCELED, finished);
    if (s_dvd.cancel_callback) {
        const u32 args[2] = {0, finished};
        gcrt_native_callback(g, s_dvd.cancel_callback, args, 2);
    }
    state_ready(g);
    return true;
}

static bool is_dma_command(u32 command) {
    return command == CMD_READ || command == CMD_BSREAD || command == CMD_READID || command == CMD_INQUIRY;
}

/* cbForStateBusy(cause): end of a hardware command (here: end of the simulated latency). */
static void cb_for_state_busy(GcrtCpu* g, u32 cause) {
    const u32 block = s_dvd.executing;
    if (s_dvd.curr_command == CMD_CHANGE_DISK || s_dvd.curr_command == CMD_BS_CHANGE_DISK) {
        /* Motor stopped: the console then waits for the lid to open and then close
         * (stateMotorStopped -> DVDLowWaitCoverClose). An image never changes: endless wait, like a console whose
         * lid is never opened. */
        if (s_dvd.curr_command == CMD_BS_CHANGE_DISK)
            s_dvd.reset_required = true;
        if (check_cancel(g, 7))
            return;
        wr32(g, block + BLK_STATE, (u32)ST_MOTOR_STOPPED);
        GCRT_TRACE(GCRT_TRACE_DVD, "motor stopped: waiting for a disc change (never)");
        return;
    }
    if (is_dma_command(s_dvd.curr_command))
        wr32(g, block + BLK_TRANSFERRED, rd32(g, block + BLK_TRANSFERRED) + rd32(g, block + BLK_CURR_TRANSFER));
    if (cause & CAUSE_BREAK) {
        s_dvd.canceling = false;
        const u32 finished = s_dvd.executing;
        s_dvd.executing = DUMMY_BLOCK;
        wr32(g, finished + BLK_STATE, (u32)ST_CANCELED);
        s_dvd.cancels++;
        call_block_callback(g, rd32(g, finished + BLK_CALLBACK), RESULT_CANCELED, finished);
        if (s_dvd.cancel_callback) {
            const u32 args[2] = {0, finished};
            gcrt_native_callback(g, s_dvd.cancel_callback, args, 2);
        }
        state_ready(g);
        return;
    }
    /* cause & CAUSE_TC */
    if (check_cancel(g, 0))
        return;
    const u32 command = s_dvd.curr_command;
    if (is_dma_command(command)) {
        if (rd32(g, block + BLK_TRANSFERRED) != rd32(g, block + BLK_LENGTH)) {
            state_busy(g, block);
            return;
        }
        finish_command(g, ST_END, rd32(g, block + BLK_TRANSFERRED));
    } else if (command >= CMD_REQUEST_AUDIO_ERROR && command <= CMD_REQUEST_LENGTH) {
        /* Disc audio streaming not supported (like the reference emulation): response 0. */
        finish_command(g, ST_END, 0);
    } else if (command == CMD_INITSTREAM) {
        finish_command(g, ST_END, 0);
    } else {
        finish_command(g, ST_END, 0);
    }
}

typedef struct {
    u32 block;
} CompletionData;

/* Native DI interrupt: end of the current transfer. */
static void on_transfer_complete(GcrtCpu* g, unsigned interrupt, u32 context, void* user) {
    (void)interrupt;
    const u32 block = (u32)(uintptr_t)user;
    s_dvd.event_id = 0;
    if (s_dvd.executing != block) {
        GCRT_TRACE(GCRT_TRACE_DVD, "end of transfer of block %08X ignored (no longer in progress)", block);
        return;
    }
    u32 cause = CAUSE_TC;
    if (s_dvd.break_requested && (s_dvd.curr_command == CMD_READ || s_dvd.curr_command == CMD_BSREAD))
        cause |= CAUSE_BREAK; /* __DVDInterruptHandler: StopAtNextInt after a read */
    s_dvd.break_requested = false;
    GCRT_TRACE(GCRT_TRACE_DVD, "end of command %u (block %08X, cause %u)", s_dvd.curr_command, block, cause);
    /* Like __DVDInterruptHandler: the processing (callbacks included) runs in a temporary exception context, then
     * the interrupted context becomes current again. */
    const u32 tmp = gcrt_native_exception_context_begin(g);
    cb_for_state_busy(g, cause);
    gcrt_native_exception_context_end(g, tmp, context);
}

static void post_completion(GcrtCpu* g, u32 block, u32 bytes) {
    u64 delay = COMMAND_LATENCY_TICKS;
    if (s_dvd.rate)
        delay += (u64)bytes * GCRT_TB_HZ / s_dvd.rate;
    s_dvd.event_block = block;
    s_dvd.event_id = gcrt_nirq_post(g, GCRT_OS_IRQ_PI_DI, g->ticks + delay, on_transfer_complete,
                                    (void*)(uintptr_t)block);
}

/* Read of a transfer: like the drive DMA (address and length aligned on 32 bytes, offset on 4), the data are copied
 * at the start of the command (same choice as the reference emulation). */
static void start_read(GcrtCpu* g, u32 block, u32 addr, u32 length, u64 disc_offset) {
    const u32 dma_addr = addr & 0x03FFFFE0u;
    const u32 dma_len = length & ~0x1Fu;
    u8* dst = gcrt_native_ram(g, dma_addr, dma_len);
    if (dst)
        image_read(disc_offset & ~3ull, dst, dma_len);
    else
        GCRT_TRACE(GCRT_TRACE_UNHANDLED, "native DVD: read to an address outside RAM 0x%08X (%u bytes)", addr, length);
    s_dvd.bytes += dma_len;
    GCRT_TRACE(GCRT_TRACE_DVD, "disc read 0x%09llX -> 0x%08X, %u bytes (block %08X)",
               (unsigned long long)disc_offset, addr, dma_len, block);
    post_completion(g, block, dma_len);
}

/* stateBusy(block): starts the command (or the next chunk of a read). */
static void state_busy(GcrtCpu* g, u32 block) {
    const u32 command = rd32(g, block + BLK_COMMAND);
    s_dvd.commands++;
    if (command < 17)
        s_dvd.by_command[command]++;
    switch (command) {
    case CMD_READID:
        wr32(g, block + BLK_CURR_TRANSFER, 0x20u);
        start_read(g, block, rd32(g, block + BLK_ADDR), 0x20u, 0);
        break;
    case CMD_READ:
    case CMD_BSREAD: {
        const u32 length = rd32(g, block + BLK_LENGTH);
        if (!length) {
            finish_command(g, ST_END, 0);
            break;
        }
        const u32 done = rd32(g, block + BLK_TRANSFERRED);
        const u32 left = length - done;
        const u32 chunk = left < MAX_TRANSFER ? left : MAX_TRANSFER;
        wr32(g, block + BLK_CURR_TRANSFER, chunk);
        start_read(g, block, rd32(g, block + BLK_ADDR) + done, chunk, (u64)rd32(g, block + BLK_OFFSET) + done);
        break;
    }
    case CMD_SEEK:
        GCRT_TRACE(GCRT_TRACE_DVD, "seek 0x%08X (block %08X)", rd32(g, block + BLK_OFFSET), block);
        post_completion(g, block, 0);
        break;
    case CMD_CHANGE_DISK:
    case CMD_BS_CHANGE_DISK:
        GCRT_TRACE(GCRT_TRACE_DVD, "motor stop (block %08X)", block);
        post_completion(g, block, 0);
        break;
    case CMD_INQUIRY: {
        /* Drive information: values of the reference emulation (Dolphin, "Taken from my Wii"): revision 2, date
         * 2006-05-26, 0x41; only the first 12 bytes are written, as it does. */
        wr32(g, block + BLK_CURR_TRANSFER, 0x20u);
        u8* p = gcrt_native_ram(g, rd32(g, block + BLK_ADDR) & 0x03FFFFE0u, 12u);
        if (p) {
            const u8 info[12] = {0, 0, 0, 2, 0x20, 0x06, 0x05, 0x26, 0x41, 0, 0, 0};
            memcpy(p, info, sizeof(info));
        }
        GCRT_TRACE(GCRT_TRACE_DVD, "drive information -> 0x%08X (block %08X)", rd32(g, block + BLK_ADDR), block);
        post_completion(g, block, 0);
        break;
    }
    case CMD_INITSTREAM:
    case CMD_CANCELSTREAM:
    case CMD_STOP_STREAM_AT_END:
    case CMD_REQUEST_AUDIO_ERROR:
    case CMD_REQUEST_PLAY_ADDR:
    case CMD_REQUEST_START_ADDR:
    case CMD_REQUEST_LENGTH:
    case CMD_AUDIO_BUFFER_CONFIG:
        /* Disc audio streaming (DTK): not supported, like in the reference emulation [hypothesis: games that do not
         * need it do not look at the result]. */
        GCRT_TRACE(GCRT_TRACE_DVD, "audio streaming command %u accepted without effect (block %08X)", command, block);
        post_completion(g, block, 0);
        break;
    default:
        GCRT_TRACE(GCRT_TRACE_UNHANDLED, "native DVD: unknown command %u (block %08X), completed without effect", command,
                   block);
        post_completion(g, block, 0);
        break;
    }
}

/* stateReady(): next command of the queue. */
static void state_ready(GcrtCpu* g) {
    if (queue_empty()) {
        s_dvd.executing = 0;
        return;
    }
    if (s_dvd.pause_flag) {
        s_dvd.pausing_flag = true;
        s_dvd.executing = 0;
        return;
    }
    s_dvd.executing = queue_pop();
    s_dvd.curr_command = rd32(g, s_dvd.executing + BLK_COMMAND);
    if (s_dvd.resume_from_here) {
        /* Resumption after cancelling a lid wait: case of a stopped motor (the only path reachable without a
         * lid); [hypothesis] the command is started again. */
        GCRT_TRACE(GCRT_TRACE_DVD, "resume %u after cancellation: command restarted", s_dvd.resume_from_here);
        s_dvd.resume_from_here = 0;
    }
    wr32(g, s_dvd.executing + BLK_STATE, (u32)ST_BUSY);
    state_busy(g, s_dvd.executing);
}

/* issueCommand(prio, block). */
static u32 issue_command(GcrtCpu* g, u32 prio, u32 block) {
    /* autoInvalidation: DCInvalidateRange on the destination (no effect natively: no simulated cache). */
    wr32(g, block + BLK_STATE, (u32)ST_WAITING);
    const bool ok = queue_push(prio, block);
    if (s_dvd.executing == 0 && !s_dvd.pause_flag)
        state_ready(g);
    return ok ? 1u : 0u;
}

/* ---------------------------------------------------------------------------------------------
 * Replacements
 * ------------------------------------------------------------------------------------------- */

static bool hle_dvd_init(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    if (!s_dvd.initialized) {
        s_dvd.initialized = true;
        /* __DVDFSInit (FST in RAM, no hardware) then unmasking of the DI interrupt in the OS: the native interrupts
         * of the drive honor that mask. Not reproduced: OSRegisterVersion (banner text), queue and workarounds of
         * dvdlow.c, DI registers, loading of the FST on a JTAG boot (__fstLoad, BootInfo->magic == 0xE5207C22). */
        u32 fsinit = gcrt_native_sdk_function("__DVDFSInit");
        if (fsinit)
            gcrt_guest_call(g, fsinit, NULL, 0, NULL);
        u32 unmask = gcrt_native_os_fn(GCRT_OSFN_UNMASK_INTERRUPTS);
        if (unmask) {
            const u32 mask = 0x80000000u >> GCRT_OS_IRQ_PI_DI;
            gcrt_guest_call(g, unmask, &mask, 1, NULL);
        }
        GCRT_TRACE(GCRT_TRACE_DVD, "native DVDInit (image %s)", s_dvd.image ? s_dvd.image_path : "absent");
    }
    gcrt_native_return(g, 0);
    return true;
}

/* Commands "fill the block then issueCommand(prio, block)". */
static void set_block(GcrtCpu* g, u32 block, u32 command, u32 addr, u32 length, u32 offset, u32 callback,
                      bool set_addr, bool set_length, bool set_offset, bool reset_transferred) {
    wr32(g, block + BLK_COMMAND, command);
    if (set_addr)
        wr32(g, block + BLK_ADDR, addr);
    if (set_length)
        wr32(g, block + BLK_LENGTH, length);
    if (set_offset)
        wr32(g, block + BLK_OFFSET, offset);
    if (reset_transferred)
        wr32(g, block + BLK_TRANSFERRED, 0);
    wr32(g, block + BLK_CALLBACK, callback);
}

/* DVDReadAbsAsyncPrio(block, addr, length, offset, callback, prio) */
static bool hle_read_abs_async_prio(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    set_block(g, block, CMD_READ, arg(g, 1), arg(g, 2), arg(g, 3), arg(g, 4), true, true, true, true);
    gcrt_native_return(g, issue_command(g, arg(g, 5), block));
    return true;
}

/* DVDReadAbsAsyncForBS(block, addr, length, offset, callback): priority 2. */
static bool hle_read_abs_async_for_bs(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    set_block(g, block, CMD_BSREAD, arg(g, 1), arg(g, 2), arg(g, 3), arg(g, 4), true, true, true, true);
    gcrt_native_return(g, issue_command(g, 2, block));
    return true;
}

/* DVDSeekAbsAsyncPrio(block, offset, callback, prio) */
static bool hle_seek_abs_async_prio(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    set_block(g, block, CMD_SEEK, 0, 0, arg(g, 1), arg(g, 2), false, false, true, false);
    gcrt_native_return(g, issue_command(g, arg(g, 3), block));
    return true;
}

/* DVDReadDiskID(block, diskID, callback): priority 2. */
static bool hle_read_disk_id(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    set_block(g, block, CMD_READID, arg(g, 1), 0x20u, 0, arg(g, 2), true, true, true, true);
    gcrt_native_return(g, issue_command(g, 2, block));
    return true;
}

/* DVDInquiryAsync(block, info, callback): priority 2. */
static bool hle_inquiry_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    set_block(g, block, CMD_INQUIRY, arg(g, 1), 0x20u, 0, arg(g, 2), true, true, false, true);
    gcrt_native_return(g, issue_command(g, 2, block));
    return true;
}

/* DVDPrepareStreamAbsAsync(block, length, offset, callback): priority 1. */
static bool hle_prepare_stream_abs_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    set_block(g, block, CMD_INITSTREAM, 0, arg(g, 1), arg(g, 2), arg(g, 3), false, true, true, false);
    gcrt_native_return(g, issue_command(g, 1, block));
    return true;
}

/* Commands without parameters (block, callback), priority 1: audio streaming; 2: motor. */
static bool simple_command(GcrtCpu* g, u32 command, u32 prio) {
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    set_block(g, block, command, 0, 0, 0, arg(g, 1), false, false, false, false);
    gcrt_native_return(g, issue_command(g, prio, block));
    return true;
}
static bool hle_cancel_stream_async(GcrtCpu* g, u32 a) { (void)a; return simple_command(g, CMD_CANCELSTREAM, 1); }
static bool hle_stop_stream_at_end_async(GcrtCpu* g, u32 a) { (void)a; return simple_command(g, CMD_STOP_STREAM_AT_END, 1); }
static bool hle_get_stream_error_status_async(GcrtCpu* g, u32 a) { (void)a; return simple_command(g, CMD_REQUEST_AUDIO_ERROR, 1); }
static bool hle_get_stream_play_addr_async(GcrtCpu* g, u32 a) { (void)a; return simple_command(g, CMD_REQUEST_PLAY_ADDR, 1); }
static bool hle_get_stream_start_addr_async(GcrtCpu* g, u32 a) { (void)a; return simple_command(g, CMD_REQUEST_START_ADDR, 1); }
static bool hle_get_stream_length_async(GcrtCpu* g, u32 a) { (void)a; return simple_command(g, CMD_REQUEST_LENGTH, 1); }

/* DVDGetDriveStatus() */
static s32 drive_status(GcrtCpu* g) {
    if (s_dvd.fatal_error_flag)
        return ST_FATAL_ERROR;
    if (s_dvd.pausing_flag)
        return ST_PAUSING;
    if (s_dvd.executing == 0 || s_dvd.executing == DUMMY_BLOCK)
        return ST_END;
    s32 state = (s32)rd32(g, s_dvd.executing + BLK_STATE);
    return state == ST_COVER_CLOSED ? ST_BUSY : state; /* DVDGetCommandBlockStatus */
}

static bool hle_get_drive_status(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, (u32)drive_status(g));
    return true;
}

/* DVDCheckDisk() */
static bool hle_check_disk(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s32 state;
    if (s_dvd.fatal_error_flag)
        state = ST_FATAL_ERROR;
    else if (s_dvd.pausing_flag)
        state = ST_PAUSING;
    else if (s_dvd.executing == 0 || s_dvd.executing == DUMMY_BLOCK)
        state = ST_END;
    else
        state = (s32)rd32(g, s_dvd.executing + BLK_STATE);
    u32 result;
    switch (state) {
    case ST_BUSY: case ST_IGNORED: case ST_CANCELED: case ST_WAITING:
        result = 1;
        break;
    case ST_END: case ST_PAUSING:
        /* Lid always closed, no pending lid interrupt. */
        result = s_dvd.resume_from_here ? 0u : 1u;
        break;
    default:
        result = 0;
        break;
    }
    gcrt_native_return(g, result);
    return true;
}

/* DVDSetAutoInvalidation stays recompiled (pure function; same masked code as OSSetCurrentHeap): its variable is
 * only used for DCInvalidateRange in issueCommand, not applicable natively. */

static void dvd_pause(void) {
    s_dvd.pause_flag = true;
    if (s_dvd.executing == 0)
        s_dvd.pausing_flag = true;
}

static void dvd_resume(GcrtCpu* g) {
    s_dvd.pause_flag = false;
    if (s_dvd.pausing_flag) {
        s_dvd.pausing_flag = false;
        state_ready(g);
    }
}

static bool hle_pause(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    dvd_pause();
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_resume(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    dvd_resume(g);
    gcrt_native_return(g, 0);
    return true;
}

/* DVDCancelAsync(block, callback) */
static u32 cancel_async(GcrtCpu* g, u32 block, u32 callback) {
    const s32 state = (s32)rd32(g, block + BLK_STATE);
    switch (state) {
    case ST_FATAL_ERROR:
    case ST_END:
    case ST_CANCELED:
        if (callback) {
            const u32 args[2] = {0, block};
            gcrt_native_callback(g, callback, args, 2);
        }
        break;
    case ST_BUSY: {
        if (s_dvd.canceling)
            return 0;
        s_dvd.canceling = true;
        s_dvd.cancel_callback = callback;
        const u32 command = rd32(g, block + BLK_COMMAND);
        if (command == CMD_BSREAD || command == CMD_READ)
            s_dvd.break_requested = true; /* DVDLowBreak */
        break;
    }
    case ST_WAITING:
        queue_remove(block);
        wr32(g, block + BLK_STATE, (u32)ST_CANCELED);
        s_dvd.cancels++;
        call_block_callback(g, rd32(g, block + BLK_CALLBACK), RESULT_CANCELED, block);
        if (callback) {
            const u32 args[2] = {0, block};
            gcrt_native_callback(g, callback, args, 2);
        }
        break;
    case ST_COVER_CLOSED: {
        const u32 command = rd32(g, block + BLK_COMMAND);
        if (command == CMD_READID || command == CMD_BSREAD || command == CMD_AUDIO_BUFFER_CONFIG ||
            command == CMD_BS_CHANGE_DISK) {
            if (callback) {
                const u32 args[2] = {0, block};
                gcrt_native_callback(g, callback, args, 2);
            }
        } else {
            if (s_dvd.canceling)
                return 0;
            s_dvd.canceling = true;
            s_dvd.cancel_callback = callback;
        }
        break;
    }
    case ST_NO_DISK:
    case ST_COVER_OPEN:
    case ST_WRONG_DISK:
    case ST_MOTOR_STOPPED:
    case ST_RETRY: {
        /* Waiting for the lid (DVDLowClearCallback == cbForStateMotorStopped): only the "motor stopped" state is
         * reachable here; the others do not exist without a lid. */
        if (state != ST_MOTOR_STOPPED)
            return 0;
        s_dvd.resume_from_here = 7;
        s_dvd.executing = DUMMY_BLOCK;
        wr32(g, block + BLK_STATE, (u32)ST_CANCELED);
        s_dvd.cancels++;
        call_block_callback(g, rd32(g, block + BLK_CALLBACK), RESULT_CANCELED, block);
        if (callback) {
            const u32 args[2] = {0, block};
            gcrt_native_callback(g, callback, args, 2);
        }
        state_ready(g);
        break;
    }
    default:
        break;
    }
    return 1;
}

static bool hle_cancel_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    GCRT_TRACE(GCRT_TRACE_DVD, "DVDCancelAsync(block %08X, callback %08X)", arg(g, 0), arg(g, 1));
    gcrt_native_return(g, cancel_async(g, arg(g, 0), arg(g, 1)));
    return true;
}

/* DVDCancelAllAsync(callback) */
static u32 cancel_all_async(GcrtCpu* g, u32 callback) {
    dvd_pause();
    u32 block;
    while ((block = queue_pop()) != 0)
        cancel_async(g, block, 0);
    u32 result = 1;
    if (s_dvd.executing && s_dvd.executing != DUMMY_BLOCK) {
        result = cancel_async(g, s_dvd.executing, callback);
    } else if (callback) {
        const u32 args[2] = {0, 0};
        gcrt_native_callback(g, callback, args, 2);
    }
    dvd_resume(g);
    return result;
}

static bool hle_cancel_all_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, cancel_all_async(g, arg(g, 0)));
    return true;
}

/* __DVDPrepareResetAsync(callback) */
static bool hle_prepare_reset_async(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 callback = arg(g, 0);
    for (unsigned p = 0; p < QUEUE_LEVELS; p++)
        s_dvd.queue_count[p] = 0; /* __DVDClearWaitingQueue */
    if (s_dvd.canceling) {
        s_dvd.cancel_callback = callback;
    } else {
        if (s_dvd.executing && s_dvd.executing != DUMMY_BLOCK)
            wr32(g, s_dvd.executing + BLK_CALLBACK, 0);
        cancel_all_async(g, callback);
    }
    gcrt_native_return(g, 0);
    return true;
}

/* DVDReset(): reset of the drive (DVDLowReset); nothing to reset for an image. */
static bool hle_reset(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    s_dvd.reset_required = false;
    s_dvd.resume_from_here = 0;
    gcrt_native_return(g, 0);
    return true;
}

static bool hle_reset_required(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, s_dvd.reset_required ? 1u : 0u);
    return true;
}

/* DVDGetTransferredSize(fileInfo): transferred size; during a transfer, the SDK adds the part of the current chunk
 * read from DILENGTH: the reference emulation only counts it at the end of the chunk (0 before), and so does the
 * native code. */
static bool hle_get_transferred_size(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    const u32 block = arg(g, 0);
    gcrt_native_return(g, rd32(g, block + BLK_TRANSFERRED));
    return true;
}

/* __OSGetDIConfig() (OS, called by EXISync): configuration byte of the drive (DICFG & 0xFF). Value of the reference
 * emulation and of Dolphin: 1. */
static bool hle_os_get_di_config(GcrtCpu* g, u32 address) {
    (void)address;
    gcrt_native_enter(g);
    gcrt_native_return(g, 1u);
    return true;
}

typedef struct {
    const char* name;
    const char* library;
    GcrtHleFn fn;
} Replacement;

/* Replaced functions (SDK names). Those missing from a game (removed at link time) are simply absent; any function
 * that still touches the DI registers would show up in the counter. */
static const Replacement k_replacements[] = {
    {"DVDInit", "DVD", hle_dvd_init},
    {"DVDReadAbsAsyncPrio", "DVD", hle_read_abs_async_prio},
    {"DVDReadAbsAsyncForBS", "DVD", hle_read_abs_async_for_bs},
    {"DVDSeekAbsAsyncPrio", "DVD", hle_seek_abs_async_prio},
    {"DVDReadDiskID", "DVD", hle_read_disk_id},
    {"DVDInquiryAsync", "DVD", hle_inquiry_async},
    {"DVDPrepareStreamAbsAsync", "DVD", hle_prepare_stream_abs_async},
    {"DVDCancelStreamAsync", "DVD", hle_cancel_stream_async},
    {"DVDStopStreamAtEndAsync", "DVD", hle_stop_stream_at_end_async},
    {"DVDGetStreamErrorStatusAsync", "DVD", hle_get_stream_error_status_async},
    {"DVDGetStreamPlayAddrAsync", "DVD", hle_get_stream_play_addr_async},
    {"DVDGetStreamStartAddrAsync", "DVD", hle_get_stream_start_addr_async},
    {"DVDGetStreamLengthAsync", "DVD", hle_get_stream_length_async},
    {"DVDGetDriveStatus", "DVD", hle_get_drive_status},
    {"DVDCheckDisk", "DVD", hle_check_disk},
    {"DVDPause", "DVD", hle_pause},
    {"DVDResume", "DVD", hle_resume},
    {"DVDCancelAsync", "DVD", hle_cancel_async},
    {"DVDCancelAllAsync", "DVD", hle_cancel_all_async},
    {"__DVDPrepareResetAsync", "DVD", hle_prepare_reset_async},
    {"DVDReset", "DVD", hle_reset},
    {"DVDResetRequired", "DVD", hle_reset_required},
    {"DVDGetTransferredSize", "DVD", hle_get_transferred_size},
    /* Outside the DVD library but reads a DI register. */
    {"__OSGetDIConfig", "OS", hle_os_get_di_config},
};

void gcrt_native_dvd_install(GcrtCpu* g, const char* disc_root) {
    char path[1024];
    if (!gcrt_disc_image_path(disc_root, path, sizeof(path)) || !image_open(path))
        gcrt_trace_printf(GCRT_TRACE_NATIVE, "native DVD: disc image not found (\"%s\"): reads return zeros "
                                             "(GCRT_DISC_IMAGE to point to it)", path);
    const char* rate = getenv("GCRT_DVD_RATE");
    if (rate && rate[0])
        s_dvd.rate = (u32)strtoul(rate, NULL, 0);
    s_dvd.installed = 0;
    s_dvd.missing[0] = '\0';
    for (size_t i = 0; i < sizeof(k_replacements) / sizeof(k_replacements[0]); i++) {
        unsigned n = gcrt_hle_replace_in_library(g, k_replacements[i].name, k_replacements[i].library, k_replacements[i].fn);
        if (n) {
            s_dvd.installed++;
        } else {
            size_t used = strlen(s_dvd.missing);
            snprintf(s_dvd.missing + used, sizeof(s_dvd.missing) - used, "%s%s", used ? " " : "",
                     k_replacements[i].name);
        }
    }
    gcrt_trace_printf(GCRT_TRACE_NATIVE, "native DVD: %u functions replaced, missing: %s; image %s, rate %u B/s",
                      s_dvd.installed, s_dvd.missing[0] ? s_dvd.missing : "none",
                      s_dvd.image ? s_dvd.image_path : "(none)", s_dvd.rate);
}

void gcrt_native_dvd_stats(u64* commands, u64* bytes) {
    *commands = s_dvd.commands;
    *bytes = s_dvd.bytes;
}

void gcrt_native_dvd_report(FILE* out) {
    fprintf(out, "native DVD: %llu commands (reads %llu, identifier %llu, information %llu, seeks %llu), "
                 "%.1f MiB read, %llu callbacks, %llu cancellations; %u functions replaced, missing: %s\n",
            (unsigned long long)s_dvd.commands, (unsigned long long)(s_dvd.by_command[CMD_READ] + s_dvd.by_command[CMD_BSREAD]),
            (unsigned long long)s_dvd.by_command[CMD_READID], (unsigned long long)s_dvd.by_command[CMD_INQUIRY],
            (unsigned long long)s_dvd.by_command[CMD_SEEK], (double)s_dvd.bytes / (1024.0 * 1024.0),
            (unsigned long long)s_dvd.callbacks, (unsigned long long)s_dvd.cancels, s_dvd.installed,
            s_dvd.missing[0] ? s_dvd.missing : "none");
}
