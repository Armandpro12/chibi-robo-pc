// Capture of Aurora's EFB to a PNG (see efb_capture.hpp).
#include "efb_capture.hpp"

#include <aurora/gfx.hpp>
#include <webgpu/webgpu_cpp.h>

#include "dolphin/vi/vi_internal.hpp" // Aurora's internal API (external/aurora/lib): displayed region of the EFB

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "../platform/png_writer.h"
#include "../platform/trace.h"
}

namespace gcrt::gx {
namespace {

struct Capture {
  uint64_t id = 0;
  std::string path;
  wgpu::Texture texture;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  wgpu::Buffer buffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytesPerRow = 0;
  std::atomic<int> state{0}; // 0 pending, 1 read, -1 failed
  std::vector<uint8_t> rgba;
};

std::mutex g_mutex;
std::vector<std::shared_ptr<Capture>> g_captures;
uint64_t g_nextId = 1;
aurora::gfx::EncoderTaskId g_task = aurora::gfx::InvalidEncoderTask;
std::vector<uint8_t> g_last;
uint32_t g_lastW = 0;
uint32_t g_lastH = 0;

std::shared_ptr<Capture> find_capture(const void* payload, size_t size) {
  if (size != sizeof(uint64_t)) {
    return nullptr;
  }
  uint64_t id;
  std::memcpy(&id, payload, sizeof(id));
  std::lock_guard lock{g_mutex};
  for (auto& c : g_captures) {
    if (c->id == id) {
      return c;
    }
  }
  return nullptr;
}

// Aurora's render thread: copies the frozen texture into the readable buffer.
void encode_copy(const aurora::gfx::EncoderTaskContext&, const wgpu::CommandEncoder& cmd, const void* payload,
                 size_t payloadSize, void*) {
  const auto cap = find_capture(payload, payloadSize);
  if (!cap) {
    return;
  }
  const wgpu::TexelCopyTextureInfo src{.texture = cap->texture};
  const wgpu::TexelCopyBufferInfo dst{
      .layout = {.offset = 0, .bytesPerRow = cap->bytesPerRow, .rowsPerImage = cap->height},
      .buffer = cap->buffer,
  };
  const wgpu::Extent3D extent{.width = cap->width, .height = cap->height, .depthOrArrayLayers = 1};
  cmd.CopyTextureToBuffer(&src, &dst, &extent);
}

// After submission: asynchronous read of the buffer (same pattern as Aurora's depth_peek.cpp).
void after_submit(const aurora::gfx::EncoderTaskCompletionContext&, const void* payload, size_t payloadSize, void*) {
  const auto cap = find_capture(payload, payloadSize);
  if (!cap) {
    return;
  }
  const uint64_t size = static_cast<uint64_t>(cap->bytesPerRow) * cap->height;
  cap->buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::AllowSpontaneous,
                       [cap](wgpu::MapAsyncStatus status, wgpu::StringView) {
                         if (status != wgpu::MapAsyncStatus::Success) {
                           cap->state.store(-1, std::memory_order_release);
                           return;
                         }
                         const auto* src = static_cast<const uint8_t*>(cap->buffer.GetConstMappedRange());
                         const bool bgra = cap->format == wgpu::TextureFormat::BGRA8Unorm ||
                                           cap->format == wgpu::TextureFormat::BGRA8UnormSrgb;
                         cap->rgba.resize(static_cast<size_t>(cap->width) * cap->height * 4);
                         for (uint32_t y = 0; y < cap->height; ++y) {
                           const uint8_t* row = src + static_cast<size_t>(y) * cap->bytesPerRow;
                           uint8_t* out = cap->rgba.data() + static_cast<size_t>(y) * cap->width * 4;
                           for (uint32_t x = 0; x < cap->width; ++x) {
                             const uint8_t* px = row + x * 4;
                             out[x * 4 + 0] = bgra ? px[2] : px[0];
                             out[x * 4 + 1] = px[1];
                             out[x * 4 + 2] = bgra ? px[0] : px[2];
                             out[x * 4 + 3] = 255; // the EFB alpha has no meaning on screen
                           }
                         }
                         cap->buffer.Unmap();
                         cap->state.store(1, std::memory_order_release);
                       });
}

bool supported_format(wgpu::TextureFormat f) {
  return f == wgpu::TextureFormat::BGRA8Unorm || f == wgpu::TextureFormat::BGRA8UnormSrgb ||
         f == wgpu::TextureFormat::RGBA8Unorm || f == wgpu::TextureFormat::RGBA8UnormSrgb;
}
} // namespace

bool request_efb_capture(const char* pngPath) {
  if (g_task == aurora::gfx::InvalidEncoderTask) {
    g_task = aurora::gfx::register_encoder_task_type({
        .label = "gcrt EFB capture",
        .callback = encode_copy,
        .userdata = nullptr,
        .afterSubmit = after_submit,
    });
  }
  aurora::gfx::ResolvedTargets targets;
  if (!aurora::gfx::resolve_pass({.color = true}, targets) || !targets.colorTexture) {
    GCRT_TRACE(GCRT_TRACE_VI, "EFB capture impossible: no active pass");
    return false;
  }
  if (!supported_format(targets.colorFormat)) {
    GCRT_TRACE(GCRT_TRACE_VI, "EFB capture: unsupported color format %u", static_cast<unsigned>(targets.colorFormat));
    return false;
  }
  auto cap = std::make_shared<Capture>();
  cap->path = pngPath;
  cap->texture = targets.colorTexture;
  cap->format = targets.colorFormat;
  // Only the displayed region (top left), when the EFB is taller than the display copy (VISetEfbSize).
  const auto shown = aurora::vi::display_size_in({targets.width, targets.height});
  cap->width = shown.x;
  cap->height = shown.y;
  cap->bytesPerRow = (cap->width * 4 + 255u) & ~255u; // alignment required by WebGPU
  const wgpu::BufferDescriptor desc{
      .label = "gcrt EFB capture readback",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = static_cast<uint64_t>(cap->bytesPerRow) * cap->height,
  };
  cap->buffer = aurora::gfx::device().CreateBuffer(&desc);
  {
    std::lock_guard lock{g_mutex};
    cap->id = g_nextId++;
    g_captures.push_back(cap);
  }
  const uint64_t id = cap->id;
  if (!aurora::gfx::push_encoder_task(g_task, &id, sizeof(id))) {
    std::lock_guard lock{g_mutex};
    std::erase(g_captures, cap);
    return false;
  }
  GCRT_TRACE(GCRT_TRACE_VI, "EFB capture requested: %ux%u -> %s", cap->width, cap->height, pngPath);
  return true;
}

int poll_efb_captures() {
  std::vector<std::shared_ptr<Capture>> done;
  {
    std::lock_guard lock{g_mutex};
    for (auto it = g_captures.begin(); it != g_captures.end();) {
      if ((*it)->state.load(std::memory_order_acquire) != 0) {
        done.push_back(*it);
        it = g_captures.erase(it);
      } else {
        ++it;
      }
    }
  }
  int written = 0;
  for (auto& cap : done) {
    if (cap->state.load() < 0) {
      GCRT_TRACE(GCRT_TRACE_VI, "EFB capture: GPU readback failed (%s)", cap->path.c_str());
      continue;
    }
    if (gcrt_png_write_rgba(cap->path.c_str(), cap->rgba.data(), cap->width, cap->height, cap->width * 4)) {
      ++written;
    } else {
      GCRT_TRACE(GCRT_TRACE_VI, "EFB capture: cannot write %s", cap->path.c_str());
    }
    g_last = std::move(cap->rgba);
    g_lastW = cap->width;
    g_lastH = cap->height;
  }
  return written;
}

int efb_captures_pending() {
  std::lock_guard lock{g_mutex};
  return static_cast<int>(g_captures.size());
}

CapturedImage last_efb_capture() { return {g_lastW, g_lastH, g_last.empty() ? nullptr : g_last.data()}; }

} // namespace gcrt::gx
