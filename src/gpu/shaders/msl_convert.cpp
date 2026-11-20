// gpu/shaders/msl_convert.cpp
// Fable 2: SPIR-V to MSL for the native Metal backend (see msl_convert.h).

#include "gpu/shaders/msl_convert.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <sstream>
#include <vector>

#include <spirv_msl.hpp>
#include <spirv_parser.hpp>

namespace eot::gpu {

namespace {

constexpr uint32_t kOpEntryPoint = 15;
constexpr uint32_t kOpExecutionMode = 16;
constexpr uint32_t kOpDecorate = 71;
constexpr uint32_t kDecorationNoContraction = 42;
constexpr uint32_t kExecutionModeSignedZeroInfNanPreserve = 4461;

// Safe math when the SPIR-V asks for exact float behaviour: NoContraction
// decorations (XenosRecomp's precise math) or the float-controls mode the
// MoltenVK path turns them into (guest_shaders.cpp StrictMathForMetal).
bool NeedsSafeMath(const uint32_t *words, size_t count) {
  for (size_t i = 5; i < count;) {
    const uint32_t opcode = words[i] & 0xFFFFu;
    const uint32_t length = words[i] >> 16;
    if (length == 0 || i + length > count)
      break;
    if (opcode == kOpDecorate && length == 3 && words[i + 2] == kDecorationNoContraction)
      return true;
    if (opcode == kOpExecutionMode && length >= 3 && words[i + 2] == kExecutionModeSignedZeroInfNanPreserve)
      return true;
    i += length;
  }
  return false;
}

// The module without its NoContraction decorations. SPIRV-Cross turns each
// decorated op into a call to an [[clang::optnone]] helper (spvFMul, spvFAdd,
// ...) that the Metal compiler can neither inline nor schedule: the vertex
// phase took ~5x the MoltenVK path's time (market: 13.6 vs 2.8 ms a frame).
// The library is compiled with safe math instead (NeedsSafeMath), as MoltenVK
// does for the same shaders after StrictMathForMetal (guest_shaders.cpp).
std::vector<uint32_t> WithoutNoContraction(std::vector<uint32_t> words) {
  std::vector<uint32_t> out(words.begin(), words.begin() + std::min<size_t>(5, words.size()));
  out.reserve(words.size());
  for (size_t i = 5; i < words.size();) {
    const uint32_t opcode = words[i] & 0xFFFFu;
    const uint32_t length = words[i] >> 16;
    if (length == 0 || i + length > words.size())
      return words;
    if (!(opcode == kOpDecorate && length == 3 && words[i + 2] == kDecorationNoContraction))
      out.insert(out.end(), words.begin() + i, words.begin() + i + length);
    i += length;
  }
  return out;
}

spv::ExecutionModel EntryModel(const uint32_t *words, size_t count) {
  for (size_t i = 5; i < count;) {
    const uint32_t opcode = words[i] & 0xFFFFu;
    const uint32_t length = words[i] >> 16;
    if (length == 0 || i + length > count)
      break;
    if (opcode == kOpEntryPoint && length >= 2)
      return static_cast<spv::ExecutionModel>(words[i + 1]);
    i += length;
  }
  return spv::ExecutionModelMax;
}

// The [[id(N)]] of an argument buffer member line, or -1.
long MemberId(const std::string &line) {
  const size_t at = line.find("[[id(");
  if (at == std::string::npos)
    return -1;
  return std::strtol(line.c_str() + at + 5, nullptr, 10);
}

// How many ids an argument buffer member takes: N for array<T, N> or name[N].
long MemberSpan(const std::string &line) {
  const size_t id_at = line.find("[[id(");
  const std::string decl = line.substr(0, id_at);
  if (const size_t a = decl.find("array<"); a != std::string::npos) {
    // array<T, N>: the last ", N>" before the name.
    const size_t close = decl.rfind('>');
    const size_t comma = decl.rfind(',', close);
    if (close != std::string::npos && comma != std::string::npos && comma > a)
      return std::max(1L, std::strtol(decl.c_str() + comma + 1, nullptr, 10));
  }
  if (const size_t b = decl.rfind('['); b != std::string::npos)
    return std::max(1L, std::strtol(decl.c_str() + b + 1, nullptr, 10));
  return 1;
}

// Tier 2 argument buffers are plain structs: a member's byte offset is the
// sum of the sizes before it, not its [[id]]. plume writes entry `binding` at
// byte binding * 8, so every id gap gets pointer-sized padding (as plume's own
// SPIRV-Cross tool does). Members are 8 bytes each (textures, samplers,
// buffer addresses), arrays 8 bytes per element.
std::string PadArgumentBuffers(const std::string &source, std::string *error) {
  std::istringstream in(source);
  std::ostringstream out;
  std::string line;
  bool inside = false;
  long next = 0;
  while (std::getline(in, line)) {
    if (!inside && line.rfind("struct spvDescriptorSetBuffer", 0) == 0) {
      inside = true;
      next = 0;
      out << line << '\n';
      if (std::getline(in, line))  // "{"
        out << line << '\n';
      continue;
    }
    if (inside) {
      if (line == "};") {
        inside = false;
        out << line << '\n';
        continue;
      }
      const long id = MemberId(line);
      if (id < 0) {
        *error = "argument buffer member without [[id]]: " + line;
        return {};
      }
      if (id < next) {
        *error = "argument buffer members out of id order: " + line;
        return {};
      }
      for (; next < id; ++next)
        out << "    constant uint64_t* spvPad" << next << " [[id(" << next << ")]];\n";
      next = id + MemberSpan(line);
    }
    out << line << '\n';
  }
  return out.str();
}

}

MslShader SpirvToMsl(const void *spirv, size_t size, const MslOptions &options) {
  MslShader result;
  if (!spirv || size < 20 || size % 4) {
    result.error = "not a SPIR-V module";
    return result;
  }
  std::vector<uint32_t> words(size / 4);
  std::memcpy(words.data(), spirv, size);
  const spv::ExecutionModel model = EntryModel(words.data(), words.size());
  if (model != spv::ExecutionModelVertex && model != spv::ExecutionModelFragment &&
      model != spv::ExecutionModelGLCompute) {
    result.error = "no vertex, fragment or compute entry point";
    return result;
  }
  result.safeMath = NeedsSafeMath(words.data(), words.size());
  if (result.safeMath)
    words = WithoutNoContraction(std::move(words));

  try {
    spirv_cross::Parser parser(std::move(words));
    parser.parse();
    spirv_cross::CompilerMSL msl(std::move(parser.get_parsed_ir()));

    spirv_cross::CompilerMSL::Options msl_options;
    msl_options.platform = spirv_cross::CompilerMSL::Options::macOS;
    msl_options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(3, 0);
    msl_options.argument_buffers = true;
    msl_options.argument_buffers_tier = spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
    msl_options.texture_buffer_native = true;
    msl_options.enable_decoration_binding = true;
    msl.set_msl_options(msl_options);
    // The bindless heaps are runtime-sized arrays, which SPIRV-Cross allows
    // only in device-space argument buffers. plume binds sets 0-7.
    for (uint32_t set = 0; set < 8; ++set)
      msl.set_argument_buffer_device_address_space(set, true);

    spirv_cross::CompilerGLSL::Options common = msl.get_common_options();
    common.vertex.flip_vert_y = model == spv::ExecutionModelVertex;
    msl.set_common_options(common);

    spirv_cross::MSLResourceBinding push;
    push.stage = model;
    push.desc_set = spirv_cross::kPushConstDescSet;
    push.binding = spirv_cross::kPushConstBinding;
    push.msl_buffer = kMslPushConstantBufferBase + options.pushConstantBinding;
    msl.add_msl_resource_binding(push);

    const std::string source = msl.compile();
    const auto entries = msl.get_entry_points_and_stages();
    if (entries.empty()) {
      result.error = "SPIRV-Cross found no entry point";
      return result;
    }
    result.entry = msl.get_cleansed_entry_point_name(entries.front().name, entries.front().execution_model);
    std::string padded = PadArgumentBuffers(source, &result.error);
    if (!result.error.empty())
      return result;
    result.source = result.safeMath ? std::string(kMslSafeMathMarker) + "\n" + padded : std::move(padded);
  } catch (const std::exception &e) {
    result.error = std::string("SPIRV-Cross: ") + e.what();
    result.source.clear();
  }
  return result;
}

}
