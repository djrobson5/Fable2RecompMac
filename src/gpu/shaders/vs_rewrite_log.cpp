// gpu/shaders/vs_rewrite_log.cpp
//
// Fable 2 diagnostic (fable2_debug_vs_rewrite_log, off by default): the game's
// bind-time vertex shader rewrite, sub_82BA40F0(VS object, 0, vertex
// declaration, stream strides, PS object or 0), edits the VS object's
// container and microcode in place (vfetch instructions from the declaration,
// export instructions and interpolant table from the pixel shader). This hook
// hashes the container before and after the original runs (XXH3-64 of virtual
// ++ physical, as RegisterGuestShader does) and logs one line per call. With
// fable2_shader_dump_dir also set it writes the before and after containers as
// vs_pre_<hash>.bin / vs_post_<hash>.bin and a vs_rw_<post>_<object>.json with
// the declaration, the strides and the hashes. Nothing here changes the call.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"

REXCVAR_DEFINE_BOOL(fable2_debug_vs_rewrite_log, false, "Fable2",
                    "Debug: log every bind-time vertex shader rewrite (object, hash before and after, pixel shader, "
                    "declaration); with fable2_shader_dump_dir also write the containers and a .json per rewrite");
REXCVAR_DECLARE(std::string, fable2_shader_dump_dir);

REX_EXTERN(__imp__sub_82BA40F0);

namespace {

using namespace eot;
using namespace eot::gpu;

struct Container {
  std::vector<u8> bytes;  // virtual ++ physical
  u64 hash = 0;
  bool ok = false;
  u32 flags = 0;
};

Container ReadContainer(u32 object, bool is_pixel) {
  Container c;
  if (!object)
    return c;
  const u32 container_va = object + (is_pixel ? obj::kPixelShaderContainer : obj::kVertexShaderContainer);
  const u32 physical_va = mem::load<u32>(object + (is_pixel ? obj::kPixelShaderPhysical : obj::kVertexShaderPhysical));
  const auto *header = mem::at<ShaderContainerHeader>(container_va);
  if (!header)
    return c;
  const u32 vsize = header->virtualSize, psize = header->physicalSize;
  const u8 *v = mem::at<u8>(container_va);
  const u8 *p = physical_va && psize ? mem::at<u8>(physical_va) : nullptr;
  if (!v || vsize < sizeof(ShaderContainerHeader) || vsize > (1u << 20) || psize > (1u << 20) || (psize && !p))
    return c;
  c.flags = header->flags;
  c.bytes.assign(v, v + vsize);
  if (psize)
    c.bytes.insert(c.bytes.end(), p, p + psize);
  c.hash = XXH3_64bits(c.bytes.data(), c.bytes.size());
  c.ok = true;
  return c;
}

std::mutex g_mutex;
std::unordered_map<u32, u64> g_pristine;  // VS object -> hash of its container before its first rewrite
std::unordered_set<u64> g_written_pre, g_written_post;  // dumped container hashes
std::unordered_set<std::string> g_logged;

void WriteBin(const std::filesystem::path &dir, const char *kind, const Container &c) {
  if (!(kind[1] == 'r' ? g_written_pre : g_written_post).insert(c.hash).second)
    return;
  std::ofstream out(dir / std::format("vs_{}_{:016x}.bin", kind, c.hash), std::ios::binary);
  out.write(reinterpret_cast<const char *>(c.bytes.data()), std::streamsize(c.bytes.size()));
}

}  // namespace

extern "C" REX_FUNC(sub_82BA40F0) {
  if (!REXCVAR_GET(fable2_debug_vs_rewrite_log)) {
    __imp__sub_82BA40F0(ctx, base);
    return;
  }
  const u32 object = ctx.r3.u32, decl = ctx.r5.u32, strides = ctx.r6.u32, ps = ctx.r7.u32;
  Container before = ReadContainer(object, false);
  const bool first = before.ok && !(before.flags & 0x40);
  if (first) {
    std::lock_guard lock(g_mutex);
    g_pristine[object] = before.hash;
  }
  // The original reads the declaration and strides; copy them first.
  struct Elem {
    u32 stream, offset, type, usage, usage_index;
  };
  std::vector<Elem> elems;
  std::vector<u32> stride_list;
  if (decl) {
    const u32 count = mem::load<u32>(decl + obj::kDeclElementCount);
    const u32 max_stream = mem::load<u32>(decl + obj::kDeclMaxStream);
    if (const auto *e = count && count <= 32 ? mem::at<DeclElement>(decl + obj::kDeclElements) : nullptr)
      for (u32 i = 0; i < count; ++i)
        elems.push_back({e[i].stream, e[i].offset, e[i].type, e[i].usage, e[i].usageIndex});
    if (strides)
      for (u32 i = 0; i <= max_stream && i < 16; ++i)
        stride_list.push_back(mem::load<u32>(strides + i * 4));
  }
  const Container ps_c = ReadContainer(ps, true);

  __imp__sub_82BA40F0(ctx, base);

  const Container after = ReadContainer(object, false);
  u64 pristine = 0;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_pristine.find(object);
    pristine = it != g_pristine.end() ? it->second : 0;
  }
  const std::string ps_text = ps_c.ok ? std::format("{:016x}", ps_c.hash) : "none";
  EOT_INFO("[vs-rewrite] obj {:#x} pre {:016x} post {:016x} first {} pristine {:016x} ps {} decl {:#x} strides {}",
           object, before.hash, after.hash, first ? 1 : 0, pristine, ps_text, decl,
           [&] {
             std::string s;
             for (u32 v : stride_list)
               s += std::format("{}{}", s.empty() ? "" : ",", v);
             return s.empty() ? std::string("-") : s;
           }());

  const std::string &dir_text = REXCVAR_GET(fable2_shader_dump_dir);
  if (dir_text.empty() || !before.ok || !after.ok)
    return;
  const std::filesystem::path dir(dir_text);
  std::lock_guard lock(g_mutex);
  const std::string key = std::format("{:x}_{:016x}_{:016x}_{}", object, before.hash, after.hash, ps_text);
  if (!g_logged.insert(key).second)
    return;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  WriteBin(dir, "pre", before);
  WriteBin(dir, "post", after);
  std::string json = std::format(
      "{{\"object\":\"{:#x}\",\"pre\":\"{:016x}\",\"post\":\"{:016x}\",\"first\":{},\"pristine\":\"{:016x}\","
      "\"ps\":\"{}\",\"decl\":\"{:#x}\",\"strides\":[",
      object, before.hash, after.hash, first ? "true" : "false", pristine, ps_text, decl);
  for (size_t i = 0; i < stride_list.size(); ++i)
    json += std::format("{}{}", i ? "," : "", stride_list[i]);
  json += "],\"decl_elements\":[";
  for (size_t i = 0; i < elems.size(); ++i)
    json += std::format("{}{{\"stream\":{},\"offset\":{},\"type\":{},\"usage\":{},\"usage_index\":{}}}", i ? "," : "",
                        elems[i].stream, elems[i].offset, elems[i].type, elems[i].usage, elems[i].usage_index);
  json += "]}\n";
  std::ofstream(dir / std::format("vs_rw_{:016x}_{:x}.json", after.hash, object)) << json;
}
