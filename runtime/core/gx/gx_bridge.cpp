// Guest GX FIFO -> Aurora bridge (see gx_bridge.hpp).
//
// References used for the encoding of commands and registers:
//   - Dolphin, Source/Core/VideoCommon/OpcodeDecoding.h (command sizes, CALL_DL
//     aligned on 32 bytes, opcodes 0x44/0x48), CPMemory.h/.cpp (physical address mask
//     0x03FFFFFF on GameCube), BPMemory.h (TexImage0/3, TexTLUT, BPU_LoadTlutInfo,
//     UPE_Copy, ScissorOffset), VertexLoaderBase.cpp + VertexLoader_*.h (vertex sizes),
//     VertexManagerBase.cpp:673 (viewport origin relative to the scissor offset),
//     commit e6f3ae17627e4344da95b13424af5baf4c892b08 (GPL-2.0-or-later). No code copied:
//     only the bit layouts are reused.
//   - Aurora @ 50e5be3 (MIT): lib/gx/command_processor.cpp and regs.cpp (what Aurora's
//     processor decodes, and in which form it expects the sub-commands).
// Unverified points are marked [hypothesis].
#include "gx_bridge.hpp"

#include "gx/fifo.hpp" // Aurora's internal API (external/aurora/lib)

#include <algorithm>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "../platform/trace.h"
}

namespace gcrt::gx {
namespace afifo = aurora::gx::fifo;

namespace {
// GX FIFO opcodes (Dolphin OpcodeDecoding.h).
constexpr uint8_t OP_NOP = 0x00;
constexpr uint8_t OP_LOAD_CP_REG = 0x08;
constexpr uint8_t OP_LOAD_XF_REG = 0x10;
constexpr uint8_t OP_LOAD_INDX_A = 0x20;
constexpr uint8_t OP_LOAD_INDX_B = 0x28;
constexpr uint8_t OP_LOAD_INDX_C = 0x30;
constexpr uint8_t OP_LOAD_INDX_D = 0x38;
constexpr uint8_t OP_CALL_DL = 0x40;
constexpr uint8_t OP_UNKNOWN_METRICS = 0x44;
constexpr uint8_t OP_INVL_VC = 0x48;
constexpr uint8_t OP_LOAD_BP_REG = 0x61;

// Aurora-specific extension (include/dolphin/gx/GXCommandList.h, GXAurora.h). Never
// accepted from the guest stream: it carries host pointers.
constexpr uint8_t OP_AURORA = 0x50;
constexpr uint16_t AUR_LOAD_ARRAYBASE = 0x0010;
constexpr uint16_t AUR_LOAD_TEXOBJ = 0x0030;
constexpr uint16_t AUR_LOAD_TLUT = 0x0031;
constexpr uint16_t AUR_LOAD_COPY_SRC = 0x0035;
constexpr uint16_t AUR_LOAD_COPY_DST = 0x0036;
constexpr uint16_t AUR_LOAD_COPY_DEST = 0x0037;

constexpr uint32_t PHYS_MASK = 0x03FFFFFFu; // GameCube (Dolphin CommandProcessor.h)

// Viewport origin: the SDK adds 342 (GXSetViewport, see the SDK decomp); Aurora
// subtracts 340 (regs.cpp, xf_load_viewport). Aurora decodes the scissor with 342.
constexpr float SDK_GUARD_OFFSET = 342.0f;
constexpr float AURORA_VIEWPORT_ORIGIN = 340.0f;

inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
inline uint32_t be32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}
inline float bits_to_float(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline uint32_t float_to_bits(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}

// Size of a component: U8, S8, U16, S16, F32 (5-7: invalid float, 4 bytes in Dolphin).
inline uint32_t comp_size(uint32_t fmt) {
  switch (fmt) {
  case 0:
  case 1:
    return 1;
  case 2:
  case 3:
    return 2;
  default:
    return 4;
  }
}

// Size of a direct color: RGB565, RGB8, RGBX8, RGBA4, RGBA6, RGBA8 (VertexLoader_Color.h).
inline uint32_t color_size(uint32_t fmt) {
  static constexpr uint8_t sizes[8] = {2, 3, 4, 2, 3, 4, 4, 4};
  return sizes[fmt & 7];
}

// Size in bytes of a texture (level 0) in its tiled format.
uint32_t texture_bytes(uint32_t fmt, uint32_t w, uint32_t h) {
  uint32_t bw = 4, bh = 4, bytes = 32;
  switch (fmt) {
  case 0x0: // I4
  case 0x8: // C4
  case 0xE: // CMPR
    bw = 8;
    bh = 8;
    break;
  case 0x1: // I8
  case 0x2: // IA4
  case 0x9: // C8
    bw = 8;
    bh = 4;
    break;
  case 0x6: // RGBA8: two 4x4 tiles of 32 bytes (AR then GB)
    bytes = 64;
    break;
  default: // IA8, RGB565, RGB5A3, C14X2
    break;
  }
  return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
}

// Hardware EFB copy format -> Aurora's GXTexFmt.
// realFmt = "real" format in Dolphin's sense (UPE_Copy::tp_realFormat).
// [hypothesis] mapping reconstructed from Dolphin's EFBCopyFormat enumeration
// (TextureDecoder.h) and the GXTexFmt values; to be checked on GXSetTexCopyDst in Ghidra.
uint32_t efb_copy_format(uint32_t realFmt, bool depth, bool intensity) {
  if (depth) {
    switch (realFmt) {
    case 0x0:
      return 0x30; // GX_CTF_Z4
    case 0x1:
    case 0x8:
      return 0x11; // GX_TF_Z8
    case 0x3:
    case 0xB:
      return 0x13; // GX_TF_Z16
    case 0x6:
      return 0x16; // GX_TF_Z24X8
    case 0x9:
      return 0x39; // GX_CTF_Z8M
    case 0xA:
      return 0x3A; // GX_CTF_Z8L
    case 0xC:
      return 0x3C; // GX_CTF_Z16L
    default:
      return 0x16;
    }
  }
  if (intensity) {
    switch (realFmt) {
    case 0x0:
      return 0x0; // GX_TF_I4
    case 0x1:
    case 0x8:
      return 0x1; // GX_TF_I8
    case 0x2:
      return 0x2; // GX_TF_IA4
    case 0x3:
      return 0x3; // GX_TF_IA8
    default:
      break;
    }
  }
  switch (realFmt) {
  case 0x0:
    return 0x20; // GX_CTF_R4
  case 0x1:
  case 0x8:
    return 0x28; // GX_CTF_R8
  case 0x2:
    return 0x22; // GX_CTF_RA4
  case 0x3:
    return 0x23; // GX_CTF_RA8
  case 0x4:
    return 0x4; // GX_TF_RGB565
  case 0x5:
    return 0x5; // GX_TF_RGB5A3
  case 0x6:
    return 0x6; // GX_TF_RGBA8
  case 0x7:
    return 0x27; // GX_CTF_A8
  case 0x9:
    return 0x29; // GX_CTF_G8
  case 0xA:
    return 0x2A; // GX_CTF_B8
  case 0xB:
    return 0x2B; // GX_CTF_RG8
  case 0xC:
    return 0x2C; // GX_CTF_GB8
  default:
    return 0x6;
  }
}

// Outputs to Aurora's FIFO, with an optional copy for diagnostics
// (FifoBridge::set_recording: comparison with the stream produced by Aurora's GX API).
std::vector<uint8_t>* g_record = nullptr;
// Forwarding to Aurora: cut while no Aurora frame is open (minimized window...),
// because its command processor requires a frame being recorded.
bool g_output = true;

void out_data(const void* data, uint32_t n) {
  if (!g_output) {
    return;
  }
  afifo::write_data(data, n);
  if (g_record != nullptr) {
    const auto* b = static_cast<const uint8_t*>(data);
    g_record->insert(g_record->end(), b, b + n);
  }
}
void out_u8(uint8_t v) { out_data(&v, 1); }
void out_u16(uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
  out_data(b, 2);
}
void out_u32(uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8),
                        static_cast<uint8_t>(v)};
  out_data(b, 4);
}
void out_u64(uint64_t v) {
  out_u32(static_cast<uint32_t>(v >> 32));
  out_u32(static_cast<uint32_t>(v));
}

void write_aurora_header(uint16_t sub) {
  out_u8(OP_AURORA);
  out_u16(sub);
}

// Publishing of draws to the "Aurora FIFO processor" thread: afifo::finish_draw() publishes everything written so far
// and wakes that thread up (a kernel call when it sleeps). It is only called on one draw out of N
// (GCRT_GX_PUBLISH_BATCH, 1 to 4096): fewer wake-ups, same stream, same order; the rest is published by the next draw
// or by afifo::drain() (draw done via GXSetDrawDone, tokens, end of frame), which waits until everything is processed.
// 128: measured (cycle bench) -6% host time and -19% CPU time versus 1 (one wake-up per draw), like 512; 8 and 32
// gain less (notes/performance.md, post-10c section). 1 restores the old behaviour. Caution: batching changes the
// outcome of a race already present with 1 (measured on one frame of a test scene, notes/performance.md, post-10c
// section, subsection 4).
// [hypothesis] cause: Aurora's thread reads the game's RAM (arrays, textures) when it processes a command, not when
// the game emits it, and batching delays that read.
// Default 1 (fidelity first) as long as the data read by that thread is not frozen at emission; go back to 128
// once that fix is done, since rendering will then no longer depend on batching.
constexpr uint32_t kDefaultPublishBatch = 1;
uint32_t g_publishBatch = 0; // 0: not read yet
uint32_t g_unpublished = 0;

uint32_t publish_batch() {
  if (g_publishBatch == 0) {
    const char* env = std::getenv("GCRT_GX_PUBLISH_BATCH");
    const long v = env != nullptr ? std::strtol(env, nullptr, 10) : static_cast<long>(kDefaultPublishBatch);
    g_publishBatch = static_cast<uint32_t>(std::clamp(v, 1L, 4096L));
  }
  return g_publishBatch;
}
} // namespace

FifoBridge::FifoBridge() { reset(); }

void FifoBridge::reset() {
  m_vcdLo = m_vcdHi = 0;
  m_vatA.fill(0);
  m_vatB.fill(0);
  m_vatC.fill(0);
  m_arrays.fill({});
  m_bp.fill(0);
  m_bpValid.fill(false);
  m_cpRegs.fill(0);
  m_cpValid.fill(false);
  m_xfRegs.fill(0);
  m_xfRegValid.fill(false);
  m_xfUnitWritten.clear();
  // Value written by GXInit (GXSetScissorBoxOffset(0, 0)): (0 + 342) / 2 for x and y.
  // [hypothesis] value at startup before any write; the SDK writes it anyway.
  m_bp[0x59] = (171u << 10) | 171u;
  m_bpMask = 0x00FFFFFFu;
  m_bpMaskPending = false;
  m_xfMem.fill(0);
  m_viewport.fill(0);
  m_projection.fill(0);
  m_viewportValid = m_projectionValid = false;
  m_texDirty = 0xFF;
  for (auto& t : m_tluts) {
    t = {};
  }
  m_tlutClock = 0;
  m_vtxSizeVat = 0xFFFFFFFFu;
  m_pending.clear();
  m_pendingOff = 0;
  m_inDisplayList = false;
  m_stats = {};
}

void FifoBridge::set_recording(std::vector<uint8_t>* out) { g_record = out; }

void FifoBridge::set_output_enabled(bool enabled) {
  const bool was = g_output;
  g_output = enabled;
  if (enabled && !was) {
    resync();
  }
}

bool FifoBridge::output_enabled() const { return g_output; }

void FifoBridge::flush() {
  if (g_output) {
    afifo::drain();
    g_unpublished = 0;
  }
}

void FifoBridge::resync() {
  // Re-sends all known state (mirrors) to Aurora, after a period without forwarding.
  if (!g_output) {
    return;
  }
  for (uint32_t r = 0; r < 0x100; ++r) {
    if (m_cpValid[r] && (r < 0xA0 || r >= 0xB0)) {
      out_u8(OP_LOAD_CP_REG);
      out_u8(static_cast<uint8_t>(r));
      out_u32(m_cpRegs[r]);
    }
  }
  for (uint32_t unit : m_xfUnitWritten) {
    emit_xf_unit(unit);
  }
  for (uint32_t r = 0; r < m_xfRegs.size(); ++r) {
    if (m_xfRegValid[r] && !(r >= 0x1A && r <= 0x26)) {
      out_u8(OP_LOAD_XF_REG);
      out_u32(0x1000u + r);
      out_u32(m_xfRegs[r]);
    }
  }
  emit_viewport();
  if (m_projectionValid) {
    out_u8(OP_LOAD_XF_REG);
    out_u32((6u << 16) | 0x1020u);
    for (uint32_t w : m_projection) {
      out_u32(w);
    }
  }
  for (uint32_t r = 0; r < 0x100; ++r) {
    switch (r) {
    case 0x20: case 0x21: case 0x45: case 0x47: case 0x48: case 0x52: case 0x59:
    case 0x64: case 0x65: case 0x66: case 0x67: case 0xFE:
      continue; // triggers or registers re-sent separately
    default:
      break;
    }
    if (m_bpValid[r]) {
      emit_bp(r << 24 | m_bp[r]);
    }
  }
  if (m_bpValid[0x59]) {
    emit_bp(0x59000000u | m_bp[0x59]);
  }
  emit_scissor();
  for (auto& arr : m_arrays) {
    arr.declaredBase = 0xFFFFFFFFu;
    arr.declaredSize = 0;
  }
  m_texDirty = 0xFF;
  GCRT_TRACE(GCRT_TRACE_GX, "GX state re-sent to Aurora");
}

void FifoBridge::set_guest_memory(const uint8_t* mem1, uint32_t size) {
  m_mem1 = mem1;
  m_mem1Size = size;
}

const uint8_t* FifoBridge::host_ptr(uint32_t phys, uint32_t size) const {
  phys &= PHYS_MASK;
  if (m_mem1 == nullptr || phys >= m_mem1Size || size > m_mem1Size - phys) {
    return nullptr;
  }
  return m_mem1 + phys;
}

bool FifoBridge::open_stream_log(const char* path, const char* dumpFrames) {
  close_stream_log();
  if (path == nullptr || path[0] == '\0') {
    return false;
  }
  m_streamLog = std::fopen(path, "w");
  if (m_streamLog == nullptr) {
    return false;
  }
  std::setvbuf(m_streamLog, nullptr, _IOFBF, 1 << 16);
  std::fprintf(m_streamLog, "# frame bytes fnv64 draws (top-level GX stream, between two display copies)\n");
  m_streamPath = path;
  m_streamDumpFrames.clear();
  if (dumpFrames != nullptr) {
    const char* s = dumpFrames;
    while (*s != '\0') {
      char* end = nullptr;
      const unsigned long long v = std::strtoull(s, &end, 10);
      if (end == s) {
        ++s;
        continue;
      }
      m_streamDumpFrames.insert(v);
      s = end;
    }
  }
  m_streamHash = 0xCBF29CE484222325ull;
  m_streamBytes = 0;
  m_streamDraws = 0;
  m_streamFrame = 1;
  if (m_streamDumpFrames.count(m_streamFrame) != 0) {
    m_streamDump = std::fopen((m_streamPath + "." + std::to_string(m_streamFrame) + ".bin").c_str(), "wb");
  }
  return true;
}

void FifoBridge::close_stream_log() {
  if (m_streamLog != nullptr) {
    std::fprintf(m_streamLog, "%llu %llu %016llx %llu end\n", static_cast<unsigned long long>(m_streamFrame),
                 static_cast<unsigned long long>(m_streamBytes), static_cast<unsigned long long>(m_streamHash),
                 static_cast<unsigned long long>(m_streamDraws));
    std::fclose(m_streamLog);
    m_streamLog = nullptr;
  }
  if (m_streamDump != nullptr) {
    std::fclose(m_streamDump);
    m_streamDump = nullptr;
  }
}

void FifoBridge::stream_hash(const uint8_t* p, size_t n) {
  uint64_t h = m_streamHash;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  m_streamHash = h;
  m_streamBytes += n;
  if (m_streamDump != nullptr) {
    std::fwrite(p, 1, n, m_streamDump);
  }
}

void FifoBridge::stream_frame_end() {
  std::fprintf(m_streamLog, "%llu %llu %016llx %llu\n", static_cast<unsigned long long>(m_streamFrame),
               static_cast<unsigned long long>(m_streamBytes), static_cast<unsigned long long>(m_streamHash),
               static_cast<unsigned long long>(m_streamDraws));
  m_streamHash = 0xCBF29CE484222325ull;
  m_streamBytes = 0;
  m_streamDraws = 0;
  ++m_streamFrame;
  if (m_streamDump != nullptr) {
    std::fclose(m_streamDump);
    m_streamDump = nullptr;
  }
  if (m_streamDumpFrames.count(m_streamFrame) != 0) {
    m_streamDump = std::fopen((m_streamPath + "." + std::to_string(m_streamFrame) + ".bin").c_str(), "wb");
  }
}

void FifoBridge::consume(const uint8_t* data, size_t size) {
  if (pending_bytes() == 0) {
    m_pending.clear();
    m_pendingOff = 0;
    size_t off = 0;
    while (off < size) {
      const size_t n = run_command(data + off, size - off, false);
      if (n == 0) {
        break;
      }
      if (m_streamLog != nullptr) {
        stream_hash(data + off, n);
      }
      off += n;
    }
    if (off < size) {
      m_pending.assign(data + off, data + size);
    }
    return;
  }
  m_pending.insert(m_pending.end(), data, data + size);
  while (m_pendingOff < m_pending.size()) {
    const size_t n = run_command(m_pending.data() + m_pendingOff, m_pending.size() - m_pendingOff, false);
    if (n == 0) {
      break;
    }
    if (m_streamLog != nullptr) {
      stream_hash(m_pending.data() + m_pendingOff, n);
    }
    m_pendingOff += n;
  }
  if (m_pendingOff == m_pending.size()) {
    m_pending.clear();
    m_pendingOff = 0;
  } else if (m_pendingOff > 64 * 1024) {
    m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<std::ptrdiff_t>(m_pendingOff));
    m_pendingOff = 0;
  }
}

void FifoBridge::abort_pending() {
  if (pending_bytes() != 0) {
    GCRT_TRACE(GCRT_TRACE_GX, "stream aborted: %zu bytes of an incomplete command discarded", pending_bytes());
  }
  m_pending.clear();
  m_pendingOff = 0;
}

void FifoBridge::forward_raw(const uint8_t* p, size_t n) { out_data(p, static_cast<uint32_t>(n)); }

void FifoBridge::emit_bp(uint32_t raw) {
  out_u8(OP_LOAD_BP_REG);
  out_u32(raw);
}

size_t FifoBridge::run_command(const uint8_t* p, size_t avail, bool inDisplayList) {
  if (avail == 0) {
    return 0;
  }
  const uint8_t op = p[0];
  switch (op) {
  case OP_NOP: {
    size_t n = 1;
    while (n < avail && p[n] == OP_NOP) {
      ++n;
    }
    return n;
  }
  case OP_LOAD_CP_REG:
    if (avail < 6) {
      return 0;
    }
    ++m_stats.commands;
    on_cp(p[1], be32(p + 2));
    return 6;
  case OP_LOAD_XF_REG: {
    if (avail < 5) {
      return 0;
    }
    const uint32_t header = be32(p + 1);
    // Dolphin: only 4 bits of the count are significant (16 words at most).
    const uint32_t count = ((header >> 16) & 0xFu) + 1u;
    if (avail < 5 + count * 4) {
      return 0;
    }
    ++m_stats.commands;
    on_xf(static_cast<uint16_t>(header & 0xFFFFu), count, p + 5);
    return 5 + count * 4;
  }
  case OP_LOAD_INDX_A:
  case OP_LOAD_INDX_B:
  case OP_LOAD_INDX_C:
  case OP_LOAD_INDX_D: {
    if (avail < 5) {
      return 0;
    }
    ++m_stats.commands;
    const uint32_t v = be32(p + 1);
    on_indexed_xf_load(12u + (op - OP_LOAD_INDX_A) / 8u, v >> 16, static_cast<uint16_t>(v & 0xFFFu),
                       ((v >> 12) & 0xFu) + 1u);
    return 5;
  }
  case OP_CALL_DL: {
    if (avail < 9) {
      return 0;
    }
    ++m_stats.commands;
    const uint32_t addr = be32(p + 1) & ~31u;
    const uint32_t size = be32(p + 5) & ~31u;
    if (inDisplayList || m_inDisplayList) {
      GCRT_TRACE(GCRT_TRACE_GX, "nested CALL_DL ignored (0x%08X, %u bytes)", addr, size);
    } else {
      run_display_list(addr, size);
    }
    return 9;
  }
  case OP_UNKNOWN_METRICS:
    ++m_stats.commands;
    GCRT_TRACE(GCRT_TRACE_GX, "opcode 0x44 (metrics) ignored");
    return 1;
  case OP_INVL_VC:
    ++m_stats.commands;
    out_u8(OP_INVL_VC);
    return 1;
  case OP_LOAD_BP_REG:
    if (avail < 5) {
      return 0;
    }
    ++m_stats.commands;
    on_bp(be32(p + 1));
    return 5;
  default:
    if (op >= 0x80 && op <= 0xBF) {
      return on_draw(p, avail);
    }
    ++m_stats.unknownOpcodes;
    GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: unknown opcode 0x%02X ignored (1 byte)", op);
    return 1;
  }
}

void FifoBridge::run_display_list(uint32_t phys, uint32_t size) {
  const uint8_t* dl = host_ptr(phys, size);
  if (dl == nullptr) {
    ++m_stats.badAddresses;
    GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: display list outside RAM (0x%08X, %u bytes)", phys, size);
    return;
  }
  ++m_stats.displayLists;
  GCRT_TRACE(GCRT_TRACE_GX, "CALL_DL 0x%08X (%u bytes)", phys, size);
  m_inDisplayList = true;
  size_t off = 0;
  while (off < size) {
    const size_t n = run_command(dl + off, size - off, true);
    if (n == 0) {
      GCRT_TRACE(GCRT_TRACE_GX, "truncated display list: %zu remaining bytes ignored", size - off);
      break;
    }
    off += n;
  }
  m_inDisplayList = false;
}

// ---------------------------------------------------------------------------------------------
// CP
// ---------------------------------------------------------------------------------------------
void FifoBridge::on_cp(uint8_t reg, uint32_t value) {
  switch (reg & 0xF0) {
  case 0x50:
    m_vcdLo = value;
    m_vtxSizeVat = 0xFFFFFFFFu;
    break;
  case 0x60:
    m_vcdHi = value;
    m_vtxSizeVat = 0xFFFFFFFFu;
    break;
  case 0x70:
    m_vatA[reg & 7] = value;
    m_vtxSizeVat = 0xFFFFFFFFu;
    break;
  case 0x80:
    m_vatB[reg & 7] = value;
    m_vtxSizeVat = 0xFFFFFFFFu;
    break;
  case 0x90:
    m_vatC[reg & 7] = value;
    m_vtxSizeVat = 0xFFFFFFFFu;
    break;
  case 0xA0:
    // Array base: physical address. Aurora rejects this register (regs.cpp,
    // cp_arraybase_unsupported); the translation is done at draw time.
    m_arrays[reg & 0xF].base = value & PHYS_MASK;
    GCRT_TRACE(GCRT_TRACE_GX, "CP array %u base=0x%08X", reg & 0xF, value & PHYS_MASK);
    return;
  case 0xB0:
    m_arrays[reg & 0xF].stride = static_cast<uint8_t>(value & 0xFF);
    break;
  default:
    break;
  }
  m_cpRegs[reg] = value;
  m_cpValid[reg] = true;
  out_u8(OP_LOAD_CP_REG);
  out_u8(reg);
  out_u32(value);
}

// ---------------------------------------------------------------------------------------------
// XF
// ---------------------------------------------------------------------------------------------
void FifoBridge::on_xf(uint16_t addr, uint32_t count, const uint8_t* words) {
  // XF memory units touched (re-sent in full: Aurora only accepts complete
  // matrices, see copy_xf_data in regs.cpp).
  uint32_t units[16];
  uint32_t unitCount = 0;
  bool viewportTouched = false;
  bool projectionTouched = false;

  auto add_unit = [&](uint32_t u) {
    for (uint32_t k = 0; k < unitCount; ++k) {
      if (units[k] == u) {
        return;
      }
    }
    if (unitCount < 16) {
      units[unitCount++] = u;
    }
  };

  uint32_t i = 0;
  while (i < count) {
    const uint32_t a = addr + i;
    if (a < 0x1000) {
      const uint32_t v = be32(words + i * 4);
      if (a < m_xfMem.size()) {
        m_xfMem[a] = v;
      }
      if (a < 0xF0) {
        add_unit((a / 12) * 12);
      } else if (a >= 0x400 && a < 0x45A) {
        add_unit(0x400 + ((a - 0x400) / 9) * 9);
      } else if (a >= 0x500 && a < 0x5F0) {
        add_unit(0x500 + ((a - 0x500) / 12) * 12);
      } else if (a >= 0x600 && a < 0x680) {
        add_unit(0x600 + ((a - 0x600) / 16) * 16);
      } else {
        GCRT_TRACE(GCRT_TRACE_GX, "XF memory 0x%04X not handled by Aurora (ignored)", a);
      }
      ++i;
      continue;
    }
    if (a >= 0x101A && a <= 0x101F) {
      m_viewport[a - 0x101A] = be32(words + i * 4);
      viewportTouched = true;
      ++i;
      continue;
    }
    if (a >= 0x1020 && a <= 0x1026) {
      m_projection[a - 0x1020] = be32(words + i * 4);
      projectionTouched = true;
      ++i;
      continue;
    }
    // Run of ordinary registers: forwarded as is.
    uint32_t j = i;
    while (j < count && (addr + j) >= 0x1000 && !((addr + j) >= 0x101A && (addr + j) <= 0x1026)) {
      ++j;
    }
    for (uint32_t k = i; k < j; ++k) {
      const uint32_t r = addr + k - 0x1000;
      if (r < m_xfRegs.size()) {
        m_xfRegs[r] = be32(words + k * 4);
        m_xfRegValid[r] = true;
      }
    }
    out_u8(OP_LOAD_XF_REG);
    out_u32(((j - i - 1) << 16) | (a & 0xFFFFu));
    out_data(words + i * 4, (j - i) * 4);
    i = j;
  }

  for (uint32_t k = 0; k < unitCount; ++k) {
    m_xfUnitWritten.insert(units[k]);
    emit_xf_unit(units[k]);
  }
  if (viewportTouched) {
    m_viewportValid = true;
    emit_viewport();
  }
  if (projectionTouched) {
    m_projectionValid = true;
    out_u8(OP_LOAD_XF_REG);
    out_u32((6u << 16) | 0x1020u);
    for (uint32_t w : m_projection) {
      out_u32(w);
    }
  }
}

void FifoBridge::emit_xf_unit(uint32_t addr) {
  uint32_t len = 12;
  if (addr >= 0x400 && addr < 0x500) {
    len = 9;
  } else if (addr >= 0x600) {
    len = 16;
  }
  out_u8(OP_LOAD_XF_REG);
  out_u32(((len - 1) << 16) | addr);
  for (uint32_t k = 0; k < len; ++k) {
    out_u32(m_xfMem[addr + k]);
  }
}

void FifoBridge::emit_viewport() {
  if (!m_viewportValid) {
    return;
  }
  // The viewport origin is relative to the scissor offset (BP 0x59, values /2),
  // as in Dolphin (VertexManagerBase.cpp). Aurora expects an origin at 340.
  const uint32_t so = m_bp[0x59];
  const float xoff = static_cast<float>(so & 0x1FFu) * 2.0f;
  const float yoff = static_cast<float>((so >> 10) & 0x1FFu) * 2.0f;
  const float ox = bits_to_float(m_viewport[3]) - xoff + AURORA_VIEWPORT_ORIGIN;
  const float oy = bits_to_float(m_viewport[4]) - yoff + AURORA_VIEWPORT_ORIGIN;
  out_u8(OP_LOAD_XF_REG);
  out_u32((5u << 16) | 0x101Au);
  out_u32(m_viewport[0]);
  out_u32(m_viewport[1]);
  out_u32(m_viewport[2]);
  out_u32(float_to_bits(ox));
  out_u32(float_to_bits(oy));
  out_u32(m_viewport[5]);
}

void FifoBridge::emit_scissor() {
  // Aurora subtracts 342 from the scissor coordinates without taking BP 0x59 into account:
  // it is given coordinates already brought back into the coordinate system of the default offset.
  const uint32_t so = m_bp[0x59];
  const int32_t xoff = static_cast<int32_t>(so & 0x1FFu) * 2;
  const int32_t yoff = static_cast<int32_t>((so >> 10) & 0x1FFu) * 2;
  const int32_t guard = static_cast<int32_t>(SDK_GUARD_OFFSET);
  for (uint32_t reg : {0x20u, 0x21u}) {
    const uint32_t v = m_bp[reg];
    const int32_t y = std::clamp(static_cast<int32_t>(v & 0x7FFu) - yoff + guard, 0, 0x7FF);
    const int32_t x = std::clamp(static_cast<int32_t>((v >> 12) & 0x7FFu) - xoff + guard, 0, 0x7FF);
    emit_bp(reg << 24 | static_cast<uint32_t>(x) << 12 | static_cast<uint32_t>(y));
  }
}

void FifoBridge::on_indexed_xf_load(uint32_t arrayIdx, uint32_t index, uint16_t addr, uint32_t len) {
  const ArrayState& arr = m_arrays[arrayIdx];
  const uint32_t src = arr.base + index * arr.stride;
  const uint8_t* data = host_ptr(src, len * 4);
  if (data == nullptr) {
    ++m_stats.badAddresses;
    GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: LOAD_INDX array %u outside RAM (0x%08X)", arrayIdx, src);
    return;
  }
  // The hardware reads RAM at command time: do the same, then go
  // through the ordinary XF path (mirror up to date, complete matrices for Aurora).
  on_xf(addr, len, data);
}

// ---------------------------------------------------------------------------------------------
// BP
// ---------------------------------------------------------------------------------------------
void FifoBridge::on_bp(uint32_t raw) {
  const uint32_t reg = raw >> 24;
  const uint32_t val = raw & 0x00FFFFFFu;
  if (reg == 0xFE) {
    // Mask for the next BP write only (Dolphin BPStructs, Aurora handle_bp).
    m_bpMask = val;
    m_bpMaskPending = true;
    emit_bp(raw);
    return;
  }
  const bool hadMask = m_bpMaskPending;
  const uint32_t merged = (m_bp[reg] & ~m_bpMask) | (val & m_bpMask);
  m_bpMask = 0x00FFFFFFu;
  m_bpMaskPending = false;
  m_bp[reg] = merged;
  m_bpValid[reg] = true;

  // When the write is not forwarded as is, the mask already forwarded to Aurora
  // must be cancelled, otherwise it would apply to our own write.
  auto cancel_mask = [&]() {
    if (hadMask) {
      emit_bp(0xFE000000u | 0x00FFFFFFu);
    }
  };

  switch (reg) {
  case 0x20:
  case 0x21:
    cancel_mask();
    emit_scissor();
    return;
  case 0x59:
    cancel_mask();
    emit_bp(reg << 24 | merged);
    emit_scissor();
    emit_viewport();
    return;
  case 0x45: // GXSetDrawDone
    emit_bp(raw);
    if (g_output) {
      afifo::drain(); // the runtime must only regain control once the RAM reads are done
    }
    GCRT_TRACE(GCRT_TRACE_GX, "PE draw done");
    if (m_hooks.drawDone != nullptr) {
      m_hooks.drawDone(m_hooks.user);
    }
    return;
  case 0x47: // token without interrupt
  case 0x48: // token with interrupt
    emit_bp(raw);
    if (g_output) {
      afifo::drain();
    }
    GCRT_TRACE(GCRT_TRACE_GX, "PE token 0x%04X%s", merged & 0xFFFFu, reg == 0x48 ? " (interrupt)" : "");
    if (m_hooks.token != nullptr) {
      m_hooks.token(m_hooks.user, static_cast<uint16_t>(merged & 0xFFFFu), reg == 0x48);
    }
    return;
  case 0x52: { // EFB copy trigger
    cancel_mask();
    const uint32_t tl = m_bp[0x49];
    const uint32_t wh = m_bp[0x4A];
    const uint32_t left = tl & 0x3FFu;
    const uint32_t top = (tl >> 10) & 0x3FFu;
    const uint32_t width = (wh & 0x3FFu) + 1u;
    const uint32_t height = ((wh >> 10) & 0x3FFu) + 1u;
    const uint32_t destPhys = ((m_bp[0x4B] & 0x00FFFFFFu) << 5) & PHYS_MASK;
    const bool clear = (merged >> 11) & 1u;
    if ((merged >> 14) & 1u) {
      DisplayCopyInfo info;
      info.xfbPhys = destPhys;
      info.srcLeft = static_cast<uint16_t>(left);
      info.srcTop = static_cast<uint16_t>(top);
      info.srcWidth = static_cast<uint16_t>(width);
      info.srcHeight = static_cast<uint16_t>(height);
      info.dstStrideBytes = (m_bp[0x4D] & 0x3FFu) * 32u;
      info.yScaleRaw = m_bp[0x4E];
      info.clear = clear;
      info.scaleEnabled = (merged >> 10) & 1u;
      info.gamma = static_cast<uint8_t>((merged >> 7) & 3u);
      info.clamp = static_cast<uint8_t>(merged & 3u);
      info.frameToField = static_cast<uint8_t>((merged >> 12) & 3u);
      info.copyFilter0 = m_bp[0x53];
      info.copyFilter1 = m_bp[0x54];
      info.pixelFormat = m_bp[0x43] & 7u;
      ++m_stats.displayCopies;
      if (m_streamLog != nullptr) {
        stream_frame_end();
      }
      GCRT_TRACE(GCRT_TRACE_GX, "EFB->XFB copy 0x%08X src=(%u,%u %ux%u) clear=%d", destPhys, left, top, width,
                 height, clear ? 1 : 0);
      if (m_hooks.displayCopy != nullptr) {
        m_hooks.displayCopy(m_hooks.user, info);
      }
      return;
    }
    // EFB -> texture copy: Aurora expects the source, the size, the format and the
    // destination through its sub-commands (GXFrameBuffer.cpp), then BP 0x52.
    const bool half = (merged >> 9) & 1u;
    const uint32_t fmtBits = (merged >> 3) & 0xFu;
    const uint32_t realFmt = fmtBits / 2u + (fmtBits & 1u) * 8u; // Dolphin UPE_Copy::tp_realFormat
    const bool depth = (m_bp[0x43] & 7u) == 3u;                   // PE in Z24: depth copy
    const bool intensity = ((merged >> 15) & 1u) && ((merged >> 16) & 1u);
    const uint32_t dstW = std::max(1u, half ? width / 2u : width);
    const uint32_t dstH = std::max(1u, half ? height / 2u : height);
    const uint8_t* dest = (m_mem1 != nullptr && destPhys < m_mem1Size) ? m_mem1 + destPhys : nullptr;
    if (dest == nullptr) {
      ++m_stats.badAddresses;
      GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: EFB copy to an address outside RAM 0x%08X", destPhys);
      return;
    }
    if (g_output && m_hooks.textureCopy != nullptr) {
      m_hooks.textureCopy(m_hooks.user); // before the copy: it ends Aurora's render pass (see BridgeHooks)
    }
    write_aurora_header(AUR_LOAD_COPY_SRC);
    out_u32(left);
    out_u32(top);
    out_u32(width);
    out_u32(height);
    write_aurora_header(AUR_LOAD_COPY_DST);
    out_u32(dstW);
    out_u32(dstH);
    out_u32(efb_copy_format(realFmt, depth, intensity));
    out_u8(half ? 1 : 0);
    write_aurora_header(AUR_LOAD_COPY_DEST);
    out_u64(reinterpret_cast<uint64_t>(dest));
    emit_bp(0x52000000u | (merged & ~(1u << 14)));
    ++m_stats.efbCopies;
    GCRT_TRACE(GCRT_TRACE_GX, "EFB->texture copy 0x%08X %ux%u fmt=0x%X", destPhys, dstW, dstH,
               efb_copy_format(realFmt, depth, intensity));
    return;
  }
  case 0x65: { // load of a TLUT into TMEM
    cancel_mask();
    const uint32_t src = (m_bp[0x64] << 5) & 0x01FFFFFFu; // GameCube: high bits ignored (Dolphin)
    const uint16_t tmem = static_cast<uint16_t>(merged & 0x3FFu);
    const uint32_t lines = (merged >> 10) & 0x7FFu;
    const uint32_t bytes = lines * 32u;
    const uint8_t* data = host_ptr(src, bytes);
    if (data == nullptr || bytes == 0) {
      ++m_stats.badAddresses;
      GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: TLUT outside RAM (0x%08X, %u bytes)", src, bytes);
      return;
    }
    const int slot = tlut_slot_for(tmem);
    auto& t = m_tluts[static_cast<size_t>(slot)];
    t.valid = true;
    t.tmemOffset = tmem;
    t.entries = static_cast<uint16_t>(std::min<uint32_t>(lines * 16u, 0xFFFFu));
    t.data.assign(data, data + bytes); // TMEM keeps a copy: RAM may change afterwards
    t.version = ++m_tlutClock;
    m_texDirty = 0xFF;
    GCRT_TRACE(GCRT_TRACE_GX, "TLUT 0x%08X -> TMEM 0x%03X (%u entries, slot %d)", src, tmem, t.entries, slot);
    return;
  }
  case 0x66: // texture cache invalidation
    emit_bp(raw);
    m_texDirty = 0xFF;
    return;
  default:
    break;
  }
  if ((reg >= 0x80 && reg <= 0x9B) || (reg >= 0xA0 && reg <= 0xBB)) {
    const uint32_t idx = reg - 0x80;
    m_texDirty |= static_cast<uint8_t>(1u << ((idx & 3u) + (idx >= 0x20 ? 4u : 0u)));
  }
  emit_bp(raw);
}

int FifoBridge::tlut_slot_for(uint16_t tmemOffset) {
  int freeSlot = -1;
  int oldest = 0;
  for (int i = 0; i < static_cast<int>(m_tluts.size()); ++i) {
    const auto& t = m_tluts[static_cast<size_t>(i)];
    if (t.valid && t.tmemOffset == tmemOffset) {
      return i;
    }
    if (!t.valid && freeSlot < 0) {
      freeSlot = i;
    }
    if (t.valid && t.version < m_tluts[static_cast<size_t>(oldest)].version) {
      oldest = i;
    }
  }
  return freeSlot >= 0 ? freeSlot : oldest;
}

void FifoBridge::flush_textures() {
  if (m_texDirty == 0) {
    return;
  }
  for (uint32_t map = 0; map < 8; ++map) {
    if (((m_texDirty >> map) & 1u) == 0) {
      continue;
    }
    const uint32_t base = map < 4 ? 0x80u + map : 0xA0u + (map - 4u);
    const uint32_t mode0 = m_bp[base];
    const uint32_t mode1 = m_bp[base + 0x04];
    const uint32_t image0 = m_bp[base + 0x08];
    const uint32_t image3 = m_bp[base + 0x14];
    const uint32_t tlutReg = m_bp[base + 0x18];
    if ((image3 & 0x00FFFFFFu) == 0 && image0 == 0) {
      continue; // never configured
    }
    const uint32_t w = (image0 & 0x3FFu) + 1u;
    const uint32_t h = ((image0 >> 10) & 0x3FFu) + 1u;
    const uint32_t fmt = (image0 >> 20) & 0xFu;
    const uint32_t phys = ((image3 & 0x00FFFFFFu) << 5) & PHYS_MASK;
    const uint8_t* data = host_ptr(phys, texture_bytes(fmt, w, h));
    if (data == nullptr) {
      ++m_stats.badAddresses;
      GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: texture %u outside RAM (0x%08X %ux%u fmt %u)", map, phys, w, h, fmt);
    }
    // [hypothesis] mipmaps present if the minification filter uses mips and max LOD > 0.
    const uint32_t minFilter = (mode0 >> 5) & 7u;
    const bool mips = (minFilter == 1 || minFilter == 2 || minFilter == 5 || minFilter == 6) &&
                      ((mode1 >> 8) & 0xFFu) != 0;
    uint32_t tlutSlot = 0;
    if (fmt == 0x8 || fmt == 0x9 || fmt == 0xA) { // C4, C8, C14X2
      const uint16_t tmem = static_cast<uint16_t>(tlutReg & 0x3FFu);
      const uint32_t tlutFmt = (tlutReg >> 10) & 3u;
      int slot = -1;
      for (int i = 0; i < static_cast<int>(m_tluts.size()); ++i) {
        if (m_tluts[static_cast<size_t>(i)].valid && m_tluts[static_cast<size_t>(i)].tmemOffset == tmem) {
          slot = i;
          break;
        }
      }
      if (slot < 0) {
        GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: color-indexed texture %u without a loaded TLUT (TMEM 0x%03X)",
                   map, tmem);
      } else {
        const auto& t = m_tluts[static_cast<size_t>(slot)];
        write_aurora_header(AUR_LOAD_TLUT);
        out_u8(static_cast<uint8_t>(slot));
        out_u64(reinterpret_cast<uint64_t>(t.data.data()));
        out_u32(tlutFmt);
        out_u16(t.entries);
        out_u32(static_cast<uint32_t>(slot) + 1u); // stable identifier per slot
        out_u32(t.version);
        ++m_stats.tlutLoads;
        tlutSlot = static_cast<uint32_t>(slot);
      }
    }
    write_aurora_header(AUR_LOAD_TEXOBJ);
    out_u8(static_cast<uint8_t>(map));
    out_u64(reinterpret_cast<uint64_t>(data));
    out_u32(w);
    out_u32(h);
    out_u32(fmt);
    out_u32(tlutSlot);
    out_u8(mips ? 1 : 0);
    out_u32(0); // texObjId 0: Aurora identifies the texture by its contents (hash)
    out_u32(0);
    ++m_stats.textureLoads;
    GCRT_TRACE(GCRT_TRACE_GX, "texture %u: 0x%08X %ux%u fmt %u", map, phys, w, h, fmt);
  }
  m_texDirty = 0;
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
void FifoBridge::compute_layout(uint32_t vat, std::array<VtxAttr, 21>& out) {
  const uint32_t a = m_vatA[vat];
  const uint32_t b = m_vatB[vat];
  const uint32_t c = m_vatC[vat];
  uint32_t off = 0;
  auto set = [&](uint32_t attr, uint32_t type, uint32_t directSize, uint32_t elemSize, uint32_t indices) {
    VtxAttr& v = out[attr];
    v.type = static_cast<uint8_t>(type);
    v.offset = static_cast<uint8_t>(off);
    v.indices = static_cast<uint8_t>(indices);
    v.elemSize = static_cast<uint8_t>(elemSize);
    switch (type) {
    case 1:
      v.size = static_cast<uint8_t>(directSize);
      break;
    case 2:
      v.size = static_cast<uint8_t>(indices);
      break;
    case 3:
      v.size = static_cast<uint8_t>(indices * 2);
      break;
    default:
      v.size = 0;
      break;
    }
    off += v.size;
  };
  // Matrix indices (1 byte each when present).
  for (uint32_t i = 0; i < 9; ++i) {
    set(i, (m_vcdLo >> i) & 1u, 1, 0, 1);
  }
  // Position
  {
    const uint32_t cnt = (a & 1u) ? 3u : 2u;
    const uint32_t cs = comp_size((a >> 1) & 7u);
    set(9, (m_vcdLo >> 9) & 3u, cnt * cs, cnt * cs, 1);
  }
  // Normal: N (3) or NTB (9); NBT3 = three indices if NTB and bit 31 (Dolphin VertexLoader_Normal.h).
  {
    const bool ntb = (a >> 9) & 1u;
    const bool index3 = (a >> 31) & 1u;
    const uint32_t cs = comp_size((a >> 10) & 7u);
    const uint32_t comps = ntb ? 9u : 3u;
    const uint32_t indices = (ntb && index3) ? 3u : 1u;
    const uint32_t elem = indices == 3u ? 3u * cs : comps * cs;
    set(10, (m_vcdLo >> 11) & 3u, comps * cs, elem, indices);
  }
  // Colors
  {
    const uint32_t s0 = color_size((a >> 14) & 7u);
    set(11, (m_vcdLo >> 13) & 3u, s0, s0, 1);
    const uint32_t s1 = color_size((a >> 18) & 7u);
    set(12, (m_vcdLo >> 15) & 3u, s1, s1, 1);
  }
  // Texture coordinates: (number of components, format) per entry.
  const uint32_t tcCnt[8] = {(a >> 21) & 1u, b & 1u, (b >> 9) & 1u, (b >> 18) & 1u,
                             (b >> 27) & 1u, (c >> 5) & 1u, (c >> 14) & 1u, (c >> 23) & 1u};
  const uint32_t tcFmt[8] = {(a >> 22) & 7u, (b >> 1) & 7u, (b >> 10) & 7u, (b >> 19) & 7u,
                             (b >> 28) & 7u, (c >> 6) & 7u, (c >> 15) & 7u, (c >> 24) & 7u};
  for (uint32_t t = 0; t < 8; ++t) {
    const uint32_t s = (tcCnt[t] ? 2u : 1u) * comp_size(tcFmt[t]);
    set(13 + t, (m_vcdHi >> (t * 2)) & 3u, s, s, 1);
  }
}

uint32_t FifoBridge::vertex_size(uint32_t vat) {
  if (m_vtxSizeVat == vat) {
    return m_vtxSize;
  }
  std::array<VtxAttr, 21> layout{};
  compute_layout(vat, layout);
  uint32_t size = 0;
  for (const auto& v : layout) {
    size += v.size;
  }
  m_vtxSizeVat = vat;
  m_vtxSize = size;
  return size;
}

size_t FifoBridge::on_draw(const uint8_t* p, size_t avail) {
  if (avail < 3) {
    return 0;
  }
  const uint8_t op = p[0];
  const uint32_t vat = op & 7u;
  uint32_t prim = (op >> 3) & 7u; // 0 quads, 1 quads (2), 2 triangles, 3 strip, 4 fan, 5 lines, 6 line strip, 7 points
  const uint16_t count = be16(p + 1);
  const uint32_t vsize = vertex_size(vat);
  const size_t total = 3 + static_cast<size_t>(count) * vsize;
  if (avail < total) {
    return 0;
  }
  ++m_stats.commands;

  // Cleanup of degenerate draws that Aurora's processor does not support
  // (e.g. a strip of fewer than 3 vertices: overflow in prepare_idx_buffer).
  if (prim == 1) {
    prim = 0; // GX_DRAW_QUADS_2 (0x88): treated as quads (Dolphin OpcodeDecoding.h)
  }
  uint32_t n = count;
  switch (prim) {
  case 0:
    n -= n % 4;
    break;
  case 2:
    n -= n % 3;
    break;
  case 3:
  case 4:
    if (n < 3) {
      n = 0;
    }
    break;
  case 5:
    n -= n % 2;
    break;
  case 6:
    if (n < 2) {
      n = 0;
    }
    break;
  default:
    break;
  }
  if (n == 0 || vsize == 0) {
    ++m_stats.skippedDraws;
    GCRT_TRACE(GCRT_TRACE_GX, "draw skipped (op 0x%02X, %u vertices, size %u)", op, count, vsize);
    return total;
  }

  // Indexed arrays: host address and size actually used by this draw.
  std::array<VtxAttr, 21> layout{};
  compute_layout(vat, layout);
  const uint8_t* verts = p + 3;
  for (uint32_t attr = 9; attr < 21; ++attr) {
    const VtxAttr& va = layout[attr];
    if (va.type != 2 && va.type != 3) {
      continue;
    }
    const uint32_t sentinel = va.type == 2 ? 0xFFu : 0xFFFFu;
    uint32_t maxIdx = 0;
    bool any = false;
    for (uint32_t v = 0; v < n; ++v) {
      const uint8_t* q = verts + static_cast<size_t>(v) * vsize + va.offset;
      for (uint32_t k = 0; k < va.indices; ++k) {
        const uint32_t idx = va.type == 2 ? q[k] : be16(q + k * 2);
        // [hypothesis] the "all ones" index does not reference the array (vertex ignored).
        if (idx == sentinel) {
          continue;
        }
        maxIdx = std::max(maxIdx, idx);
        any = true;
      }
    }
    if (!any) {
      continue;
    }
    ArrayState& arr = m_arrays[attr - 9];
    const uint32_t need = maxIdx * arr.stride + va.elemSize;
    if (arr.base == arr.declaredBase && need <= arr.declaredSize) {
      continue;
    }
    uint32_t size = (need + 63u) & ~63u;
    const uint32_t phys = arr.base & PHYS_MASK;
    if (m_mem1 != nullptr && phys < m_mem1Size) {
      size = std::min(size, m_mem1Size - phys);
    }
    const uint8_t* host = host_ptr(phys, size);
    if (host == nullptr || size < need) {
      ++m_stats.badAddresses;
      ++m_stats.skippedDraws;
      GCRT_TRACE(GCRT_TRACE_UNHANDLED, "GX: array %u outside RAM (base 0x%08X, %u bytes): draw skipped", attr - 9,
                 phys, need);
      return total;
    }
    write_aurora_header(static_cast<uint16_t>(AUR_LOAD_ARRAYBASE + (attr - 9)));
    out_u64(reinterpret_cast<uint64_t>(host));
    out_u32(size);
    out_u8(0); // big-endian data
    arr.declaredBase = arr.base;
    arr.declaredSize = size;
    ++m_stats.arrayBaseUpdates;
  }

  flush_textures();

  const uint8_t header[3] = {static_cast<uint8_t>(0x80u | (prim << 3) | vat), static_cast<uint8_t>(n >> 8),
                             static_cast<uint8_t>(n & 0xFFu)};
  out_data(header, 3);
  out_data(verts, n * vsize);
  if (g_output && ++g_unpublished >= publish_batch()) {
    g_unpublished = 0;
    afifo::finish_draw();
  }
  ++m_stats.draws;
  ++m_streamDraws;
  m_stats.vertices += n;
  return total;
}

} // namespace gcrt::gx
