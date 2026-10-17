// Bridge between the guest's GX command stream and Aurora's command processor.
//
// Generic GameCube: no reference to any specific game.
//
// Role: the native GX functions (hle/native_gx*.c) produce the command stream that the SDK functions sent to the
// console's write-gather pipe (same commands, same order; the vertices that the game writes itself, inline, during a
// draw are appended as is) and hand it over in 32-byte packets (GcrtGxSink, see consume()). These (big-endian)
// bytes are read here command by command, then forwarded to Aurora's internal FIFO
// (aurora::gx::fifo), which already knows how to decode BP/CP/XF and the draw commands.
//
// What Aurora cannot do on its own, and what this bridge translates (see notes/runtime.md):
//   - the guest's physical addresses (CP 0xA0-0xAF vertex arrays, textures
//     BP 0x94-0x97/0xB4-0xB7, TLUT BP 0x64, EFB copies BP 0x4B) become
//     host pointers, passed through Aurora's GX_AURORA_* sub-commands;
//   - display lists (CALL_DL) are read from guest memory and
//     executed inline (Aurora ignores nested CALL_DLs);
//   - indexed XF loads (LOAD_INDX_A-D) are resolved here;
//   - the EFB -> XFB copy (end of frame) and the PE tokens/draw done are
//     reported to the runtime through callbacks (PE interrupts, presentation);
//   - a few convention differences between the SDK and Aurora are corrected (viewport
//     origin at 342 and scissor offset BP 0x59, see Aurora's regs.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace gcrt::gx {

// EFB -> XFB copy requested by the guest (BP 0x52, bit 14): this is the end of a frame.
struct DisplayCopyInfo {
  uint32_t xfbPhys = 0;      // destination in guest RAM (BP 0x4B << 5)
  uint16_t srcLeft = 0;      // source rectangle in the EFB (BP 0x49 / 0x4A)
  uint16_t srcTop = 0;
  uint16_t srcWidth = 0;
  uint16_t srcHeight = 0;
  uint32_t dstStrideBytes = 0; // BP 0x4D * 32
  uint32_t yScaleRaw = 0;      // raw BP 0x4E (vertical scale: 256 / value, 9 bits)
  bool clear = false;          // bit 11: clear the EFB after the copy
  bool scaleEnabled = false;   // bit 10: vertical scale applied
  uint8_t gamma = 0;           // bits 7-8: 0 = 1.0; 1 = 1.7; 2 = 2.2
  uint8_t clamp = 0;           // bits 0-1: top / bottom edges
  uint8_t frameToField = 0;    // bits 12-13
  uint32_t copyFilter0 = 0;    // BP 0x53 (coefficients 0-3 of the vertical filter, 6 bits each)
  uint32_t copyFilter1 = 0;    // BP 0x54 (coefficients 4-6)
  uint32_t pixelFormat = 0;    // BP 0x43 bits 0-2 (EFB format)
};

// Callbacks to the runtime (all optional). Called on the guest thread, in
// command stream order.
struct BridgeHooks {
  void* user = nullptr;
  // BP 0x45 (GXSetDrawDone): the PE raises the "finish" interrupt.
  void (*drawDone)(void* user) = nullptr;
  // BP 0x47 / 0x48 (GXSetDrawSync): 16-bit token, with or without interrupt.
  void (*token)(void* user, uint16_t token, bool interrupt) = nullptr;
  // EFB -> XFB copy. The runtime ends the Aurora frame there (aurora_end_frame) and presents it.
  void (*displayCopy)(void* user, const DisplayCopyInfo& info) = nullptr;
  // EFB -> texture copy (BP 0x52 without bit 14), called just BEFORE the copy is forwarded to Aurora (only while
  // forwarding is on). Aurora ends its current render pass at such a copy and hands it to its render thread at once:
  // a draw of that pass whose pipeline is still compiling at that moment is skipped. The runtime can make sure that
  // every pipeline requested so far is ready here (see GCRT_PIPELINES in platform/gcrt_platform.h).
  void (*textureCopy)(void* user) = nullptr;
};

struct BridgeStats {
  uint64_t commands = 0;
  uint64_t draws = 0;
  uint64_t vertices = 0;
  uint64_t skippedDraws = 0;     // empty or degenerate draws (not forwarded)
  uint64_t displayLists = 0;
  uint64_t unknownOpcodes = 0;
  uint64_t arrayBaseUpdates = 0; // GX_AURORA_LOAD_ARRAYBASE sub-commands emitted
  uint64_t textureLoads = 0;     // GX_AURORA_LOAD_TEXOBJ sub-commands emitted
  uint64_t tlutLoads = 0;
  uint64_t efbCopies = 0;        // EFB -> texture copies
  uint64_t displayCopies = 0;    // EFB -> XFB copies
  uint64_t badAddresses = 0;     // physical addresses outside guest RAM
  uint64_t copyTextureLoads = 0; // textures loaded from the destination of an earlier EFB -> texture copy
  uint64_t staleCopies = 0;      // ... whose guest memory was rewritten since the copy (copy evicted in Aurora)
  // Frozen guest data (see FifoBridge, "frozen copies"): new copies taken, bytes copied, re-checks that found the
  // previous copy still identical (no new copy), and epochs (frame ends, cache invalidations) that forced re-checks.
  uint64_t frozenTexCopies = 0;
  uint64_t frozenTexBytes = 0;
  uint64_t frozenTexReused = 0;
  uint64_t frozenArrayCopies = 0;
  uint64_t frozenArrayBytes = 0;
  uint64_t frozenArrayReused = 0;
  uint64_t frozenTluts = 0;
  uint64_t frozenTexEpochs = 0;
  uint64_t frozenArrayEpochs = 0;
  uint64_t frozenLiveBytes = 0;  // bytes held by frozen copies right now
  uint64_t frozenPeakBytes = 0;
  // GCRT_GX_FREEZE_CHECK=1 (diagnostic): draws for which a frozen copy given to Aurora no longer matched guest memory
  // (the game rewrote it after its first use in the epoch, without an invalidation).
  uint64_t frozenOutdated = 0;
};

class FifoBridge {
public:
  FifoBridge();

  // Resets the mirror registers to zero (console startup).
  void reset();
  // Guest main RAM (MEM1), bytes in guest order (big-endian).
  void set_guest_memory(const uint8_t* mem1, uint32_t size);
  void set_hooks(const BridgeHooks& hooks) { m_hooks = hooks; }
  // Diagnostics: copies into `out` everything forwarded to Aurora (nullptr to stop).
  // A single recorder for the whole process.
  static void set_recording(std::vector<uint8_t>* out);
  // Diagnostics (GCRT_GX_STREAM): fingerprint of the received command stream, frame by frame. One line per display
  // copy: "frame bytes fnv64 draws", over the top-level commands received since the previous copy (independent of
  // how the packets are split). Used to compare the streams of two executables byte for byte (at step 10c: the
  // emulated write-gather pipe against the native functions). `dumpFrames`: frames whose whole stream is also
  // written (file <path>.<frame>.bin), separated by commas; nullptr or empty: none.
  bool open_stream_log(const char* path, const char* dumpFrames);
  void close_stream_log();
  // Forwarding to Aurora (cut while no frame is open). When it is turned back on, all known
  // state is re-sent (resync).
  void set_output_enabled(bool enabled);
  bool output_enabled() const;
  void resync();
  // Waits until Aurora has processed everything forwarded to it (end of frame).
  void flush();

  // Consumes bytes of the command stream (produced by native drawing, hle/native_gx*.c, in 32-byte packets).
  // An incomplete command stays pending until the next call.
  void consume(const uint8_t* data, size_t size);
  // Discards the pending incomplete command (GXAbortFrame: stream aborted).
  void abort_pending();
  size_t pending_bytes() const { return m_pending.size() - m_pendingOff; }

  const BridgeStats& stats() const { return m_stats; }
  // Frozen copies on (default) or off (GCRT_GX_FREEZE=0: Aurora reads guest memory itself, former behavior).
  bool freeze_enabled() const { return m_freeze; }
  // Draws published to Aurora's thread per wake-up (GCRT_GX_PUBLISH_BATCH; default 128, 1 with GCRT_GX_FREEZE=0).
  static uint32_t publish_batch_size();

private:
  // Frozen copies. Aurora's "FIFO processor" thread reads the data that the stream passes by pointer (vertex arrays,
  // textures, TLUTs) when it processes the command, which is some time after the game emitted it: if the game rewrites
  // that memory in between, the frame depends on thread timing (notes/blocages.md, rendering race). The bridge
  // therefore hands Aurora an immutable copy taken when the command is emitted. One copy per guest range and per
  // epoch: a new epoch starts at each frame end (Aurora re-reads its arrays and re-resolves its textures every frame)
  // and at the invalidations that tell the hardware that memory changed (INVL_VC for arrays, BP 0x66 for textures).
  // A copy is freed once Aurora can no longer read it: not referenced by the snapshot cache, not the last pointer
  // given to an Aurora slot, and no longer in flight (every command sent before the last drain is processed).
  struct FrozenBlock {
    std::unique_ptr<uint8_t[]> bytes;
    uint32_t size = 0;
    uint64_t sentGen = 0; // drain generation in which it was last sent (in-flight list membership)
    uint32_t phys = 0;    // guest range copied (diagnostics)
    explicit FrozenBlock(uint32_t n);
    ~FrozenBlock(); // keeps the count of live bytes (frozenLiveBytes)
  };
  using FrozenRef = std::shared_ptr<FrozenBlock>;
  struct Snapshot {
    FrozenRef block;
    uint64_t epoch = 0;
    uint64_t lastUseFrame = 0;
  };
  FrozenRef snapshot(std::unordered_map<uint64_t, Snapshot>& cache, uint64_t key, uint32_t phys, uint32_t size,
                     uint64_t epoch, bool texture);
  FrozenRef frozen_copy(uint32_t phys, uint32_t size) const; // nullptr if phys is outside guest RAM
  void mark_sent(const FrozenRef& block);
  void rebind(FrozenRef& slot, FrozenRef next); // replaces the copy of an Aurora slot, keeping the old one in flight
  void after_drain();
  void new_frame_epoch();
  void check_frozen(uint32_t arrayMask); // GCRT_GX_FREEZE_CHECK: arrays read by the draw (bit = array)

private:
  struct VtxAttr {
    uint8_t type = 0;   // 0 absent, 1 direct, 2 index8, 3 index16
    uint8_t offset = 0; // position in the vertex (bytes)
    uint8_t size = 0;   // size in the vertex (bytes)
    uint8_t indices = 1; // number of indices (3 for NBT3 normals)
    uint8_t elemSize = 0; // size of one array element (for indexed attributes)
  };
  struct ArrayState {
    uint32_t base = 0;     // physical address (CP 0xA0 + n)
    uint8_t stride = 0;    // CP 0xB0 + n
    uint32_t declaredBase = 0xFFFFFFFFu; // last value forwarded to Aurora
    uint32_t declaredSize = 0;
    uint64_t declaredEpoch = 0;          // array epoch of the frozen copy forwarded (frozen copies on)
  };
  struct TlutSlot {
    bool valid = false;
    uint16_t tmemOffset = 0;
    uint16_t entries = 0;
    uint32_t version = 0;
    // Copy taken at load time, like the hardware TMEM. A new block at each load: the previous one may still be read
    // by Aurora's thread (frozen copies, see above).
    std::shared_ptr<FrozenBlock> data;
  };

  size_t run_command(const uint8_t* p, size_t avail, bool inDisplayList);
  void run_display_list(uint32_t phys, uint32_t size);
  void on_cp(uint8_t reg, uint32_t value);
  void on_bp(uint32_t raw);
  void on_xf(uint16_t addr, uint32_t count, const uint8_t* bigEndianWords);
  void on_indexed_xf_load(uint32_t arrayIdx, uint32_t index, uint16_t addr, uint32_t len);
  size_t on_draw(const uint8_t* p, size_t avail);

  uint32_t vertex_size(uint32_t vat);
  void compute_layout(uint32_t vat, std::array<VtxAttr, 21>& out);
  void flush_textures();
  void emit_viewport();
  void emit_scissor();
  void emit_xf_unit(uint32_t addr);
  void emit_bp(uint32_t raw);
  void forward_raw(const uint8_t* p, size_t n);
  int tlut_slot_for(uint16_t tmemOffset);
  const uint8_t* host_ptr(uint32_t phys, uint32_t size) const;

  // Guest memory
  const uint8_t* m_mem1 = nullptr;
  uint32_t m_mem1Size = 0;

  // Mirror registers
  uint32_t m_vcdLo = 0;
  uint32_t m_vcdHi = 0;
  std::array<uint32_t, 8> m_vatA{};
  std::array<uint32_t, 8> m_vatB{};
  std::array<uint32_t, 8> m_vatC{};
  std::array<ArrayState, 16> m_arrays{};
  std::array<uint32_t, 256> m_bp{};
  std::array<bool, 256> m_bpValid{};
  std::array<uint32_t, 256> m_cpRegs{};
  std::array<bool, 256> m_cpValid{};
  std::array<uint32_t, 0x58> m_xfRegs{};  // XF 0x1000-0x1057
  std::array<bool, 0x58> m_xfRegValid{};
  std::set<uint32_t> m_xfUnitWritten;     // matrices and lights already loaded
  uint32_t m_bpMask = 0x00FFFFFFu;
  bool m_bpMaskPending = false;
  std::array<uint32_t, 0x680> m_xfMem{}; // matrices and lights (XF addresses 0x0000-0x067F)
  std::array<uint32_t, 6> m_viewport{};  // XF 0x101A-0x101F (float bits)
  std::array<uint32_t, 7> m_projection{}; // XF 0x1020-0x1026
  bool m_viewportValid = false;
  bool m_projectionValid = false;
  uint8_t m_texDirty = 0xFF;             // one bit per texmap 0-7
  std::array<TlutSlot, 20> m_tluts{};    // 20 = Aurora's MaxTluts (lib/gx/gx.hpp)
  uint32_t m_tlutClock = 0;

  // Vertex size cache
  uint32_t m_vtxSizeVat = 0xFFFFFFFFu;
  uint32_t m_vtxSize = 0;

  // Pending bytes (incomplete command)
  std::vector<uint8_t> m_pending;
  size_t m_pendingOff = 0;
  bool m_inDisplayList = false;

  BridgeHooks m_hooks{};
  BridgeStats m_stats{};
  // EFB -> texture copies by guest destination (physical address): fingerprint of the first bytes of guest memory at
  // the time of the copy. Aurora keeps the copy on the GPU and binds it to any later texture whose data pointer is
  // that destination; on the console, the texture unit reads guest memory. If the game has rewritten that memory
  // since the copy (other data loaded at the same address), the copy is stale: it is evicted (GXDestroyCopyTex).
  struct CopyRecord {
    uint64_t prefixHash;
    uint64_t frame;
  };
  std::unordered_map<uint32_t, CopyRecord> m_copies;

  // Frozen copies (see FrozenBlock).
  bool m_freeze = true;                                  // GCRT_GX_FREEZE=0: off (diagnostic)
  bool m_freezeCheck = false;                            // GCRT_GX_FREEZE_CHECK=1 (diagnostic)
  std::unordered_map<uint64_t, Snapshot> m_texSnaps;     // key: physical address << 32 | size in bytes
  std::unordered_map<uint64_t, Snapshot> m_arraySnaps;   // key: physical address
  std::array<FrozenRef, 8> m_texBound{};                 // last copy given to each Aurora texture map
  std::array<FrozenRef, 16> m_arrayBound{};              // last copy given to each Aurora vertex array
  std::vector<FrozenRef> m_inFlight;                     // sent since the last drain
  uint64_t m_sendGen = 1;
  uint64_t m_texEpoch = 1;
  uint64_t m_arrayEpoch = 1;
  uint64_t m_frameNo = 0;
  bool m_evictStaleCopies = true; // GCRT_GX_STALE_COPIES=keep: former behavior (diagnostic)
  uint64_t copy_prefix_hash(uint32_t phys) const;
  void check_copy_destination(uint32_t phys, const uint8_t* data, uint32_t map, uint32_t w, uint32_t h, uint32_t fmt);

  // Stream fingerprint (GCRT_GX_STREAM).
  void stream_hash(const uint8_t* p, size_t n);
  void stream_frame_end();
  std::FILE* m_streamLog = nullptr;
  std::FILE* m_streamDump = nullptr;
  std::string m_streamPath;
  std::set<uint64_t> m_streamDumpFrames;
  uint64_t m_streamHash = 0xCBF29CE484222325ull;
  uint64_t m_streamBytes = 0;
  uint64_t m_streamDraws = 0;
  uint64_t m_streamFrame = 1;
};

} // namespace gcrt::gx
