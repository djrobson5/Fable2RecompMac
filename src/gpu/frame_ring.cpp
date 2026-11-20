// gpu/frame_ring.cpp
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#include <plume_render_interface.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <format>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#endif

#if defined(EOT_D3D12)
#include <plume_d3d12.h>
#else
#include <plume_vulkan.h>
#endif
#if defined(FABLE2_METAL_BACKEND)
#include <dispatch/dispatch.h>
#include <plume_metal.h>
#endif
#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(fable2_chunk_submit_draws, 600, "Fable2",
                     "Native renderer: submit the frame so far at the first resolve after this many draws "
                     "(up to 4 times a frame; 0 = one submit at Present)");
REXCVAR_DEFINE_BOOL(fable2_destroy_when_queue_idle, true, "Fable2",
                    "Native renderer: destroy retired textures and buffers only while MoltenVK's "
                    "submit thread is idle (async submits lost the device otherwise)");

#include "core/logging.h"
#include "gpu/queries.h"
#include "gpu/writeback.h"
#include "gpu/render_thread.h"
#include "gpu/backend.h"
#include "gpu/device.h"
#include "gpu/draw.h"
#include "gpu/gpu_timing.h"
#include "gpu/memory_report.h"
#include "gpu/format.h"

namespace eot::gpu {

void DestroyHostTexture(VideoState &s, HostTexture &host);

namespace {

bool SameTextureShape(const HostTexture &pooled, const HostTexture &want,
                      const plume::RenderTextureDesc &d) {
  const plume::RenderTextureDesc &p = pooled.desc;
  return p.dimension == d.dimension && p.width == d.width && p.height == d.height &&
         p.depth == d.depth && p.mipLevels == d.mipLevels && p.arraySize == d.arraySize &&
         p.format == d.format && p.flags == d.flags &&
         p.multisampling.sampleCount == d.multisampling.sampleCount &&
         pooled.viewDimension == want.viewDimension &&
         pooled.isDepth == want.isDepth;
}

constexpr f64 kHostTexturePoolSeconds = 10.0;
constexpr size_t kHostTexturePoolMax = 192;
constexpr u64 kHostTexturePoolMaxBytes = 256ull << 20;

void TrimHostTexturePool(VideoState &s) {
  while (!s.host_texture_pool.empty() && s.host_texture_pool_bytes > kHostTexturePoolMaxBytes) {
    auto &oldest = s.host_texture_pool.front();
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, oldest.bytes);
    DestroyHostTexture(s, oldest.host);
    s.host_texture_pool.erase(s.host_texture_pool.begin());
  }
}

}

namespace {
void BindListStateLocked(VideoState &s) {
  s.command_list->setGraphicsPipelineLayout(s.pipeline_layout.get());
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 0);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 1);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 2);
  s.command_list->setGraphicsDescriptorSet(s.sampler_descriptor_set.get(), 3);
  s.command_list->setGraphicsDescriptorSet(s.texture_descriptor_set.get(), 4);
}
}

void BeginCommandList(VideoState &s) {
  if (s.command_list_open)
    return;
  if (s.shutting_down.load(std::memory_order_acquire) || !s.pipeline_layout)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  s.command_list = s.command_lists[cur].get();
  s.command_list->begin();
  QueriesCommandListBegunLocked(s);
  if (!s.null_texture_barriers_submitted) {
    // The null textures read (0, 0, 0, 1) through a view swizzle, which
    // MoltenVK ignores without full image view swizzle support; their texels
    // were never written, so shaders sampled uninitialised memory (different
    // every run). Write (0, 0, 0, 1) into every texel.
    plume::RenderTextureBarrier barriers[kNullTextureDescriptorCount];
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i)
      barriers[i] = plume::RenderTextureBarrier(s.null_textures[i].get(),
                                                plume::RenderTextureLayout::COPY_DEST);
    s.command_list->barriers(plume::RenderBarrierStage::COPY, barriers, kNullTextureDescriptorCount);
    UploadAlloc texel;
    if (UploadAllocate(kTextureRowPitchAlignment, kTexturePlacementAlignment, &texel)) {
      for (u64 b = 0; b < kTextureRowPitchAlignment; b += 4) {
        texel.cpu[b] = texel.cpu[b + 1] = texel.cpu[b + 2] = 0;
        texel.cpu[b + 3] = 0xFF;
      }
      for (u32 i = 0; i < kNullTextureDescriptorCount; ++i) {
        const u32 layers = i == kNullTextureCubeDescriptorIndex ? 6u : 1u;
        for (u32 layer = 0; layer < layers; ++layer)
          s.command_list->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(s.null_textures[i].get(), 0, layer),
              plume::RenderTextureCopyLocation::PlacedFootprint(
                  texel.buffer, plume::RenderFormat::R8G8B8A8_UNORM, 1, 1, 1,
                  static_cast<u32>(kTextureRowPitchAlignment / 4), texel.offset));
      }
    }
    for (u32 i = 0; i < kNullTextureDescriptorCount; ++i)
      barriers[i] = plume::RenderTextureBarrier(s.null_textures[i].get(),
                                                plume::RenderTextureLayout::SHADER_READ);
    s.command_list->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, kNullTextureDescriptorCount);
    s.null_texture_barriers_submitted = true;
  }
  BindListStateLocked(s);
  s.command_list_open = true;
  s.list_draws = 0;
  GpuTimingFrameBegin(s, s.command_list, cur);
#if EOT_GPU_PROFILING
  SetGPUProfilerCommandList(static_cast<plume::D3D12CommandList *>(s.command_list)->d3d);
#endif
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
}

void SubmitOpenListLocked(VideoState &s) {
  if (!s.command_list_open)
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  QueriesBeforeSubmitLocked(s);
  FlushGeometryStaging(s);
  GpuTimingFrameEnd(s.command_list);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
  FlushRetiredLocked(s);
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, s.fences[cur].get());
  s.command_list_submitted[cur] = true;
  NoteQueueSubmittedLocked(s);
}

// Chunked submits: the frame so far goes to the GPU (no fence: the frame's
// last submit signals the slot fence, and the queue runs in order) and the
// frame continues in a fresh command list. MoltenVK encodes and the GPU runs
// this part while the rest of the frame is still being recorded; with one
// submit per frame the GPU started only at Present and the next Present
// waited ~2 ms for it (the game thread waits in Present).
void MaybeSubmitChunkLocked(VideoState &s) {
  const i32 every = REXCVAR_GET(fable2_chunk_submit_draws);
  if (every <= 0 || !s.command_list_open || s.list_draws < static_cast<u32>(every) || s.frame_chunks >= 4 ||
      s.depth_prerun || QueryActiveLocked())
    return;
  const u32 cur = s.frame.load(std::memory_order_relaxed);
  FlushGeometryStaging(s);
  s.command_lists[cur]->end();
  s.command_list_open = false;
  s.bound_framebuffer = nullptr;
  s.bound_pipeline = nullptr;
  s.bound_draw_targets_valid = false;
  for (auto &stream : s.bound_vertex_streams)
    stream.valid = false;
  s.bound_index_stream.valid = false;
  for (u32 i = 0; i < 3; ++i) {
    s.bound_root_buffer[i] = nullptr;
    s.bound_root_offset[i] = 0;
  }
  s.bound_spec = ~0u;
  const plume::RenderCommandList *lists[] = {s.command_lists[cur].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, nullptr);
  NoteQueueSubmittedLocked(s);
  s.chunk_inflight[cur].push_back(std::move(s.command_lists[cur]));
  if (!s.chunk_spare[cur].empty()) {
    s.command_lists[cur] = std::move(s.chunk_spare[cur].back());
    s.chunk_spare[cur].pop_back();
  } else {
    s.command_lists[cur] = s.queue->createCommandList();
  }
  s.command_list = s.command_lists[cur].get();
  s.command_list->begin();
  QueriesChunkListBegunLocked(s);
  BindListStateLocked(s);
  s.command_list_open = true;
  s.list_draws = 0;
  s.frame_chunks++;
  s.perf.chunk_submits++;
}

namespace {

// Metal: names a list that only carries a fence (it records nothing), so the GPU busy figure
// (gpu_timing.cpp) leaves its command buffer out; a name is a string literal.
void NameFenceList(plume::RenderCommandList *list, const char *name) {
#if defined(FABLE2_METAL_BACKEND)
  if (UseMetal() && list)
    static_cast<plume::MetalCommandList *>(list)->debugLabel = name;
#else
  (void)list;
  (void)name;
#endif
}

VkFence TailFence(VideoState &s) {
#if defined(EOT_D3D12)
  return nullptr;
#else
  if (!s.tail_fence)
    s.tail_fence = s.device->createCommandFence();
  return s.tail_fence ? static_cast<plume::VulkanCommandFence *>(s.tail_fence.get())->vk : VK_NULL_HANDLE;
#endif
}

// Reclaims the marker fence once it has signalled; true if the queue is idle.
bool QueueIdleLocked(VideoState &s, bool wait) {
  if (s.queue_idle)
    return true;
#if defined(EOT_D3D12)
  (void)wait;
  return true;
#else
  if (!s.tail_fence_inflight)
    return false;
#if defined(FABLE2_METAL_BACKEND)
  if (UseMetal()) {
    // plume's Metal fence is a dispatch semaphore the marker's completion
    // handler signals; a successful wait consumes the signal (the reset).
    if (!s.tail_fence_is_last)
      wait = false;
    auto *fence = static_cast<plume::MetalCommandFence *>(s.tail_fence.get());
    if (dispatch_semaphore_wait(fence->semaphore, wait ? DISPATCH_TIME_FOREVER : DISPATCH_TIME_NOW) != 0)
      return false;
  } else
#endif
  {
  auto *dev = static_cast<plume::VulkanDevice *>(s.device.get());
  VkFence fence = TailFence(s);
  if (!s.tail_fence_is_last)
    wait = false;
  VkResult res = wait ? vkWaitForFences(dev->vk, 1, &fence, VK_TRUE, UINT64_MAX)
                      : vkGetFenceStatus(dev->vk, fence);
  if (res != VK_SUCCESS)
    return false;
  vkResetFences(dev->vk, 1, &fence);
  }
  s.tail_fence_inflight = false;
  if (s.tail_fence_is_last) {
    s.tail_fence_is_last = false;
    s.queue_idle = true;
  }
  return s.queue_idle;
#endif
}

template <typename T> void MoveAll(std::vector<T> &from, std::vector<T> &to) {
  for (auto &p : from)
    to.push_back(std::move(p));
  from.clear();
}

}

void NoteQueueSubmittedLocked(VideoState &s) {
  s.queue_idle = false;
  s.tail_fence_is_last = false;
}

void NoteQueueDrainedLocked(VideoState &s) {
  s.queue_idle = true;
  FlushRetiredLocked(s);
}

void SubmitQueueMarkerLocked(VideoState &s) {
#if !defined(EOT_D3D12)
  if (!REXCVAR_GET(fable2_destroy_when_queue_idle))
    return;
  QueueIdleLocked(s, false); // reclaim the previous marker if it signalled
  if (s.tail_fence_inflight)
    return;
#if defined(FABLE2_METAL_BACKEND)
  if (UseMetal()) {
    // Metal: an empty command list carries the marker fence (plume submits
    // at least one list); the queue runs command buffers in commit order.
    if (!s.tail_fence)
      s.tail_fence = s.device->createCommandFence();
    if (!s.tail_list) {
      s.tail_list = s.queue->createCommandList();
      NameFenceList(s.tail_list.get(), "Queue Idle Marker");
    }
    if (!s.tail_fence || !s.tail_list)
      return;
    s.tail_list->begin();
    s.tail_list->end();
    const plume::RenderCommandList *lists[] = {s.tail_list.get()};
    s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, s.tail_fence.get());
    s.tail_fence_inflight = true;
    s.tail_fence_is_last = true;
    return;
  }
#endif
  VkFence fence = TailFence(s);
  if (fence == VK_NULL_HANDLE)
    return;
  auto *q = static_cast<plume::VulkanCommandQueue *>(s.queue.get())->queue;
  VkResult res;
  {
    const std::scoped_lock queue_lock(*q->mutex);
    res = vkQueueSubmit(q->vk, 0, nullptr, fence);
  }
  if (res != VK_SUCCESS) {
    EOT_WARN("[gpu] fence-only vkQueueSubmit failed ({})", static_cast<int>(res));
    return;
  }
  s.tail_fence_inflight = true;
  s.tail_fence_is_last = true;
#endif
}

void FlushRetiredLocked(VideoState &s) {
  if (s.texture_retired.empty() && s.view_retired.empty() && s.buffer_retired.empty() &&
      s.framebuffer_retired.empty())
    return;
  // Waiting is rare: only when the lists are 8 presents old (GPU bound).
  const bool overdue = s.presented_frames >= s.retired_since + 8;
  if (REXCVAR_GET(fable2_destroy_when_queue_idle) && !QueueIdleLocked(s, overdue))
    return;
  s.perf.retired_flushes++;
  static u64 flushes = 0, overdue_flushes = 0;
  ++flushes;
  if (overdue)
    ++overdue_flushes;
  if (flushes % 200 == 1)
    EOT_INFO("[gpu] retired objects destroyed with the queue idle: {} flushes ({} waited), this one "
             "{} textures {} buffers {} views {} framebuffers, {} presents old",
             flushes, overdue_flushes, s.texture_retired.size(), s.buffer_retired.size(),
             s.view_retired.size(), s.framebuffer_retired.size(), s.presented_frames - s.retired_since);
  s.framebuffer_retired.clear();
  s.view_retired.clear();
  s.texture_retired.clear();
  s.buffer_retired.clear();
}

static bool DoneMarkersOn();

void AdvanceAndWaitReused(VideoState &s) {
  UploadRingSlotSubmitted(s.frame.load(std::memory_order_relaxed));
  const u32 slot = s.next_frame;
  s.frame.store(slot, std::memory_order_relaxed);
  s.frame_chunks = 0;
  s.next_frame = (slot + 1) % kNumFrames;
  s.command_list = s.command_lists[slot].get();
  // With GPU-done markers the game's thread may be waiting here because the
  // marker thread fell behind; it is released (UploadRingSlotFinished) only
  // after the slot's query counts and write-backs are published below, since
  // it reads them. Without markers the release stays first (as before).
  const bool publish_first = DoneMarkersOn();
  bool slot_finished = false;
  if (s.command_list_submitted[slot]) {
    s.queue->waitForCommandFence(s.fences[slot].get());
    s.command_list_submitted[slot] = false;
    if (publish_first)
      slot_finished = true;
    else
      UploadRingSlotFinished(slot);
    // The slot's chunks ran before its fenced submit.
    for (auto &l : s.chunk_inflight[slot])
      s.chunk_spare[slot].push_back(std::move(l));
    s.chunk_inflight[slot].clear();
    GpuTimingCollect(s, slot);
#if EOT_GPU_PROFILING
    if (auto *ctx = GpuProfilerCtx()) {
      TracyD3D12NewFrame(ctx);
      TracyD3D12Collect(ctx);
    }
#endif
  } else if (s.chunk_inflight[slot].empty()) {
    // Nothing of this slot's frames is on the GPU (no submit, no chunks).
    if (publish_first)
      slot_finished = true;
    else
      UploadRingSlotFinished(slot);
  }
  // Fable 2: the bindless texture set is shared by both frames in flight.
  // Textures released while this slot's frame was recorded were still in the
  // set when the other slot's frame (recorded after them, possibly still on
  // the GPU) bound it, so MoltenVK made them resident for that command buffer.
  // Destroying them here gave "Invalid Resource" GPU faults (device lost) in
  // texture-heavy places such as Lucian's castle. So: null their slots first,
  // and destroy them after the other slot's fence (the next call).
  if (s.texture_retired.empty() && s.view_retired.empty() && s.buffer_retired.empty() &&
      s.framebuffer_retired.empty())
    s.retired_since = s.presented_frames;
  MoveAll(s.view_deferred, s.view_retired);
  MoveAll(s.texture_deferred, s.texture_retired);
  MoveAll(s.buffer_deferred, s.buffer_retired);
  MoveAll(s.framebuffer_graveyard[slot], s.framebuffer_retired);
  if (!REXCVAR_GET(fable2_destroy_when_queue_idle))
    FlushRetiredLocked(s);
  for (auto &b : s.buffer_graveyard[slot])
    s.buffer_deferred.push_back(std::move(b));
  s.buffer_graveyard[slot].clear();
  QueriesSlotRecycledLocked(s, slot);
  WritebacksSlotRecycledLocked(s, slot);
  if (slot_finished)
    UploadRingSlotFinished(slot);
  DrainDescriptorSlotsLocked(s, slot);
  for (auto &v : s.view_graveyard[slot])
    s.view_deferred.push_back(std::move(v));
  s.view_graveyard[slot].clear();
  for (auto &t : s.texture_graveyard[slot])
    s.texture_deferred.push_back(std::move(t));
  s.texture_graveyard[slot].clear();
  UploadRingParkExcess(s);
}

void ParkTexture(VideoState &s, std::unique_ptr<plume::RenderTexture> t) {
  if (!t)
    return;
  s.perf.host_parked++;
  NoteHostRelease(t.get());
  s.texture_graveyard[s.recording_slot()].push_back(std::move(t));
}

void ParkView(VideoState &s, std::unique_ptr<plume::RenderTextureView> v) {
  if (v)
    s.view_graveyard[s.recording_slot()].push_back(std::move(v));
}

void ParkFramebuffer(VideoState &s, std::unique_ptr<plume::RenderFramebuffer> f) {
  if (f)
    s.framebuffer_graveyard[s.recording_slot()].push_back(std::move(f));
}

void ParkBuffer(VideoState &s, std::unique_ptr<plume::RenderBuffer> b) {
  if (!b)
    return;
  NoteHostRelease(b.get());
  s.buffer_graveyard[s.recording_slot()].push_back(std::move(b));
}

void ParkHostTexture(VideoState &s, HostTexture &host) {
  const u64 bytes = host.texture ? HostTextureBytes(host) : 0;
  if (host.texture && s.host_texture_pool.size() < kHostTexturePoolMax &&
      bytes <= kHostTexturePoolMaxBytes) {
    VideoState::PooledHostTexture entry;
    entry.host = std::move(host);
    entry.freedFrame = s.guest_frames;
    entry.bytes = bytes;
    s.host_texture_pool.push_back(std::move(entry));
    s.host_texture_pool_bytes += bytes;
    host = HostTexture{};
    TrimHostTexturePool(s);
    return;
  }
  DestroyHostTexture(s, host);
}

void DestroyHostTexture(VideoState &s, HostTexture &host) {
  ReleaseTextureSRVLocked(s, host);
  if (host.texture) {
    const auto *dead = host.texture.get();
    for (auto it = s.framebuffers.begin(); it != s.framebuffers.end();) {
      bool names_dead = false;
      for (u32 i = 0; i < it->second.attachmentCount; ++i)
        names_dead = names_dead || it->second.attachments[i] == dead;
      if (!names_dead) {
        ++it;
        continue;
      }
      if (s.bound_framebuffer == it->second.fb.get())
        s.bound_framebuffer = nullptr;
      ParkFramebuffer(s, std::move(it->second.fb));
      it = s.framebuffers.erase(it);
    }
  }
  for (auto &fb : host.mipFramebuffers)
    ParkFramebuffer(s, std::move(fb));
  host.mipFramebuffers.clear();
  for (auto &v : host.mipViews)
    ParkView(s, std::move(v));
  host.mipViews.clear();
  ParkView(s, std::move(host.depthView));
  ParkView(s, std::move(host.srv));
  ParkTexture(s, std::move(host.texture));
  host.layout = plume::RenderTextureLayout::UNKNOWN;
}

bool CreateOrRecycleHostTexture(VideoState &s, HostTexture &host,
                                const plume::RenderTextureDesc &desc, const char *tag,
                                plume::RenderPool *pool) {
  const bool renderable = (desc.flags & plume::RenderTextureFlag::RENDER_TARGET) ||
                          (desc.flags & plume::RenderTextureFlag::DEPTH_TARGET);
  const bool transient = pool != nullptr;
  for (size_t i = 0; i < s.host_texture_pool.size(); ++i) {
    auto &entry = s.host_texture_pool[i];
    if (entry.freedFrame + kNumFrames > s.guest_frames || entry.host.transientPool != transient ||
        !SameTextureShape(entry.host, host, desc))
      continue;
    HostTexture pooled = std::move(entry.host);
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, entry.bytes);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
    host.texture = std::move(pooled.texture);
    host.desc = pooled.desc;
    host.srv = std::move(pooled.srv);
    host.descriptorIndex = pooled.descriptorIndex;
    host.swizzledSrvs = std::move(pooled.swizzledSrvs);
    host.mipViews = std::move(pooled.mipViews);
    host.mipFramebuffers = std::move(pooled.mipFramebuffers);
    host.depthView = std::move(pooled.depthView);
    host.transientPool = pooled.transientPool;
    host.layout = pooled.layout;
    host.needsClear = renderable;
    s.perf.host_tex_recycled++;
    RetagHostAllocation(host.texture.get(), tag, desc);
    return true;
  }
  host.texture = CreateHostTexture(s.device.get(), desc, tag, pool);
  host.transientPool = transient && host.texture != nullptr;
  if (!host.texture && transient)
    host.texture = CreateHostTexture(s.device.get(), desc, tag);
  host.desc = desc;
  host.desc.optimizedClearValue = nullptr;
  host.layout = plume::RenderTextureLayout::UNKNOWN;
  host.needsClear = renderable && host.texture != nullptr;
  return host.texture != nullptr;
}

void EvictHostTexturePool(VideoState &s) {
  size_t i = 0;
  while (i < s.host_texture_pool.size()) {
    const bool idle = FrameAgeSeconds(s, s.host_texture_pool[i].freedFrame) > kHostTexturePoolSeconds;
    const bool over = s.host_texture_pool.size() > kHostTexturePoolMax ||
                      s.host_texture_pool_bytes > kHostTexturePoolMaxBytes;
    if (!idle && !over) {
      ++i;
      continue;
    }
    s.host_texture_pool_bytes -= std::min(s.host_texture_pool_bytes, s.host_texture_pool[i].bytes);
    DestroyHostTexture(s, s.host_texture_pool[i].host);
    s.host_texture_pool.erase(s.host_texture_pool.begin() + i);
  }
  s.perf.pool_size = static_cast<u32>(s.host_texture_pool.size());
}

void TransitionLocked(VideoState &s, HostTexture &host, plume::RenderTextureLayout layout) {
  if (!host.texture || host.layout == layout || !s.command_list_open)
    return;
  if (s.defer_shader_read_transitions && layout == plume::RenderTextureLayout::SHADER_READ &&
      s.pending_transition_count < kMaxPendingTransitions) {
    s.pending_transitions[s.pending_transition_count++] = {&host, layout};
    return;
  }
  const HostTextureTransition transition{&host, layout};
  TransitionManyLocked(s, &transition, 1);
}

void TransitionManyLocked(VideoState &s, const HostTextureTransition *transitions, u32 count) {
  if (!transitions || !count || !s.command_list_open)
    return;
  constexpr u32 kBatch = 8;
  for (u32 first = 0; first < count; first += kBatch) {
    plume::RenderTextureBarrier barriers[kBatch];
    u32 barrier_count = 0;
    plume::RenderBarrierStages stages = plume::RenderBarrierStage::NONE;
    const u32 end = std::min(first + kBatch, count);
    for (u32 i = first; i < end; ++i) {
      HostTexture *host = transitions[i].host;
      const plume::RenderTextureLayout layout = transitions[i].layout;
      if (!host || !host->texture || host->layout == layout)
        continue;
      barriers[barrier_count++] = plume::RenderTextureBarrier(host->texture.get(), layout);
      if (layout == plume::RenderTextureLayout::COPY_SOURCE ||
          layout == plume::RenderTextureLayout::COPY_DEST)
        stages |= plume::RenderBarrierStage::COPY;
      else
        stages |= plume::RenderBarrierStage::GRAPHICS;
      host->layout = layout;
    }
    if (!barrier_count)
      continue;
    s.command_list->barriers(stages, barriers, barrier_count);
    s.perf.texture_barrier_calls++;
    s.perf.texture_barrier_resources += barrier_count;
  }
}

}

namespace eot::gpu {

namespace {

constexpr u64 kChunkSize = 32ull * 1024 * 1024;
// The ring belongs to the frames as the game's thread captures them, not to the
// render thread's command list slots: frame i's data goes to ring slot
// i % kRingSlots, and the game's thread resets a slot (at its Present) only
// after the GPU has finished the frame that used it last (with the game's
// thread one frame ahead it may wait for that). Reset per command list slot
// (before), a game thread one frame ahead (fable2_present_pipelined) put its
// next frame's first data into a slot that was then reset while the GPU still
// read it: missing and garbage geometry, a washed-out picture.
constexpr u32 kRingSlots = kUploadRingSlots;

struct Chunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *cpu = nullptr;
  u64 capacity = 0;
  u64 used = 0;
  u64 gpuVa = 0;
};

struct Ring {
  std::vector<Chunk> chunks[kRingSlots];
  u64 frame_bytes[kRingSlots] = {};
};

Ring &ring() {
  static Ring r;
  return r;
}

bool MakeChunk(Chunk &chunk, u64 size) {
  auto &s = state();
  // A 1 MB guard tail that is never handed out: a vertex fetch past the end
  // of a draw's data near the end of a chunk stays inside the buffer (Metal
  // reported "Invalid Resource" and lost the device, e.g. in Lucian's castle
  // and with fable2_geometry_caches off).
  plume::RenderBufferDesc desc = plume::RenderBufferDesc::UploadBuffer(size + (1ull << 20));
  desc.flags = plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
               plume::RenderBufferFlag::CONSTANT;
  chunk.buffer = CreateHostBuffer(s.device.get(), desc, "upload-ring");
  if (!chunk.buffer)
    return false;
  chunk.cpu = static_cast<u8 *>(chunk.buffer->map());
  if (!chunk.cpu) {
    EOT_ERROR("upload ring: map() failed for a {} byte chunk", size);
    chunk.buffer.reset();
    return false;
  }
  chunk.capacity = size;
  chunk.used = 0;
#if defined(EOT_D3D12)
  chunk.gpuVa = static_cast<plume::D3D12Buffer *>(chunk.buffer.get())->d3d->GetGPUVirtualAddress();
#else
  chunk.gpuVa = chunk.buffer->getDeviceAddress();
  if (!chunk.gpuVa) {
    EOT_ERROR("upload ring: the device gave the chunk no buffer device address (VK_KHR_buffer_device_address)");
    chunk.buffer->unmap();
    chunk.buffer.reset();
    return false;
  }
#endif
  return true;
}

}

static std::mutex g_ring_mutex;
// Bumped after the ring's chunks are rebuilt, under g_ring_mutex, so a
// thread's sub-block (UploadAllocate with fable2_ring_subblock) taken from the
// old chunks fails its check. Today the ring is built once, at boot.
static std::atomic<u64> g_ring_generation{1};

bool UploadRingInit() {
  auto &r = ring();
  std::lock_guard lock(g_ring_mutex);
  bool ok = true;
  for (u32 i = 0; i < kRingSlots && ok; ++i) {
    r.chunks[i].clear();
    Chunk c;
    ok = MakeChunk(c, kChunkSize);
    if (ok)
      r.chunks[i].push_back(std::move(c));
  }
  g_ring_generation.fetch_add(1, std::memory_order_acq_rel);
  return ok;
}

static std::atomic<u64> g_ring_epoch{0};
u64 UploadRingEpoch() { return g_ring_epoch.load(std::memory_order_acquire); }

// Frames whose Present the render thread has processed, and of those, how many
// the GPU has finished (published when the render thread waits for a slot fence).
static std::atomic<u64> g_frames_presented{0};
static u64 g_slot_frames[kNumFrames] = {};
static std::mutex g_done_mutex;
static std::condition_variable g_done_cv;
static u64 g_gpu_done_frames = 0;
// The frame the game's thread is capturing (index from 0): its ring slot.
static std::atomic<u64> g_capture_frame{0};
static u32 SlotOf(u64 frame) { return static_cast<u32>(frame % kRingSlots); }
// Chunks over the usual 4 left by a slot reset, for the render thread to park.
static std::vector<std::unique_ptr<plume::RenderBuffer>> g_ring_excess;

REXCVAR_DEFINE_BOOL(fable2_gpu_done_wait_replay, false, "Fable2",
                    "Native renderer: with fable2_gpu_done_markers, the game's next frame also waits until the "
                    "render thread has read the previous frame's guest data (safety switch; ~0.6 ms slower)");

void UploadRingFramePresented() { g_frames_presented.fetch_add(1, std::memory_order_acq_rel); }
u64 UploadRingFramesPresented() { return g_frames_presented.load(std::memory_order_acquire); }

// Presents whose guest-memory reads are over (the front buffer, announced
// texture preloads): the render thread is done with that frame's guest data.
static std::atomic<u64> g_frames_released{0};
static bool DoneMarkersOn();
void UploadRingFrameReleased() {
  g_frames_released.fetch_add(1, std::memory_order_acq_rel);
  if (DoneMarkersOn() && REXCVAR_GET(fable2_gpu_done_wait_replay)) {
    { std::lock_guard lock(g_done_mutex); }
    g_done_cv.notify_all();
  }
}
static void ResetRingSlotLocked(u32 slot);

void UploadRingNextCapturedFrame() {
  const u64 next = g_capture_frame.load(std::memory_order_relaxed) + 1;
  if (next >= kRingSlots) {
    // The slot's previous user is frame next - kRingSlots: frames 0..that one done.
    const u64 need = next - kRingSlots + 1;
    std::unique_lock lock(g_done_mutex);
    // With GPU-done markers the game may start this frame while the render
    // thread still replays the previous one. Replay takes its guest data from
    // the draw packets and the upload ring, except a few live reads: texture
    // uploads and content checks, a stream's endian bits, the front buffer.
    // Those can see the game's next frame. fable2_diag_late_reads counted none
    // in Bowerstone market, but it cannot see writes still in progress (a
    // texture locked, not yet unlocked) or writes made without an Unlock.
    // fable2_gpu_done_wait_replay also waits until the render thread's Present
    // has read the frame's last guest data.
    const bool markers = DoneMarkersOn() && REXCVAR_GET(fable2_gpu_done_wait_replay);
    if (!g_done_cv.wait_for(lock, std::chrono::seconds(2), [&] {
          return g_gpu_done_frames >= need &&
                 (!markers || g_frames_released.load(std::memory_order_acquire) >= next);
        })) {
      static std::atomic<u32> logged{0};
      if (logged.fetch_add(1) < 8)
        EOT_WARN("[upload-ring] frame {}: the GPU finished only {} of {} frames (render thread released {}) "
                 "after 2 s; reusing the slot",
                 next, g_gpu_done_frames, need, g_frames_released.load(std::memory_order_relaxed));
    }
  }
  std::lock_guard lock(g_ring_mutex);
  ResetRingSlotLocked(SlotOf(next));
  g_capture_frame.store(next, std::memory_order_relaxed);
}

void UploadRingParkExcess(VideoState &s) {
  std::vector<std::unique_ptr<plume::RenderBuffer>> excess;
  {
    std::lock_guard lock(g_ring_mutex);
    excess.swap(g_ring_excess);
  }
  for (auto &b : excess)
    ParkBuffer(s, std::move(b));
}

void UploadRingSlotSubmitted(u32 command_slot) {
  g_slot_frames[command_slot] = g_frames_presented.load(std::memory_order_acquire);
}

void UploadRingSlotFinished(u32 command_slot) {
  {
    std::lock_guard lock(g_done_mutex);
    g_gpu_done_frames = std::max(g_gpu_done_frames, g_slot_frames[command_slot]);
  }
  g_done_cv.notify_all();
}

void UploadRingFramesDone(u64 frames) {
  {
    std::lock_guard lock(g_done_mutex);
    if (frames <= g_gpu_done_frames)
      return;
    g_gpu_done_frames = frames;
  }
  g_done_cv.notify_all();
}

// GPU-done markers (fable2_gpu_done_markers). Without them the game's thread
// learns that frame N is off the GPU only when the render thread waits for
// that slot's fence, at its Present of frame N+1: the game's Present then waits
// for the render thread's whole tail, not for the GPU. With them, an empty
// command list with its own fence follows each Present's submit, and a helper
// thread waits that fence, publishes the frame's occlusion query counts
// (QueriesPublishSubmitted) and then the frame count. The queue runs in submit
// order, so the marker signals after everything before it (the frame's chunks
// and its fenced submit). Two upload-ring slots stay, so the game's lead over
// the GPU is unchanged.
//
// The query counts must come from here: published only at the render thread's
// recycle, they arrived after the released game had already asked for them,
// so 80-100% of its reads got the stale "fully visible" answer (occlusion
// culling off, extra draws, and flicker in the host counts when on from boot).
namespace {

constexpr u32 kDoneMarkers = 4;

struct DoneMarkers {
  std::unique_ptr<plume::RenderCommandFence> fence[kDoneMarkers];
  std::unique_ptr<plume::RenderCommandList> list[kDoneMarkers];
  u64 frames[kDoneMarkers] = {};
  u32 querySlot[kDoneMarkers] = {};
  u64 querySerial[kDoneMarkers] = {};
  std::atomic<bool> busy[kDoneMarkers] = {};
  u32 next = 0; // render thread only
  // Set on a fence error (device lost) or a failed init: back to Present-time
  // publishing. Read by the game's thread too.
  std::atomic<bool> failed{false};
  // The thread left with a marker unsignalled: its fence and list may still be
  // used by the GPU or plume's completion handler, so they are never destroyed.
  bool abandoned = false;
  std::mutex mutex;
  std::condition_variable cv;
  std::deque<u32> pending;
  bool stop = false;
  std::thread thread;
  plume::RenderDevice *device = nullptr;
  u64 skipped = 0;
};

// Never destroyed: a renderer that skipped its drain leaves the thread running at exit.
DoneMarkers &g_markers = *new DoneMarkers;

#if !defined(EOT_D3D12)

enum class FenceWait { Signalled, Timeout, Error };

// Waits up to timeout_ms for marker i's fence and resets it.
FenceWait WaitMarkerFence(u32 i, u32 timeout_ms) {
  auto &m = g_markers;
#if defined(FABLE2_METAL_BACKEND)
  if (UseMetal()) {
    auto *fence = static_cast<plume::MetalCommandFence *>(m.fence[i].get());
    return dispatch_semaphore_wait(fence->semaphore,
                                   dispatch_time(DISPATCH_TIME_NOW, int64_t(timeout_ms) * int64_t(NSEC_PER_MSEC))) == 0
               ? FenceWait::Signalled
               : FenceWait::Timeout;
  }
#endif
  auto *dev = static_cast<plume::VulkanDevice *>(m.device);
  VkFence fence = static_cast<plume::VulkanCommandFence *>(m.fence[i].get())->vk;
  const VkResult res = vkWaitForFences(dev->vk, 1, &fence, VK_TRUE, u64(timeout_ms) * 1000000ull);
  if (res == VK_TIMEOUT)
    return FenceWait::Timeout;
  if (res != VK_SUCCESS)
    return FenceWait::Error; // device lost: returns at once, so retrying would spin
  vkResetFences(dev->vk, 1, &fence);
  return FenceWait::Signalled;
}

void DoneMarkerThread() {
#if defined(__APPLE__)
  pthread_setname_np("Fable2 GPU done");
  // The game's thread waits on this one: wake it on a performance core.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  auto &m = g_markers;
  for (;;) {
    u32 i;
    {
      std::unique_lock lock(m.mutex);
      m.cv.wait(lock, [&] { return m.stop || !m.pending.empty(); });
      if (m.pending.empty())
        return;
      i = m.pending.front();
      m.pending.pop_front();
    }
    for (;;) {
      const FenceWait w = WaitMarkerFence(i, 100);
      if (w == FenceWait::Signalled)
        break;
      std::lock_guard lock(m.mutex);
      if (w == FenceWait::Error) {
        EOT_WARN("[upload-ring] GPU-done marker fence failed (device lost?); back to Present-time publishing");
        m.failed.store(true, std::memory_order_release);
        m.abandoned = true;
        return;
      }
      if (m.stop) {
        m.abandoned = true; // the GPU never finished it: leave it
        return;
      }
    }
    // The frame's occlusion counts first: the game, once released, reads them.
    QueriesPublishSubmitted(m.querySlot[i], m.querySerial[i]);
    UploadRingFramesDone(m.frames[i]);
    m.busy[i].store(false, std::memory_order_release);
  }
}
#endif

}

REXCVAR_DEFINE_BOOL(fable2_gpu_done_markers, true, "Fable2",
                    "Native renderer: let the game's thread start its next frame as soon as the GPU has "
                    "finished the frame its upload slot held, not at the render thread's next Present "
                    "(Bowerstone market on Metal: 9.6 -> 8.3 ms)");

static bool DoneMarkersOn() {
  return REXCVAR_GET(fable2_gpu_done_markers) && !g_markers.failed.load(std::memory_order_acquire);
}

void SubmitDoneMarkerLocked(VideoState &s) {
#if defined(EOT_D3D12)
  (void)s;
#else
  auto &m = g_markers;
  if (!REXCVAR_GET(fable2_gpu_done_markers) || m.failed.load(std::memory_order_relaxed))
    return;
  if (!m.thread.joinable()) {
    m.device = s.device.get();
    for (u32 i = 0; i < kDoneMarkers; ++i) {
      m.fence[i] = s.device->createCommandFence();
      m.list[i] = s.queue->createCommandList();
      NameFenceList(m.list[i].get(), "GPU Done Marker");
      if (!m.fence[i] || !m.list[i]) {
        EOT_WARN("[upload-ring] GPU-done markers: no fence or command list; staying with Present-time publishing");
        m.failed.store(true, std::memory_order_release);
        return;
      }
    }
    m.stop = false;
    m.thread = std::thread(DoneMarkerThread);
    EOT_INFO("[upload-ring] GPU-done markers on");
  }
  const u32 i = m.next;
  if (m.busy[i].load(std::memory_order_acquire)) {
    // Four frames' markers still unsignalled (GPU far behind): the next one
    // covers this frame's count; its query counts wait for the slot recycle.
    if (m.skipped++ % 600 == 0)
      EOT_INFO("[upload-ring] GPU-done markers skipped: {} (all four in flight)", m.skipped);
    return;
  }
  m.next = (i + 1) % kDoneMarkers;
  m.list[i]->begin();
  m.list[i]->end();
  m.frames[i] = g_frames_presented.load(std::memory_order_acquire);
  m.querySlot[i] = s.frame.load(std::memory_order_relaxed);
  m.querySerial[i] = QueriesSlotSerial(m.querySlot[i]);
  m.busy[i].store(true, std::memory_order_relaxed);
  const plume::RenderCommandList *lists[] = {m.list[i].get()};
  s.queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, m.fence[i].get());
  NoteQueueSubmittedLocked(s);
  {
    std::lock_guard lock(m.mutex);
    m.pending.push_back(i);
  }
  m.cv.notify_one();
#endif
}

void DoneMarkersShutdown() {
#if !defined(EOT_D3D12)
  auto &m = g_markers;
  if (m.thread.joinable()) {
    {
      std::lock_guard lock(m.mutex);
      m.stop = true;
    }
    m.cv.notify_all();
    m.thread.join();
  }
  // Markers still pending were never waited: their fences may yet be signalled.
  const bool leak = m.abandoned || !m.pending.empty();
  m.pending.clear();
  for (u32 i = 0; i < kDoneMarkers; ++i) {
    if (leak) {
      (void)m.list[i].release();
      (void)m.fence[i].release();
    } else {
      m.list[i].reset();
      m.fence[i].reset();
    }
    m.busy[i].store(false, std::memory_order_relaxed);
  }
  m.next = 0;
  m.abandoned = false;
  m.device = nullptr;
  m.failed.store(false, std::memory_order_relaxed);
#endif
}

static void ResetRingSlotLocked(u32 slot) {
  auto &r = ring();
  g_ring_epoch.fetch_add(1, std::memory_order_acq_rel);
  auto &chunks = r.chunks[slot];
  for (size_t i = 0; i < chunks.size(); ++i)
    chunks[i].used = 0;
  // Chunks over the usual 4 (a spike, e.g. the first frame in Lucian's
  // castle) go to the buffer graveyard instead of being destroyed here: the
  // other frame slot's command buffer may still be on the GPU, and MoltenVK
  // makes every device-addressable buffer resident for each command buffer,
  // so destroying them now was an "Invalid Resource" GPU fault (device lost).
  while (chunks.size() > 4) {
    chunks.back().buffer->unmap();
    // Parked by the render thread (the graveyards are its own); the GPU is
    // done with this slot's frames already.
    g_ring_excess.push_back(std::move(chunks.back().buffer));
    chunks.pop_back();
  }
  if (r.frame_bytes[slot] > 64ull << 20) {
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1) < 40)
      EOT_INFO("[upload-ring] slot {} frame used {:.1f} MB in {} chunks", slot, r.frame_bytes[slot] / 1048576.0,
               chunks.size());
  }
  r.frame_bytes[slot] = 0;
}

REXCVAR_DEFINE_BOOL(fable2_ring_subblock, true, "Fable2",
                     "Native renderer: each thread takes 64 KB blocks of the upload ring under its lock and "
                     "places small allocations in them without it (one lock per block, not per allocation)");

static bool UploadAllocateLocked(u64 size, u64 alignment, UploadAlloc *out, u64 *frame_out, u64 *gen_out);

// A thread's current block of the upload ring (fable2_ring_subblock), taken
// under the lock in the slot of capture frame C and used only while
// g_capture_frame is still C at check time. An allocation handed out just
// before the game advances stays valid for the same reason a locked one does:
// slot C is reset only at the advance to C + kRingSlots, which first waits for
// the GPU to finish frame C + 1, and replay never runs ahead of capture.
// frame_bytes counts whole blocks, not the bytes placed in them; each thread
// abandons up to one block's tail per frame change.
namespace {
constexpr u64 kSubBlockBytes = 64 * 1024;
struct SubBlock {
  u64 frame = ~0ull, gen = 0;
  plume::RenderBuffer *buffer = nullptr;
  u8 *cpu = nullptr;     // the buffer's mapping at offset 0
  u64 gpuVa = 0;         // the buffer's device address at offset 0 (0 if none)
  u64 next = 0, end = 0; // offsets in the buffer
};
thread_local SubBlock t_block;
}  // namespace

bool UploadAllocate(u64 size, u64 alignment, UploadAlloc *out) {
  *out = UploadAlloc{};
  if (size == 0)
    return false;
  if (alignment == 0)
    alignment = 1;
  if (size <= kSubBlockBytes / 4 && alignment <= kTexturePlacementAlignment &&
      REXCVAR_GET(fable2_ring_subblock)) {
    SubBlock &b = t_block;
    for (u32 attempt = 0; attempt < 2; ++attempt) {
      if (b.buffer && b.frame == g_capture_frame.load(std::memory_order_relaxed) &&
          b.gen == g_ring_generation.load(std::memory_order_relaxed)) {
        const u64 start = (b.next + alignment - 1) / alignment * alignment;
        if (start + size <= b.end) {
          out->buffer = b.buffer;
          out->offset = start;
          out->cpu = b.cpu + start;
          out->size = size;
          out->gpuVa = b.gpuVa ? b.gpuVa + start : 0;
          b.next = start + size;
          return true;
        }
      }
      if (attempt)
        break;
      UploadAlloc blk;
      u64 frame = 0, gen = 0;
      if (!UploadAllocateLocked(kSubBlockBytes, kTexturePlacementAlignment, &blk, &frame, &gen)) {
        b = SubBlock{};
        break;
      }
      b.frame = frame;
      b.gen = gen;
      b.buffer = blk.buffer;
      b.cpu = blk.cpu - blk.offset;
      b.gpuVa = blk.gpuVa ? blk.gpuVa - blk.offset : 0;
      b.next = blk.offset;
      b.end = blk.offset + blk.size;
    }
    // Fall through: allocate this one directly.
  }
  return UploadAllocateLocked(size, alignment, out, nullptr, nullptr);
}

static bool UploadAllocateLocked(u64 size, u64 alignment, UploadAlloc *out, u64 *frame_out, u64 *gen_out) {
  *out = UploadAlloc{};
  auto &r = ring();
  std::lock_guard lock(g_ring_mutex);
  const u64 frame = g_capture_frame.load(std::memory_order_relaxed);
  if (frame_out)
    *frame_out = frame;
  if (gen_out)
    *gen_out = g_ring_generation.load(std::memory_order_relaxed);
  const u32 slot = SlotOf(frame);
  auto &chunks = r.chunks[slot];
  for (u32 attempt = 0; attempt < 2; ++attempt) {
    for (auto &c : chunks) {
      const u64 start = (c.used + alignment - 1) / alignment * alignment;
      if (start + size <= c.capacity) {
        out->buffer = c.buffer.get();
        out->offset = start;
        out->cpu = c.cpu + start;
        out->size = size;
        out->gpuVa = c.gpuVa ? c.gpuVa + start : 0;
        c.used = start + size;
        r.frame_bytes[slot] += size;
        return true;
      }
    }
    Chunk c;
    const u64 want = size + alignment > kChunkSize ? size + alignment : kChunkSize;
    if (!MakeChunk(c, want))
      return false;
    chunks.push_back(std::move(c));
  }
  return false;
}

bool UploadBytes(const void *src, u64 size, u64 alignment, UploadAlloc *out) {
  if (!UploadAllocate(size, alignment, out))
    return false;
  std::memcpy(out->cpu, src, size);
  return true;
}

static u64 g_ring_peak[kRingSlots];
u64 UploadRingBytesThisFrame() {
  return ring().frame_bytes[SlotOf(g_capture_frame.load(std::memory_order_relaxed))];
}
u32 UploadRingSlot() { return SlotOf(g_capture_frame.load(std::memory_order_relaxed)); }
u64 UploadRingCaptureFrame() { return g_capture_frame.load(std::memory_order_relaxed); }

u64 UploadRingCapacityBytes(u32 *chunks) {
  std::lock_guard lock(g_ring_mutex);
  u64 bytes = 0;
  u32 n = 0;
  for (const auto &slot : ring().chunks) {
    for (const Chunk &c : slot) {
      bytes += c.capacity;
      n++;
    }
  }
  if (chunks)
    *chunks = n;
  return bytes;
}

}

namespace eot::gpu {

u32 AllocateDescriptorSlot(VideoState &s) {
  for (size_t i = kNullTextureDescriptorCount; i < s.descriptor_slot_used.size(); ++i) {
    if (!s.descriptor_slot_used[i]) {
      s.descriptor_slot_used[i] = true;
      return static_cast<u32>(i);
    }
  }
  return kInvalidDescriptorIndex;
}

void DrainDescriptorSlotsLocked(VideoState &s, u32 slot) {
  for (const auto &d : s.descriptor_graveyard[slot]) {
    if (s.texture_descriptor_set) {
      s.texture_descriptor_set->setTexture(d.slot, s.null_textures[d.null_index].get(),
                                           plume::RenderTextureLayout::SHADER_READ,
                                           s.null_texture_views[d.null_index].get());
    }
    if (d.slot >= kNullTextureDescriptorCount && d.slot < s.descriptor_slot_used.size())
      s.descriptor_slot_used[d.slot] = false;
  }
  s.descriptor_graveyard[slot].clear();
}

namespace {

u32 NullIndexFor(const HostTexture &host) {
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D)
    return kNullTexture3DDescriptorIndex;
  if (host.viewDimension == plume::RenderTextureViewDimension::TEXTURE_CUBE)
    return kNullTextureCubeDescriptorIndex;
  return kNullTexture2DDescriptorIndex;
}

plume::RenderTextureViewDesc SamplingViewDesc(const HostTexture &host) {
  plume::RenderTextureViewDesc view_desc;
  view_desc.format = SampledViewFormat(host.format);
  view_desc.dimension = host.viewDimension;
  view_desc.mipLevels = host.mipLevels ? host.mipLevels : 1;
  return view_desc;
}

plume::RenderSwizzle ToHostSwizzle(u32 source) {
  switch (source & 7) {
  case 0:
    return plume::RenderSwizzle::R;
  case 1:
    return plume::RenderSwizzle::G;
  case 2:
    return plume::RenderSwizzle::B;
  case 3:
    return plume::RenderSwizzle::A;
  case 5:
    return plume::RenderSwizzle::ONE;
  default:
    return plume::RenderSwizzle::ZERO;
  }
}

u32 PublishView(VideoState &s, HostTexture &host, plume::RenderTextureView *view) {
  const u32 slot = AllocateDescriptorSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    EOT_ERROR("bindless texture heap full at {} slots", kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  s.texture_descriptor_set->setTexture(slot, host.texture.get(),
                                       plume::RenderTextureLayout::SHADER_READ, view);
  return slot;
}

}

u32 BindStencilSRVLocked(VideoState &s, HostTexture &host) {
  if (!host.texture || !host.isDepth || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (host.stencilDescriptorIndex != kInvalidDescriptorIndex)
    return host.stencilDescriptorIndex;
#if defined(EOT_D3D12)
  if (host.format != plume::RenderFormat::D32_FLOAT_S8_UINT)
    return kInvalidDescriptorIndex;
  const u32 slot = AllocateDescriptorSlot(s);
  if (slot == kInvalidDescriptorIndex) {
    EOT_ERROR("bindless texture heap full at {} slots", kBindlessTextureCount);
    return kInvalidDescriptorIndex;
  }
  D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
  desc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  if (host.sampleCount > 1) {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
  } else {
    desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    desc.Texture2D.PlaneSlice = 1;
  }
  auto *set = static_cast<plume::D3D12DescriptorSet *>(s.texture_descriptor_set.get());
  auto *texture = static_cast<plume::D3D12Texture *>(host.texture.get());
  set->setSRV(slot, texture->d3d, &desc);
  s.perf.host_views++;
  host.stencilDescriptorIndex = slot;
  return slot;
#else
  return kInvalidDescriptorIndex;
#endif
}

void ReleaseTextureSRVLocked(VideoState &s, HostTexture &host) {
  const u32 null_index = NullIndexFor(host);
  s.texture_generation.fetch_add(1, std::memory_order_relaxed);
  if (host.descriptorIndex != kInvalidDescriptorIndex) {
    s.descriptor_graveyard[s.recording_slot()].push_back({host.descriptorIndex, null_index});
    host.descriptorIndex = kInvalidDescriptorIndex;
  }
  if (host.stencilDescriptorIndex != kInvalidDescriptorIndex) {
    s.descriptor_graveyard[s.recording_slot()].push_back({host.stencilDescriptorIndex, null_index});
    host.stencilDescriptorIndex = kInvalidDescriptorIndex;
  }
  for (auto &[swizzle, srv] : host.swizzledSrvs) {
    if (srv.descriptorIndex != kInvalidDescriptorIndex)
      s.descriptor_graveyard[s.recording_slot()].push_back({srv.descriptorIndex, null_index});
    if (srv.view)
      ParkView(s, std::move(srv.view));
  }
  host.swizzledSrvs.clear();
}

u32 BindTextureSRVLocked(VideoState &s, HostTexture &host) {
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  if (host.descriptorIndex != kInvalidDescriptorIndex)
    return host.descriptorIndex;
  if (!host.srv) {
    s.perf.host_views++;
    host.srv = host.texture->createTextureView(SamplingViewDesc(host));
  }
  if (!host.srv)
    return kInvalidDescriptorIndex;
  host.descriptorIndex = PublishView(s, host, host.srv.get());
  return host.descriptorIndex;
}

u32 BindTextureSRVSwizzledLocked(VideoState &s, HostTexture &host, u32 swizzle) {
  swizzle &= 0xFFF;
  if (swizzle == kIdentityFetchSwizzle)
    return BindTextureSRVLocked(s, host);
  if (!host.texture || !s.texture_descriptor_set)
    return kInvalidDescriptorIndex;
  auto &entry = host.swizzledSrvs[swizzle];
  if (entry.descriptorIndex != kInvalidDescriptorIndex)
    return entry.descriptorIndex;
  if (!entry.view) {
    plume::RenderTextureViewDesc view_desc = SamplingViewDesc(host);
    view_desc.componentMapping = plume::RenderComponentMapping(
        ToHostSwizzle(swizzle), ToHostSwizzle(swizzle >> 3), ToHostSwizzle(swizzle >> 6),
        ToHostSwizzle(swizzle >> 9));
    s.perf.host_views++;
    entry.view = host.texture->createTextureView(view_desc);
  }
  if (!entry.view)
    return kInvalidDescriptorIndex;
  entry.descriptorIndex = PublishView(s, host, entry.view.get());
  return entry.descriptorIndex;
}

}
