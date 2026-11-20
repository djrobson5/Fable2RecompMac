// gpu/main_layout.h
// Fable 2: the renderer's main pipeline layout (guest shaders and the host
// copy/blit/resolve passes), described once for device.cpp and the offline
// Metal self-test (tools/metal_selftest), so the test checks the real layout.
// Plain plume builders and constants only: no game runtime.

#pragma once

#include <cstdint>

#include <plume_render_interface_builders.h>

namespace eot::gpu {

inline constexpr uint32_t kBindlessTextureCount = 32768;
inline constexpr uint32_t kBindlessSamplerCount = 512;

// Vulkan/Metal push constants: the guest shaders' three constant-block
// addresses (24 bytes), then the host passes' CopyPushConstants (64 bytes) at
// kCopyPushConstantByteOffset (device.cpp checks both).
inline constexpr uint32_t kMainLayoutPushConstantBytes = 24 + 64;

// The bindless texture heap: one set, bound at indices 0, 1, 2 and 4 (2D, 3D,
// cube and 1D views of the same descriptors).
inline void DescribeTextureHeapSet(plume::RenderDescriptorSetBuilder &b) {
  b.begin();
  b.addTexture(0, kBindlessTextureCount);
  b.end(true, kBindlessTextureCount);
}

inline void DescribeSamplerHeapSet(plume::RenderDescriptorSetBuilder &b) {
  b.begin();
  b.addSampler(0, kBindlessSamplerCount);
  b.end(true, kBindlessSamplerCount);
}

// Sets 0-2 texture heap, 3 samplers, 4 texture heap (shader_common.h spaces).
inline void DescribeMainLayoutSets(plume::RenderPipelineLayoutBuilder &layout,
                                   const plume::RenderDescriptorSetBuilder &textures,
                                   const plume::RenderDescriptorSetBuilder &samplers) {
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(textures);
  layout.addDescriptorSet(samplers);
  layout.addDescriptorSet(textures);
}

// Vulkan and Metal: one push constant range for both stages, and with
// constant_buffers (FABLE2_CONSTANT_BUFFERS) the guest constant blocks as root
// descriptors 0..2 in set 5.
inline void DescribeMainLayoutPushConstants(plume::RenderPipelineLayoutBuilder &layout,
                                            bool constant_buffers) {
  layout.addPushConstant(0, 4, kMainLayoutPushConstantBytes,
                         plume::RenderShaderStageFlag::VERTEX | plume::RenderShaderStageFlag::PIXEL);
  if (constant_buffers)
    for (uint32_t r = 0; r < 3; ++r)
      layout.addRootDescriptor(r, 5, plume::RenderRootDescriptorType::CONSTANT_BUFFER);
}

}
