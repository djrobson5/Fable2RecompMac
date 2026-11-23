// gpu/shaders/vs_identity.cpp
//
// See vs_identity.h. Two hooks feed the registry: sub_82BA40F0 (the bind path;
// in vs_rewrite_log.cpp, which calls VsIdentityBeforeBind / VsIdentityAfterBind
// around the original) and sub_821D3570, the vfetch patcher both rewrite
// paths call. sub_821D3570 keeps the declaration and strides it is given for
// whoever ends the rewrite (the bind hook, or its own device-path branch, which
// only matters when the patcher writes the object's own microcode: the
// device's usual call patches a copy in the command ring and changes no
// container).
//
// A container is taken for a pristine original only if its hash is a bank
// vertex shader's (IsBankVsHash); a rewritten form is remembered only with the
// declaration it came from; nothing is ever evicted.

#include "gpu/shaders/vs_identity.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"

REXCVAR_DEFINE_BOOL(fable2_vs_pristine, true, "Fable2",
                    "Native renderer: a vertex shader the game rewrote at bind time is translated and keyed as "
                    "its pristine bank original (declaration, strides and component order come as runtime data); "
                    "false translates every rewritten form of its own");

REX_EXTERN(__imp__sub_821D3570);

namespace eot::gpu {

namespace {

constexpr u64 kBankVsHashes[] = {
#include "gpu/shaders/bank_vs_hashes.inc"
};

constexpr u32 kMaxPosts = 32;
constexpr size_t kMaxObjects = 8192;

struct Post {
  u64 hash = 0;
  std::shared_ptr<const VsRecordedDecl> rec;
};

struct Entry {
  u64 pristine = 0;
  std::shared_ptr<const std::vector<u8>> bytes;
  u32 virtualSize = 0, physicalSize = 0;
  std::vector<Post> posts;
  bool saturated = false;

  const Post *FindPost(u64 hash) const {
    for (const Post &p : posts)
      if (p.hash == hash)
        return &p;
    return nullptr;
  }
};

std::mutex g_mutex;
std::unordered_map<u32, Entry> g_entries;
// One copy of each distinct pristine container.
std::unordered_map<u64, std::shared_ptr<const std::vector<u8>>> g_bytes;
thread_local int t_bind_depth = 0;
// The declaration the last sub_821D3570 call of this thread was given, for the rewrite that ends next.
thread_local std::shared_ptr<VsRecordedDecl> t_pending;

void NoteBefore(u32 object, const VsContainer &before) {
  if (!before.ok)
    return;
  std::lock_guard lock(g_mutex);
  auto it = g_entries.find(object);
  if (it != g_entries.end()) {
    Entry &e = it->second;
    if (e.pristine == before.hash || e.FindPost(before.hash))
      return;
  }
  if (!IsBankVsHash(before.hash)) {
    // Not a bank container and not a form remembered for this object (a rewrite from before the switch was
    // on, memory the game reused): nothing to normalise.
    if (it != g_entries.end())
      g_entries.erase(it);
    return;
  }
  if (it == g_entries.end() && g_entries.size() >= kMaxObjects)
    return;
  Entry &e = g_entries[object];
  e = Entry{};
  e.pristine = before.hash;
  e.virtualSize = before.virtualSize;
  e.physicalSize = before.physicalSize;
  auto &shared = g_bytes[before.hash];
  if (!shared)
    shared = std::make_shared<const std::vector<u8>>(before.bytes);
  e.bytes = shared;
}

void NoteAfter(u32 object, const VsContainer &after) {
  std::shared_ptr<VsRecordedDecl> rec = std::move(t_pending);
  t_pending.reset();
  if (!after.ok || !rec)
    return;
  std::lock_guard lock(g_mutex);
  auto it = g_entries.find(object);
  if (it == g_entries.end())
    return;
  Entry &e = it->second;
  if (after.hash == e.pristine || e.FindPost(after.hash))
    return;
  if (e.posts.size() >= kMaxPosts) {
    if (!e.saturated) {
      e.saturated = true;
      EOT_INFO("[vs-pristine] object {:#x}: more than {} rewritten forms; further ones are translated as they are",
               object, kMaxPosts);
    }
    return;
  }
  e.posts.push_back({after.hash, std::move(rec)});
}

void CapturePending(u32 decl, u32 strides) {
  const u32 count = decl ? mem::load<u32>(decl + obj::kDeclElementCount) : 0;
  const auto *elements = count && count <= 32 ? mem::at<DeclElement>(decl + obj::kDeclElements) : nullptr;
  const u8 *table = strides ? mem::at<u8>(strides) : nullptr;
  if (!elements) {
    t_pending.reset();
    return;
  }
  auto rec = std::make_shared<VsRecordedDecl>();
  rec->decl.assign(elements, elements + count);
  if (table)
    std::memcpy(rec->strides, table, sizeof(rec->strides));
  t_pending = std::move(rec);
}

}  // namespace

bool IsBankVsHash(u64 hash) {
  return std::binary_search(std::begin(kBankVsHashes), std::end(kBankVsHashes), hash);
}

VsContainer ReadVsContainer(u32 object_va, bool is_pixel) {
  VsContainer c;
  if (!object_va)
    return c;
  const u32 container_va = object_va + (is_pixel ? obj::kPixelShaderContainer : obj::kVertexShaderContainer);
  const u32 physical_va = mem::load<u32>(object_va + (is_pixel ? obj::kPixelShaderPhysical : obj::kVertexShaderPhysical));
  const auto *header = mem::at<ShaderContainerHeader>(container_va);
  if (!header)
    return c;
  const u32 vsize = header->virtualSize, psize = header->physicalSize;
  const u8 *v = mem::at<u8>(container_va);
  const u8 *p = physical_va && psize ? mem::at<u8>(physical_va) : nullptr;
  if (!v || vsize < sizeof(ShaderContainerHeader) || vsize > (1u << 20) || psize > (1u << 20) || (psize && !p))
    return c;
  c.flags = header->flags;
  c.virtualSize = vsize;
  c.physicalSize = psize;
  c.bytes.assign(v, v + vsize);
  if (psize)
    c.bytes.insert(c.bytes.end(), p, p + psize);
  c.hash = XXH3_64bits(c.bytes.data(), c.bytes.size());
  c.ok = true;
  return c;
}

bool VsHashKeyedPristine(u64 vs_hash) {
  return VsPristineEnabled() && IsBankVsHash(vs_hash);
}

bool VsPristineEnabled() {
  return REXCVAR_GET(fable2_vs_pristine);
}

VsRecord VsClassify(u32 object_va, u64 live_hash, VsPristine *out) {
  if (!VsPristineEnabled())
    return VsRecord::None;
  std::lock_guard lock(g_mutex);
  auto it = g_entries.find(object_va);
  if (it == g_entries.end())
    return VsRecord::None;
  const Entry &e = it->second;
  if (live_hash == e.pristine)
    return VsRecord::Pristine;
  const Post *post = e.FindPost(live_hash);
  if (!post)
    return VsRecord::Unknown;
  if (out) {
    out->hash = e.pristine;
    out->bytes = e.bytes;
    out->virtualSize = e.virtualSize;
    out->physicalSize = e.physicalSize;
    out->rec = post->rec;
  }
  return VsRecord::Rewritten;
}

void VsIdentityBeforeBind(u32 object_va, const VsContainer &before) {
  if (VsPristineEnabled())
    NoteBefore(object_va, before);
}

void VsIdentityAfterBind(u32 object_va, const VsContainer &after) {
  if (VsPristineEnabled())
    NoteAfter(object_va, after);
}

void VsIdentityEnterBind() { ++t_bind_depth; }
void VsIdentityLeaveBind() { --t_bind_depth; }
static bool VsIdentityInBind() { return t_bind_depth > 0; }

}  // namespace eot::gpu

// sub_821D3570(VS object, microcode to patch, declaration, per-stream stride table (bytes, dwords), variant)
extern "C" REX_FUNC(sub_821D3570) {
  using namespace eot;
  using namespace eot::gpu;
  if (!VsPristineEnabled()) {
    __imp__sub_821D3570(ctx, base);
    return;
  }
  const u32 object = ctx.r3.u32, code = ctx.r4.u32, decl = ctx.r5.u32, strides = ctx.r6.u32;
  if (VsIdentityInBind()) {
    // Inside the bind hook, which takes the pristine hash before and the post hash after.
    CapturePending(decl, strides);
    __imp__sub_821D3570(ctx, base);
    return;
  }
  // The device's own patch path. It usually patches a copy of the microcode in the command ring, which
  // changes no container: only a destination inside the object's own physical part needs the hashes (a
  // range test, so the common call costs nothing: 2 ns against 0.5 to 3.7 us for copying and hashing the
  // container twice).
  const u32 physical_va = mem::load<u32>(object + obj::kVertexShaderPhysical);
  const auto *header = mem::at<ShaderContainerHeader>(object + obj::kVertexShaderContainer);
  const u32 physical_size = header ? static_cast<u32>(header->physicalSize) : 0;
  const bool own = physical_va && physical_size && (code & 0x1FFFFFFFu) >= (physical_va & 0x1FFFFFFFu) &&
                   (code & 0x1FFFFFFFu) < (physical_va & 0x1FFFFFFFu) + physical_size;
  if (!own) {
    __imp__sub_821D3570(ctx, base);
    return;
  }
  CapturePending(decl, strides);
  const VsContainer before = ReadVsContainer(object, false);
  NoteBefore(object, before);
  __imp__sub_821D3570(ctx, base);
  const VsContainer after = ReadVsContainer(object, false);
  if (before.ok && after.ok && after.hash != before.hash) {
    NoteAfter(object, after);
    static u32 logged = 0;
    if (logged++ < 32)
      EOT_INFO("[vs-pristine] device patch: object {:#x} {:016x} -> {:016x}", object, before.hash, after.hash);
  }
  t_pending.reset();
}
