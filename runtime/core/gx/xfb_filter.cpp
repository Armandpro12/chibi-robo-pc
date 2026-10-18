// Display copy filter (see xfb_filter.hpp).
//
// Filter computation adapted from Dolphin (GPL-2.0-or-later, commit e6f3ae17627e4344da95b13424af5baf4c892b08):
//   Source/Core/VideoCommon/TextureConverterShaderGen.cpp (Copyright 2017 Dolphin Emulator Project):
//     weighted sum of lines y-1, y, y+1, division by 64, clamp to 255, then gamma;
//   Source/Core/VideoCommon/TextureCacheBase.cpp (Copyright 2010 Dolphin Emulator Project):
//     GetRAMCopyFilterCoefficients / GetVRAMCopyFilterCoefficients (grouping of the 7 coefficients).
// SPDX-License-Identifier: GPL-2.0-or-later (adapted parts)
//
// Implementation with Aurora's API: resolve_pass() freezes the EFB into a texture, then an encoder
// task (push_encoder_task) draws, between two passes of the frame, a full-screen triangle that
// reads this copy and writes the filtered version into the EFB (webgpu::g_frameBuffer, internal API), which
// the next pass reloads and aurora_end_frame presents.
//
// Threads: apply_xfb_filter runs on the game thread while Aurora's render worker encodes and submits on the
// same Dawn device, and Aurora does not request Dawn's ImplicitDeviceSynchronization feature. So the game thread
// makes no Dawn call here: the parameters go into Aurora's per-frame uniform stream (push_uniform, copied by the
// worker) and the pipeline is created on the worker, in the encoder task. Creating and unmapping a buffer on the
// game thread for every filtered frame raced with the worker and crashed inside Dawn (issue 22).
#include "xfb_filter.hpp"

#include <aurora/gfx.hpp>
#include <webgpu/webgpu_cpp.h>

#include "dolphin/vi/vi_internal.hpp" // Aurora's internal API (external/aurora/lib): displayed region of the EFB
#include "webgpu/gpu.hpp"               // Aurora's internal API (external/aurora/lib): EFB texture

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

extern "C" {
#include "../platform/trace.h"
}

namespace gcrt::gx {
namespace {

constexpr const char* kShader = R"(
struct Params {
  coef: vec4<f32>,   // x: line above, y: current line, z: line below (out of 64)
  misc: vec4<f32>,   // x: 1/gamma, y: step in target lines for one EFB line, z: last target line read
};
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var<uniform> p: Params;

var<private> pos: array<vec2<f32>, 3> = array<vec2<f32>, 3>(
  vec2(-1.0, 1.0), vec2(-1.0, -3.0), vec2(3.0, 1.0));

@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4<f32> {
  return vec4<f32>(pos[i], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) fragPos: vec4<f32>) -> @location(0) vec4<f32> {
  let dims = vec2<i32>(textureDimensions(src));
  let x = i32(fragPos.x);
  let y = i32(fragPos.y);
  let step = max(1, i32(round(p.misc.y)));
  let a = round(textureLoad(src, vec2<i32>(x, clamp(y - step, 0, dims.y - 1)), 0) * 255.0);
  let b = round(textureLoad(src, vec2<i32>(x, y), 0) * 255.0);
  let c = round(textureLoad(src, vec2<i32>(x, clamp(y + step, 0, min(i32(p.misc.z), dims.y - 1))), 0) * 255.0);
  var rgb = floor((a.rgb * p.coef.x + b.rgb * p.coef.y + c.rgb * p.coef.z) / 64.0);
  rgb = min(rgb, vec3<f32>(255.0));
  if (p.misc.x != 1.0) {
    rgb = round(pow(rgb / 255.0, vec3<f32>(p.misc.x)) * 255.0);
  }
  return vec4<f32>(rgb / 255.0, b.a / 255.0);
}
)";

struct Params {
  float coef[4];
  float misc[4];
};

struct Pending {
  uint64_t id;
  wgpu::TextureView source; // snapshot of the EFB (resolve_pass)
  wgpu::TextureView target; // EFB of the frame being recorded, taken on the game thread like Aurora's own passes
  aurora::gfx::Range uniform; // Params in the frame's uniform stream
  uint32_t width = 0; // displayed region of the EFB (target pixels): the only part written
  uint32_t height = 0;
};

std::mutex g_mutex;
std::deque<Pending> g_pending;
uint64_t g_nextId = 1;
aurora::gfx::EncoderTaskId g_task = aurora::gfx::InvalidEncoderTask; // game thread only
std::atomic<bool> g_failed{false}; // set by the render worker if the pipeline cannot be created
// Render worker only (created lazily in encode).
bool g_pipelineTried = false;
wgpu::RenderPipeline g_pipeline;
wgpu::BindGroupLayout g_layout;

bool create_pipeline(const wgpu::Device& device);

void encode(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload,
            size_t payloadSize, void*) {
  if (payloadSize != sizeof(uint64_t)) {
    return;
  }
  uint64_t id;
  std::memcpy(&id, payload, sizeof(id));
  Pending job{};
  {
    std::lock_guard lock{g_mutex};
    for (auto it = g_pending.begin(); it != g_pending.end(); ++it) {
      if (it->id == id) {
        job = std::move(*it);
        g_pending.erase(it);
        break;
      }
    }
  }
  if (!job.source || !job.target) {
    return;
  }
  if (!g_pipelineTried) {
    g_pipelineTried = true;
    if (!create_pipeline(ctx.device)) {
      g_failed.store(true, std::memory_order_release);
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "display copy filter: creation failed, disabled");
    } else {
      GCRT_TRACE(GCRT_TRACE_VI, "display copy filter: pipeline created on the render worker");
    }
  }
  if (!g_pipeline) {
    return;
  }
  const std::array entries{
      wgpu::BindGroupEntry{.binding = 0, .textureView = job.source},
      wgpu::BindGroupEntry{
          .binding = 1, .buffer = ctx.uniformBuffer, .offset = job.uniform.offset, .size = job.uniform.size},
  };
  const wgpu::BindGroupDescriptor bgDesc{
      .label = "gcrt XFB filter",
      .layout = g_layout,
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  const auto bindGroup = ctx.device.CreateBindGroup(&bgDesc);
  const std::array attachments{wgpu::RenderPassColorAttachment{
      .view = job.target,
      .loadOp = wgpu::LoadOp::Load,
      .storeOp = wgpu::StoreOp::Store,
  }};
  const wgpu::RenderPassDescriptor passDesc{
      .label = "gcrt XFB filter",
      .colorAttachmentCount = attachments.size(),
      .colorAttachments = attachments.data(),
  };
  const auto pass = cmd.BeginRenderPass(&passDesc);
  pass.SetPipeline(g_pipeline);
  pass.SetBindGroup(0, bindGroup, 0, nullptr);
  pass.SetScissorRect(0, 0, job.width, job.height);
  pass.Draw(3);
  pass.End();
}

// Render worker thread (first encode).
bool create_pipeline(const wgpu::Device& device) {
  wgpu::ShaderSourceWGSL source{};
  source.code = kShader;
  const wgpu::ShaderModuleDescriptor moduleDesc{.nextInChain = &source, .label = "gcrt XFB filter"};
  const auto module = device.CreateShaderModule(&moduleDesc);
  const std::array layoutEntries{
      wgpu::BindGroupLayoutEntry{
          .binding = 0,
          .visibility = wgpu::ShaderStage::Fragment,
          .texture = wgpu::TextureBindingLayout{.sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                                                .viewDimension = wgpu::TextureViewDimension::e2D},
      },
      wgpu::BindGroupLayoutEntry{
          .binding = 1,
          .visibility = wgpu::ShaderStage::Fragment,
          .buffer = wgpu::BufferBindingLayout{.type = wgpu::BufferBindingType::Uniform,
                                              .minBindingSize = sizeof(Params)},
      },
  };
  const wgpu::BindGroupLayoutDescriptor bglDesc{.entryCount = layoutEntries.size(), .entries = layoutEntries.data()};
  g_layout = device.CreateBindGroupLayout(&bglDesc);
  const wgpu::PipelineLayoutDescriptor plDesc{.bindGroupLayoutCount = 1, .bindGroupLayouts = &g_layout};
  const auto pipelineLayout = device.CreatePipelineLayout(&plDesc);
  const std::array targets{wgpu::ColorTargetState{.format = aurora::gfx::color_format(),
                                                  .writeMask = wgpu::ColorWriteMask::All}};
  const wgpu::FragmentState fragment{
      .module = module, .entryPoint = "fs_main", .targetCount = targets.size(), .targets = targets.data()};
  const wgpu::RenderPipelineDescriptor desc{
      .label = "gcrt XFB filter",
      .layout = pipelineLayout,
      .vertex = wgpu::VertexState{.module = module, .entryPoint = "vs_main"},
      .primitive = wgpu::PrimitiveState{.topology = wgpu::PrimitiveTopology::TriangleList},
      .multisample = wgpu::MultisampleState{.count = 1, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  g_pipeline = device.CreateRenderPipeline(&desc);
  return g_pipeline && g_layout;
}

// Game thread: registers the encoder task type (Aurora bookkeeping, no Dawn call).
bool initialize() {
  if (g_failed.load(std::memory_order_acquire)) {
    return false;
  }
  if (g_task == aurora::gfx::InvalidEncoderTask) {
    g_task = aurora::gfx::register_encoder_task_type({.label = "gcrt XFB filter", .callback = encode});
    if (g_task == aurora::gfx::InvalidEncoderTask) {
      g_failed.store(true, std::memory_order_release);
      gcrt_trace_printf(GCRT_TRACE_PLATFORM, "display copy filter: encoder task registration failed, disabled");
      return false;
    }
  }
  return true;
}
} // namespace

XfbFilterParams xfb_filter_params(uint32_t bp53, uint32_t bp54, uint8_t gamma, uint32_t efbHeight,
                                  bool clampBottom) {
  XfbFilterParams p;
  for (int i = 0; i < 4; ++i) {
    p.coefficients[i] = static_cast<uint8_t>((bp53 >> (6 * i)) & 0x3Fu);
  }
  for (int i = 0; i < 3; ++i) {
    p.coefficients[4 + i] = static_cast<uint8_t>((bp54 >> (6 * i)) & 0x3Fu);
  }
  p.gamma = gamma;
  p.efbHeight = efbHeight;
  p.clampBottom = clampBottom;
  return p;
}

XfbFilterParams with_mode(XfbFilterParams p, CopyFilterMode mode) {
  if (mode == CopyFilterMode::Unblended) {
    // 8-bit sum: coefficients are at most 63, 7 * 63 < 256.
    const unsigned sum = p.coefficients[0] + p.coefficients[1] + p.coefficients[2] + p.coefficients[3] +
                         p.coefficients[4] + p.coefficients[5] + p.coefficients[6];
    for (auto& c : p.coefficients) {
      c = 0;
    }
    p.coefficients[3] = static_cast<uint8_t>(sum);
  }
  return p;
}

bool apply_xfb_filter(const XfbFilterParams& in) {
  const float top = static_cast<float>(in.coefficients[0] + in.coefficients[1]);
  const float mid = static_cast<float>(in.coefficients[2] + in.coefficients[3] + in.coefficients[4]);
  const float bottom = static_cast<float>(in.coefficients[5] + in.coefficients[6]);
  static const float kGamma[4] = {1.0f, 1.7f, 2.2f, 1.0f}; // value 3 undocumented: 1.0 [hypothesis]
  const float gammaRcp = 1.0f / kGamma[in.gamma & 3u];
  if (top == 0.0f && bottom == 0.0f && mid == 64.0f && gammaRcp == 1.0f) {
    return false; // neutral filter
  }
  if (aurora::gfx::sample_count() > 1 || !initialize()) {
    return false;
  }
  aurora::gfx::ResolvedTargets targets;
  if (!aurora::gfx::resolve_pass({.color = true}, targets) || !targets.color) {
    return false;
  }
  // Displayed region (the whole EFB unless it is taller than the display copy, VISetEfbSize): the source of the
  // display copy, at the top left.
  const auto shown = aurora::vi::display_size_in({targets.width, targets.height});
  Params params{};
  params.coef[0] = top;
  params.coef[1] = mid;
  params.coef[2] = bottom;
  params.misc[0] = gammaRcp;
  params.misc[1] = in.efbHeight != 0 ? static_cast<float>(shown.y) / static_cast<float>(in.efbHeight) : 1.0f;
  params.misc[2] = static_cast<float>((in.clampBottom ? shown.y : targets.height) - 1u);
  // CPU-side append to the frame's uniform stream: the worker uploads it before running the task.
  const auto uniform = aurora::gfx::push_uniform(reinterpret_cast<const uint8_t*>(&params), sizeof(params));
  if (uniform.size != sizeof(params)) {
    return false;
  }
  static uint32_t lastW = 0, lastH = 0;
  if (shown.x != lastW || shown.y != lastH) {
    lastW = shown.x;
    lastH = shown.y;
    GCRT_TRACE(GCRT_TRACE_VI, "display copy filter: region %ux%u of a %ux%u EFB, step %.3f, last line %.0f", shown.x,
               shown.y, targets.width, targets.height, params.misc[1], params.misc[2]);
  }
  uint64_t id;
  {
    std::lock_guard lock{g_mutex};
    id = g_nextId++;
    g_pending.push_back({id, targets.color, aurora::webgpu::g_frameBuffer.view, uniform, shown.x, shown.y});
  }
  if (!aurora::gfx::push_encoder_task(g_task, &id, sizeof(id))) {
    std::lock_guard lock{g_mutex};
    g_pending.pop_back();
    return false;
  }
  return true;
}

} // namespace gcrt::gx
