// gpu/queries.h
// Fable 2: XDK occlusion queries on host occlusion query pools.

#pragma once

#include <rex/types.h>

namespace eot::gpu {

struct VideoState;

// Render thread (video lock held).
void QueriesCommandListBegunLocked(VideoState &s);
void QueriesChunkListBegunLocked(VideoState &s);
void QueryBeginLocked(VideoState &s, u32 query_va, u32 generation);
void QueryEndLocked(VideoState &s, u32 query_va, u32 generation);
// Render thread: a host occlusion query is open.
bool QueryActiveLocked();
void QueriesBeforeSubmitLocked(VideoState &s);
void QueriesSlotRecycledLocked(VideoState &s, u32 slot);
// GPU-done markers: the slot's serial when its frame is submitted, and (from
// the marker thread, once the GPU has finished that submit) its counts published
// before the render thread recycles it. No-op if it was recycled meanwhile.
u64 QueriesSlotSerial(u32 slot);
void QueriesPublishSubmitted(u32 slot, u64 serial);

// Guest threads.
void QueryIssued(u32 query_va, bool begin, bool end);
// Fable 2: draws inside an open query; rectangle-list (fullscreen) passes mark
// it as an auto-exposure query, whose real count GetData must return even when
// culling queries are forced visible.
void QueryNoteDraw(bool rect_list);
u32 QueryOpenVa();
void QueryFlickerStats(u64 *flicker, u64 *zero, u64 *published);
bool QueryIsExposure(u32 query_va);
void QueryGetDataCalled(bool orig_ready, bool occlusion);
bool QueryResult(u32 query_va, u64 *samples);
// The sun glare's visibility queries: real counts even with fable2_query_force_visible.
void QueryMarkSun(u32 query_va);
bool QueryIsSun(u32 query_va);

}
