// gpu/shaders/runtime_shaders.h
// Fable 2: shaders the built-in cache lacks, translated while the game runs.
//
// The built-in cache holds the shaders of the game's shader bank and any captured
// before the build (the vertex shaders the console patches when it binds them are
// not in the bank as the game then hashes them). A shader met for the first time
// (a new area) used to drop its draws (black ground in the cave). Now the
// container goes to a background worker that runs XenosRecomp --blob on it; the
// result is loaded into a runtime table and kept on disk
// (fable2_runtime_shader_dir), so later launches have it from the start. A draw
// that needs a shader not translated yet waits for it (RuntimeShaderWait) rather
// than being skipped.

#pragma once

#include <cstddef>
#include <cstdint>

#include "gpu/shaders/shader_cache.h"

namespace eot::gpu {

// Loads the shaders translated in earlier runs (once; later calls do nothing).
void RuntimeShadersInit();

// Queues the translation of a container (exactly the hashed bytes) unless it
// is known, queued, or failed before. Thread safe.
void RuntimeShaderRequest(uint64_t hash, const uint8_t *virtual_bytes, uint32_t virtual_size,
                          const uint8_t *physical_bytes, uint32_t physical_size);

// Shutdown: the translator children are killed, the workers leave and every waiting draw goes on (skipped). Call
// it before the renderer's threads are stopped. Thread safe; later requests and waits do nothing.
void RuntimeShadersStop();

// A draw whose shaders have no translation yet waits for them (fable2_shader_wait) instead of being skipped.
// `hashes` are the shaders the draw lacks (one or two). Thread safe; the game's drawing thread calls it. True when
// every one is translated (RuntimeShaderEntry has them); false when the draw stays skipped: waiting is off, one of
// them failed, ran its wait out before or was never asked for (none of these waits at all), or the translations
// did not land within fable2_shader_wait_max_ms for the whole draw (the translation goes on; later draws attach it).
// Nothing is held by the caller, and the translator runs as a child process the waiter does not depend on, so a
// wait cannot deadlock; it ends with the translations, a failure, the limit or Stop.
bool RuntimeShaderWaitAll(const uint64_t *hashes, size_t count);

// The waits since the last call (the [shaders] line of each [perf] window).
struct RuntimeShaderWaitStats {
  uint32_t waits = 0;    // draws that waited
  uint32_t gave_up = 0;  // of them, that ran out the limit
  double total_ms = 0;   // the sum of the waits
  double max_ms = 0;     // the longest
};
RuntimeShaderWaitStats RuntimeShaderTakeWaitStats();

// The translated entry, or nullptr while it is not ready. Thread safe.
const ShaderCacheEntry *RuntimeShaderEntry(uint64_t hash);

// For entries RuntimeShaderEntry returned: the bases their SPIR-V and vertex
// layout offsets count from (nullptr for built-in entries).
const uint8_t *RuntimeShaderBlob(const ShaderCacheEntry *e);
const uint32_t *RuntimeShaderLayouts(const ShaderCacheEntry *e);

}  // namespace eot::gpu
