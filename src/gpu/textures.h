// gpu/textures.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <rex/types.h>

#include <plume_render_interface.h>

#include "gpu/resources.h"

namespace eot::gpu {

struct VideoState;

GuestTexture *GetGuestTexture(VideoState &s, u32 header_va, bool create_host_image = true);
// Debug: the header's memory uploaded from guest memory now (no resolve image).
GuestTexture *GuestMemoryCopy(VideoState &s, u32 header_va);
// The same, with the fetch constant given (textures no header points at, e.g.
// Fable 2's vertex textures: header_va is then a synthetic key).
GuestTexture *GetGuestTextureForFetch(VideoState &s, u32 header_va, const u32 fetch[6],
                                      bool create_host_image = true);
// A texture header's fetch constant as the guest holds it now (false if unset).
// Draws and resolves record their textures' headers with this: Fable 2 re-points
// headers at other render targets later in the frame, before the replay.
bool ReadTextureHeader(u32 header_va, u32 out[6]);
u32 StorageFormat(u32 format);
// A fetch constant's base as a physical address: headers hold the 0xE0000000
// alias (the physical page 0x1000 above), the device's copy the physical one.
inline u32 PhysicalBase(u32 word1) {
  const u32 base = word1 & 0xFFFFF000u;
  return (base >= 0xE0000000u ? base - 0xE0000000u + 0x1000u : base) & 0x1FFFF000u;
}
// Fable 2 re-points the header objects of its render targets at other targets
// right after binding them (the shadow map's at a bloom buffer), while the GPU
// samples what the device's fetch constant names. Resolve destinations are kept
// under a key made from their memory (physical base, size, format), which a
// header and the device's copy of its fetch constant both map to; draws find
// them by their fetch constant (null: not a render target, or switched off).
u32 RenderTargetKey(const u32 fetch[6]);
GuestTexture *FindRenderTarget(VideoState &s, const u32 fetch[6]);

// Storage index over VideoState::textures (physical base | storage format of
// fetch[1], and fetch[2]): a texture is listed while at least one table entry
// holds it. Every change to the table goes through these, and every write to a
// listed texture's fetch[1] or fetch[2] is followed by TextureIndexRekey.
// TextureIndexRemove is called before the entry's shared_ptr is dropped.
void TextureIndexAdd(VideoState &s, GuestTexture *t);
void TextureIndexRemove(VideoState &s, GuestTexture *t);
void TextureIndexRekey(VideoState &s, GuestTexture *t);
// The vertex-texture pick: among the textures with this fetch constant's
// storage that have a host image and are not depth, the one sampled most
// recently (the first of equals). The old scan over the whole table picked the
// first equal in unordered_map order, which the index cannot reproduce; *ties
// (if given) is the number of distinct candidates sharing the best frame.
GuestTexture *FindTextureByStorage(VideoState &s, const u32 fc[6], u32 *ties = nullptr);

u32 PrepareTextureForSampling(VideoState &s, GuestTexture &t,
                              u32 swizzle = kIdentityFetchSwizzle);
u32 SamplingSwizzle(const GuestTexture &t, u32 fetch_swizzle);

bool EnsureResolveMirror(VideoState &s, GuestTexture &t, bool depth_source, float scale = 0.0f,
                         plume::RenderFormat depth_format = plume::RenderFormat::UNKNOWN);

void NoteResolveDestination(VideoState &s, const GuestTexture &t);

void PreloadAnnouncedTextures(VideoState &s, f64 budget_ms);

plume::RenderFramebuffer *GetMipFramebuffer(VideoState &s, GuestTexture &t, u32 mip);

void NotifyResourceUnlocked(u32 resource_va);
void NotifyResourceUnlockedLocked(VideoState &s, u32 resource_va);
u64 ResourceUnlockSeq(u32 resource_va);
u64 ResourceUnlockGlobal();
// fable2_diag_late_reads: the replayed draw's unlock sequence at capture (~0 outside draws).
inline u64 g_replay_unlock_global = ~0ull;
inline std::atomic<u64> g_late_texture_uploads{0}, g_replay_texture_uploads{0}, g_late_endian{0};

}

namespace eot::gpu {

// The fetch constant's LOD bias as bits to OR into a pixel sampler index.
u32 SamplerLodBiasBits(const u32 fc[6], u32 sampler_index);
// Adds bias_32nds (1/32 steps) to the LOD bias packed in a sampler index.
u32 SamplerAddLodBias(u32 sampler_index, i32 bias_32nds);
plume::RenderSamplerDesc DecodeSamplerFromFetch(const u32 fc[6], bool mipmapped_upload = true,
                                                bool synthesized_mips = false);

u32 ResolveSamplerSlotLocked(const plume::RenderSamplerDesc &desc);

}
