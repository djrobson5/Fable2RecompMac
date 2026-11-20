// gpu/backend.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

#include <memory>

#include <plume_render_interface.h>

namespace eot::gpu {

#if defined(EOT_D3D12)
inline constexpr bool g_vulkan = false;
inline constexpr plume::RenderShaderFormat kHostShaderFormat =
    plume::RenderShaderFormat::DXIL;
#define EOT_BLOB_SYMBOL(name) g_##name##_dxil
#else
inline constexpr bool g_vulkan = true;
inline constexpr plume::RenderShaderFormat kHostShaderFormat =
    plume::RenderShaderFormat::SPIRV;
#define EOT_BLOB_SYMBOL(name) g_##name##_spirv
#endif

#if defined(EOT_MVK)
inline constexpr bool g_mvk = true;
static_assert(g_vulkan);
#else
inline constexpr bool g_mvk = false;
#endif

#define EOT_SHADER_BLOB(name) EOT_BLOB_SYMBOL(name), sizeof(EOT_BLOB_SYMBOL(name))

#if !defined(EOT_D3D12)
// Fable 2: true when fable2_gpu_backend=metal chose plume's Metal backend
// (macOS builds with FABLE2_METAL_BACKEND); fixed for the run. g_vulkan and
// g_mvk keep meaning "built for Vulkan" and "built for Apple GPUs": the
// g_mvk paths that work around Apple GPU limits apply to both backends.
bool UseMetal();
#else
inline bool UseMetal() { return false; }
#endif

// A built-in host shader (EOT_SHADER_BLOB): as built for Vulkan/D3D12, or
// converted to MSL on Metal. push_binding: the shader's push constant range
// binding in its pipeline layout (Metal binds range b at buffer 8 + b).
std::unique_ptr<plume::RenderShader> CreateHostShader(plume::RenderDevice *device, const void *blob, u64 size,
                                                      u32 push_binding = 0);

#if defined(EOT_D3D12)
inline constexpr u32 kCopyPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantByteOffset = 0;
inline constexpr u32 kSpecPushConstantRangeIndex = 1;
#else
inline constexpr u32 kGuestPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantRangeIndex = 0;
inline constexpr u32 kCopyPushConstantByteOffset = 24;
#endif

}
