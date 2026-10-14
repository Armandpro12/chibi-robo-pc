/*
 * Host audio output (generic GameCube, C interface).
 *
 * 1. Microcodes: see dsp_ucode.h (HLE DSP microcodes plugged into the SDK's native DSP library,
 *    hle/native_audio.c; no emulated DSP at all since step 10d).
 *
 * 2. Output: gcrt_audio_out_open() opens the host output according to the environment:
 *      GCRT_AUDIO=sdl|off        speakers (default sdl; "off" for tests without sound)
 *      GCRT_AUDIO_WAV=file       records everything the audio DMA reads (audio milestone, diagnostics)
 *      GCRT_AUDIO_LATENCY_MS=64  target latency; GCRT_AUDIO_DRIFT=0 disables drift compensation
 *      GCRT_AUDIO_CAPTURE=prefix, GCRT_AUDIO_MUTE=1: output diagnostics (host_audio.hpp)
 *    then gcrt_native_set_audio_sink(gcrt_audio_out_sink, out) plugs it into the native audio DMA (hle/gcrt_native.h).
 *
 * 3. DSP polyphase table: by default Dolphin's free table (built in). GCRT_DSP_COEF=path of a
 *    dsp_coef.bin (4096 bytes) to use another one; GCRT_DSP_POLYPHASE=0 for linear
 *    interpolation (Dolphin without dsp_coef.bin). Read by gcrt_audio_register_ucodes().
 *
 * Traces: GCRT_TRACE=audio (or dsp, dspmail, ax, axvoice, aidma, audioout), see audio_trace.hpp.
 */
#ifndef GCRT_AUDIO_AUDIO_HW_H
#define GCRT_AUDIO_AUDIO_HW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcrtAudioOut GcrtAudioOut;
GcrtAudioOut* gcrt_audio_out_open(void);
void gcrt_audio_out_close(GcrtAudioOut* out);
/* In the GcrtAudioSink format of hle/gcrt_native.h: `block32` = 8 stereo frames as they are in RAM. Actual order
   in each frame: RIGHT sample then LEFT, 16-bit big-endian (Dolphin: AX OutputSamples writes right
   then left, Mixer::PushSamples reads left at ptr[1]). `rate`: rate of these frames, in Hz. */
void gcrt_audio_out_sink(void* out, const uint8_t* block32, uint32_t rate);

/* Statistics for the end-of-run report. */
typedef struct GcrtAudioStats {
  uint64_t command_lists;   /* AX command lists processed */
  uint64_t voices;          /* active voices processed (per 5 ms frame) */
  uint64_t frames_out;      /* frames received by the output */
  uint64_t frames_dropped;  /* frames dropped (speaker queue full) */
  uint64_t underruns;       /* speaker underruns */
} GcrtAudioStats;
void gcrt_audio_get_stats(const GcrtAudioOut* out, GcrtAudioStats* stats);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_AUDIO_AUDIO_HW_H */
