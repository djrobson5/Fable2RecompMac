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
//
// Usage: fable2_metal_selftest [shader_runtime dir] [--dump <dir>] [--errors N]

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <smolv.h>
#include <zstd.h>

#include <plume_metal.h>
#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

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
                                                    plume::RenderFormat color, plume::RenderFormat depth) {
  plume::RenderGraphicsPipelineDesc desc;
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

  const bool ok = g_failures.empty() && pipelines_ok && total > 0;
  std::printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
