/*
 * Runtime platform: connects the CPU layer (cpu/gcrt_cpu.h) and the native replacements of the SDK functions
 * (hle/gcrt_native.h; no emulated hardware at all since step 10d) to the window and inputs (Aurora/SDL3), the
 * audio output, the screenshots and the run log.
 *
 * Generic GameCube. Settings through environment variables (tools/run.py fills them in):
 *   GCRT_DATA_DIR      Aurora data folder: pipeline and shader caches, imgui.ini (default: <executable
 *                      folder>/gcrt-data if a file can be created there, otherwise <user folder>/gcrt-data, e.g. for
 *                      a copy installed in a read-only folder; notes/decisions.md, 2026-10-19)
 *   GCRT_USER_DIR      user folder of the tests and automated runs, instead of the SDL one: controls.ini
 *                      inside, saves in <folder>/saves/ (platform/gcrt_user_dir.h)
 *   GCRT_INPUT_KEYS    simulated physical keys, before translation by controls.ini (input_host.hpp)
 *   GCRT_DISC_IMAGE    disc image (.iso); default: "image" field of <disc>/../manifest.json
 *   GCRT_LANGUAGE      SRAM language: 0 English (default), 1 German, 2 French, 3 Spanish,
 *                      4 Italian, 5 Dutch
 *   GCRT_DVD_RATE      simulated drive throughput in bytes/s (default 3145728; 0 = instantaneous)
 *   GCRT_CAPTURE       screenshot times, in seconds of emulated time ("5,20,60")
 *   GCRT_CAPTURE_FRAMES numbers of the presented frames to capture (1 = first copy to the XFB)
 *   GCRT_CAPTURE_FIELDS VI field numbers: capture at the first frame presented afterwards
 *   GCRT_CAPTURE_DIR   screenshot folder (default: current folder)
 *   GCRT_PIPELINES     "sync" (default): before each EFB -> texture copy and at the end of each frame, wait until
 *                      every pipeline requested so far has been compiled (no draw skipped: Aurora skips a draw
 *                      whose pipeline is not ready when it hands the render pass over, which happens at both
 *                      places; emulated clock suspended during the wait);
 *                      "frame": wait at the end of each frame only (former behaviour: the draws of a pass ended
 *                      by an EFB -> texture copy can still be skipped); diagnostic;
 *                      "async": Aurora behaviour (draws skipped while the pipeline compiles); diagnostic
 *   GCRT_PIPELINES_PROBE 1: with "frame" or "async", count and trace the EFB -> texture copies forwarded while
 *                      pipelines are still compiling (makes Aurora catch up at each copy); default 0
 *   GCRT_PACING        "throttle" (default): emulated time = executed cycles, the host waits at each VI
 *                      field if it is ahead, a lag above GCRT_MAX_LAG (0.1 s) is dropped
 *                      (the game slows down instead of skipping time); "host": real-time clock of the
 *                      CPU layer (former behaviour); no effect with --clock cycles (no pacing)
 *   GCRT_IDLE_SKIP     fast-forward of busy waits (cpu/idle_skip.c): 1 (default) = active with the
 *                      "throttle" pacing only; 0 = off; force = also active with free cycles (evaluation:
 *                      A/B comparisons are then no longer strict)
 *   GCRT_HOST_CALL_FILTER 0 = no filter of the host_call addresses in the chassis (speed diagnostic)
 *   GCRT_TIMING_LOG    CSV pacing log (VI field starts, pacing, end of frame, presentation), in us
 *                      of steady_clock like GCRT_AUDIO_CAPTURE (notes/audio.md, section 14)
 *   GCRT_AUDIO, GCRT_AUDIO_WAV, GCRT_AUDIO_LATENCY_MS...: audio output (runtime/core/audio/audio_hw.h);
 *                      GCRT_AUDIO=sdl (default: speakers) | wav (recording only, into
 *                      GCRT_AUDIO_WAV or ./audio.wav) | off (no sound)
 *   GCRT_WINDOW        normal (default) | background (shown without taking the foreground) |
 *                      minimized: for automated runs
 *   GCRT_NO_FOCUS      1: "no focus" test mode, for launches on a computer someone is using (platform/gcrt_focus.h;
 *                      tools/run.py sets it unless the caller did): the game window is never activated nor raised
 *                      (shown without activation behind the other windows and not focusable, "normal" counting as
 *                      "background"; minimized if GCRT_WINDOW=minimized, then behind the others until minimized),
 *                      the menu's fullscreen setting is not applied, a console opened by the console option is created
 *                      minimized without activation, the foreground is never handed to the game window, and Shift
 *                      held at launch no longer shows a skipped setup menu. Default 0
 *   GCRT_SETUP_MENU    setup menu before the game (window size, fullscreen, render scale, console; gcrt_game_config.h):
 *                      auto (default): on a direct launch only, i.e. none of GCRT_WINDOW, GCRT_USER_DIR, GCRT_INPUT,
 *                      GCRT_INPUT_FRAMES, GCRT_STOP_AT, GCRT_STOP_FRAME, GCRT_CAPTURE_DIR, GCRT_RTC, GCRT_NO_FOCUS is set;
 *                      1 always; 0 never.
 *                      When the menu is used, its render scale replaces GCRT_RENDER_SCALE
 *   GCRT_SETUP_MENU_AUTO diagnostic, for tests without keyboard or foreground: start | close, optionally ":N".
 *                      After N passes of the menu loop (default 10; one pass = one menu frame, or 16 ms while the
 *                      window is minimized), acts as if "Start game" were pressed (with the current box values: the
 *                      saved file, or --console / --no-console) or as if the window were closed. No effect when the
 *                      menu is not shown
 *   GCRT_BACKEND       graphics backend: auto | d3d12 | vulkan (like --backend, which wins; both win over the
 *                      setup menu's saved choice, which only player launches read). Unknown or unavailable: auto,
 *                      with a message (trace and stderr). Trace line "graphics backend: ... in use, requested ..."
 *   GCRT_STOP_AT       stop requested when the emulated time reaches this value (seconds)
 *   GCRT_RENDER_SCALE  rendering resolution in multiples of the EFB (default 1 = console; 0 = window)
 *   GCRT_VIEWPORT      stretch (default: 4:3 width of the window) | fit (diagnostic: EFB rendered
 *                      at its exact size, e.g. 640x528, presented at that ratio)
 *   GCRT_COPY_FILTER   filter and gamma of the display copy: unblended (default, like Dolphin:
 *                      coefficients carried over to the current line) | full (vertical filter of the
 *                      console) | off (neither filter nor gamma)
 *   GCRT_EFB_HEIGHT    "console" (default): Aurora's logical EFB has the console's 528 lines, the display copy's
 *                      rectangle being the only part presented, captured and filtered (lines below it: shadow maps
 *                      and other EFB -> texture copies); "display": the EFB has the display copy's height
 *                      (former behaviour: a copy reaching below it is clipped, then stretched)
 *   GCRT_RTC           fixed clock: seconds since January 1st, 2000 at startup (reproducibility)
 *   GCRT_INPUT         scripted inputs of port 0: "t:BUTTONS[:duration]" separated by ";",
 *                      BUTTONS = A, B, X, Y, Z, L, R, START, UP, DOWN, LEFT, RIGHT joined by "+"
 *                      (e.g. "25:A:0.2;30:START"), default duration 0.2 s
 *   GCRT_INPUT_FRAMES  scripted inputs anchored on the presented frames: "frame:BUTTONS[:duration in frames]"
 *                      separated by ";" (A/B bench of milestone 10, notes/runtime.md, requests of 2026-10-10)
 *   GCRT_STOP_FRAME    stop requested once this frame has been presented
 *   GCRT_TIMELINE      diagnostic: timeline stamped in ticks (native interrupts, native audio events)
 *                      in this file, to compare two executables instant by instant (hle/gcrt_native.h)
 *   GCRT_CARD_MBIT     size of the native memory card (4 to 128, default 16); GCRT_CARD=absent: port A empty
 *                      (diagnostic: comparison with a reference without a card) (hle/native_card.c)
 *   GCRT_SAVE_FAULT    tests only: "before-rename:N" or "mid-write:N" kills the process during
 *                      the N-th safe write of a save (platform/gcrt_save_file.h)
 *   GCRT_HW_COUNT_FILE hardware access counter in JSON at the end (also --hw-count); a "hardware
 *                      accesses" line is written to the trace at each report
 *   GCRT_FORCE_ARG     diagnostic: "ADDRESS:rN=VALUE" forces a register on entry to a function,
 *                      "ADDRESS:[rN+OFFSET]=VALUE" or "ADDRESS:[ABSOLUTE]=VALUE" a 32-bit word of guest memory;
 *                      "&=" / "|=" instead of "=" for a read-modify-write; "ADDRESS@RETURN:" acts only on the
 *                      entries whose return address is RETURN; "#COUNT" at the end: first COUNT (matching)
 *                      entries only; items separated by ";" (hle/os_report.c)
 *   GCRT_WATCH         addresses (hex) whose passes at block start are counted (e.g. "80001234,80005678")
 *   GCRT_GX_STALE_COPIES "evict" (default): an EFB -> texture copy whose guest memory the game has rewritten since
 *                      is dropped and the texture is read from memory, like the console; "keep": former behavior
 *                      (Aurora keeps binding the copy); counted in the final GX report (gx/gx_bridge.hpp)
 *   GCRT_GX_FREEZE     "1" (default): the bridge gives Aurora's thread copies, taken when the game emits the command,
 *                      of the data passed by pointer (vertex arrays, textures, TLUTs), so rendering no longer depends
 *                      on when that thread runs; "0": no vertex array or texture copies, Aurora reads that guest memory itself
 *                      (diagnostic; TLUTs are still copied at every load, so not the behavior before the fix).
 *                      Counted in the final report ("GX frozen copies") (gx/gx_bridge.hpp)
 *   GCRT_GX_FREEZE_CHECK diagnostic: "1" compares, at every draw, the copies Aurora holds for the arrays and texture
 *                      maps the draw reads with guest memory; differences counted and the first 200 traced
 *                      ("GX frozen check", platform category)
 *   GCRT_GX_PUBLISH_BATCH draws published to Aurora's thread per wake-up, 1 to 4096 (default 128; 1 with
 *                      GCRT_GX_FREEZE=0)
 *   GCRT_GX_STREAM     diagnostic: fingerprint of the received GX command stream, one line per frame ("frame bytes
 *                      fnv64 draws") in this file; "1" = gx-stream.txt in GCRT_CAPTURE_DIR. GCRT_GX_STREAM_DUMP:
 *                      frames ("12,500") whose full stream is also written (<file>.<frame>.bin)
 *                      With GCRT_GX_STREAM on, two more diagnostics use its frame numbers and the draw index within
 *                      the frame (0-based, every draw counted, display lists included): GCRT_GX_SKIP_DRAWS
 *                      ("FRAME:FIRST-LAST[,...]") does not forward those draws to Aurora (find which draw makes a
 *                      pixel by bisection), GCRT_GX_DRAW_DUMP ("FRAME:DRAW[,...]") writes the state that draw uses
 *                      to stderr (lines "GXDRAW": BP, CP, XF registers, matrices, array bases, vertex bytes)
 *   GCRT_GX_TEV_COLOR_TRACE diagnostic: "1" writes one stderr line per GXSetTevColor / GXSetTevKColor call
 *                      ("TEVCOLOR C|K id RRGGBBAA return-address") (hle/native_gx_tev.c)
 *   GCRT_HLE_STACK_FILL diagnostic: "BYTE[:SIZE]" (hex byte, SIZE bytes, default 256) writes BYTE into the free guest
 *                      stack below r1 on entry into every replacement, where the original SDK code had its stack
 *                      frames: shows whether game code that reads uninitialized locals changes the output (hle/hle.c)
 *   GCRT_REPORT       period of the intermediate report in seconds (default 5); each report also gives the process
 *                      memory (working set, private bytes, handles)
 *   GCRT_CRASH_DIR, GCRT_CRASH_HANDLER: crash reports (platform/gcrt_crash.h; default folder <user folder>/crashes/,
 *                      or GCRT_CAPTURE_DIR in automated runs); GCRT_CRASH_TEST=<kind>:<frame>: deliberate fault once
 *                      that frame is presented (test of the crash handler). Kinds: read (non-canonical address, general
 *                      protection fault), write (NULL), abort, terminate (std::terminate), stack (stack overflow of the
 *                      game thread), purecall, invalid (invalid parameter to a C runtime function), thread (access
 *                      violation in a new host thread), render (access violation in Aurora's render worker thread),
 *                      heaplock (fault while the game thread holds the process heap lock), hangheap (the game thread
 *                      holds the heap lock for GCRT_CRASH_TEST_HANG seconds, then faults: hang report, then crash
 *                      report), hang (the game thread stops for GCRT_CRASH_TEST_HANG seconds, default 15, then goes on: test of
 *                      the watchdog). GCRT_CRASH_TEST_SUSPENDED=<ms>|heap|fault: what a report does while the other
 *                      threads are suspended (sleep, a 1 MiB allocation on the process heap, or a fault of the
 *                      reporting thread): test of the suspension guard (GCRT_CRASH_SUSPEND_LIMIT, platform/gcrt_crash.h)
 *                      and of the cut report
 *   GCRT_WATCHDOG      seconds without progress of the game loop (one heartbeat per emulated frame) before a hang
 *                      report (hang-*.txt/.dmp in the crash folder, threads and their stacks); default 60, 0 = off;
 *                      at most 3 per session, never kills the game, skipped under a debugger, not during the exit
 *                      (platform/gcrt_crash.h); the start-up is watched too (GCRT_WATCHDOG_STARTUP, text only)
 *   GCRT_WATCHDOG_STARTUP  the same before the game loop starts: seconds without progress of the start-up (window
 *                      creation, one beat per setup menu pass); default GCRT_WATCHDOG, at least 60; 0 = off
 *   GCRT_DIAG_EVENTS  size of the ring of recent native events kept for crash reports (frames, copies, disc reads,
 *                      save files, window events, settings, DSP tasks; platform/gcrt_diag.h): rounded up to a power of
 *                      two, default 4096, 0 = off
 *   GCRT_GUEST_HISTORY last guest blocks kept for crash reports (cpu/gcrt_cpu.h): power of two from 64 to 1048576,
 *                      default 1024
 *   GCRT_TRACE, GCRT_TRACE_FILE: traces (platform/trace.h)
 */
#ifndef GCRT_PLATFORM_H
#define GCRT_PLATFORM_H

#include "cpu/gcrt_cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GcrtPlatform GcrtPlatform;

/* To be called after loading the DOL and the HLE startup, before gcrt_cpu_run.
   `title`: window title; `game_id`: disc identifier; `disc_root`: root of the
   extracted disc. Returns NULL on failure (the CPU then runs without hardware). */
GcrtPlatform* gcrt_platform_attach(GcrtCpu* g, const char* title, const char* game_id, const char* disc_root);
/* After gcrt_cpu_run: finishes the current frame, writes the pending screenshots and the report. */
void gcrt_platform_detach(GcrtPlatform* platform);

#ifdef __cplusplus
}
#endif

#endif /* GCRT_PLATFORM_H */
