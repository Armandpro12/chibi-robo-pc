// Runtime platform (see gcrt_platform.h).
#include "gcrt_platform.h"

#include "../audio/audio_hw.h"
#include "../audio/audio_trace.hpp"
#include "../audio/dsp_ucode.h"
#include "../gx/efb_capture.hpp"
#include "../gx/gx_bridge.hpp"
#include "../gx/xfb_filter.hpp"
#include "aurora_host.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
#include "../hle/gcrt_native.h"
#include "gcrt_console.h"
#include "gcrt_focus.h"
#include "gcrt_crash.h"
#include "gcrt_game_config.h"
#include "gcrt_user_dir.h"
#include "trace.h"
}

namespace fs = std::filesystem;

namespace {
using gcrt::platform::PadState;

// Height of the console's EFB in lines (Dolphin, Source/Core/VideoCommon/VideoCommon.h: EFB_HEIGHT = 528). The
// display copy usually reads fewer lines (448 or 480); the game can still draw below them and copy that area to a
// texture, so Aurora's logical EFB gets this height (GCRT_EFB_HEIGHT, notes/runtime.md).
constexpr uint32_t kConsoleEfbLines = 528;

struct ScriptedInput {
  double start;
  double duration;
  uint16_t buttons;
};

// Scripted input anchored on the presented frames (A/B bench of milestone 10, GCRT_INPUT_FRAMES): active during
// the controller reads that precede the presentation of frames [first, first + count) (1 = first frame).
struct FrameInput {
  uint64_t first;
  uint64_t count;
  uint16_t buttons;
  bool applied;
  // Sticks pushed all the way (SUP, SDOWN, SLEFT, SRIGHT: main stick; CUP, CDOWN, CLEFT, CRIGHT: C-stick), 0 = untouched.
  int8_t stickX, stickY, substickX, substickY;
};

struct Watch {
  uint32_t address;
  uint64_t hits;
};

// Inputs of ports 0 to 3: host state (keyboard, SDL gamepads) plus the scripted inputs; read by the
// native controllers at PADRead time.
class PlatformInput {
public:
  bool read_pad(int port, PadState& out);

  std::vector<ScriptedInput> script;
  std::vector<FrameInput> frameScript;
  uint64_t framesPresented = 0; // frames already presented (copied from GcrtPlatform)
  uint64_t fields = 0;          // VI fields already delivered (for the trace)
  double now = 0.0;
  PadState cache[4]{};
  bool connected[4]{};
  uint64_t scriptedPresses = 0;
};

} // namespace

struct GcrtPlatform {
  GcrtCpu* g = nullptr;
  // GX command processor: decodes the stream produced by the native drawing (hle/native_gx*.c) and passes it to Aurora.
  gcrt::gx::FifoBridge bridge;
  PlatformInput input;
  bool frameOpen = false;
  bool stopRequested = false;
  uint64_t framesPresented = 0;
  uint64_t framesSkipped = 0;
  uint32_t efbWidth = 0;
  uint32_t efbHeight = 0;
  uint32_t efbLines = kConsoleEfbLines; // GCRT_EFB_HEIGHT: 0 = the display copy's height
  std::vector<double> captureTimes;
  size_t nextCapture = 0;
  std::vector<uint64_t> captureFrames; // numbers of the presented frames (1 = first)
  size_t nextCaptureFrame = 0;
  std::vector<uint64_t> captureFields; // VI field numbers
  size_t nextCaptureField = 0;
  // GCRT_PIPELINES (see gcrt_platform.h): when the game thread waits for Aurora's pipeline compilations.
  //   Sync : before each EFB -> texture copy and at the end of each frame (no draw ever skipped);
  //   Frame: at the end of each frame only (draws of the passes ended by an EFB -> texture copy can still be skipped);
  //   Async: never (Aurora's own behaviour).
  enum class PipelineWait { Sync, Frame, Async } pipelineMode = PipelineWait::Sync;
  bool xfbFilter = true;
  gcrt::gx::CopyFilterMode copyFilterMode = gcrt::gx::CopyFilterMode::Unblended;
  uint64_t filteredFrames = 0;
  double stopAt = 0.0; // stop requested at this emulated time (0: never)
  uint64_t stopFrame = 0; // stop requested when this frame is presented (GCRT_STOP_FRAME, 0: never)
  // Test of the crash handler (GCRT_CRASH_TEST=read|write|abort:<frame>): deliberate fault once this frame is presented.
  std::string crashTestKind;
  uint64_t crashTestFrame = 0;
  uint64_t pipelineStalls = 0;       // waits for pipelines (all places)
  double pipelineWait = 0.0;          // seconds
  uint64_t pipelineStallsCopy = 0;    // of which before an EFB -> texture copy
  double pipelineWaitCopy = 0.0;
  uint64_t copySyncs = 0;             // Aurora caught up before an EFB -> texture copy (Sync): count and host time
  double copySyncTime = 0.0;          // seconds spent in these flushes (cost of the Sync mode once the pipelines exist)
  uint64_t pendingAtCopy = 0;         // EFB -> texture copies forwarded with pipelines still compiling (Frame, Async)
  uint64_t pendingAtFrameEnd = 0;     // frames ended with pipelines still compiling (Async)
  bool pipelineProbe = false;         // GCRT_PIPELINES_PROBE=1: count pendingAtCopy (Frame, Async)
  std::string captureDir = ".";
  uint32_t capturesWritten = 0;
  std::vector<Watch> watches;
  GcrtHostCall previousHostCall = nullptr;
  double reportPeriod = 5.0;
  double nextReport = 5.0;
  std::chrono::steady_clock::time_point hostStart;
  GcrtAudioOut* audio = nullptr;
  gcrt::gx::DisplayCopyInfo lastCopy{};
  // Pacing (see gcrt_platform.h, GCRT_PACING): the emulated time advances with the executed cycles and
  // the host waits at each VI field if it is ahead; a lag is caught up to `maxLag`
  // seconds, beyond that the game slows down (like Dolphin) instead of skipping emulated time.
  enum class Pacing { Host, Throttle, Free } pacing = Pacing::Host;
  std::chrono::steady_clock::time_point paceHost;
  double paceEmu = 0.0;
  double maxLag = 0.1;
  uint64_t slowdowns = 0;
  double droppedLag = 0.0;
  double sleptTotal = 0.0;
  uint64_t lastFrames = 0;
  // GCRT_TIMING_LOG=file.csv: pacing log (diagnostic; line format: "Pacing log" comment above steady_us()).
  std::FILE* timingLog = nullptr;
};

namespace {
GcrtPlatform* g_platform = nullptr;
[[noreturn]] void crash_test(const std::string& kind);

double emulated_seconds(const GcrtPlatform* p) {
  return static_cast<double>(gcrt_cpu_ticks(p->g)) / static_cast<double>(GCRT_TB_HZ);
}

double trace_clock(void* user) { return emulated_seconds(static_cast<GcrtPlatform*>(user)); }

// Pacing log (GCRT_TIMING_LOG, diagnostic of stutters and audio gaps): one CSV line per
// event, instants in microseconds of steady_clock (same base as the audio capture, GCRT_AUDIO_CAPTURE):
//   F,host,emulated,wait,lag,dropped: VI field start; wait = pacing sleep (us), lag =
//                                        lag behind real time before pacing (us), dropped = 1 if dropped
//   P,host,emulated,gx,pipelines,filter,present,frame: end of frame (durations in us: GX flush, pipeline
//                                        wait, copy filter, aurora_end_frame)
//   B,host,emulated,duration,ok: aurora_begin_frame
int64_t steady_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void set_process_env(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}

std::string env(const char* name, const char* fallback = "") {
  const char* v = std::getenv(name);
  return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string(fallback);
}

std::vector<std::string> split(const std::string& s, const char* seps) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::strchr(seps, c) != nullptr) {
      if (!cur.empty()) {
        out.push_back(cur);
      }
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) {
    out.push_back(cur);
  }
  return out;
}

// Stick directions of the frame-scripted inputs (same names as tools/make_dtm.py for the main stick).
bool is_stick_name(const std::string& name) {
  static const char* const kNames[] = {"SUP", "SDOWN", "SLEFT", "SRIGHT", "CUP", "CDOWN", "CLEFT", "CRIGHT"};
  for (const char* n : kNames) {
    if (name == n) {
      return true;
    }
  }
  return false;
}

void parse_sticks(const std::string& spec, FrameInput& f) {
  for (const auto& name : split(spec, "+")) {
    if (name == "SUP") f.stickY = 127;
    if (name == "SDOWN") f.stickY = -127;
    if (name == "SLEFT") f.stickX = -127;
    if (name == "SRIGHT") f.stickX = 127;
    if (name == "CUP") f.substickY = 127;
    if (name == "CDOWN") f.substickY = -127;
    if (name == "CLEFT") f.substickX = -127;
    if (name == "CRIGHT") f.substickX = 127;
  }
}

uint16_t parse_buttons(const std::string& spec) {
  static const struct {
    const char* name;
    uint16_t bit;
  } kNames[] = {{"A", 0x0100},    {"B", 0x0200},  {"X", 0x0400},   {"Y", 0x0800},  {"START", 0x1000},
                {"Z", 0x0010},    {"R", 0x0020},  {"L", 0x0040},   {"UP", 0x0008}, {"DOWN", 0x0004},
                {"LEFT", 0x0001}, {"RIGHT", 0x0002}};
  uint16_t bits = 0;
  for (const auto& name : split(spec, "+")) {
    bool found = false;
    for (const auto& k : kNames) {
      if (name == k.name) {
        bits |= k.bit;
        found = true;
      }
    }
    if (!found && !is_stick_name(name)) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "GCRT_INPUT: unknown button '%s' ignored", name.c_str());
    }
  }
  return bits;
}

fs::path executable_dir() {
#if defined(_WIN32)
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    return fs::path(buf).parent_path();
  }
#endif
  return fs::current_path();
}

bool PlatformInput::read_pad(int port, PadState& out) {
  if (port == 0) {
    gcrt::platform::host_read_pads(cache, connected);
    // Scripted inputs (port 0): added to the real state.
    for (const auto& s : script) {
      if (now >= s.start && now < s.start + s.duration) {
        cache[0].buttons |= s.buttons;
        connected[0] = true;
      }
    }
    // Inputs anchored on frames: the read belongs to frame no. framesPresented + 1.
    const uint64_t reading = framesPresented + 1;
    for (size_t i = 0; i < frameScript.size(); ++i) {
      auto& f = frameScript[i];
      if (reading >= f.first && reading < f.first + f.count) {
        cache[0].buttons |= f.buttons;
        if (f.stickX != 0) cache[0].stickX = f.stickX;
        if (f.stickY != 0) cache[0].stickY = f.stickY;
        if (f.substickX != 0) cache[0].substickX = f.substickX;
        if (f.substickY != 0) cache[0].substickY = f.substickY;
        connected[0] = true;
        if (!f.applied) {
          f.applied = true;
          gcrt_trace_printf(GCRT_TRACE_PLATFORM, "scripted input applied (no. %zu, frame %llu, VI field %llu): buttons %04X",
                            i + 1, static_cast<unsigned long long>(reading), static_cast<unsigned long long>(fields),
                            f.buttons);
        }
      }
    }
  }
  if (port < 0 || port > 3) {
    return false;
  }
  out = cache[port];
  return connected[port];
}

// --------------------------------------------------------------------------------------------
// Callbacks of the CPU layer and of the native replacements
// --------------------------------------------------------------------------------------------
// Write-gather pipe (0xCC008000-0xCC008FFF and physical alias): vertices written inline by the game after GXBegin,
// handed to the native drawing (hle/native_gx.c), which counts them and adds them to the stream. No other register is
// served any more (step 10d: no emulated hardware): any other access outside RAM is "unhandled" (counted and traced).
inline bool is_gather_pipe(u32 address) { return (address & 0x3FFFF000u) == 0x0C008000u; }

bool bus_write(void* user, u32 address, u32 size, u64 value) {
  auto* p = static_cast<GcrtPlatform*>(user);
  if (is_gather_pipe(address)) {
    gcrt_native_gx_pipe_write(p->g, address, size, value);
    return true;
  }
  return false;
}

// Native drawing stream -> command processor (bridge to Aurora).
void gx_sink_consume(void* user, const u8* data, size_t size) { static_cast<GcrtPlatform*>(user)->bridge.consume(data, size); }

void gx_sink_abort(void* user) { static_cast<GcrtPlatform*>(user)->bridge.abort_pending(); }

// HLE DSP microcodes (runtime/core/audio) connected to the native DSP library (hle/native_audio.c).
u32 dsp_boot(void*, u32 iram, u32 length, u32 start_pc) { return gcrt_audio_dsp_boot(iram, length, start_pc); }
void dsp_mail(void*, u32 mail) { gcrt_audio_dsp_mail(mail); }
void dsp_update(void*) { gcrt_audio_dsp_update(); }
const char* dsp_name(void*) { return gcrt_audio_dsp_name(); }
void dsp_push_mail(void*, uint32_t mail, bool interrupt, uint32_t delay_cycles) {
  gcrt_native_dsp_push_mail(mail, interrupt, delay_cycles);
}
void dsp_clear_mails(void*) { gcrt_native_dsp_clear_mails(); }

// Makes Aurora process every GX command written so far, then waits until every pipeline requested so far is compiled.
// Why: Aurora skips a draw whose pipeline is not compiled yet when its render thread encodes the draw's render pass,
// and it hands a pass over at the end of the frame AND at each EFB -> texture copy (the copy ends the pass). Waiting
// before both keeps every draw, like the console. The time waited does not exist for the console: it is removed from
// the emulated clock, which restarts from here without catching up. Returns the time waited, in seconds.
double wait_for_pipelines(GcrtPlatform* p, bool beforeTextureCopy) {
  if (beforeTextureCopy) {
    const auto t0 = std::chrono::steady_clock::now();
    p->bridge.flush();
    ++p->copySyncs;
    p->copySyncTime += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  } else {
    p->bridge.flush();
  }
  const double waited = gcrt::platform::host_wait_pipelines(30.0);
  if (waited <= 0.0) {
    return 0.0;
  }
  ++p->pipelineStalls;
  p->pipelineWait += waited;
  if (beforeTextureCopy) {
    ++p->pipelineStallsCopy;
    p->pipelineWaitCopy += waited;
  }
  gcrt_cpu_set_clock(p->g, p->g->clock);
  p->paceHost += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(waited));
  GCRT_TRACE(GCRT_TRACE_PLATFORM, "pipelines: %.1f ms wait (image %llu%s)", waited * 1000.0,
             static_cast<unsigned long long>(p->framesPresented + 1), beforeTextureCopy ? ", EFB -> texture copy" : "");
  return waited;
}

// Bridge hook: EFB -> texture copy about to be forwarded to Aurora (end of an Aurora render pass).
void on_texture_copy(void* user) {
  auto* p = static_cast<GcrtPlatform*>(user);
  if (p->pipelineMode == GcrtPlatform::PipelineWait::Sync) {
    wait_for_pipelines(p, true);
    return;
  }
  // Diagnostic only (GCRT_PIPELINES_PROBE=1): pipelines still compiling when the pass is handed over. Reading the
  // count needs Aurora to have processed the commands written so far (a flush), which these modes do not do otherwise.
  if (p->pipelineProbe) {
    p->bridge.flush();
    if (const uint32_t pending = gcrt::platform::host_pending_pipelines(); pending != 0) {
      ++p->pendingAtCopy;
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "pipelines: %u still compiling at an EFB -> texture copy (image %llu): "
                        "draws of the pass may be skipped", pending,
                        static_cast<unsigned long long>(p->framesPresented + 1));
    }
  }
}

void open_frame(GcrtPlatform* p) {
  const int64_t t0 = p->timingLog != nullptr ? steady_us() : 0;
  p->frameOpen = gcrt::platform::host_begin_frame();
  p->bridge.set_output_enabled(p->frameOpen);
  if (p->timingLog != nullptr) {
    std::fprintf(p->timingLog, "B,%lld,%lld,%lld,%d\n", static_cast<long long>(t0),
                 static_cast<long long>(emulated_seconds(p) * 1e6), static_cast<long long>(steady_us() - t0),
                 p->frameOpen ? 1 : 0);
  }
}

// Video fields and vertical retraces of the native VI (hle/native_vi.c).
uint64_t vi_fields(GcrtPlatform*) { return gcrt_native_vi_fields(); }

uint64_t vi_interrupts(GcrtPlatform*) { return gcrt_native_vi_retraces(); }

void request_capture(GcrtPlatform* p, const char* name) {
  const std::string path = (fs::path(p->captureDir) / name).string();
  if (gcrt::gx::request_efb_capture(path.c_str())) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "capture requested (frame %llu, VI field %llu): %s",
                      static_cast<unsigned long long>(p->framesPresented + 1),
                      static_cast<unsigned long long>(vi_fields(p)), path.c_str());
  }
}

void on_display_copy(void* user, const gcrt::gx::DisplayCopyInfo& info) {
  auto* p = static_cast<GcrtPlatform*>(user);
  {
    // Display copy parameters: traced when they change (accuracy diagnostic).
    const gcrt::gx::DisplayCopyInfo& l = p->lastCopy;
    if (p->framesPresented == 0 || l.srcLeft != info.srcLeft || l.srcTop != info.srcTop ||
        l.srcWidth != info.srcWidth || l.srcHeight != info.srcHeight || l.yScaleRaw != info.yScaleRaw ||
        l.scaleEnabled != info.scaleEnabled || l.clear != info.clear || l.gamma != info.gamma ||
        l.clamp != info.clamp || l.frameToField != info.frameToField || l.copyFilter0 != info.copyFilter0 ||
        l.copyFilter1 != info.copyFilter1 || l.pixelFormat != info.pixelFormat || l.dstStrideBytes != info.dstStrideBytes) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                        "display copy (frame %llu): source (%u,%u) %ux%u, XFB 0x%08X stride %u, Y scale %s "
                        "(BP4E=%03X), clear %d, gamma %u, clamp %u, frame-to-field %u, filter %06X/%06X, EFB format %u",
                        static_cast<unsigned long long>(p->framesPresented + 1), info.srcLeft, info.srcTop,
                        info.srcWidth, info.srcHeight, info.xfbPhys, info.dstStrideBytes,
                        info.scaleEnabled ? "yes" : "no", info.yScaleRaw & 0x1FFu, info.clear ? 1 : 0, info.gamma,
                        info.clamp, info.frameToField, info.copyFilter0 & 0xFFFFFFu, info.copyFilter1 & 0xFFFFFFu,
                        info.pixelFormat);
    }
    p->lastCopy = info;
  }
  if (info.srcWidth != p->efbWidth || info.srcHeight != p->efbHeight) {
    p->efbWidth = info.srcWidth;
    p->efbHeight = info.srcHeight;
    gcrt::platform::host_configure_efb(info.srcWidth, info.srcHeight, p->efbLines);
  }
  const double now = emulated_seconds(p);
  if (p->frameOpen) {
    const int64_t tFrame = p->timingLog != nullptr ? steady_us() : 0;
    int64_t tPipe = 0, tFilter = 0, tPresent = 0;
    // All the commands of the frame processed, then all the pipelines compiled: otherwise Aurora
    // skips the draws whose pipeline is not ready (different rendering on the first launch).
    p->bridge.flush();
    if (p->timingLog != nullptr) {
      tPipe = steady_us();
    }
    if (p->pipelineMode != GcrtPlatform::PipelineWait::Async) {
      wait_for_pipelines(p, false);
    } else if (const uint32_t pending = gcrt::platform::host_pending_pipelines(); pending != 0) {
      ++p->pendingAtFrameEnd;
      GCRT_TRACE(GCRT_TRACE_PLATFORM, "pipelines: %u still compiling at the end of image %llu: draws may be skipped",
                 pending, static_cast<unsigned long long>(p->framesPresented + 1));
    }
    if (p->timingLog != nullptr) {
      tFilter = steady_us();
    }
    // Vertical filter and gamma of the display copy (like the console and Dolphin). Only if the
    // copy clears the EFB: otherwise the filtered content would stay in the EFB for the next frame.
    if (p->xfbFilter && info.clear &&
        gcrt::gx::apply_xfb_filter(gcrt::gx::with_mode(
            gcrt::gx::xfb_filter_params(info.copyFilter0, info.copyFilter1, info.gamma, info.srcHeight,
                                        (info.clamp & 2u) != 0),
            p->copyFilterMode))) {
      ++p->filteredFrames;
    }
    const uint64_t frame = p->framesPresented + 1;
    const uint64_t field = vi_fields(p);
    char name[64];
    if (p->nextCapture < p->captureTimes.size() && now >= p->captureTimes[p->nextCapture]) {
      std::snprintf(name, sizeof(name), "capture_%03.0fs.png", p->captureTimes[p->nextCapture]);
      request_capture(p, name);
      ++p->nextCapture;
    }
    while (p->nextCaptureFrame < p->captureFrames.size() && p->captureFrames[p->nextCaptureFrame] < frame) {
      ++p->nextCaptureFrame; // already passed
    }
    if (p->nextCaptureFrame < p->captureFrames.size() && p->captureFrames[p->nextCaptureFrame] == frame) {
      std::snprintf(name, sizeof(name), "capture_img%06llu.png", static_cast<unsigned long long>(frame));
      request_capture(p, name);
      ++p->nextCaptureFrame;
    }
    if (p->nextCaptureField < p->captureFields.size() && field >= p->captureFields[p->nextCaptureField]) {
      std::snprintf(name, sizeof(name), "capture_vi%06llu.png",
                    static_cast<unsigned long long>(p->captureFields[p->nextCaptureField]));
      request_capture(p, name);
      while (p->nextCaptureField < p->captureFields.size() && field >= p->captureFields[p->nextCaptureField]) {
        ++p->nextCaptureField;
      }
    }
    if (p->timingLog != nullptr) {
      tPresent = steady_us();
    }
    gcrt::platform::host_end_frame();
    ++p->framesPresented;
    p->input.framesPresented = p->framesPresented;
    if (p->timingLog != nullptr) {
      const int64_t tEnd = steady_us();
      std::fprintf(p->timingLog, "P,%lld,%lld,%lld,%lld,%lld,%lld,%llu\n", static_cast<long long>(tFrame),
                   static_cast<long long>(now * 1e6), static_cast<long long>(tPipe - tFrame),
                   static_cast<long long>(tFilter - tPipe), static_cast<long long>(tPresent - tFilter),
                   static_cast<long long>(tEnd - tPresent), static_cast<unsigned long long>(p->framesPresented));
    }
    if (p->stopFrame > 0 && p->framesPresented >= p->stopFrame && !p->stopRequested) {
      p->stopRequested = true;
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "frame %llu presented (GCRT_STOP_FRAME=%llu): stop requested",
                        static_cast<unsigned long long>(p->framesPresented), static_cast<unsigned long long>(p->stopFrame));
      gcrt_cpu_request_stop(p->g);
    }
    p->capturesWritten += static_cast<uint32_t>(gcrt::gx::poll_efb_captures());
    if (p->crashTestFrame > 0 && p->framesPresented >= p->crashTestFrame) {
      crash_test(p->crashTestKind);
    }
  } else {
    ++p->framesSkipped;
  }
  open_frame(p);
}

// Deliberate fault (test of the crash handler, GCRT_CRASH_TEST): read of a non-canonical address, write to NULL or
// abort().
[[noreturn]] void crash_test(const std::string& kind) {
  std::fflush(nullptr);
  if (kind == "abort") {
    std::abort();
  }
  // 0x8000000000000000 is non-canonical on x86-64 (bits 63..47 differ): a general protection fault, as described.
  volatile uintptr_t bad = kind == "write" ? 0 : static_cast<uintptr_t>(1) << (sizeof(uintptr_t) * 8 - 1);
  if (kind == "write") {
    *reinterpret_cast<volatile int*>(bad) = 1;
  } else {
    (void)*reinterpret_cast<volatile int*>(bad);
  }
  std::abort();
}

void report(GcrtPlatform* p, const char* label) {
  const double now = emulated_seconds(p);
  const double host = std::chrono::duration<double>(std::chrono::steady_clock::now() - p->hostStart).count();
  GcrtNativeAudioStats aus{};
  gcrt_native_audio_stats(&aus); // native DSP and AI (hle/native_audio.c)
  GcrtGxStats gxs{};
  gcrt_native_gx_stats(&gxs); // native drawing (hle/native_gx.c)
  u64 dvdCommands = 0, dvdBytes = 0;
  gcrt_native_dvd_stats(&dvdCommands, &dvdBytes); // native drive (hle/native_dvd.c)
  std::string watches;
  for (const auto& w : p->watches) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), " %08X=%llu", w.address, static_cast<unsigned long long>(w.hits));
    watches += buf;
  }
  gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                    "%s t=%.1f s (host %.1f s): images %llu (+%llu), VI fields %llu, VI interrupts %llu, "
                    "DSP %llu, AI %llu, draw finishes %llu; disc reads %llu "
                    "(%.1f MiB), DSP mails %llu/%llu (%s), blocks %llu; hits:%s",
                    label, now, host, static_cast<unsigned long long>(p->framesPresented),
                    static_cast<unsigned long long>(p->framesPresented - p->lastFrames),
                    static_cast<unsigned long long>(vi_fields(p)),
                    static_cast<unsigned long long>(vi_interrupts(p)),
                    // DSP: interrupts no. 5 (audio DMA) and no. 7 (DSP) delivered; AI: disc stream (none).
                    static_cast<unsigned long long>(aus.aid_interrupts + aus.dsp_interrupts), 0ull,
                    static_cast<unsigned long long>(gxs.finish_delivered),
                    static_cast<unsigned long long>(dvdCommands), static_cast<double>(dvdBytes) / (1024.0 * 1024.0),
                    static_cast<unsigned long long>(aus.mails_to_dsp),
                    static_cast<unsigned long long>(aus.mails_to_cpu), aus.ucode,
                    static_cast<unsigned long long>(p->g->blocks), watches.empty() ? " (none)" : watches.c_str());
  p->lastFrames = p->framesPresented;
  // Hardware access counter (milestone 10, cpu/hw_counter.c): line read by tools/abtrace.py (last line
  // containing "hardware accesses"; NAME=reads/writes pairs). Format described in notes/runtime.md. The total does
  // not count WGPIPE_OUTSIDE, already included in WGPIPE.
  char hw[512];
  const uint64_t total = gcrt_hw_count_total(p->g);
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "hardware accesses (0xCC00xxxx) %s: total %llu; %s", label,
                    static_cast<unsigned long long>(total), gcrt_hw_count_summary(p->g, hw, sizeof(hw)));
  // Process memory (diagnostic of leaks over long sessions; platform/gcrt_crash.h).
  u64 workingSet = 0, privateBytes = 0;
  u32 handles = 0;
  if (gcrt_process_memory(&workingSet, &privateBytes, &handles)) {
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "process memory %s: working set %.1f MiB, private %.1f MiB, handles %u",
                      label, static_cast<double>(workingSet) / 1048576.0, static_cast<double>(privateBytes) / 1048576.0,
                      handles);
  }
}

// Lines added to a crash report (platform/gcrt_crash.h): where the game was.
void crash_context(void* user, GcrtCrashAppend append, void* sink) {
  auto* p = static_cast<GcrtPlatform*>(user);
  char line[512];
  std::snprintf(line, sizeof(line),
                "guest time %.3f s, host time %.1f s; frames presented %llu (skipped %llu), VI fields %llu; "
                "pipeline waits %llu (%.3f s); last display copy %ux%u\n",
                emulated_seconds(p),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - p->hostStart).count(),
                static_cast<unsigned long long>(p->framesPresented), static_cast<unsigned long long>(p->framesSkipped),
                static_cast<unsigned long long>(vi_fields(p)), static_cast<unsigned long long>(p->pipelineStalls),
                p->pipelineWait, p->efbWidth, p->efbHeight);
  append(sink, line);
}

// Controller inputs for the native replacements (hle/native_pad.c): keyboard, SDL gamepads, inputs
// scripted in seconds or in frames.
bool native_pad_source(void* user, int port, GcrtPadInput* out) {
  auto* p = static_cast<GcrtPlatform*>(user);
  PadState st;
  const bool present = p->input.read_pad(port, st);
  out->buttons = st.buttons;
  out->stick_x = st.stickX;
  out->stick_y = st.stickY;
  out->substick_x = st.substickX;
  out->substick_y = st.substickY;
  out->trigger_l = st.triggerL;
  out->trigger_r = st.triggerR;
  out->analog_a = st.analogA;
  out->analog_b = st.analogB;
  return present;
}

void native_pad_rumble(void*, int port, int command) { gcrt::platform::host_rumble(port, command); }

void pace(GcrtPlatform* p, double now) {
  if (p->pacing != GcrtPlatform::Pacing::Throttle) {
    if (p->timingLog != nullptr) {
      std::fprintf(p->timingLog, "F,%lld,%lld,0,0,0\n", static_cast<long long>(steady_us()),
                   static_cast<long long>(now * 1e6));
    }
    return;
  }
  using clock = std::chrono::steady_clock;
  const double host = std::chrono::duration<double>(clock::now() - p->paceHost).count();
  const double ahead = (now - p->paceEmu) - host;
  if (p->timingLog != nullptr) {
    std::fprintf(p->timingLog, "F,%lld,%lld,%lld,%lld,%d\n", static_cast<long long>(steady_us()),
                 static_cast<long long>(now * 1e6), static_cast<long long>(ahead > 0.0 ? ahead * 1e6 : 0.0),
                 static_cast<long long>(ahead < 0.0 ? -ahead * 1e6 : 0.0), (-ahead > p->maxLag) ? 1 : 0);
  }
  if (ahead > 0.0) {
    // Ahead of the host: wait (sleep, then busy wait for the last millisecond).
    const auto until = clock::now() + std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(ahead));
    if (ahead > 0.002) {
      std::this_thread::sleep_for(std::chrono::duration<double>(ahead - 0.0015));
    }
    while (clock::now() < until) {
      std::this_thread::yield();
    }
    p->sleptTotal += ahead;
  } else if (-ahead > p->maxLag) {
    // Lag too large: it is dropped (slowdown) rather than caught up in one go.
    ++p->slowdowns;
    p->droppedLag += -ahead;
    p->paceHost = clock::now();
    p->paceEmu = now;
  }
}

void audio_trace_sink(void*, const char* category, const char* message) {
  gcrt_trace_printf(GCRT_TRACE_DSP, "[%s] %s", category, message);
}

void on_field(void* user, uint32_t, uint32_t, bool);

// Field start of the native VI.
void native_field(void* user, u64, bool odd) { on_field(user, 0, 0, odd); }

void on_field(void* user, uint32_t, uint32_t, bool) {
  auto* p = static_cast<GcrtPlatform*>(user);
  const double now = emulated_seconds(p);
  pace(p, now);
  p->input.now = now;
  p->input.fields = vi_fields(p);
  if (!gcrt::platform::host_poll_events() && !p->stopRequested) {
    p->stopRequested = true;
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "window closed: stop requested");
    gcrt_cpu_request_stop(p->g);
  }
  if (!p->frameOpen) {
    open_frame(p);
  }
  if (p->stopAt > 0.0 && now >= p->stopAt && !p->stopRequested) {
    p->stopRequested = true;
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "emulated time reached (GCRT_STOP_AT=%.2f s): stop requested", p->stopAt);
    gcrt_cpu_request_stop(p->g);
  }
  if (p->reportPeriod > 0.0 && now >= p->nextReport) {
    report(p, "report");
    while (p->nextReport <= now) {
      p->nextReport += p->reportPeriod;
    }
  }
}

bool watch_host_call(CPUState* cpu, u32 address) {
  GcrtPlatform* p = g_platform;
  if (p != nullptr) {
    for (auto& w : p->watches) {
      if (w.address == address) {
        ++w.hits;
      }
    }
    if (p->previousHostCall != nullptr) {
      return p->previousHostCall(cpu, address);
    }
  }
  return false;
}

void configure(GcrtPlatform* p) {
  for (const auto& t : split(env("GCRT_CAPTURE"), ",; ")) {
    p->captureTimes.push_back(std::strtod(t.c_str(), nullptr));
  }
  std::sort(p->captureTimes.begin(), p->captureTimes.end());
  p->captureDir = env("GCRT_CAPTURE_DIR", ".");
  for (const auto& t : split(env("GCRT_CAPTURE_FRAMES"), ",; ")) {
    p->captureFrames.push_back(std::strtoull(t.c_str(), nullptr, 0));
  }
  std::sort(p->captureFrames.begin(), p->captureFrames.end());
  for (const auto& t : split(env("GCRT_CAPTURE_FIELDS"), ",; ")) {
    p->captureFields.push_back(std::strtoull(t.c_str(), nullptr, 0));
  }
  std::sort(p->captureFields.begin(), p->captureFields.end());
  {
    const std::string mode = env("GCRT_PIPELINES", "sync");
    p->pipelineMode = mode == "async"   ? GcrtPlatform::PipelineWait::Async
                      : mode == "frame" ? GcrtPlatform::PipelineWait::Frame
                                        : GcrtPlatform::PipelineWait::Sync;
  }
  p->pipelineProbe = env("GCRT_PIPELINES_PROBE", "0") != "0";
  // EFB height: the console's 528 lines (default), or the display copy's height (former behaviour, diagnostic).
  p->efbLines = env("GCRT_EFB_HEIGHT", "console") == "display" ? 0u : kConsoleEfbLines;
  {
    // Display copy filter: unblended (default, like Dolphin), full (console) or off.
    const std::string mode = env("GCRT_COPY_FILTER", "unblended");
    p->xfbFilter = mode != "off";
    p->copyFilterMode =
        mode == "full" ? gcrt::gx::CopyFilterMode::Full : gcrt::gx::CopyFilterMode::Unblended;
  }
  p->stopAt = std::strtod(env("GCRT_STOP_AT", "0").c_str(), nullptr);
  for (const auto& item : split(env("GCRT_INPUT"), ";")) {
    const auto parts = split(item, ":");
    if (parts.size() < 2) {
      continue;
    }
    ScriptedInput s{};
    s.start = std::strtod(parts[0].c_str(), nullptr);
    s.buttons = parse_buttons(parts[1]);
    s.duration = parts.size() > 2 ? std::strtod(parts[2].c_str(), nullptr) : 0.2;
    p->input.script.push_back(s);
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "scripted input: t=%.2f s, buttons %04X, %.2f s", s.start, s.buttons,
                      s.duration);
  }
  p->stopFrame = std::strtoull(env("GCRT_STOP_FRAME", "0").c_str(), nullptr, 0);
  if (const auto parts = split(env("GCRT_CRASH_TEST"), ":"); parts.size() == 2) {
    p->crashTestKind = parts[0];
    p->crashTestFrame = std::strtoull(parts[1].c_str(), nullptr, 0);
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "GCRT_CRASH_TEST: deliberate %s fault at frame %llu", parts[0].c_str(),
                      static_cast<unsigned long long>(p->crashTestFrame));
  }
  for (const auto& item : split(env("GCRT_INPUT_FRAMES"), ";")) {
    const auto parts = split(item, ":");
    if (parts.size() < 2) {
      continue;
    }
    FrameInput f{};
    f.first = std::strtoull(parts[0].c_str(), nullptr, 0);
    f.buttons = parse_buttons(parts[1]);
    parse_sticks(parts[1], f);
    f.count = parts.size() > 2 ? std::strtoull(parts[2].c_str(), nullptr, 0) : 10;
    p->input.frameScript.push_back(f);
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "frame-scripted input: frame %llu, buttons %04X, %llu frames",
                      static_cast<unsigned long long>(f.first), f.buttons, static_cast<unsigned long long>(f.count));
  }
  for (const auto& a : split(env("GCRT_WATCH"), ",; ")) {
    p->watches.push_back({static_cast<uint32_t>(std::strtoul(a.c_str(), nullptr, 16)), 0});
  }
  p->reportPeriod = std::strtod(env("GCRT_REPORT", "5").c_str(), nullptr);
  p->nextReport = p->reportPeriod;
}
} // namespace

extern "C" GcrtPlatform* gcrt_platform_attach(GcrtCpu* g, const char* title, const char* game_id,
                                              const char* disc_root) {
  gcrt_trace_init();
  auto* p = new GcrtPlatform();
  p->g = g;
  g_platform = p;
  gcrt_trace_set_clock(trace_clock, p);
  p->hostStart = std::chrono::steady_clock::now();
  configure(p);

  // No emulated hardware any more (step 10d): disc, controllers, display, card, OS, EXI bus, GX drawing, audio memory,
  // DSP and AI are native (hle/native*.c: the disc image is opened there, GCRT_DISC_IMAGE and GCRT_DVD_RATE are
  // read there; the SRAM language, GCRT_LANGUAGE, is read by the native OS; the console clock, GCRT_RTC, at
  // startup).
  (void)disc_root;
  // Controllers: the native replacements read the inputs at PADRead time.
  gcrt_native_set_pad_source(native_pad_source, native_pad_rumble, p);

  // Audio (runtime/core/audio, notes/audio.md): HLE DSP microcodes (AX, CARD) connected to the native DSP
  // library, output of the native audio DMA (SDL speakers, WAV recording: GCRT_AUDIO, GCRT_AUDIO_WAV).
  // GCRT_AUDIO=wav: recording only, without speakers (automated runs). The audio module only
  // knows sdl|off: translated here, in the process environment only.
  if (env("GCRT_AUDIO") == "wav") {
    set_process_env("GCRT_AUDIO", "off");
    if (env("GCRT_AUDIO_WAV").empty()) {
      set_process_env("GCRT_AUDIO_WAV", "audio.wav");
    }
  }
  gcrt::audio::set_trace_sink(audio_trace_sink, nullptr);
  gcrt_audio_register_ucodes();
  p->audio = gcrt_audio_out_open();
  {
    const std::string speakers = env("GCRT_AUDIO", "sdl");
    const std::string wav = env("GCRT_AUDIO_WAV");
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "audio: speakers %s, WAV recording %s",
                      (speakers == "off" || speakers == "0" || speakers == "null") ? "no" : "yes",
                      wav.empty() ? "no" : wav.c_str());
  }
  gcrt_native_set_audio_sink(gcrt_audio_out_sink, p->audio);
  {
    static const GcrtDspBackend kBackend = {nullptr, dsp_boot, dsp_mail, dsp_update, dsp_name};
    gcrt_native_set_dsp_backend(&kBackend);
    const GcrtDspMailSink mails = {nullptr, dsp_push_mail, dsp_clear_mails};
    uint32_t aramSize = 0;
    uint8_t* aram = gcrt_native_aram(&aramSize);
    // Initial DSP state: ROM (mail 0x8071FEED in the queue, visible after DSPInit).
    gcrt_audio_dsp_attach(&mails, g->state.ram, std::min<uint32_t>(g->state.ram_size, GCRT_MEM1_SIZE), aram, aramSize);
  }

  // Pacing: by default, clock in cycles + pacing on real time (GCRT_PACING=throttle);
  // "host" keeps the real-time clock of the CPU layer; with --clock cycles, no pacing.
  if (g->clock == GCRT_CLOCK_CYCLES) {
    p->pacing = GcrtPlatform::Pacing::Free;
  } else if (env("GCRT_PACING", "throttle") == "throttle") {
    p->pacing = GcrtPlatform::Pacing::Throttle;
    gcrt_cpu_set_clock(g, GCRT_CLOCK_CYCLES);
  }
  p->maxLag = std::strtod(env("GCRT_MAX_LAG", "0.1").c_str(), nullptr);
  // Recognized busy waits of the game (cpu/idle_skip.c): the host waits for the next event instead of spinning, with
  // real-time pacing only (the emulated time follows the host clock there); with the clock in free cycles
  // (A/B comparisons), nothing changes. GCRT_IDLE_SKIP=0 turns them off. Milestone 10b: the OS idle state and the
  // looping OSYieldThread calls are waits of the native OS (hle/native_os.c), always active.
  {
    const std::string idle = env("GCRT_IDLE_SKIP", "1");
    const bool on = idle != "0" && p->pacing == GcrtPlatform::Pacing::Throttle;
    if (on) {
      gcrt_cpu_idle_skip_enable(g, true);
    }
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "game busy waits: host wait %s", on ? "active" : "inactive");
  }
  if (const std::string log = env("GCRT_TIMING_LOG"); !log.empty()) {
    p->timingLog = std::fopen(log.c_str(), "w");
    if (p->timingLog != nullptr) {
      std::setvbuf(p->timingLog, nullptr, _IOFBF, 1 << 20);
      std::fprintf(p->timingLog, "kind,host_us,emu_us,a,b,c,d,e\n");
    }
  }
  p->paceHost = std::chrono::steady_clock::now();
  p->paceEmu = emulated_seconds(p);

  // Window, rendering, gamepads.
  gcrt::platform::HostConfig host;
  host.title = std::string(title != nullptr ? title : "gcrt") + (game_id != nullptr ? std::string(" (") + game_id + ")" : "");
  host.dataDir = env("GCRT_DATA_DIR", (executable_dir() / "gcrt-data").string().c_str());
  host.renderScale = static_cast<float>(std::strtod(env("GCRT_RENDER_SCALE", "1").c_str(), nullptr));
  {
    const std::string window = env("GCRT_WINDOW", "normal");
    host.windowMode = window == "background" ? 1 : window == "minimized" ? 2 : 0;
    if (gcrt_no_focus() && host.windowMode == 0) {
      host.windowMode = 1; // "no focus" test mode (gcrt_focus.h): never activated, at least shown in the background
    }
  }
  host.viewportFit = env("GCRT_VIEWPORT", "stretch") == "fit";
  {
    // Setup menu (gcrt_game_config.h): for a player's direct launch only. "1" forces it, "0" disables it; by default
    // ("auto") it is off as soon as a launcher sets one of these variables (tools/run.py, test benches): nobody would
    // be there to click "Start game".
    const std::string menu = env("GCRT_SETUP_MENU", "auto");
    static const char* const kLauncherVars[] = {"GCRT_WINDOW",       "GCRT_USER_DIR", "GCRT_INPUT",
                                                "GCRT_INPUT_FRAMES", "GCRT_STOP_AT",  "GCRT_STOP_FRAME",
                                                "GCRT_CAPTURE_DIR",  "GCRT_RTC"};
    bool launched = false;
    for (const char* var : kLauncherVars) {
      launched |= std::getenv(var) != nullptr;
    }
    host.setupMenu = menu == "1" || (menu != "0" && host.windowMode == 0 && !launched);
  }
  std::error_code ec;
  fs::create_directories(host.dataDir, ec);
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "Aurora data: %s", host.dataDir.c_str());
  gcrt_trace_printf(GCRT_TRACE_PLATFORM, "%s", gcrt::platform::host_keyboard_help());
  if (!p->captureTimes.empty() || !p->captureFrames.empty() || !p->captureFields.empty()) {
    fs::create_directories(p->captureDir, ec);
  }
  if (!gcrt::platform::host_init(host)) {
    // The window was closed from the setup menu, before the game started: quit without running it.
    gcrt_trace_printf(GCRT_TRACE_PLATFORM, "window closed from the setup menu: exit");
    std::fflush(nullptr);
    std::exit(0);
  }
  // Console window (gcrt_console.h), opened or not now that the game starts. With the setup menu: its box (or the saved
  // value when the menu is skipped), in which --console / --no-console already set this launch's value. Without the
  // menu (automated launches): those options, otherwise the console of the parent process only (a launch from a
  // terminal prints there, like the former console program). Nothing happens when the parent redirected the output.
  if (host.setupMenu) {
    gcrt_console_start(gcrt_game_config()->console ? GCRT_CONSOLE_OPEN : GCRT_CONSOLE_NONE);
  } else {
    const int request = gcrt_console_request();
    gcrt_console_start(request == 1 ? GCRT_CONSOLE_OPEN : request == 0 ? GCRT_CONSOLE_NONE : GCRT_CONSOLE_PARENT);
  }
  // Crash reports of a direct launch go to the user folder (runs set GCRT_CAPTURE_DIR or GCRT_CRASH_DIR first).
  if (const char* userDir = gcrt_user_dir(); userDir != nullptr && userDir[0] != '\0') {
    gcrt_crash_handler_set_default_dir((std::string(userDir) + "crashes").c_str());
  }
  gcrt_crash_handler_set_context(crash_context, p);
  // GX command processor (bridge to Aurora): end of frame at the display copy; the stream comes from native drawing.
  p->bridge.set_guest_memory(g->state.ram, std::min<uint32_t>(g->state.ram_size, GCRT_MEM1_SIZE));
  p->bridge.reset();
  {
    gcrt::gx::BridgeHooks hooks;
    hooks.user = p;
    hooks.displayCopy = on_display_copy;
    hooks.textureCopy = on_texture_copy;
    p->bridge.set_hooks(hooks);
    const GcrtGxSink sink = {p, gx_sink_consume, gx_sink_abort};
    gcrt_native_set_gx_sink(&sink);
  }
  // Diagnostic: fingerprint of the GX command stream per frame (proof of identity between two paths, step 10c).
  // GCRT_GX_STREAM=1: file gx-stream.txt in the screenshot folder (GCRT_CAPTURE_DIR, run folder).
  if (std::string stream = env("GCRT_GX_STREAM"); !stream.empty() && stream != "0") {
    if (stream == "1") {
      stream = (fs::path(p->captureDir) / "gx-stream.txt").string();
    }
    const std::string dump = env("GCRT_GX_STREAM_DUMP");
    if (p->bridge.open_stream_log(stream.c_str(), dump.c_str())) {
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "GX stream fingerprint: %s", stream.c_str());
    }
  }
  // Field starts: native VI (hle/native_vi.c).
  gcrt_native_set_field_hook(native_field, p);
  open_frame(p);

  // Hook-up to the CPU layer: only the write-gather pipe is served outside RAM.
  gcrt_cpu_set_bus(g, nullptr, bus_write, p);
  if (!p->watches.empty()) {
    p->previousHostCall = g->host_call;
    g->host_call = watch_host_call;
    for (const auto& w : p->watches) {
      gcrt_cpu_mark_host_call(g, w.address); // host_call filter of the chassis
    }
  }
  static const char* const kPacing[] = {"host clock", "cycles paced on real time", "free cycles"};
  gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                    "hardware connected (language %u, RTC %s, pipelines %s, pacing: %s)",
                    static_cast<unsigned>(std::strtoul(env("GCRT_LANGUAGE", "0").c_str(), nullptr, 0)),
                    env("GCRT_RTC").empty() ? "host" : "fixed",
                    p->pipelineMode == GcrtPlatform::PipelineWait::Sync    ? "awaited before each render pass ends"
                    : p->pipelineMode == GcrtPlatform::PipelineWait::Frame ? "awaited at the end of each frame only"
                                                                           : "asynchronous",
                    kPacing[static_cast<int>(p->pacing)]);
  return p;
}

extern "C" void gcrt_platform_detach(GcrtPlatform* p) {
  if (p == nullptr) {
    return;
  }
  // Screenshots requested but not read back yet: a few more frames.
  if (p->frameOpen) {
    gcrt::platform::host_end_frame();
    p->frameOpen = false;
  }
  p->capturesWritten += static_cast<uint32_t>(gcrt::gx::poll_efb_captures());
  for (int i = 0; i < 120 && gcrt::gx::efb_captures_pending() > 0; ++i) {
    gcrt::platform::host_poll_events();
    if (gcrt::platform::host_begin_frame()) {
      gcrt::platform::host_end_frame();
    }
    p->capturesWritten += static_cast<uint32_t>(gcrt::gx::poll_efb_captures());
  }
  report(p, "FINAL REPORT");
  p->bridge.close_stream_log();
  // Host waits (native OS idle state, looping OSYieldThread, recognized busy waits of the game).
  gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                    "host waits: %llu, %.2f s of emulated time waited (%.1f %% of emulated time); of which game "
                    "busy waits: %llu (%u loops recognized)",
                    static_cast<unsigned long long>(p->g->waits),
                    static_cast<double>(p->g->waited_ticks) / static_cast<double>(GCRT_TB_HZ),
                    emulated_seconds(p) > 0.0 ? 100.0 * static_cast<double>(p->g->waited_ticks) /
                                                    static_cast<double>(GCRT_TB_HZ) / emulated_seconds(p)
                                              : 0.0,
                    static_cast<unsigned long long>(p->g->idle_skips), p->g->idle_loops);
  if (p->g->idle != nullptr) {
    gcrt_cpu_idle_report(p->g, stdout);
  }
  if (p->timingLog != nullptr) {
    std::fclose(p->timingLog);
    p->timingLog = nullptr;
  }
  const auto& gx = p->bridge.stats();
  gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                    "GX: %llu commands, %llu draws, %llu vertices, %llu display lists, %llu XFB copies, %llu texture "
                    "copies, %llu unknown opcodes, %llu invalid addresses; skipped frames %llu; "
                    "captures written %u; unhandled out-of-RAM accesses: %llu; pipeline "
                    "waits: %llu (%.2f s), of which before an EFB -> texture copy: %llu (%.2f s; %llu synchronizations "
                    "with Aurora at these copies, %.1f ms); pipelines still "
                    "compiling at an EFB -> texture copy: %llu, at a frame end: %llu; filtered frames (display copy): %llu; "
                    "textures read at a copy destination %llu, of which stale copies evicted %llu",
                    static_cast<unsigned long long>(gx.commands), static_cast<unsigned long long>(gx.draws),
                    static_cast<unsigned long long>(gx.vertices), static_cast<unsigned long long>(gx.displayLists),
                    static_cast<unsigned long long>(gx.displayCopies), static_cast<unsigned long long>(gx.efbCopies),
                    static_cast<unsigned long long>(gx.unknownOpcodes), static_cast<unsigned long long>(gx.badAddresses),
                    static_cast<unsigned long long>(p->framesSkipped), p->capturesWritten,
                    static_cast<unsigned long long>(p->g->io_unhandled),
                    static_cast<unsigned long long>(p->pipelineStalls), p->pipelineWait,
                    static_cast<unsigned long long>(p->pipelineStallsCopy), p->pipelineWaitCopy,
                    static_cast<unsigned long long>(p->copySyncs), p->copySyncTime * 1000.0,
                    static_cast<unsigned long long>(p->pendingAtCopy), static_cast<unsigned long long>(p->pendingAtFrameEnd),
                    static_cast<unsigned long long>(p->filteredFrames),
                    static_cast<unsigned long long>(gx.copyTextureLoads), static_cast<unsigned long long>(gx.staleCopies));
  if (p->audio != nullptr) {
    GcrtAudioStats st{};
    gcrt_audio_get_stats(p->audio, &st);
    GcrtNativeAudioStats aus{};
    gcrt_native_audio_stats(&aus);
    const uint64_t cycles = aus.dma_cycles;
    gcrt_trace_printf(GCRT_TRACE_PLATFORM,
                      "audio: %llu AX lists for %llu audio DMA cycles (%.1f %% of 5 ms frames not "
                      "computed), %llu voices, %llu frames output, %llu dropped, %llu underruns; "
                      "pacing: %llu slowdowns (%.2f s of lag dropped), %.1f s of host wait",
                      static_cast<unsigned long long>(st.command_lists), static_cast<unsigned long long>(cycles),
                      cycles > 0 && cycles > st.command_lists
                          ? 100.0 * static_cast<double>(cycles - st.command_lists) / static_cast<double>(cycles)
                          : 0.0,
                      static_cast<unsigned long long>(st.voices), static_cast<unsigned long long>(st.frames_out),
                      static_cast<unsigned long long>(st.frames_dropped),
                      static_cast<unsigned long long>(st.underruns), static_cast<unsigned long long>(p->slowdowns),
                      p->droppedLag, p->sleptTotal);
    gcrt_native_set_audio_sink(nullptr, nullptr);
    gcrt_audio_out_close(p->audio);
    p->audio = nullptr;
  }
  if (p->g->host_call == watch_host_call) {
    p->g->host_call = p->previousHostCall;
  }
  gcrt_native_set_pad_source(nullptr, nullptr, nullptr);
  gcrt_native_set_field_hook(nullptr, nullptr);
  gcrt_native_set_dsp_backend(nullptr);
  gcrt_audio_dsp_detach();
  gcrt_native_set_gx_sink(nullptr);
  gcrt_cpu_set_bus(p->g, nullptr, nullptr, nullptr);
  gcrt::platform::host_shutdown();
  gcrt_trace_set_clock(nullptr, nullptr);
  g_platform = nullptr;
  delete p;
}
