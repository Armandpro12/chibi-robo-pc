/*
 * DSP microcodes in high-level emulation (HLE), C interface for the SDK's native DSP library
 * (hle/native_audio.c). Generic GameCube.
 *
 * Step 10d of milestone 10: no DSP register is emulated any more. The SDK's DSP functions are replaced natively;
 * through plain calls, they hand over here what the game entrusted to the DSP:
 *   - starting a task (gcrt_audio_dsp_boot: the "ector" hash of its IRAM code, taken from main
 *     memory, selects the microcode, like Dolphin's DSP ROM);
 *   - CPU -> DSP mails (gcrt_audio_dsp_mail: AX command lists, task management);
 *   - the update every millisecond (gcrt_audio_dsp_update).
 * The microcodes answer with DSP -> CPU mails (GcrtDspMailSink.push), possibly with an interrupt:
 * the native DSP library stores them in its queue and delivers interrupt #7 on the game thread.
 * AX command lists are data in guest memory (like GX display lists): processing them is the
 * native mixer (ax_gc.cpp, ported from Dolphin), without any register.
 *
 * Microcodes: GameCube AX (every hash known to Dolphin, and as the fallback for an unknown hash, like
 * Dolphin), CARD; "ROM" (waiting for a task: mail 0x8071FEED); "INIT" (OS bootstrap program:
 * mail 0x80544348, conventional hash 1).
 *
 * Every function is called on the game thread (no host thread).
 */
#ifndef GCRT_AUDIO_DSP_UCODE_H
#define GCRT_AUDIO_DSP_UCODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CPU side of the DSP -> CPU mailbox (provided by the native DSP library). */
typedef struct GcrtDspMailSink {
    void* user;
    /* DSP -> CPU mail; `interrupt`: DSP interrupt when it becomes the first in the queue, `delay_cycles`
       processor cycles (486 MHz) later if it already is. */
    void (*push)(void* user, uint32_t mail, bool interrupt, uint32_t delay_cycles);
    /* Microcode switch: pending mails are discarded (Dolphin DSPHLE::SetUCode, ClearPending). */
    void (*clear)(void* user);
} GcrtDspMailSink;

/* Polyphase table (GCRT_DSP_COEF, GCRT_DSP_POLYPHASE) and audio traces (GCRT_TRACE): once, at startup. */
void gcrt_audio_register_ucodes(void);
/* Plugs in the microcodes: mailbox, main memory (`ram`, big-endian) and ARAM as seen by the DSP. Then
   initial state: ROM (mail 0x8071FEED, without interrupt). `sink` is copied. */
void gcrt_audio_dsp_attach(const GcrtDspMailSink* sink, uint8_t* ram, uint32_t ram_size, uint8_t* aram,
                           uint32_t aram_size);
void gcrt_audio_dsp_detach(void);
/* Task start by the ROM: hash of [iram_ram_addr, + iram_length) in main memory (virtual or physical
   address), matching microcode started (the current microcode, if any, is kept for a later resume,
   like Dolphin SwapUCode). Returns the hash. */
uint32_t gcrt_audio_dsp_boot(uint32_t iram_ram_addr, uint32_t iram_length, uint32_t start_pc);
/* CPU -> DSP mail (32 bits). */
void gcrt_audio_dsp_mail(uint32_t mail);
/* Every millisecond of emulated time. */
void gcrt_audio_dsp_update(void);
/* Name of the current microcode ("ROM" if there is no task). */
const char* gcrt_audio_dsp_name(void);
/* "ector" hash (Dolphin, Common/Hash.cpp): 3-bit rotation after each byte. */
uint32_t gcrt_audio_hash_ector(const uint8_t* data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_AUDIO_DSP_UCODE_H */
