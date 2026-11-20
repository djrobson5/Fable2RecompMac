// gpu/shaders/msl_convert.h
// Fable 2: SPIR-V to MSL for the native Metal backend (fable2_gpu_backend=metal).
// One converter for the host shaders (built in as SPIR-V) and the guest shaders
// (the shader cache's SPIR-V), set up for plume's Metal backend:
//  - descriptor set N is an argument buffer at [[buffer(N)]] (plume binds set N
//    there), each binding at [[id(binding)]], with pointer-sized padding so entry
//    `binding` sits at byte binding * 8 (Tier 2 argument buffers, as plume writes
//    them);
//  - the push constant block is at [[buffer(8 + range binding)]], members at their
//    Vulkan offsets (plume patch 0010);
//  - vertex shaders flip Y (the SPIR-V uses Vulkan's clip space);
//  - specialization constant N is function constant N.
// No dependency on the game runtime: the offline self-test links it too.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace eot::gpu {

// Matches plume::PLUME_METAL_SAFE_MATH_MARKER (plume patch 0008): sources that carry
// it compile without fast math (the self-test checks the two agree).
inline constexpr const char *kMslSafeMathMarker = "// plume-metal: safe-math";

// plume_metal.cpp PUSH_CONSTANTS_BINDING_INDEX.
inline constexpr uint32_t kMslPushConstantBufferBase = 8;

struct MslOptions {
  // The push constant range's binding in the pipeline layout (its buffer is
  // kMslPushConstantBufferBase + this).
  uint32_t pushConstantBinding = 0;
};

struct MslShader {
  std::string source;  // MSL text for plume::RenderShaderFormat::METAL
  std::string entry;   // the MSL function name (SPIRV-Cross renames main to main0)
  std::string error;   // empty on success
  bool safeMath = false;
  bool ok() const { return error.empty() && !source.empty(); }
};

MslShader SpirvToMsl(const void *spirv, size_t size, const MslOptions &options = {});

}
