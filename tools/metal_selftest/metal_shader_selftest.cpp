// tools/metal_selftest/metal_shader_selftest.cpp
// Fable 2: offline gate for the native Metal backend (fable2_gpu_backend=metal).
// No window and no game: it
//  1. converts every built-in host shader, every guest shader in the runtime
//     shader cache (shader_runtime/*.f2sc) and in the built-in pack to MSL with
//     the renderer's converter (msl_convert.cpp), every variant of each entry;
//  2. compiles each with MTLCreateSystemDefaultDevice() and newLibraryWithSource
//     (plume::MetalCompileLibrary, the options the game uses) and looks up its
//     entry point;
//  3. checks the bindings each shader declares against plume's Metal layout
//     (sets at buffers 0-7, push constants at 8 + range binding);
//  4. builds the copy/blit/depth-copy pipelines through plume with the real
//     main pipeline layout (main_layout.h) and runs the blit on the GPU into an
//     offscreen target from bindless slot 20000 (past the 16384 plume used to
//     size the heap for), reading the pixels back with a texture-to-buffer copy.
//  5. runs MetalFX's spatial scaler through plume (patch 0022, 1920x1080 ->
//     3024x1898) on a frame image written by a render pass that is still open,
//     samples the result through the bindless table and checks four flat
//     quadrants and the mean colour against a bilinear upscale's.
//
// Usage: fable2_metal_selftest [shader_runtime dir] [--dump <dir>] [--errors N]

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <time.h>

#include <smolv.h>
#include <zstd.h>

#include <plume_metal.h>
#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#include "gpu/gpu_busy.h"
#include "gpu/main_layout.h"
#include "gpu/shaders/msl_convert.h"
#include "gpu/shaders/shader_cache.h"

#include "shaders/blit_ps.hlsl.spirv.h"
#include "shaders/copy_depth_ps.hlsl.spirv.h"
#include "shaders/copy_vs.hlsl.spirv.h"
#include "shaders/derive_depth_stencil_2x_ps.hlsl.spirv.h"
#include "shaders/derive_depth_stencil_4x_ps.hlsl.spirv.h"
#include "shaders/derive_depth_stencil_8x_ps.hlsl.spirv.h"
#include "shaders/imgui_ps.hlsl.spirv.h"
#include "shaders/imgui_vs.hlsl.spirv.h"
#include "shaders/resolve_msaa_color_2x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_color_4x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_color_8x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_depth_2x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_depth_4x_ps.hlsl.spirv.h"
#include "shaders/resolve_msaa_depth_8x_ps.hlsl.spirv.h"
#include "shaders/taa_ps.hlsl.spirv.h"
#include "shaders/velocity_resolve_2x_ps.hlsl.spirv.h"
#include "shaders/velocity_resolve_4x_ps.hlsl.spirv.h"
#include "shaders/velocity_resolve_8x_ps.hlsl.spirv.h"

namespace plume {
extern std::unique_ptr<RenderInterface> CreateMetalInterface();
}

namespace fs = std::filesystem;
using eot::gpu::MslOptions;
using eot::gpu::MslShader;
using eot::gpu::SpirvToMsl;

namespace {

struct Counts {
  uint32_t total = 0, converted = 0, compiled = 0, bindings_ok = 0;
};

struct Failure {
  std::string name;
  std::string stage;
  std::string message;
};

std::vector<Failure> g_failures;
fs::path g_dump_dir;

std::string FirstLines(const std::string &text, int lines) {
  size_t at = 0;
  for (int i = 0; i < lines && at != std::string::npos; ++i)
    at = text.find('\n', at + 1);
  return at == std::string::npos ? text : text.substr(0, at);
}

// Every [[buffer(N)]] the shader declares: argument buffers 0-7 (descriptor
// sets) or the push constant buffer; anything else is a binding plume does not
// make.
bool CheckBindings(const MslShader &msl, uint32_t push_buffer, std::string *why) {
  size_t at = 0;
  while ((at = msl.source.find("[[buffer(", at)) != std::string::npos) {
    const long index = std::strtol(msl.source.c_str() + at + 9, nullptr, 10);
    // The declaration: back to the parameter list's previous ',' or '('.
    const size_t decl_start = msl.source.find_last_of(",(", at) + 1;
    const std::string line = msl.source.substr(decl_start, at - decl_start);
    at += 9;
    if (line.find("spvDescriptorSetBuffer") != std::string::npos) {
      // "constant spvDescriptorSetBufferN& spvDescriptorSetN [[buffer(N)]]"
      const size_t n = line.find("spvDescriptorSetBuffer") + std::strlen("spvDescriptorSetBuffer");
      const long set = std::strtol(line.c_str() + n, nullptr, 10);
      if (set != index || index > 7) {
        *why = "descriptor set " + std::to_string(set) + " at buffer " + std::to_string(index);
        return false;
      }
      continue;
    }
    if (index == static_cast<long>(push_buffer))
      continue;
    *why = "unexpected buffer binding: " + line;
    return false;
  }
  return true;
}

// Converts, compiles and checks one SPIR-V module.
void TestModule(MTL::Device *device, Counts &counts, const std::string &name, const void *spirv, size_t size,
                uint32_t push_binding = 0) {
  counts.total++;
  MslOptions options;
  options.pushConstantBinding = push_binding;
  const MslShader msl = SpirvToMsl(spirv, size, options);
  if (!msl.ok()) {
    g_failures.push_back({name, "convert", msl.error});
    return;
  }
  counts.converted++;
  if (!g_dump_dir.empty()) {
    std::ofstream f(g_dump_dir / (name + ".metal"));
    f << msl.source;
  }
  std::string why;
  if (CheckBindings(msl, eot::gpu::kMslPushConstantBufferBase + push_binding, &why))
    counts.bindings_ok++;
  else
    g_failures.push_back({name, "bindings", why});

  std::string error;
  MTL::Library *library = plume::MetalCompileLibrary(device, msl.source.data(), msl.source.size(), &error);
  if (!library) {
    g_failures.push_back({name, "compile", FirstLines(error, 6)});
    return;
  }
  MTL::FunctionConstantValues *values = MTL::FunctionConstantValues::alloc()->init();
  const uint32_t zero = 0;
  values->setConstantValue(&zero, MTL::DataTypeUInt, NS::UInteger(0));
  NS::Error *ns_error = nullptr;
  MTL::Function *function =
      library->newFunction(NS::String::string(msl.entry.c_str(), NS::UTF8StringEncoding), values, &ns_error);
  values->release();
  if (!function) {
    g_failures.push_back({name, "entry point", msl.entry + ": " +
                                                    (ns_error ? ns_error->localizedDescription()->utf8String() : "")});
  } else {
    counts.compiled++;
    function->release();
  }
  library->release();
}

std::vector<uint8_t> ReadFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

uint32_t Read32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

struct Variant {
  const char *name;
  uint32_t offset, size;
};

// Each entry's SPIR-V variants (trimmed, full, position-only, velocity), as
// guest_shaders.cpp EntryBytes picks them.
std::vector<Variant> Variants(const ShaderCacheEntry &e) {
  std::vector<Variant> v;
  v.push_back({"trimmed", e.spirvOffset, e.spirvSize});
  if (e.fullSpirvSize)
    v.push_back({"full", e.fullSpirvOffset, e.fullSpirvSize});
  if (e.posSpirvSize)
    v.push_back({"pos", e.posSpirvOffset, e.posSpirvSize});
  if (e.velSpirvSize)
    v.push_back({"vel", e.velSpirvOffset, e.velSpirvSize});
  return v;
}

// SMOL-V decode, as GetHostShaderByHash does before createShader.
void TestGuestEntry(MTL::Device *device, Counts &counts, const ShaderCacheEntry &e, const uint8_t *spirv_base,
                    size_t spirv_bytes, const std::string &origin) {
  for (const Variant &v : Variants(e)) {
    char name[96];
    std::snprintf(name, sizeof(name), "%016llx.%s", static_cast<unsigned long long>(e.hash), v.name);
    if (size_t(v.offset) + v.size > spirv_bytes) {
      counts.total++;
      g_failures.push_back({name, "read", origin + ": variant outside the blob"});
      continue;
    }
    std::vector<uint8_t> decoded(smolv::GetDecodedBufferSize(spirv_base + v.offset, v.size));
    if (decoded.empty() || !smolv::Decode(spirv_base + v.offset, v.size, decoded.data(), decoded.size())) {
      counts.total++;
      g_failures.push_back({name, "smol-v", origin + ": decode failed"});
      continue;
    }
    TestModule(device, counts, name, decoded.data(), decoded.size());
  }
}

// shader_runtime/*.f2sc (runtime_shaders.cpp ParseBlob's format).
void TestRuntimeCache(MTL::Device *device, Counts &counts, const fs::path &dir, uint32_t *files) {
  constexpr uint32_t kMagic = 0x43533246u, kVersion = 1;
  constexpr size_t kEntryBytes = 8 + 24 * 4;
  std::error_code ec;
  std::vector<fs::path> paths;
  for (const auto &item : fs::directory_iterator(dir, ec))
    if (item.path().extension() == ".f2sc")
      paths.push_back(item.path());
  std::sort(paths.begin(), paths.end());
  *files = static_cast<uint32_t>(paths.size());
  for (const fs::path &path : paths) {
    const std::vector<uint8_t> b = ReadFile(path);
    const std::string file = path.filename().string();
    if (b.size() < 20 || Read32(b.data()) != kMagic || Read32(b.data() + 4) != kVersion) {
      counts.total++;
      g_failures.push_back({file, "read", "not an F2SC v1 file"});
      continue;
    }
    const uint32_t count = Read32(b.data() + 8), layout_words = Read32(b.data() + 12), spirv_bytes = Read32(b.data() + 16);
    const size_t need = 20 + size_t(count) * kEntryBytes + size_t(layout_words) * 4 + spirv_bytes;
    if (b.size() != need) {
      counts.total++;
      g_failures.push_back({file, "read", "size mismatch"});
      continue;
    }
    const uint8_t *spirv = b.data() + 20 + size_t(count) * kEntryBytes + size_t(layout_words) * 4;
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t *p = b.data() + 20 + size_t(i) * kEntryBytes;
      ShaderCacheEntry e{};
      std::memcpy(&e.hash, p, 8);
      uint32_t f[24];
      std::memcpy(f, p + 8, sizeof(f));
      e.spirvOffset = f[2];
      e.spirvSize = f[3];
      e.fullSpirvOffset = f[12];
      e.fullSpirvSize = f[13];
      e.posSpirvOffset = f[16];
      e.posSpirvSize = f[17];
      e.velSpirvOffset = f[20];
      e.velSpirvSize = f[21];
      TestGuestEntry(device, counts, e, spirv, spirv_bytes, file);
    }
  }
}

void TestBuiltinPack(MTL::Device *device, Counts &counts) {
  if (g_shaderCacheEntryCount == 0 || g_spirvCacheDecompressedSize == 0)
    return;
  std::vector<uint8_t> blob(g_spirvCacheDecompressedSize);
  const size_t got = ZSTD_decompress(blob.data(), blob.size(), g_compressedSpirvCache, g_spirvCacheCompressedSize);
  if (ZSTD_isError(got) || got != blob.size()) {
    counts.total++;
    g_failures.push_back({"built-in pack", "read", "zstd decompression failed"});
    return;
  }
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i)
    TestGuestEntry(device, counts, g_shaderCacheEntries[i], blob.data(), blob.size(), "built-in pack");
}

// ---- Pipelines and a GPU run through plume ----------------------------------

std::unique_ptr<plume::RenderShader> HostShader(plume::RenderDevice *device, const void *spirv, size_t size) {
  const MslShader msl = SpirvToMsl(spirv, size);
  if (!msl.ok())
    return nullptr;
  return device->createShader(msl.source.data(), msl.source.size(), msl.entry.c_str(), plume::RenderShaderFormat::METAL);
}

// The host copy pipeline as device.cpp builds it (GetBlitPipeline /
// GetDepthCopyPipeline, CreateHostGraphicsPipeline's dynamic state).
std::unique_ptr<plume::RenderPipeline> CopyPipeline(plume::RenderDevice *device, plume::RenderPipelineLayout *layout,
                                                    plume::RenderShader *vs, plume::RenderShader *ps,
                                                    plume::RenderFormat color, plume::RenderFormat depth,
                                                    plume::RenderSampleCounts samples = plume::RenderSampleCount::COUNT_1) {
  plume::RenderGraphicsPipelineDesc desc;
  desc.multisampling.sampleCount = samples;
  desc.pipelineLayout = layout;
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  desc.depthFunction = plume::RenderComparisonFunction::ALWAYS;
  desc.depthEnabled = depth != plume::RenderFormat::UNKNOWN;
  desc.depthWriteEnabled = depth != plume::RenderFormat::UNKNOWN;
  desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  desc.cullMode = plume::RenderCullMode::NONE;
  desc.renderTargetCount = color != plume::RenderFormat::UNKNOWN ? 1 : 0;
  desc.renderTargetFormat[0] = color;
  desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  desc.depthTargetFormat = depth;
  desc.dynamicBlendConstantsEnabled = true;
  desc.dynamicStencilReferenceEnabled = true;
  auto pipeline = device->createGraphicsPipeline(desc);
  if (pipeline && !static_cast<plume::MetalGraphicsPipeline *>(pipeline.get())->state.renderPipelineState)
    pipeline.reset();
  return pipeline;
}

// Mirrors CopyPushConstants (device.h).
struct CopyPushConstants {
  uint32_t resourceDescriptorIndex = 0;
  uint32_t resourceDescriptorIndex2 = 0;
  float param0 = 1.0f;
  float param1 = 0.0f;
  float rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  float colorAdjust[4] = {0.0f, 1.0f, 1.0f, 0.0f};
  float extra[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(CopyPushConstants) == 64);
constexpr uint32_t kCopyPushConstantByteOffset = 24;
static_assert(eot::gpu::kMainLayoutPushConstantBytes == kCopyPushConstantByteOffset + sizeof(CopyPushConstants));

bool RunPipelineTests(std::string *report) {
  auto iface = plume::CreateMetalInterface();
  if (!iface) {
    *report = "plume CreateMetalInterface failed";
    return false;
  }
  auto device = iface->createDevice();
  if (!device) {
    *report = "plume createDevice failed";
    return false;
  }
  bool ok = true;
  auto fail = [&](const std::string &what) {
    ok = false;
    *report += "  FAIL " + what + "\n";
  };
  auto pass = [&](const std::string &what) { *report += "  ok   " + what + "\n"; };

  plume::RenderDescriptorSetBuilder tex_builder, sampler_builder;
  eot::gpu::DescribeTextureHeapSet(tex_builder);
  eot::gpu::DescribeSamplerHeapSet(sampler_builder);
  auto tex_set = tex_builder.create(device.get());
  auto sampler_set = sampler_builder.create(device.get());
  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);
  eot::gpu::DescribeMainLayoutSets(layout_builder, tex_builder, sampler_builder);
  eot::gpu::DescribeMainLayoutPushConstants(layout_builder, false);
  layout_builder.end();
  auto layout = layout_builder.create(device.get());
  if (!tex_set || !sampler_set || !layout) {
    fail("main layout");
    return false;
  }
  const auto *metal_tex_set = static_cast<const plume::MetalDescriptorSet *>(tex_set.get());
  const uint64_t heap_bytes = metal_tex_set->argumentBuffer.mtl->length();
  if (heap_bytes < uint64_t(eot::gpu::kBindlessTextureCount) * 8)
    fail("texture heap argument buffer " + std::to_string(heap_bytes) + " bytes < " +
         std::to_string(eot::gpu::kBindlessTextureCount) + " entries");
  else
    pass("texture heap argument buffer " + std::to_string(heap_bytes) + " bytes for " +
         std::to_string(eot::gpu::kBindlessTextureCount) + " entries");

  auto copy_vs = HostShader(device.get(), g_copy_vs_spirv, sizeof(g_copy_vs_spirv));
  auto blit_ps = HostShader(device.get(), g_blit_ps_spirv, sizeof(g_blit_ps_spirv));
  auto copy_depth_ps = HostShader(device.get(), g_copy_depth_ps_spirv, sizeof(g_copy_depth_ps_spirv));
  if (!copy_vs || !blit_ps || !copy_depth_ps) {
    fail("plume createShader (MSL source) for copy_vs / blit_ps / copy_depth_ps");
    return false;
  }
  pass("plume createShader from MSL source: copy_vs, blit_ps, copy_depth_ps");

  const plume::RenderFormat color_formats[] = {plume::RenderFormat::B8G8R8A8_UNORM, plume::RenderFormat::R8G8B8A8_UNORM,
                                               plume::RenderFormat::R16G16B16A16_FLOAT};
  std::unique_ptr<plume::RenderPipeline> blit_rgba8;
  for (const plume::RenderFormat f : color_formats) {
    auto p = CopyPipeline(device.get(), layout.get(), copy_vs.get(), blit_ps.get(), f, plume::RenderFormat::UNKNOWN);
    const std::string what = "blit pipeline (copy_vs + blit_ps), format " + std::to_string(static_cast<int>(f));
    p ? pass(what) : fail(what);
    if (f == plume::RenderFormat::R8G8B8A8_UNORM)
      blit_rgba8 = std::move(p);
  }
  {
    auto p = CopyPipeline(device.get(), layout.get(), copy_vs.get(), copy_depth_ps.get(), plume::RenderFormat::UNKNOWN,
                          plume::RenderFormat::D32_FLOAT);
    p ? pass("depth copy pipeline (copy_vs + copy_depth_ps), D32_FLOAT") : fail("depth copy pipeline, D32_FLOAT");
  }
  if (!blit_rgba8)
    return false;

  // The blit on the GPU: a 4x4 source with a distinct colour per texel in
  // bindless slot 20000, sampled one-to-one (point sampler, slot 1) into a 4x4
  // target, read back with a texture-to-buffer copy.
  constexpr uint32_t kSize = 4, kSlot = 20000;
  uint32_t texels[kSize * kSize];
  for (uint32_t i = 0; i < kSize * kSize; ++i)
    texels[i] = 0xFF000000u | (i * 16u) << 16 | (255u - i * 8u) << 8 | (i * 3u + 7u);

  plume::RenderSamplerDesc linear;
  linear.minFilter = linear.magFilter = plume::RenderFilter::LINEAR;
  linear.addressU = linear.addressV = linear.addressW = plume::RenderTextureAddressMode::CLAMP;
  plume::RenderSamplerDesc point = linear;
  point.minFilter = point.magFilter = plume::RenderFilter::NEAREST;
  point.mipmapMode = plume::RenderMipmapMode::NEAREST;
  auto linear_sampler = device->createSampler(linear);
  auto point_sampler = device->createSampler(point);
  sampler_set->setSampler(0, linear_sampler.get());
  sampler_set->setSampler(1, point_sampler.get());

  plume::RenderTextureDesc td;
  td.dimension = plume::RenderTextureDimension::TEXTURE_2D;
  td.width = td.height = kSize;
  td.depth = td.mipLevels = td.arraySize = 1;
  td.format = plume::RenderFormat::R8G8B8A8_UNORM;
  auto source = device->createTexture(td);
  td.flags = plume::RenderTextureFlag::RENDER_TARGET;
  auto target = device->createTexture(td);
  auto upload = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(sizeof(texels)));
  auto readback = device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(sizeof(texels)));
  if (!source || !target || !upload || !readback) {
    fail("test resources");
    return false;
  }
  std::memcpy(upload->map(), texels, sizeof(texels));
  upload->unmap();
  plume::RenderTextureViewDesc vd;
  vd.format = td.format;
  vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
  vd.mipLevels = 1;
  auto source_view = source->createTextureView(vd);
  tex_set->setTexture(kSlot, source.get(), plume::RenderTextureLayout::SHADER_READ, source_view.get());
  const plume::RenderTexture *colors[1] = {target.get()};
  auto framebuffer = device->createFramebuffer(plume::RenderFramebufferDesc(colors, 1));

  auto queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto *metal_queue = static_cast<plume::MetalCommandQueue *>(queue.get());
  metal_queue->setGpuIntervalCapture(true);
  const double host_before = static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-9;
  auto list = queue->createCommandList();
  auto fence = device->createCommandFence();
  list->begin();
  list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(source.get()),
                          plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), td.format, kSize, kSize, 1, kSize));
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(source.get(), plume::RenderTextureLayout::SHADER_READ));
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE));
  list->setGraphicsPipelineLayout(layout.get());
  list->setPipeline(blit_rgba8.get());
  list->setGraphicsDescriptorSet(tex_set.get(), 0);
  list->setGraphicsDescriptorSet(tex_set.get(), 1);
  list->setGraphicsDescriptorSet(tex_set.get(), 2);
  list->setGraphicsDescriptorSet(sampler_set.get(), 3);
  list->setGraphicsDescriptorSet(tex_set.get(), 4);
  list->setFramebuffer(framebuffer.get());
  const plume::RenderViewport vp(0.0f, 0.0f, float(kSize), float(kSize), 0.0f, 1.0f);
  const plume::RenderRect sc(0, 0, kSize, kSize);
  list->setViewports(&vp, 1);
  list->setScissors(&sc, 1);
  CopyPushConstants pc;
  pc.resourceDescriptorIndex = kSlot;
  pc.param1 = 1.0f;  // alpha 1
  list->setGraphicsPushConstants(0, &pc, kCopyPushConstantByteOffset, sizeof(pc));
  list->drawInstanced(3, 1, 0, 0);
  list->setFramebuffer(nullptr);
  list->barriers(plume::RenderBarrierStage::COPY,
                 plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COPY_SOURCE));
  list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), td.format, kSize, kSize, 1, kSize),
                          plume::RenderTextureCopyLocation::Subresource(target.get()));
  list->end();
  const plume::RenderCommandList *lists[] = {list.get()};
  queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
  queue->waitForCommandFence(fence.get());

  const auto *out = static_cast<const uint32_t *>(readback->map());
  uint32_t mismatches = 0;
  std::string first;
  for (uint32_t i = 0; i < kSize * kSize; ++i)
    if (out[i] != texels[i] && mismatches++ == 0) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "texel (%u,%u): got %08x, want %08x", i % kSize, i / kSize, out[i], texels[i]);
      first = buf;
    }
  readback->unmap();
  if (mismatches)
    fail("GPU blit from bindless slot 20000 through push constants: " + std::to_string(mismatches) +
         "/16 texels differ, first " + first);
  else
    pass("GPU blit from bindless slot 20000 (push constants at byte 24, point sampler slot 1), 16/16 texels "
         "match, read back with a texture-to-buffer copy");

  // GPU intervals (plume patch 0018): the blit's command buffer reports its GPU start and end on the
  // host clock, under the list's label; the wait buffer plume commits for the semaphores is not recorded.
  {
    metal_queue->waitIdle();
    const double host_after = static_cast<double>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-9;
    std::vector<plume::MetalGpuInterval> intervals;
    const uint64_t dropped = metal_queue->drainGpuIntervals(intervals);
    const plume::MetalGpuInterval *work = nullptr;
    for (const plume::MetalGpuInterval &i : intervals)
      if (eot::gpu::IsGpuWorkLabel(i.label))
        work = &i;
    char buf[192];
    if (!work || dropped != 0 || intervals.size() != 1) {
      std::snprintf(buf, sizeof(buf), "GPU intervals: %zu recorded (want the list's only), work buffer %s, %llu dropped",
                    intervals.size(), work ? "found" : "missing", static_cast<unsigned long long>(dropped));
      fail(buf);
    } else if (work->start < host_before - 0.5 || work->end > host_after + 0.5 || work->end < work->start) {
      std::snprintf(buf, sizeof(buf),
                    "GPU interval [%.6f, %.6f] is not inside the host clock window [%.6f, %.6f]", work->start,
                    work->end, host_before, host_after);
      fail(buf);
    } else {
      std::snprintf(buf, sizeof(buf), "GPU intervals: %zu buffers, the list's [%.3f ms] lies on the host clock "
                    "(inside the %.1f ms the test took); the wait buffer is not recorded", intervals.size(),
                    (work->end - work->start) * 1e3, (host_after - host_before) * 1e3);
      pass(buf);
    }
  }

  // Occlusion queries (plume patch 0016): the same blit into the 4x4 target,
  // its samples counted under scissors of known size.
  auto queries = device->createOcclusionQueryPool(8);
  if (!queries) {
    fail("createOcclusionQueryPool returned no pool");
    return false;
  }
  auto draw_rect = [&](uint32_t w, uint32_t h) {
    const plume::RenderRect r(0, 0, int32_t(w), int32_t(h));
    list->setScissors(&r, 1);
    list->drawInstanced(3, 1, 0, 0);
  };
  auto bind_blit = [&]() {
    list->setGraphicsPipelineLayout(layout.get());
    list->setPipeline(blit_rgba8.get());
    list->setGraphicsDescriptorSet(tex_set.get(), 0);
    list->setGraphicsDescriptorSet(tex_set.get(), 1);
    list->setGraphicsDescriptorSet(tex_set.get(), 2);
    list->setGraphicsDescriptorSet(sampler_set.get(), 3);
    list->setGraphicsDescriptorSet(tex_set.get(), 4);
    list->setViewports(&vp, 1);
    list->setGraphicsPushConstants(0, &pc, kCopyPushConstantByteOffset, sizeof(pc));
  };
  list->begin();
  list->resetQueryPool(queries.get(), 0, 8);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE));
  // 0: a query begun before the framebuffer is bound (the encoder starts later).
  list->beginQuery(queries.get(), 0);
  list->setFramebuffer(framebuffer.get());
  bind_blit();
  draw_rect(4, 4);
  list->endQuery(queries.get(), 0);
  // 1: inside the running encoder.
  list->beginQuery(queries.get(), 1);
  draw_rect(2, 2);
  list->endQuery(queries.get(), 1);
  // 2: empty.
  list->beginQuery(queries.get(), 2);
  list->endQuery(queries.get(), 2);
  // 3: across a framebuffer rebind with a full clear (a new encoder), plus a
  // quad clear inside the query that must not count: 4 + 2.
  list->beginQuery(queries.get(), 3);
  draw_rect(2, 2);
  list->setFramebuffer(framebuffer.get());
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
  bind_blit();
  const plume::RenderRect clear_rect(0, 0, 3, 3);
  list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f), &clear_rect, 1);
  bind_blit();
  draw_rect(2, 1);
  list->endQuery(queries.get(), 3);
  // 4: draws outside any query are not counted; then a draw after a barrier
  // (another encoder) inside one: 3x1 = 3.
  draw_rect(4, 4);
  list->beginQuery(queries.get(), 4);
  list->barriers(plume::RenderBarrierStage::GRAPHICS,
                 plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE));
  bind_blit();
  draw_rect(3, 1);
  list->endQuery(queries.get(), 4);
  list->setFramebuffer(nullptr);
  list->end();
  uint64_t counts[8] = {};
  const bool early = queries->getRawResults(0, 8, counts);
  queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
  queue->waitForCommandFence(fence.get());
  early ? fail("occlusion query results reported available before the command buffer ran")
        : pass("occlusion query results not available before the command buffer ran");
  if (!queries->getRawResults(0, 8, counts)) {
    fail("occlusion query results not available after the fence");
  } else {
    const uint64_t want[5] = {16, 4, 0, 6, 3};
    std::string got;
    bool match = true;
    for (uint32_t i = 0; i < 5; ++i) {
      got += (i ? " " : "") + std::to_string(counts[i]);
      match = match && counts[i] == want[i];
    }
    match ? pass("occlusion query sample counts " + got + " (lazy encoder, 2x2, empty, across encoders "
                 "without the quad clear, after a barrier)")
          : fail("occlusion query sample counts " + got + ", want 16 4 0 6 3");
  }

  // A quad clear (a rect clear) in a running encoder leaves its own pipeline, viewport and scissor on
  // the encoder (plume patch 0017). The draw after it, which sets no state because nothing of the
  // draw's changed, must run with the draw's state again.
  auto run_target = [&](auto &&record) {
    list->begin();
    list->barriers(plume::RenderBarrierStage::GRAPHICS,
                   plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE));
    list->setFramebuffer(framebuffer.get());
    record();
    list->setFramebuffer(nullptr);
    list->barriers(plume::RenderBarrierStage::COPY,
                   plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COPY_SOURCE));
    list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), td.format, kSize, kSize, 1, kSize),
                            plume::RenderTextureCopyLocation::Subresource(target.get()));
    list->end();
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());
    std::vector<uint32_t> pixels(kSize * kSize);
    std::memcpy(pixels.data(), readback->map(), sizeof(texels));
    readback->unmap();
    return pixels;
  };
  auto count_differences = [&](const std::vector<uint32_t> &got, const std::vector<uint32_t> &want) {
    uint32_t differences = 0;
    for (uint32_t i = 0; i < kSize * kSize; ++i)
      differences += got[i] != want[i];
    return differences;
  };
  {
    // A: blit the whole target, rect-clear its top-left quarter green, blit again without setting
    // any state: the second blit overwrites the clear, the whole target is the source.
    const plume::RenderRect full(0, 0, kSize, kSize), quarter(0, 0, 2, 2);
    const auto got = run_target([&]() {
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
      bind_blit();
      list->setScissors(&full, 1);
      list->drawInstanced(3, 1, 0, 0);
      list->clearColor(0, plume::RenderColor(0.0f, 1.0f, 0.0f, 1.0f), &quarter, 1);
      list->drawInstanced(3, 1, 0, 0);
    });
    const std::vector<uint32_t> want(texels, texels + kSize * kSize);
    const uint32_t differences = count_differences(got, want);
    differences == 0 ? pass("draw after a rect clear in the same encoder uses its own pipeline again (16/16 texels)")
                     : fail("draw after a rect clear ran with the clear's pipeline: " + std::to_string(differences) +
                            "/16 texels differ from the blit");
  }
  {
    // B: blit under a 2x2 scissor, rect-clear the bottom-right quarter red, blit again without
    // setting state: the scissor is the draw's again, so the clear survives.
    const plume::RenderRect quarter(0, 0, 2, 2), bottom_right(2, 2, 4, 4);
    const auto got = run_target([&]() {
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
      bind_blit();
      list->setScissors(&quarter, 1);
      list->drawInstanced(3, 1, 0, 0);
      list->clearColor(0, plume::RenderColor(1.0f, 0.0f, 0.0f, 1.0f), &bottom_right, 1);
      list->drawInstanced(3, 1, 0, 0);
    });
    std::vector<uint32_t> want(kSize * kSize, 0xFF000000u);
    for (uint32_t y = 0; y < kSize; ++y)
      for (uint32_t x = 0; x < kSize; ++x) {
        if (x < 2 && y < 2)
          want[y * kSize + x] = texels[y * kSize + x];
        else if (x >= 2 && y >= 2)
          want[y * kSize + x] = 0xFF0000FFu;
      }
    const uint32_t differences = count_differences(got, want);
    differences == 0 ? pass("draw after a rect clear in the same encoder uses its own scissor again (16/16 texels)")
                     : fail("draw after a rect clear ran with the clear's scissor: " + std::to_string(differences) +
                            "/16 texels differ");
  }

  // Per-encoder GPU timing (plume patch 0019): the blit's render pass and the readback copy each get
  // two counter samples through their own pass descriptors (no encoder is added or split). The
  // samples, converted with the device's GPU clock, must fall inside the command buffer's GPU
  // interval on the host clock: that checks the tick unit and the clock mapping at once.
  {
    plume::MetalDevice *metal_device = static_cast<plume::MetalDevice *>(device.get());
    plume::MetalPassTiming timing(metal_device);
    std::string error;
    if (!timing.create(4096, &error)) {
      fail("pass timing: " + error);
    } else {
      static uint32_t hook_calls, hook_width, hook_height, hook_attachments;
      hook_calls = hook_width = hook_height = hook_attachments = 0;
      timing.renderPassHook = [](uint32_t w, uint32_t h, uint32_t a) {
        hook_calls++;
        hook_width = w;
        hook_height = h;
        hook_attachments = a;
      };
      std::vector<plume::MetalGpuInterval> stale;
      metal_queue->drainGpuIntervals(stale);
      timing.begin();
      metal_queue->passTiming = &timing;
      const plume::RenderRect full(0, 0, kSize, kSize);
      run_target([&]() {
        bind_blit();
        list->setScissors(&full, 1);
        list->drawInstanced(3, 1, 0, 0);
      });
      metal_queue->passTiming = nullptr;
      timing.end();
      metal_queue->waitIdle();
      std::vector<plume::MetalGpuInterval> intervals;
      metal_queue->drainGpuIntervals(intervals);
      const plume::MetalGpuInterval *work = nullptr;
      for (const plume::MetalGpuInterval &i : intervals)
        if (eot::gpu::IsGpuWorkLabel(i.label))
          work = &i;
      std::vector<plume::MetalPassTiming::Span> spans;
      uint32_t invalid = 0, rejected = 0;
      timing.resolve(spans, &invalid, &rejected);
      const auto &clock = metal_device->getGpuClock();
      uint32_t renders = 0, blits = 0, outside = 0;
      double span_total = 0.0;
      for (const auto &span : spans) {
        renders += span.kind == plume::MetalPassTiming::Kind::Render;
        blits += span.kind == plume::MetalPassTiming::Kind::Blit;
        span_total += span.end - span.start;
        if (work && (span.start < work->start - 0.001 || span.end > work->end + 0.001))
          outside++;
      }
      char buf[320];
      std::snprintf(buf, sizeof(buf),
                    "%zu spans (%u render, %u blit), %u invalid, %u rejected, %u untimed, hook %u calls (%ux%u, %u attachments), "
                    "work buffer %s, %u spans outside it, GPU tick %.3f ns",
                    spans.size(), renders, blits, invalid, rejected, timing.untimed, hook_calls, hook_width, hook_height,
                    hook_attachments, work ? "found" : "missing", outside, clock.secondsPerTick * 1e9);
      const bool good = clock.valid && work && renders >= 1 && blits >= 1 && invalid == 0 && rejected == 0 && timing.untimed == 0 &&
                        outside == 0 && span_total > 0.0 && hook_calls == renders && hook_width == kSize &&
                        hook_height == kSize && hook_attachments == 1;
      if (good) {
        std::snprintf(buf + std::strlen(buf), sizeof(buf) - std::strlen(buf), "; spans total %.1f us of the buffer's %.1f us",
                      span_total * 1e6, (work->end - work->start) * 1e6);
        pass(std::string("per-encoder GPU timing from a sample buffer of ") + std::to_string(timing.capacity) + " samples: " + buf);
      } else {
        fail(std::string("per-encoder GPU timing: ") + buf);
      }
    }
  }
  {
    // A barrier that does not concern the open render pass leaves it open (plume patch 0020,
    // MetalDevice::keepPassOnBarrier). A 1024x1024 texture T is written by a blit encoder, a render pass
    // begins (its start waits for nothing of the blit), draws without T, takes a barrier on T, and draws
    // sampling T into the target, which is read back: byte for byte the same with the switch on (the
    // pass stays open, waits for the blit's fence at the barrier) and off (the barrier ends the pass), for
    // two patterns of T, 40 times each. Also: a barrier that includes an attachment, one with a buffer
    // and one with a full clear pending must still end the pass (counted by reason).
    constexpr uint32_t kBig = 1024, kSlotT = 20001;
    plume::RenderTextureDesc big_desc = td;
    big_desc.width = big_desc.height = kBig;
    big_desc.flags = plume::RenderTextureFlag::NONE;
    auto big = device->createTexture(big_desc);
    std::unique_ptr<plume::RenderBuffer> big_upload[2];
    for (int k = 0; k < 2; ++k) {
      big_upload[k] = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(kBig * kBig * 4));
      if (big_upload[k]) {
        uint32_t *w = static_cast<uint32_t *>(big_upload[k]->map());
        for (uint32_t i = 0; i < kBig * kBig; ++i)
          w[i] = 0xFF000000u | ((i * (k ? 7919u : 104729u)) & 0xFFFFFFu);
        big_upload[k]->unmap();
      }
    }
    auto side_buffer = device->createBuffer(plume::RenderBufferDesc::DefaultBuffer(256, plume::RenderBufferFlag::VERTEX));
    if (!big || !big_upload[0] || !big_upload[1] || !side_buffer) {
      fail("barrier keeps the pass: test resources");
    } else {
      plume::RenderTextureViewDesc big_vd;
      big_vd.format = td.format;
      big_vd.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
      big_vd.mipLevels = 1;
      auto big_view = big->createTextureView(big_vd);
      tex_set->setTexture(kSlotT, big.get(), plume::RenderTextureLayout::SHADER_READ, big_view.get());
      plume::MetalDevice *metal_device = static_cast<plume::MetalDevice *>(device.get());
      auto &stats = plume::g_metalBarrierStats;
      enum Variant { Plain, WithAttachment, WithBuffer, WithPendingClear, WithGeneral };
      auto run = [&](bool keep, Variant variant, int pattern) {
        metal_device->keepPassOnBarrier.store(keep);
        const plume::RenderRect full_rect(0, 0, kSize, kSize);
        list->begin();
        list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE));
        list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(big.get(), plume::RenderTextureLayout::COPY_DEST));
        list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(big.get()),
                                plume::RenderTextureCopyLocation::PlacedFootprint(big_upload[pattern].get(), td.format, kBig, kBig, 1, kBig));
        list->setFramebuffer(framebuffer.get());
        auto draw_sampling = [&](uint32_t slot) {
          CopyPushConstants p = pc;
          p.resourceDescriptorIndex = slot;
          list->setGraphicsPipelineLayout(layout.get());
          list->setPipeline(blit_rgba8.get());
          list->setGraphicsDescriptorSet(tex_set.get(), 0);
          list->setGraphicsDescriptorSet(tex_set.get(), 1);
          list->setGraphicsDescriptorSet(tex_set.get(), 2);
          list->setGraphicsDescriptorSet(sampler_set.get(), 3);
          list->setGraphicsDescriptorSet(tex_set.get(), 4);
          list->setViewports(&vp, 1);
          list->setScissors(&full_rect, 1);
          list->setGraphicsPushConstants(0, &p, kCopyPushConstantByteOffset, sizeof(p));
          list->drawInstanced(3, 1, 0, 0);
        };
        draw_sampling(kSlot);
        if (variant == WithPendingClear)
          list->clearColor(0, plume::RenderColor(0.0f, 1.0f, 0.0f, 1.0f));
        const plume::RenderTextureBarrier textures[2] = {
            plume::RenderTextureBarrier(big.get(), variant == WithGeneral ? plume::RenderTextureLayout::GENERAL : plume::RenderTextureLayout::SHADER_READ),
            plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE)};
        const plume::RenderBufferBarrier buffers[1] = {plume::RenderBufferBarrier(side_buffer.get(), plume::RenderBufferAccess::WRITE)};
        list->barriers(plume::RenderBarrierStage::GRAPHICS, variant == WithBuffer ? buffers : nullptr, variant == WithBuffer ? 1 : 0,
                       textures, variant == WithAttachment ? 2 : 1);
        draw_sampling(kSlotT);
        list->setFramebuffer(nullptr);
        list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), td.format, kSize, kSize, 1, kSize),
                                plume::RenderTextureCopyLocation::Subresource(target.get()));
        list->end();
        queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
        std::vector<uint32_t> pixels(kSize * kSize);
        std::memcpy(pixels.data(), readback->map(), sizeof(texels));
        readback->unmap();
        return pixels;
      };
      auto snapshot = [&]() {
        return std::array<uint64_t, 8>{stats.kept.load(), stats.noRenderPass.load(), stats.switchOff.load(), stats.bufferBarrier.load(),
                                       stats.attachment.load(), stats.pooledTexture.load(), stats.writableTextures.load(), stats.pendingClear.load()};
      };
      const bool layout_flag = !static_cast<plume::MetalPipelineLayout *>(layout.get())->writesTextures;
      layout_flag ? pass("barrier keeps the pass: the main pipeline layout has no writable texture range, so a pass drawing with it writes only its attachments")
                  : fail("barrier keeps the pass: the main layout reports writable textures");
      {
        plume::RenderDescriptorRange rw_range;
        rw_range.type = plume::RenderDescriptorRangeType::READ_WRITE_TEXTURE;
        rw_range.count = 1;
        rw_range.binding = 0;
        plume::RenderDescriptorSetDesc rw_set(&rw_range, 1);
        plume::RenderPipelineLayoutDesc rw_desc;
        rw_desc.descriptorSetDescs = &rw_set;
        rw_desc.descriptorSetDescsCount = 1;
        auto rw_layout = device->createPipelineLayout(rw_desc);
        rw_layout && static_cast<plume::MetalPipelineLayout *>(rw_layout.get())->writesTextures
            ? pass("barrier keeps the pass: a layout with a read-write texture range reports writable textures (the pass is not kept)")
            : fail("barrier keeps the pass: a layout with a read-write texture range does not report writable textures");
      }
      std::vector<uint32_t> reference[2];
      for (int k = 0; k < 2; ++k)
        reference[k] = run(false, Plain, k);
      bool distinct = reference[0] != reference[1];
      uint32_t mismatches = 0;
      const auto before = snapshot();
      for (int i = 0; i < 80; ++i)
        mismatches += run(true, Plain, i & 1) != reference[i & 1];
      const auto after = snapshot();
      const uint64_t kept = after[0] - before[0];
      distinct && mismatches == 0 && kept == 80
          ? pass("barrier keeps the pass: with the switch on, 80 runs (2 patterns of T) read back byte for byte what the ended-pass runs did; " +
                 std::to_string(kept) + " barriers kept the pass")
          : fail("barrier keeps the pass: " + std::to_string(mismatches) + " of 80 runs differ from the reference, " + std::to_string(kept) +
                 " kept, patterns " + (distinct ? "distinct" : "equal"));
      const auto before_off = snapshot();
      uint32_t off_mismatches = 0;
      for (int i = 0; i < 10; ++i)
        off_mismatches += run(false, Plain, i & 1) != reference[i & 1];
      const auto after_off = snapshot();
      off_mismatches == 0 && after_off[0] == before_off[0] && after_off[2] - before_off[2] == 10
          ? pass("barrier keeps the pass: with the switch off nothing is kept (10 barriers ended the pass, results unchanged)")
          : fail("barrier keeps the pass: switch off kept " + std::to_string(after_off[0] - before_off[0]) + ", " + std::to_string(off_mismatches) + " differ");
      struct Case { Variant v; const char *name; size_t counter; };
      const Case cases[] = {{WithAttachment, "an attachment in the barrier", 4}, {WithBuffer, "a buffer barrier", 3}, {WithPendingClear, "a pending full clear", 7},
                           {WithGeneral, "a texture moved to the general (storage) layout", 6}};
      for (const Case &c : cases) {
        const auto b = snapshot();
        const auto ref = run(false, c.v, 0);
        const auto got = run(true, c.v, 0);
        const auto a = snapshot();
        got == ref && a[0] == b[0] && a[c.counter] - b[c.counter] == 1
            ? pass(std::string("barrier keeps the pass: ") + c.name + " still ends the pass (counted), same texels with the switch on")
            : fail(std::string("barrier keeps the pass: ") + c.name + ": kept " + std::to_string(a[0] - b[0]) + ", reason counter " +
                   std::to_string(a[c.counter] - b[c.counter]) + (got == ref ? "" : ", texels differ"));
      }
      // The other direction: a barrier that moves T to be written (the pass only reads it, through the
      // bindless table, so plume cannot tell) keeps the pass open, yet the blit that overwrites T after the
      // pass must wait for the whole pass, not just for the part before the barrier. 150 draws sample T
      // into a 1024x1024 target (identity copy), then the barrier, then the pass ends and T is overwritten.
      auto target_big = device->createTexture(plume::RenderTextureDesc::ColorTarget(kBig, kBig, td.format));
      auto readback_big = device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(kBig * kBig * 4));
      if (!target_big || !readback_big) {
        fail("barrier keeps the pass: test resources (write after read)");
      } else {
        const plume::RenderTexture *big_colors[1] = {target_big.get()};
        auto big_fb = device->createFramebuffer(plume::RenderFramebufferDesc(big_colors, 1));
        const plume::RenderViewport big_vp(0.0f, 0.0f, float(kBig), float(kBig), 0.0f, 1.0f);
        const plume::RenderRect big_rect(0, 0, kBig, kBig);
        auto write_after_read = [&](bool keep) {
          metal_device->keepPassOnBarrier.store(keep);
          // T holds pattern 0 (written and waited for here).
          list->begin();
          list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(big.get(), plume::RenderTextureLayout::COPY_DEST));
          list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(big.get()),
                                  plume::RenderTextureCopyLocation::PlacedFootprint(big_upload[0].get(), td.format, kBig, kBig, 1, kBig));
          list->end();
          queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
          queue->waitForCommandFence(fence.get());
          list->begin();
          list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(big.get(), plume::RenderTextureLayout::SHADER_READ));
          list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(target_big.get(), plume::RenderTextureLayout::COLOR_WRITE));
          list->setFramebuffer(big_fb.get());
          CopyPushConstants p = pc;
          p.resourceDescriptorIndex = kSlotT;
          list->setGraphicsPipelineLayout(layout.get());
          list->setPipeline(blit_rgba8.get());
          list->setGraphicsDescriptorSet(tex_set.get(), 0);
          list->setGraphicsDescriptorSet(tex_set.get(), 1);
          list->setGraphicsDescriptorSet(tex_set.get(), 2);
          list->setGraphicsDescriptorSet(sampler_set.get(), 3);
          list->setGraphicsDescriptorSet(tex_set.get(), 4);
          list->setViewports(&big_vp, 1);
          list->setScissors(&big_rect, 1);
          list->setGraphicsPushConstants(0, &p, kCopyPushConstantByteOffset, sizeof(p));
          for (int d = 0; d < 150; ++d)
            list->drawInstanced(3, 1, 0, 0);
          // T is written next: the barrier (kept open or not) makes later work wait for this pass.
          list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(big.get(), plume::RenderTextureLayout::COPY_DEST));
          list->setFramebuffer(nullptr);
          list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(big.get()),
                                  plume::RenderTextureCopyLocation::PlacedFootprint(big_upload[1].get(), td.format, kBig, kBig, 1, kBig));
          list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(target_big.get(), plume::RenderTextureLayout::COPY_SOURCE));
          list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(readback_big.get(), td.format, kBig, kBig, 1, kBig),
                                  plume::RenderTextureCopyLocation::Subresource(target_big.get()));
          list->end();
          queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
          queue->waitForCommandFence(fence.get());
          std::vector<uint32_t> pixels(kBig * kBig);
          std::memcpy(pixels.data(), readback_big->map(), pixels.size() * 4);
          readback_big->unmap();
          return pixels;
        };
        const uint64_t kept_before = stats.kept.load();
        const auto off_pixels = write_after_read(false);
        const auto on_pixels = write_after_read(true);
        const uint64_t kept_now = stats.kept.load() - kept_before;
        const uint32_t *want = static_cast<const uint32_t *>(big_upload[0]->map());
        uint32_t bad_off = 0, bad_on = 0;
        for (uint32_t i = 0; i < kBig * kBig; ++i) {
          bad_off += off_pixels[i] != want[i];
          bad_on += on_pixels[i] != want[i];
        }
        big_upload[0]->unmap();
        bad_off == 0 && bad_on == 0 && kept_now == 1
            ? pass("barrier keeps the pass: a texture the pass only reads is overwritten after it: the blit waited for the whole pass (1M texels intact, switch on and off; " +
                   std::to_string(kept_now) + " barrier kept the pass)")
            : fail("barrier keeps the pass: write after read: " + std::to_string(bad_off) + " texels wrong with the switch off, " + std::to_string(bad_on) +
                   " with it on, " + std::to_string(kept_now) + " kept");
      }
      metal_device->keepPassOnBarrier.store(false);
    }
  }
  {
    // Resolve folded into the pass (plume patch 0021). A 2x multisampled target is cleared and drawn
    // with a triangle whose left and bottom edges cut through pixels (a half-pixel viewport offset), so the
    // resolve really averages. The same scene is resolved the old way (a pass of its own that loads the
    // multisampled texture) and folded (the scene pass stores and resolves as it ends): the bytes must
    // match. Also: a pass with a resolve target resolves into it as it ends, asked or not, and stores the
    // multisampled texture; a barrier that ended the pass first, a pending full clear and a target of another
    // size all make the fold refuse (false, nothing changed) so the old path still works.
    auto blit_ms = CopyPipeline(device.get(), layout.get(), copy_vs.get(), blit_ps.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                plume::RenderFormat::UNKNOWN, plume::RenderSampleCount::COUNT_2);
    auto msaa = device->createTexture(plume::RenderTextureDesc::ColorTarget(
        kSize, kSize, plume::RenderFormat::R8G8B8A8_UNORM, plume::RenderMultisampling(plume::RenderSampleCount::COUNT_2)));
    auto single = device->createTexture(plume::RenderTextureDesc::ColorTarget(kSize, kSize, plume::RenderFormat::R8G8B8A8_UNORM));
    auto wrong_size = device->createTexture(plume::RenderTextureDesc::ColorTarget(8, 8, plume::RenderFormat::R8G8B8A8_UNORM));
    if (!blit_ms || !msaa || !single || !wrong_size) {
      fail("resolve fold: test resources");
    } else {
      const plume::RenderTexture *msaa_color[1] = {msaa.get()};
      auto msaa_fb = device->createFramebuffer(plume::RenderFramebufferDesc(msaa_color, 1));
      const plume::RenderViewport half_pixel(0.5f, 0.5f, float(kSize), float(kSize), 0.0f, 1.0f);
      const plume::RenderRect full_rect(0, 0, kSize, kSize);
      auto read_single = [&]() {
        list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(single.get(), plume::RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), td.format, kSize, kSize, 1, kSize),
                                plume::RenderTextureCopyLocation::Subresource(single.get()));
        list->end();
        queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
        std::vector<uint32_t> pixels(kSize * kSize);
        std::memcpy(pixels.data(), readback->map(), sizeof(texels));
        readback->unmap();
        return pixels;
      };
      struct Mode {
        const plume::RenderTexture *target = nullptr;       // setColorResolveTarget
        const plume::RenderTexture *request_dst = nullptr;  // resolveTextureAtPassEnd (null: not asked)
        bool barrier_first = false;
        bool barrier_on_twin = false;  // the barrier is on the resolve target
        bool retarget_null = false;    // the resolve target is taken away after the draw (setColorResolveTarget(0, null))
        bool handoff = false;          // releaseResolveTarget after the draw, the twin gets other texels, the pass goes on
        bool pending_clear = false;
        bool prefill = false;
        bool resolve_old = true;  // resolveTexture when the fold was not done
      };
      struct Result {
        std::vector<uint32_t> pixels;
        bool asked = false;
        std::string refusal;
      };
      auto scene = [&](const Mode &m) {
        Result r;
        list->begin();
        if (m.prefill)
          list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(single.get()),
                                  plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), td.format, kSize, kSize, 1, kSize));
        list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(msaa.get(), plume::RenderTextureLayout::COLOR_WRITE));
        list->setFramebuffer(msaa_fb.get());
        list->setColorResolveTarget(0, m.target);
        list->clearColor(0, plume::RenderColor(0.2f, 0.4f, 0.6f, 1.0f));
        auto draw_ms = [&]() {
          list->setGraphicsPipelineLayout(layout.get());
          list->setPipeline(blit_ms.get());
          list->setGraphicsDescriptorSet(tex_set.get(), 0);
          list->setGraphicsDescriptorSet(tex_set.get(), 1);
          list->setGraphicsDescriptorSet(tex_set.get(), 2);
          list->setGraphicsDescriptorSet(sampler_set.get(), 3);
          list->setGraphicsDescriptorSet(tex_set.get(), 4);
          list->setViewports(&half_pixel, 1);
          list->setScissors(&full_rect, 1);
          list->setGraphicsPushConstants(0, &pc, kCopyPushConstantByteOffset, sizeof(pc));
          list->drawInstanced(3, 1, 0, 0);
        };
        draw_ms();
        if (m.retarget_null)
          list->setColorResolveTarget(0, nullptr);
        if (m.handoff) {
          r.asked = list->releaseResolveTarget(m.target);  // reused as "the release found the target"
          list->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(single.get(), plume::RenderTextureLayout::COPY_DEST));
          list->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(single.get()),
                                  plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), td.format, kSize, kSize, 1, kSize));
          list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(msaa.get(), plume::RenderTextureLayout::COLOR_WRITE));
          draw_ms();  // the pass is reopened on the same framebuffer
          list->setFramebuffer(nullptr);
          list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(msaa.get(), plume::RenderTextureLayout::RESOLVE_SOURCE));
          r.pixels = read_single();
          return r;
        }
        if (m.barrier_first)
          list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(source.get(), plume::RenderTextureLayout::SHADER_READ));
        if (m.barrier_on_twin)
          list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(single.get(), plume::RenderTextureLayout::RESOLVE_DEST));
        if (m.pending_clear)
          list->clearColor(0, plume::RenderColor(1.0f, 0.0f, 0.0f, 1.0f));
        if (m.request_dst) {
          r.asked = list->resolveTextureAtPassEnd(m.request_dst, msaa.get());
          const char *why = static_cast<plume::MetalCommandList *>(list.get())->lastFoldRefusal;
          r.refusal = r.asked ? "" : (why ? why : "?");
        }
        list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(msaa.get(), plume::RenderTextureLayout::RESOLVE_SOURCE));
        list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(single.get(), plume::RenderTextureLayout::RESOLVE_DEST));
        if (m.resolve_old && !r.asked)
          list->resolveTexture(single.get(), msaa.get());
        list->setFramebuffer(nullptr);
        r.pixels = read_single();
        return r;
      };
      auto hex = [](const std::vector<uint32_t> &p) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%08x %08x %08x", p[0], p[1], p[kSize]);
        return std::string(buf);
      };

      const Result old_path = scene({});
      bool varied = false;
      for (uint32_t i = 1; i < kSize * kSize; ++i)
        varied = varied || old_path.pixels[i] != old_path.pixels[0];
      old_path.pixels[0] != 0 && varied ? pass("resolve fold: the reference scene (old path: a resolve pass of its own) resolves to " + hex(old_path.pixels) + " ...")
                                         : fail("resolve fold: the reference scene is blank or uniform: " + hex(old_path.pixels));

      Mode fold;
      fold.target = single.get();
      fold.request_dst = single.get();
      const uint64_t pass_ends_before = plume::g_metalResolvePassEnds.load();
      const Result folded = scene(fold);
      const uint64_t pass_ends = plume::g_metalResolvePassEnds.load() - pass_ends_before;
      pass_ends == 1 ? pass("resolve fold: one render pass ended resolving into its target (g_metalResolvePassEnds)")
                     : fail("resolve fold: " + std::to_string(pass_ends) + " passes ended resolving, want 1");
      folded.asked && folded.pixels == old_path.pixels
          ? pass("resolve fold: folded into the scene pass's store action, 16/16 texels equal the old path's (" + hex(folded.pixels) + " ...)")
          : fail(std::string("resolve fold: asked=") + (folded.asked ? "true" : "false") + " " + folded.refusal + ", texels differ from the old path: " + hex(folded.pixels));

      // A pass that names a resolve target resolves into it as it ends, asked or not (Metal fixes the
      // store action when the pass begins). Nobody asks here: the prefilled texels are overwritten.
      Mode unused;
      unused.target = single.get();
      unused.prefill = true;
      unused.resolve_old = false;
      const Result unasked = scene(unused);
      unasked.pixels == old_path.pixels
          ? pass("resolve fold: a pass with a resolve target resolves into it as it ends even when nobody asks (prefilled texels replaced by the resolve)")
          : fail("resolve fold: an unrequested resolve target holds " + hex(unasked.pixels) + ", want the resolve " + hex(old_path.pixels));
      // The multisampled texture was stored: resolve it the old way, without drawing again.
      list->begin();
      list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(msaa.get(), plume::RenderTextureLayout::RESOLVE_SOURCE));
      list->barriers(plume::RenderBarrierStage::GRAPHICS, plume::RenderTextureBarrier(single.get(), plume::RenderTextureLayout::RESOLVE_DEST));
      list->resolveTexture(single.get(), msaa.get());
      const auto stored = read_single();
      stored == old_path.pixels ? pass("resolve fold: the multisampled texture of that pass was stored (resolving it later gives the same texels)")
                                : fail("resolve fold: the multisampled texture was not stored by a pass with a resolve target: " + hex(stored));

      Mode barrier = fold;
      barrier.barrier_first = true;
      const Result after_barrier = scene(barrier);
      !after_barrier.asked && after_barrier.pixels == old_path.pixels
          ? pass("resolve fold: refused after a barrier ended the pass (" + after_barrier.refusal + "); the old path gives the same texels")
          : fail("resolve fold: after a barrier asked=" + std::string(after_barrier.asked ? "true" : "false") + ", " + hex(after_barrier.pixels));

      // With the pass-keeping barriers on (plume patch 0020), the same unrelated barrier leaves the pass
      // open and the fold goes through; a barrier on the resolve target itself still ends the pass.
      plume::MetalDevice *fold_device = static_cast<plume::MetalDevice *>(device.get());
      fold_device->keepPassOnBarrier.store(true);
      const Result kept_open = scene(barrier);
      Mode twin_barrier = fold;
      twin_barrier.barrier_on_twin = true;
      const Result twin_ended = scene(twin_barrier);
      fold_device->keepPassOnBarrier.store(false);
      kept_open.asked && kept_open.pixels == old_path.pixels
          ? pass("resolve fold: a pass kept open across an unrelated barrier is then folded, 16/16 texels equal the old path's")
          : fail("resolve fold: after a kept barrier asked=" + std::string(kept_open.asked ? "true" : "false") + " " + kept_open.refusal + ", " + hex(kept_open.pixels));
      !twin_ended.asked && twin_ended.pixels == old_path.pixels
          ? pass("resolve fold: a barrier on the resolve target ends the pass even with the switch on (" + twin_ended.refusal + "); the old path gives the same texels")
          : fail("resolve fold: a barrier on the resolve target asked=" + std::string(twin_ended.asked ? "true" : "false") + ", " + hex(twin_ended.pixels));

      // The target taken away mid-pass ends a pass that would resolve into it (and the fold is refused).
      Mode retarget = fold;
      retarget.retarget_null = true;
      const Result taken_away = scene(retarget);
      !taken_away.asked && taken_away.pixels == old_path.pixels
          ? pass("resolve fold: taking the resolve target away mid-pass ends the pass that resolved into it (" + taken_away.refusal + "); the old path gives the same texels")
          : fail("resolve fold: after taking the target away asked=" + std::string(taken_away.asked ? "true" : "false") + ", " + hex(taken_away.pixels));
      // A texture handed over (releaseResolveTarget) is not written by the pass that is reopened on the
      // framebuffer; without the release it is (the control).
      Mode handoff;
      handoff.target = single.get();
      handoff.handoff = true;
      const Result handed = scene(handoff);
      const std::vector<uint32_t> sentinel_texels(texels, texels + kSize * kSize);
      handed.asked && handed.pixels == sentinel_texels
          ? pass("resolve fold: a texture handed over mid-frame (releaseResolveTarget) stays as it was handed, the reopened pass does not resolve into it")
          : fail("resolve fold: after the hand-over the texture holds " + hex(handed.pixels) + (handed.asked ? "" : " (the release found no target)"));
      Mode no_target_twin;
      no_target_twin.prefill = true;
      no_target_twin.resolve_old = false;
      const Result untouched_twin = scene(no_target_twin);
      untouched_twin.pixels == sentinel_texels
          ? pass("resolve fold: a pass that names no target (the twin holds newer content) leaves the twin alone")
          : fail("resolve fold: a pass without a target changed the twin: " + hex(untouched_twin.pixels));

      Mode pending = fold;
      pending.pending_clear = true;
      const Result with_clear = scene(pending);
      const std::vector<uint32_t> red(kSize * kSize, 0xFF0000FFu);
      !with_clear.asked && with_clear.pixels == red
          ? pass("resolve fold: refused with a full clear pending (" + with_clear.refusal + "); the old path resolves the cleared texture, as before")
          : fail("resolve fold: pending clear asked=" + std::string(with_clear.asked ? "true" : "false") + ", " + hex(with_clear.pixels));

      Mode wrong = fold;
      wrong.target = wrong_size.get();
      wrong.request_dst = wrong_size.get();
      const Result mismatched = scene(wrong);
      !mismatched.asked && mismatched.pixels == old_path.pixels
          ? pass("resolve fold: a resolve target of another size is ignored (" + mismatched.refusal + ")")
          : fail("resolve fold: a mismatched target asked=" + std::string(mismatched.asked ? "true" : "false") + ", " + hex(mismatched.pixels));

      Mode not_set;
      not_set.request_dst = single.get();
      const Result no_target = scene(not_set);
      !no_target.asked && no_target.pixels == old_path.pixels
          ? pass("resolve fold: refused when the pass named no resolve target (" + no_target.refusal + ")")
          : fail("resolve fold: without a target asked=" + std::string(no_target.asked ? "true" : "false") + ", " + hex(no_target.pixels));
    }
  }
  {
    // A waiter that sees the fence finds the list's interval recorded (the interval's handler is
    // registered before the fence's): 200 empty lists, drained right after each fence wait.
    auto empty = queue->createCommandList();
    auto empty_fence = device->createCommandFence();
    std::vector<plume::MetalGpuInterval> got;
    uint32_t missing = 0;
    metal_queue->waitIdle();
    metal_queue->drainGpuIntervals(got);  // the earlier tests' lists
    for (int i = 0; i < 200; ++i) {
      empty->begin();
      empty->end();
      const plume::RenderCommandList *el[] = {empty.get()};
      queue->executeCommandLists(el, 1, nullptr, 0, nullptr, 0, empty_fence.get());
      queue->waitForCommandFence(empty_fence.get());
      got.clear();
      metal_queue->drainGpuIntervals(got);
      uint32_t lists = 0;
      for (const plume::MetalGpuInterval &g : got)
        lists += eot::gpu::IsGpuWorkLabel(g.label);
      missing += lists != 1;
    }
    metal_queue->waitIdle();
    missing == 0 ? pass("the list's GPU interval is recorded before its fence is seen (200 of 200 lists)")
                 : fail("the list's GPU interval was not there when the fence was seen: " + std::to_string(missing) + " of 200");
  }
  {
    // Passes with no draw: a load-and-resolve pass (MSAA source) and a clear-only pass, in three rounds
    // that reuse the same sample indices (the sample buffer is never cleared). Each pass must write
    // both of its samples afresh: the value of an index is replaced by a later GPU timestamp, not
    // left over from the round before. Metal's answer to "what does a pass with no vertex work write
    // for start-of-vertex" is part of the message.
    plume::MetalDevice *md = static_cast<plume::MetalDevice *>(device.get());
    plume::MetalPassTiming t(md);
    std::string err;
    auto msaa = device->createTexture(plume::RenderTextureDesc::ColorTarget(
        kSize, kSize, plume::RenderFormat::R8G8B8A8_UNORM, plume::RenderMultisampling(plume::RenderSampleCount::COUNT_4)));
    if (!t.create(256, &err) || !msaa) {
      fail("draw-less pass timing: " + (err.empty() ? std::string("no MSAA texture") : err));
    } else {
      (void)md->getGpuClock();
      auto raw = [&](uint32_t n) {
        std::vector<uint64_t> v(n);
        const NS::Data *d = t.sampleBuffer->resolveCounterRange(NS::Range(0, n));
        std::memcpy(v.data(), d->mutableBytes(), n * sizeof(uint64_t));
        return v;
      };
      auto timed = [&](auto &&submit) {
        t.begin();
        metal_queue->passTiming = &t;
        submit();
        metal_queue->passTiming = nullptr;
        t.end();
        metal_queue->waitIdle();
      };
      bool good = true;
      std::string detail;
      std::vector<uint64_t> before;
      for (int round = 0; round < 3; ++round) {
        // A real draw pass first: the indices 0..3 hold its samples.
        const plume::RenderRect full(0, 0, kSize, kSize);
        timed([&]() {
          run_target([&]() {
            bind_blit();
            list->setScissors(&full, 1);
            list->drawInstanced(3, 1, 0, 0);
          });
        });
        before = raw(2);
        // The draw-less resolve.
        timed([&]() {
          list->begin();
          list->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(msaa.get(), plume::RenderTextureLayout::RESOLVE_SOURCE));
          list->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::RESOLVE_DEST));
          list->resolveTexture(target.get(), msaa.get());
          list->end();
          queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
          queue->waitForCommandFence(fence.get());
        });
        std::vector<plume::MetalPassTiming::Span> spans;
        uint32_t invalid = 0, rejected = 0;
        t.resolve(spans, &invalid, &rejected);
        const auto after = raw(2);
        const bool resolve_ok = t.records.size() == 1 && spans.size() == 1 && invalid == 0 && rejected == 0 &&
                                after[0] > before[0] && after[1] > after[0] && after[1] != ~0ull;
        good = good && resolve_ok;
        char buf[200];
        if (round == 0) {
          std::snprintf(buf, sizeof(buf), "load+resolve pass: start-of-vertex %llu -> %llu (+%.1f us over the draw pass's), end-of-fragment %llu, span %.1f us",
                        static_cast<unsigned long long>(before[0]), static_cast<unsigned long long>(after[0]),
                        double(after[0] - before[0]) * 1e-3, static_cast<unsigned long long>(after[1]),
                        spans.empty() ? 0.0 : (spans[0].end - spans[0].start) * 1e6);
          detail += buf;
        }
        // The clear-only pass (and the readback blit after it).
        before = raw(2);
        timed([&]() {
          run_target([&]() { list->clearColor(0, plume::RenderColor(0.0f, 1.0f, 0.0f, 1.0f)); });
        });
        spans.clear();
        invalid = rejected = 0;
        t.resolve(spans, &invalid, &rejected);
        const auto clear_after = raw(2);
        const bool clear_ok = spans.size() == 2 && invalid == 0 && rejected == 0 && clear_after[0] > before[0] &&
                              clear_after[1] > clear_after[0];
        good = good && clear_ok;
        if (round == 0) {
          std::snprintf(buf, sizeof(buf), "; clear-only pass: start-of-vertex %llu -> %llu, end-of-fragment %llu, span %.1f us",
                        static_cast<unsigned long long>(before[0]), static_cast<unsigned long long>(clear_after[0]),
                        static_cast<unsigned long long>(clear_after[1]),
                        spans.empty() ? 0.0 : (spans[0].end - spans[0].start) * 1e6);
          detail += buf;
        }
      }
      good ? pass("draw-less passes (3 rounds on reused indices): Metal writes both samples afresh, none stale or error: " + detail)
           : fail("draw-less passes wrote a stale, error or missing sample: " + detail);

      // The guard: an index nothing wrote this frame still holds the last frame's value, and resolve
      // rejects the span (and counts it) instead of reporting it.
      t.begin();
      uint32_t a = 0, b = 0;
      t.allocate(plume::MetalPassTiming::Kind::Compute, a, b);
      std::vector<plume::MetalPassTiming::Span> spans;
      uint32_t invalid = 0, rejected = 0;
      t.resolve(spans, &invalid, &rejected);
      spans.empty() && rejected == 1 && invalid == 0
          ? pass("a span whose samples predate begin() is rejected and counted (stale index)")
          : fail("a stale span was not rejected: " + std::to_string(spans.size()) + " spans, " + std::to_string(rejected) + " rejected");
    }
  }
  {
    // The sample buffer's capacity: a frame with more encoders than it holds leaves the rest untimed.
    plume::MetalPassTiming small(static_cast<plume::MetalDevice *>(device.get()));
    std::string error;
    if (!small.create(64, &error)) {
      fail("pass timing: " + error);
    } else {
      small.begin();
      uint32_t timed = 0, a = 0, b = 0;
      for (uint32_t i = 0; i < small.capacity / 2 + 3; ++i)
        timed += small.allocate(plume::MetalPassTiming::Kind::Compute, a, b);
      timed == small.capacity / 2 && small.untimed == 3 && small.records.size() == timed
          ? pass("pass timing capacity: " + std::to_string(timed) + " encoders timed in " + std::to_string(small.capacity) +
                 " samples, 3 more left untimed")
          : fail("pass timing capacity: " + std::to_string(timed) + " timed, " + std::to_string(small.untimed) + " untimed");
    }
  }
  return ok;
}

// ---- MetalFX spatial scaler through plume (patch 0022) --------------------------------------------------------------
//
// A frame image F (1920x1080, a render target) is written by a render pass that is still open when the scaler runs,
// as the renderer's present does: four flat quadrants by quad clears. MetalCommandList::encodeExternal ends the pass,
// MetalFX scales F into O (3024x1898, the fullscreen size of the machine this was written for), and the barrier on O
// then carries the order to a render pass that samples O through the bindless table (the way the present pass reads
// it) into a target T, which is read back. The readback itself is an explicit use (a blit bound to T), so only the
// bindless read of O says whether the ordering of the scaler's writes is carried. The palette rotates every
// iteration: a stale read of F (the scaler running before the clears) or of O (the sampling pass before the scaler)
// shows as the previous iteration's colours.
bool RunMetalFxTests(std::string *report) {
  auto iface = plume::CreateMetalInterface();
  auto device = iface ? iface->createDevice() : nullptr;
  if (!device) {
    *report = "  FAIL no Metal device\n";
    return false;
  }
  bool ok = true;
  auto fail = [&](const std::string &what) {
    ok = false;
    *report += "  FAIL " + what + "\n";
  };
  auto pass = [&](const std::string &what) { *report += "  ok   " + what + "\n"; };
  plume::MetalDevice *metal_device = static_cast<plume::MetalDevice *>(device.get());
  if (!plume::MetalSpatialScaler::isSupported(metal_device)) {
    *report += "  --   MTLFXSpatialScalerDescriptor supportsDevice is false here: nothing to test\n";
    return true;
  }
  pass("MTLFXSpatialScalerDescriptor supportsDevice is true");

  plume::RenderDescriptorSetBuilder tex_builder, sampler_builder;
  eot::gpu::DescribeTextureHeapSet(tex_builder);
  eot::gpu::DescribeSamplerHeapSet(sampler_builder);
  auto tex_set = tex_builder.create(device.get());
  auto sampler_set = sampler_builder.create(device.get());
  plume::RenderPipelineLayoutBuilder layout_builder;
  layout_builder.begin(false, true);
  eot::gpu::DescribeMainLayoutSets(layout_builder, tex_builder, sampler_builder);
  eot::gpu::DescribeMainLayoutPushConstants(layout_builder, false);
  layout_builder.end();
  auto layout = layout_builder.create(device.get());
  auto copy_vs = HostShader(device.get(), g_copy_vs_spirv, sizeof(g_copy_vs_spirv));
  auto blit_ps = HostShader(device.get(), g_blit_ps_spirv, sizeof(g_blit_ps_spirv));
  if (!tex_set || !sampler_set || !layout || !copy_vs || !blit_ps) {
    fail("main layout and blit shaders");
    return false;
  }
  auto blit = CopyPipeline(device.get(), layout.get(), copy_vs.get(), blit_ps.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                           plume::RenderFormat::UNKNOWN);
  if (!blit) {
    fail("blit pipeline");
    return false;
  }
  plume::RenderSamplerDesc linear;
  linear.minFilter = linear.magFilter = plume::RenderFilter::LINEAR;
  linear.addressU = linear.addressV = linear.addressW = plume::RenderTextureAddressMode::CLAMP;
  plume::RenderSamplerDesc point = linear;
  point.minFilter = point.magFilter = plume::RenderFilter::NEAREST;
  point.mipmapMode = plume::RenderMipmapMode::NEAREST;
  auto linear_sampler = device->createSampler(linear);
  auto point_sampler = device->createSampler(point);
  sampler_set->setSampler(0, linear_sampler.get());
  sampler_set->setSampler(1, point_sampler.get());

  constexpr uint32_t kInW = 1920, kInH = 1080, kOutW = 3024, kOutH = 1898, kSlotO = 20010;
  auto target = device->createTexture(plume::RenderTextureDesc::ColorTarget(kOutW, kOutH, plume::RenderFormat::R8G8B8A8_UNORM));
  auto readback = device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(kOutW) * kOutH * 4));
  if (!target || !readback) {
    fail("test resources");
    return false;
  }
  const plume::RenderTexture *target_colors[1] = {target.get()};
  auto target_fb = device->createFramebuffer(plume::RenderFramebufferDesc(target_colors, 1));
  auto queue = device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  auto list = queue->createCommandList();
  auto fence = device->createCommandFence();
  const plume::RenderCommandList *lists[] = {list.get()};

  struct Case {
    plume::RenderFormat format;
    plume::MetalSpatialScaler::ColorMode mode;
    const char *name;
    int iterations;
    // The largest error of a quadrant's mean, in 1/255 (R11G11B10 keeps 5 bits of mantissa in blue).
    double tolerance = 6.0;
    // Informational control: the barrier on O that carries the scaler's order to the sampling pass is left out.
    bool control = false;
  };
  const Case cases[] = {
      {plume::RenderFormat::R8G8B8A8_UNORM, plume::MetalSpatialScaler::ColorMode::Perceptual, "R8G8B8A8_UNORM, perceptual", 40},
      {plume::RenderFormat::B8G8R8A8_UNORM, plume::MetalSpatialScaler::ColorMode::Perceptual, "B8G8R8A8_UNORM, perceptual", 8},
      {plume::RenderFormat::R8G8B8A8_UNORM, plume::MetalSpatialScaler::ColorMode::Linear, "R8G8B8A8_UNORM, linear", 4},
      {plume::RenderFormat::R16G16B16A16_FLOAT, plume::MetalSpatialScaler::ColorMode::HDR, "R16G16B16A16_FLOAT, hdr", 8},
      {plume::RenderFormat::R16G16B16A16_UNORM, plume::MetalSpatialScaler::ColorMode::Perceptual, "R16G16B16A16_UNORM, perceptual", 4},
      {plume::RenderFormat::R11G11B10_FLOAT, plume::MetalSpatialScaler::ColorMode::HDR, "R11G11B10_FLOAT, hdr", 4, 12.0},
      {plume::RenderFormat::R8G8B8A8_UNORM, plume::MetalSpatialScaler::ColorMode::Perceptual,
       "control without the barrier on the output (R8G8B8A8_UNORM)", 40, 6.0, true},
  };
  const float palette[8][3] = {{0.85f, 0.15f, 0.15f}, {0.15f, 0.75f, 0.2f}, {0.2f, 0.25f, 0.9f}, {0.9f, 0.85f, 0.1f},
                               {0.1f, 0.8f, 0.8f},    {0.8f, 0.2f, 0.75f},  {0.5f, 0.5f, 0.5f},  {0.95f, 0.6f, 0.15f}};
  for (const Case &c : cases) {
    plume::MetalSpatialScaler::Desc desc;
    desc.colorFormat = desc.outputFormat = c.format;
    desc.inputWidth = kInW;
    desc.inputHeight = kInH;
    desc.outputWidth = kOutW;
    desc.outputHeight = kOutH;
    desc.colorMode = c.mode;
    std::string why;
    auto scaler = plume::MetalSpatialScaler::create(metal_device, desc, &why);
    if (!scaler) {
      *report += std::string("  --   ") + c.name + ": no scaler (" + why + ")\n";
      continue;
    }
    auto frame = device->createTexture(plume::RenderTextureDesc::ColorTarget(kInW, kInH, c.format));
    plume::RenderTextureDesc out_desc = plume::RenderTextureDesc::Texture2D(kOutW, kOutH, 1, c.format, scaler->outputTextureFlags());
    auto out = device->createTexture(out_desc);
    if (!frame || !out) {
      *report += std::string("  --   ") + c.name + ": a texture could not be created (output flags " +
                 std::to_string(static_cast<uint32_t>(scaler->outputTextureFlags())) + ")\n";
      continue;
    }
    plume::RenderTextureViewDesc view_desc;
    view_desc.format = c.format;
    view_desc.dimension = plume::RenderTextureViewDimension::TEXTURE_2D;
    view_desc.mipLevels = 1;
    auto out_view = out->createTextureView(view_desc);
    tex_set->setTexture(kSlotO, out.get(), plume::RenderTextureLayout::SHADER_READ, out_view.get());
    const plume::RenderTexture *frame_colors[1] = {frame.get()};
    auto frame_fb = device->createFramebuffer(plume::RenderFramebufferDesc(frame_colors, 1));

    uint32_t wrong = 0, trivial = 0, refused = 0;
    double worst_quadrant = 0.0, worst_mean = 0.0;
    std::string first_wrong;
    for (int k = 0; k < c.iterations; ++k) {
      const float *quad[4];
      for (int q = 0; q < 4; ++q)
        quad[q] = palette[(k + 2 * q) % 8];
      list->begin();
      // Before the scaler runs: this barrier takes its source stage from the target (copy, left by the last
      // readback), and a wait it set after the scaler would stand in for the one on the output.
      list->barriers(plume::RenderBarrierStage::GRAPHICS,
                     plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COLOR_WRITE));
      list->barriers(plume::RenderBarrierStage::GRAPHICS,
                     plume::RenderTextureBarrier(frame.get(), plume::RenderTextureLayout::COLOR_WRITE));
      list->setFramebuffer(frame_fb.get());
      list->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
      const int32_t mx = kInW / 2, my = kInH / 2;
      const plume::RenderRect rects[4] = {{0, 0, mx, my}, {mx, 0, int32_t(kInW), my}, {0, my, mx, int32_t(kInH)},
                                          {mx, my, int32_t(kInW), int32_t(kInH)}};
      for (int q = 0; q < 4; ++q)
        list->clearColor(0, plume::RenderColor(quad[q][0], quad[q][1], quad[q][2], 1.0f), &rects[q], 1);
      // The render pass is still open here, as at the present.
      std::string error;
      if (!scaler->encode(static_cast<plume::MetalCommandList *>(list.get()), frame.get(), out.get(), &error)) {
        refused++;
        if (first_wrong.empty())
          first_wrong = "encode refused: " + error;
        list->setFramebuffer(nullptr);
        list->end();
        continue;
      }
      if (!c.control)
        list->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(out.get(), plume::RenderTextureLayout::SHADER_READ));
      list->setGraphicsPipelineLayout(layout.get());
      list->setPipeline(blit.get());
      list->setGraphicsDescriptorSet(tex_set.get(), 0);
      list->setGraphicsDescriptorSet(tex_set.get(), 1);
      list->setGraphicsDescriptorSet(tex_set.get(), 2);
      list->setGraphicsDescriptorSet(sampler_set.get(), 3);
      list->setGraphicsDescriptorSet(tex_set.get(), 4);
      list->setFramebuffer(target_fb.get());
      const plume::RenderViewport vp(0.0f, 0.0f, float(kOutW), float(kOutH), 0.0f, 1.0f);
      const plume::RenderRect sc(0, 0, kOutW, kOutH);
      list->setViewports(&vp, 1);
      list->setScissors(&sc, 1);
      CopyPushConstants pc;
      pc.resourceDescriptorIndex = kSlotO;
      pc.param1 = 1.0f;
      list->setGraphicsPushConstants(0, &pc, kCopyPushConstantByteOffset, sizeof(pc));
      list->drawInstanced(3, 1, 0, 0);
      list->setFramebuffer(nullptr);
      list->barriers(plume::RenderBarrierStage::COPY,
                     plume::RenderTextureBarrier(target.get(), plume::RenderTextureLayout::COPY_SOURCE));
      list->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(readback.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                                                kOutW, kOutH, 1, kOutW),
                              plume::RenderTextureCopyLocation::Subresource(target.get()));
      list->end();
      queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
      queue->waitForCommandFence(fence.get());

      const auto *pixels = static_cast<const uint8_t *>(readback->map());
      // The mean of each quadrant, away from the lines where the colours meet (a filter rings there), and of
      // the whole image: what a bilinear upscale of the four flat quadrants keeps (the mean of the four).
      double whole[3] = {};
      uint64_t whole_count = 0;
      for (uint32_t y = 0; y < kOutH; y += 2)
        for (uint32_t x = 0; x < kOutW; x += 2) {
          const uint8_t *p = pixels + (size_t(y) * kOutW + x) * 4;
          for (int ch = 0; ch < 3; ++ch)
            whole[ch] += p[ch];
          whole_count++;
        }
      double mean_error = 0.0, sum = 0.0;
      for (int ch = 0; ch < 3; ++ch) {
        const double want = 255.0 * (quad[0][ch] + quad[1][ch] + quad[2][ch] + quad[3][ch]) * 0.25;
        mean_error = std::max(mean_error, std::fabs(whole[ch] / whole_count - want));
        sum += whole[ch];
      }
      worst_mean = std::max(worst_mean, mean_error);
      if (sum < 1.0)
        trivial++;
      const uint32_t qx = kOutW / 2, qy = kOutH / 2, margin = 48;
      const uint32_t x0[4] = {margin, qx + margin, margin, qx + margin};
      const uint32_t x1[4] = {qx - margin, kOutW - margin, qx - margin, kOutW - margin};
      const uint32_t y0[4] = {margin, margin, qy + margin, qy + margin};
      const uint32_t y1[4] = {qy - margin, qy - margin, kOutH - margin, kOutH - margin};
      bool iteration_wrong = mean_error > std::max(3.0, c.tolerance * 0.5);
      for (int q = 0; q < 4; ++q) {
        double m[3] = {};
        uint64_t n = 0;
        for (uint32_t y = y0[q]; y < y1[q]; y += 3)
          for (uint32_t x = x0[q]; x < x1[q]; x += 3) {
            const uint8_t *p = pixels + (size_t(y) * kOutW + x) * 4;
            for (int ch = 0; ch < 3; ++ch)
              m[ch] += p[ch];
            n++;
          }
        double err = 0.0;
        for (int ch = 0; ch < 3; ++ch)
          err = std::max(err, std::fabs(m[ch] / n - 255.0 * quad[q][ch]));
        worst_quadrant = std::max(worst_quadrant, err);
        if (err > c.tolerance) {
          iteration_wrong = true;
          if (first_wrong.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "iteration %d quadrant %d: got (%.1f %.1f %.1f) want (%.1f %.1f %.1f)", k, q,
                          m[0] / n, m[1] / n, m[2] / n, 255.0 * quad[q][0], 255.0 * quad[q][1], 255.0 * quad[q][2]);
            first_wrong = buf;
          }
        }
      }
      readback->unmap();
      wrong += iteration_wrong;
    }
    char buf[400];
    std::snprintf(buf, sizeof(buf),
                  "%s, %ux%u -> %ux%u, %d iterations with the pass open: %u wrong, %u all-zero, %u refused; worst quadrant error "
                  "%.2f/255, worst whole-image mean error vs a bilinear upscale's %.2f/255%s%s",
                  c.name, kInW, kInH, kOutW, kOutH, c.iterations, wrong, trivial, refused, worst_quadrant, worst_mean,
                  first_wrong.empty() ? "" : "; first: ", first_wrong.c_str());
    if (c.control)
      *report += std::string("  --   ") + buf + " (informational: a nonzero count shows the test sees a missing order)\n";
    else
      (wrong == 0 && trivial == 0 && refused == 0) ? pass(buf) : fail(buf);
  }

  {
    // The refusals: a texture of the wrong size, and a texture that is not hazard-tracked (a pooled one).
    plume::MetalSpatialScaler::Desc desc;
    desc.colorFormat = desc.outputFormat = plume::RenderFormat::R8G8B8A8_UNORM;
    desc.inputWidth = kInW;
    desc.inputHeight = kInH;
    desc.outputWidth = kOutW;
    desc.outputHeight = kOutH;
    std::string error;
    auto scaler = plume::MetalSpatialScaler::create(metal_device, desc, &error);
    if (!scaler) {
      fail("refusals: the scaler could not be created (" + error + ")");
      return false;
    }
    auto wrong_size = device->createTexture(plume::RenderTextureDesc::ColorTarget(kInW / 2, kInH / 2, desc.colorFormat));
    auto out = device->createTexture(plume::RenderTextureDesc::Texture2D(kOutW, kOutH, 1, desc.colorFormat, scaler->outputTextureFlags()));
    list->begin();
    const bool refused_size = !scaler->encode(static_cast<plume::MetalCommandList *>(list.get()), wrong_size.get(), out.get(), &error);
    list->end();
    refused_size ? pass("an input of the wrong size is refused and nothing is encoded (" + error + ")")
                 : fail("an input of the wrong size was accepted");
    std::string too_small;
    desc.outputWidth = kInW / 2;
    desc.outputHeight = kInH / 2;
    plume::MetalSpatialScaler::create(metal_device, desc, &too_small)
        ? fail("a scaler whose output is smaller than its input was created")
        : pass("a scaler whose output is smaller than its input is refused (" + too_small + ")");
  }
  return ok;
}

// The GPU busy arithmetic (src/gpu/gpu_busy.h): union of overlapping intervals, clipping to the
// window, what is carried over, and which buffer labels count.
bool RunBusyMathTests(std::string *report) {
  using eot::gpu::BusyInterval;
  bool ok = true;
  auto check = [&](const char *what, double got, double want) {
    const bool good = std::fabs(got - want) < 1e-9;
    ok = ok && good;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "  %s %s: %.6f (want %.6f)\n", good ? "ok  " : "FAIL", what, got, want);
    *report += buf;
  };
  {
    std::vector<BusyInterval> v = {{0.0, 1.0}, {2.0, 3.0}};
    check("disjoint intervals add", eot::gpu::UnionBusySeconds(v, 0.0, 10.0), 2.0);
  }
  {
    // Out of order, overlapping and nested: [0,4] covers [1,2]; [3,6] extends it; [5,6] is inside.
    std::vector<BusyInterval> v = {{3.0, 6.0}, {1.0, 2.0}, {0.0, 4.0}, {5.0, 6.0}};
    check("overlapping buffers count once", eot::gpu::UnionBusySeconds(v, 0.0, 10.0), 6.0);
  }
  {
    std::vector<BusyInterval> v = {{0.0, 3.0}, {5.0, 9.0}};
    check("clipped to the window", eot::gpu::UnionBusySeconds(v, 2.0, 6.0), 2.0);
  }
  {
    std::vector<BusyInterval> v = {{0.0, 1.0}, {8.0, 9.0}};
    check("outside the window adds nothing", eot::gpu::UnionBusySeconds(v, 2.0, 6.0), 0.0);
  }
  {
    std::vector<BusyInterval> v;
    check("no intervals", eot::gpu::UnionBusySeconds(v, 0.0, 1.0), 0.0);
  }
  {
    // One buffer straddling two windows is counted once overall: 1 s in the first, 2 s in the next.
    std::vector<BusyInterval> carry = {{4.0, 8.0}, {0.5, 1.5}};
    const double first = eot::gpu::UnionBusySeconds(carry, 0.0, 5.0);
    eot::gpu::DropIntervalsEndingBy(carry, 5.0);
    const double second = eot::gpu::UnionBusySeconds(carry, 5.0, 7.0);
    check("straddling buffer, first window ([0.5,1.5] and [4,5])", first, 2.0);
    check("straddling buffer, second window", second, 2.0);
    check("straddling buffer, kept for the next window (the finished one is dropped)", double(carry.size()), 1.0);
  }
  const bool labels = eot::gpu::IsGpuWorkLabel("RT64 Command List") && !eot::gpu::IsGpuWorkLabel("Wait Command Buffer") &&
                      !eot::gpu::IsGpuWorkLabel("GPU Done Marker") && !eot::gpu::IsGpuWorkLabel("Queue Idle Marker") &&
                      !eot::gpu::IsGpuWorkLabel("Acquire Drawable Command Buffer") && !eot::gpu::IsGpuWorkLabel("");
  ok = ok && labels;
  *report += std::string("  ") + (labels ? "ok  " : "FAIL") + " only the frame lists' label counts as GPU work\n";
  return ok;
}

}

int main(int argc, char **argv) {
  fs::path runtime_dir;
  size_t max_errors = 10;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--dump" && i + 1 < argc)
      g_dump_dir = argv[++i];
    else if (arg == "--errors" && i + 1 < argc)
      max_errors = std::strtoul(argv[++i], nullptr, 10);
    else
      runtime_dir = arg;
  }
  if (!g_dump_dir.empty())
    fs::create_directories(g_dump_dir);
  if (std::strcmp(plume::PLUME_METAL_SAFE_MATH_MARKER, eot::gpu::kMslSafeMathMarker) != 0) {
    std::printf("FAIL: the converter's safe-math marker differs from plume's\n");
    return 1;
  }

  MTL::Device *device = MTL::CreateSystemDefaultDevice();
  if (!device) {
    std::printf("FAIL: MTLCreateSystemDefaultDevice returned nil\n");
    return 1;
  }
  std::printf("Metal device: %s\n", device->name()->utf8String());

  Counts host, runtime, builtin;
  struct HostBlob {
    const char *name;
    const void *data;
    size_t size;
    uint32_t push_binding;
  };
#define HOST(n, b) {#n, g_##n##_spirv, sizeof(g_##n##_spirv), b}
  const HostBlob host_blobs[] = {
      HOST(copy_vs, 0), HOST(imgui_vs, 0), HOST(blit_ps, 0), HOST(copy_depth_ps, 0), HOST(imgui_ps, 1),
      HOST(taa_ps, 0), HOST(velocity_resolve_2x_ps, 0), HOST(velocity_resolve_4x_ps, 0),
      HOST(velocity_resolve_8x_ps, 0), HOST(resolve_msaa_color_2x_ps, 0), HOST(resolve_msaa_color_4x_ps, 0),
      HOST(resolve_msaa_color_8x_ps, 0), HOST(resolve_msaa_depth_2x_ps, 0), HOST(resolve_msaa_depth_4x_ps, 0),
      HOST(resolve_msaa_depth_8x_ps, 0), HOST(derive_depth_stencil_2x_ps, 0), HOST(derive_depth_stencil_4x_ps, 0),
      HOST(derive_depth_stencil_8x_ps, 0),
  };
#undef HOST
  for (const HostBlob &h : host_blobs)
    TestModule(device, host, std::string("host.") + h.name, h.data, h.size, h.push_binding);

  uint32_t runtime_files = 0;
  if (!runtime_dir.empty())
    TestRuntimeCache(device, runtime, runtime_dir, &runtime_files);
  TestBuiltinPack(device, builtin);

  auto line = [](const char *what, const Counts &c) {
    std::printf("%-34s %5u modules: %5u converted, %5u compiled, %5u bindings ok\n", what, c.total, c.converted,
                c.compiled, c.bindings_ok);
  };
  line("host shaders", host);
  char runtime_label[64];
  std::snprintf(runtime_label, sizeof(runtime_label), "runtime cache (%u .f2sc files)", runtime_files);
  line(runtime_label, runtime);
  line("built-in pack", builtin);
  if (g_shaderCacheEntryCount == 0)
    std::printf("  (the built-in pack is empty in this build: no captured shaders in shader_dump/)\n");
  const uint32_t total = host.total + runtime.total + builtin.total;
  const uint32_t compiled = host.compiled + runtime.compiled + builtin.compiled;
  const uint32_t bindings = host.bindings_ok + runtime.bindings_ok + builtin.bindings_ok;
  std::printf("TOTAL %u/%u compiled (%.1f%%), %u/%u bindings ok\n", compiled, total,
              total ? 100.0 * compiled / total : 0.0, bindings, total);
  std::map<std::string, uint32_t> by_stage;
  for (const Failure &f : g_failures)
    by_stage[f.stage]++;
  for (const auto &[stage, n] : by_stage)
    std::printf("  %u failures at %s\n", n, stage.c_str());
  for (size_t i = 0; i < g_failures.size() && i < max_errors; ++i)
    std::printf("--- %s (%s)\n%s\n", g_failures[i].name.c_str(), g_failures[i].stage.c_str(),
                g_failures[i].message.c_str());
  device->release();

  std::string report;
  std::printf("Pipelines through plume (main layout from main_layout.h):\n");
  const bool pipelines_ok = RunPipelineTests(&report);
  std::printf("%s", report.c_str());
  std::string busy_report;
  const bool busy_ok = RunBusyMathTests(&busy_report);
  std::printf("GPU busy arithmetic (gpu_busy.h):\n%s", busy_report.c_str());

  std::string metalfx_report;
  const bool metalfx_ok = RunMetalFxTests(&metalfx_report);
  std::printf("MetalFX spatial scaler through plume (patch 0022):\n%s", metalfx_report.c_str());

  const bool ok = g_failures.empty() && pipelines_ok && busy_ok && metalfx_ok && total > 0;
  std::printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
