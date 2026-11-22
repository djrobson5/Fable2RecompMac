// gpu/shaders/runtime_shaders.cpp
// Fable 2: shaders the built-in cache lacks, translated while the game runs
// (see runtime_shaders.h).

#include "gpu/shaders/runtime_shaders.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include "app/app_paths.h"
#include "core/logging.h"
#include "gpu/shaders/runtime_child.h"
#include "gpu/shaders/runtime_jobs.h"
#include "gpu/shaders/runtime_stamp.h"

REXCVAR_DEFINE_BOOL(fable2_runtime_shaders, true, "Fable2",
                    "Native renderer: translate shaders missing from the built-in cache while the game runs "
                    "(their draws were dropped: missing or black geometry in areas not captured before the build)");
REXCVAR_DEFINE_STRING(fable2_runtime_shader_dir, "", "Fable2",
                      "Native renderer: where translated shaders are kept between runs (default: "
                      "shader_runtime next to the executable)");
REXCVAR_DEFINE_STRING(fable2_xenosrecomp, "", "Fable2",
                      "Native renderer: the XenosRecomp executable for runtime shader translation (default: the "
                      "one this build made, or XenosRecomp next to the executable)");
REXCVAR_DEFINE_STRING(fable2_shader_common, "", "Fable2",
                      "Native renderer: shader_common.h for runtime shader translation (default: the one this "
                      "build used, or shader_common.h next to the executable)");
REXCVAR_DEFINE_INT32(fable2_runtime_shader_workers, 3, "Fable2",
                     "Native renderer: background threads translating missing shaders");
REXCVAR_DEFINE_BOOL(fable2_shader_wait, true, "Fable2",
                    "Native renderer: a draw whose shader is not translated yet waits for its translation instead "
                    "of being skipped (a draw the game makes only once, such as the hero's face texture at load, "
                    "is not made again)");
REXCVAR_DEFINE_INT32(fable2_shader_wait_max_ms, 5000, "Fable2",
                     "Native renderer: the longest a draw waits for its shaders' translations (all of them "
                     "together); after that the draw is skipped as without fable2_shader_wait, with a warning, "
                     "and the translation goes on for later draws");

namespace eot::gpu {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMagic = 0x43533246u;  // 'F2SC'
constexpr uint32_t kVersion = 1;
constexpr size_t kEntryBytes = 8 + 24 * 4;

struct Blob {
  std::vector<uint8_t> spirv;
  std::vector<uint32_t> layouts;
  std::vector<ShaderCacheEntry> entries;
};

// The jobs: the container to translate, by hash (runtime_jobs.h: queued, running, done or failed this session).
using JobBoard = jobs::Board<std::vector<uint8_t>>;

constexpr int kUrgentWorkers = 2;  // they take only the jobs a draw is waiting for

struct State {
  std::mutex mutex;  // the tables below; never held while a job runs or a draw waits
  std::deque<std::unique_ptr<Blob>> blobs;
  std::unordered_map<uint64_t, const ShaderCacheEntry *> by_hash;
  std::unordered_map<const ShaderCacheEntry *, const Blob *> by_entry;
  JobBoard board;
  std::vector<std::thread> workers;
  fs::path dir;
  bool initialized = false;
  std::mutex wait_mutex;  // the draws' waits since the last window line
  RuntimeShaderWaitStats wait_stats;
};

// Never destroyed: the workers are detached, and one that is finishing a job when the process exits must still
// find its board. Stop() (shutdown) makes them leave.
State &state() {
  static State &s = *new State;
  return s;
}

std::atomic<bool> g_stopping{false};  // shutdown: a running translator is killed
constexpr auto kTranslatorLimit = std::chrono::seconds(60);  // one translator run (they take 0.3 to 5 s)

// The cvar wins, then (Fable 2.app) the copy inside the bundle, then the one
// this build made, then one beside the executable. In app mode the build
// folder's copy must never be used: it exists on the machine that built the app.
fs::path FirstExisting(const std::string &override_path, const fs::path &app_path, const char *built,
                       const char *beside_exe) {
  if (!override_path.empty())
    return override_path;
  if (fable2::app::AppMode())
    return app_path;
  std::error_code ec;
  if (built && *built && fs::exists(built, ec))
    return built;
  return rex::filesystem::GetExecutableFolder() / beside_exe;
}

fs::path TranslatorPath() {
#ifdef FABLE2_XENOSRECOMP_EXE
  return FirstExisting(REXCVAR_GET(fable2_xenosrecomp), fable2::app::GetPaths().xenosrecomp, FABLE2_XENOSRECOMP_EXE,
                       "XenosRecomp");
#else
  return FirstExisting(REXCVAR_GET(fable2_xenosrecomp), fable2::app::GetPaths().xenosrecomp, nullptr, "XenosRecomp");
#endif
}

fs::path ShaderCommonPath() {
#ifdef FABLE2_SHADER_COMMON_H
  return FirstExisting(REXCVAR_GET(fable2_shader_common), fable2::app::GetPaths().shader_common,
                       FABLE2_SHADER_COMMON_H, "shader_common.h");
#else
  return FirstExisting(REXCVAR_GET(fable2_shader_common), fable2::app::GetPaths().shader_common, nullptr,
                       "shader_common.h");
#endif
}

uint32_t Read32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// Parses a --blob file into a Blob; false if it is malformed. Empty (0 entries)
// means XenosRecomp could not translate the shader.
bool ParseBlob(const std::vector<uint8_t> &b, Blob &out) {
  if (b.size() < 20 || Read32(b.data()) != kMagic || Read32(b.data() + 4) != kVersion)
    return false;
  const uint32_t count = Read32(b.data() + 8), layout_words = Read32(b.data() + 12),
                 spirv_bytes = Read32(b.data() + 16);
  const size_t need = 20 + size_t(count) * kEntryBytes + size_t(layout_words) * 4 + spirv_bytes;
  if (count > 4096 || b.size() != need)
    return false;
  const uint8_t *p = b.data() + 20;
  out.entries.resize(count);
  for (uint32_t i = 0; i < count; ++i, p += kEntryBytes) {
    ShaderCacheEntry &e = out.entries[i];
    std::memcpy(&e.hash, p, 8);
    uint32_t f[24];
    std::memcpy(f, p + 8, sizeof(f));
    e.dxilOffset = f[0];
    e.dxilSize = f[1];
    e.spirvOffset = f[2];
    e.spirvSize = f[3];
    e.specConstantsMask = f[4];
    e.vertexLayoutOffset = f[5];
    e.vertexLayoutCount = f[6];
    e.vfetchCodeOffset = f[7];
    e.usesFloatConstants = f[8];
    e.interpolantMask = f[9];
    e.fullDxilOffset = f[10];
    e.fullDxilSize = f[11];
    e.fullSpirvOffset = f[12];
    e.fullSpirvSize = f[13];
    e.posDxilOffset = f[14];
    e.posDxilSize = f[15];
    e.posSpirvOffset = f[16];
    e.posSpirvSize = f[17];
    e.velDxilOffset = f[18];
    e.velDxilSize = f[19];
    e.velSpirvOffset = f[20];
    e.velSpirvSize = f[21];
    e.textureFetchMask = f[22];
    e.shaderFlags = f[23];
    auto in_spirv = [&](uint32_t off, uint32_t size) { return size == 0 || (off <= spirv_bytes && size <= spirv_bytes - off); };
    if (!in_spirv(e.spirvOffset, e.spirvSize) || !in_spirv(e.fullSpirvOffset, e.fullSpirvSize) ||
        !in_spirv(e.posSpirvOffset, e.posSpirvSize) || !in_spirv(e.velSpirvOffset, e.velSpirvSize) ||
        e.spirvSize == 0 || uint64_t(e.vertexLayoutOffset) + uint64_t(e.vertexLayoutCount) * 2 > layout_words)
      return false;
  }
  out.layouts.resize(layout_words);
  std::memcpy(out.layouts.data(), p, size_t(layout_words) * 4);
  p += size_t(layout_words) * 4;
  out.spirv.assign(p, p + spirv_bytes);
  return true;
}

std::vector<uint8_t> ReadFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Adds a parsed blob's entries to the table; returns how many were new.
size_t Publish(std::unique_ptr<Blob> blob) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  size_t added = 0;
  for (const ShaderCacheEntry &e : blob->entries)
    if (s.by_hash.emplace(e.hash, &e).second) {
      s.by_entry.emplace(&e, blob.get());
      s.board.MarkDone(e.hash);  // (the board's lock is taken under this one, never the other way round)
      ++added;
    }
  s.blobs.push_back(std::move(blob));
  return added;
}

// Runs XenosRecomp --blob on `in`; a run that fails to start, exits with a status or runs over the limit (a hung
// translator would hold a worker, and the draws waiting for it, for good) is killed and run once more.
bool RunTranslator(const fs::path &in, const fs::path &out, bool urgent, std::string *error) {
  const std::vector<std::string> argv = {TranslatorPath().string(), in.string(), out.string(),
                                         ShaderCommonPath().string(), "--blob"};
  int attempts = 0;
  const child::Result r = child::RunRetrying(argv, urgent, kTranslatorLimit, &g_stopping, &attempts);
  if (r.outcome == child::Outcome::Ok)
    return true;
  *error = attempts > 1 ? r.error + " (after a second try)" : r.error;
  return false;
}

// Translates one container and publishes the result. False when there is none.
bool Translate(uint64_t hash, const std::vector<uint8_t> &container, bool urgent) {
  auto &s = state();
  const auto start = std::chrono::steady_clock::now();
  const std::string name = std::format("{:016x}", hash);
  std::error_code ec;
  const fs::path in = s.dir / "in" / (name + ".bin");
  const fs::path tmp = s.dir / (name + ".f2sc.tmp");
  const fs::path out = s.dir / (name + ".f2sc");
  {
    std::ofstream f(in, std::ios::binary);
    f.write(reinterpret_cast<const char *>(container.data()), std::streamsize(container.size()));
  }
  std::string error;
  if (!RunTranslator(in, tmp, urgent, &error)) {
    EOT_WARN("[shaders] runtime translation of {} failed: {}", name, error);
    fs::remove(in, ec);
    fs::remove(tmp, ec);
    return false;
  }
  auto blob = std::make_unique<Blob>();
  if (!ParseBlob(ReadFile(tmp), *blob)) {
    EOT_WARN("[shaders] runtime translation of {}: unreadable output", name);
    fs::remove(in, ec);
    fs::remove(tmp, ec);
    return false;
  }
  if (blob->entries.empty()) {
    EOT_WARN("[shaders] runtime translation of {}: XenosRecomp could not translate it (its draws stay dropped)",
             name);
    fs::remove(in, ec);
    fs::remove(tmp, ec);
    return false;
  }
  fs::rename(tmp, out, ec);
  fs::remove(in, ec);
  Publish(std::move(blob));
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  EOT_INFO("[shaders] runtime: {} translated in {:.0f} ms{} (kept in {})", name, ms,
           urgent ? " for a waiting draw" : "", s.dir.string());
  return true;
}

void Worker(bool urgent_only) {
  auto &s = state();
  uint64_t hash = 0;
  std::vector<uint8_t> container;
  bool urgent = false;
  while (s.board.Take(urgent_only, &hash, &container, &urgent)) {
    const bool ok = Translate(hash, container, urgent);
    s.board.Finish(hash, ok);  // after Translate published it: a waiter that sees Done finds the entry
  }
}

}  // namespace

void RuntimeShadersInit() {
  auto &s = state();
  {
    std::lock_guard lock(s.mutex);
    if (s.initialized)
      return;
    s.initialized = true;
    const std::string &dir = REXCVAR_GET(fable2_runtime_shader_dir);
#if defined(FABLE2_CONSTANT_BUFFERS)
    // Translations differ by how shaders read constants: never mix the two kinds.
    // (Fable 2.app: <caches>/shader_runtime.)
    s.dir = !dir.empty() ? fs::path(dir) / "cb"
            : fable2::app::AppMode() ? fable2::app::GetPaths().shader_runtime / "cb"
                                     : rex::filesystem::GetExecutableFolder() / "shader_runtime_cb";
#else
    s.dir = !dir.empty() ? fs::path(dir)
            : fable2::app::AppMode() ? fable2::app::GetPaths().shader_runtime
                                     : rex::filesystem::GetExecutableFolder() / "shader_runtime";
#endif
  }
  if (!REXCVAR_GET(fable2_runtime_shaders))
    return;
  std::error_code ec;
  fs::create_directories(s.dir / "in", ec);
  {
    // Translations from another shader_common.h, translator or shader compiler are
    // stale (the helpers' interface with the renderer changes): drop them. The
    // translator and the compiler sit beside each other in the app's Resources.
    const fs::path translator = TranslatorPath();
    const std::string current =
        stamp::Make(ShaderCommonPath(), translator, translator.parent_path() / "libdxcompiler.dylib");
    const fs::path stamp_path = s.dir / "stamp.txt";
    const std::vector<uint8_t> old_bytes = ReadFile(stamp_path);
    const stamp::Verdict verdict = stamp::Judge(std::string(old_bytes.begin(), old_bytes.end()), current);
    if (verdict == stamp::Verdict::CarryOver) {
      stamp::WriteAtomic(stamp_path, current);
      EOT_INFO("[shaders] runtime: kept earlier translations (the stamp is now by contents, not dates)");
    } else if (verdict == stamp::Verdict::Stale) {
      size_t dropped = 0;
      for (const auto &item : fs::directory_iterator(s.dir, ec))
        if (item.path().extension() == ".f2sc" && fs::remove(item.path(), ec))
          ++dropped;
      stamp::WriteAtomic(stamp_path, current);
      EOT_INFO("[shaders] runtime: shader_common.h, the translator or the shader compiler changed; dropped {} "
               "earlier translations",
               dropped);
    } else {
      EOT_INFO("[shaders] runtime: translations from earlier runs fit this translator (stamp {})", current);
    }
  }
  size_t files = 0, entries = 0;
  for (const auto &item : fs::directory_iterator(s.dir, ec)) {
    if (item.path().extension() != ".f2sc")
      continue;
    auto blob = std::make_unique<Blob>();
    if (!ParseBlob(ReadFile(item.path()), *blob) || blob->entries.empty()) {
      fs::remove(item.path(), ec);
      continue;
    }
    ++files;
    entries += Publish(std::move(blob));
  }
  EOT_INFO("[shaders] runtime: {} shaders from earlier runs ({} files) in {}; translator {}", entries, files,
           s.dir.string(), TranslatorPath().string());
}

void RuntimeShaderRequest(uint64_t hash, const uint8_t *virtual_bytes, uint32_t virtual_size,
                          const uint8_t *physical_bytes, uint32_t physical_size) {
  if (!REXCVAR_GET(fable2_runtime_shaders) || !virtual_bytes || !virtual_size)
    return;
  RuntimeShadersInit();
  auto &s = state();
  std::vector<uint8_t> container(virtual_bytes, virtual_bytes + virtual_size);
  if (physical_bytes && physical_size)
    container.insert(container.end(), physical_bytes, physical_bytes + physical_size);
  if (!s.board.Add(hash, std::move(container)))
    return;  // queued, running, done or failed already
  {
    std::lock_guard lock(s.mutex);
    if (s.workers.empty()) {
      const int n = std::max(1, std::min(8, int(REXCVAR_GET(fable2_runtime_shader_workers))));
      for (int i = 0; i < n; ++i)
        s.workers.emplace_back(Worker, false).detach();
      for (int i = 0; i < kUrgentWorkers; ++i)
        s.workers.emplace_back(Worker, true).detach();
    }
  }
  EOT_INFO("[shaders] runtime: {:016x} is not in the built-in cache, translating it", hash);
}

void RuntimeShadersStop() {
  g_stopping.store(true, std::memory_order_release);
  state().board.Stop();
}

bool RuntimeShaderWaitAll(const uint64_t *hashes, size_t count) {
  if (!REXCVAR_GET(fable2_shader_wait) || !REXCVAR_GET(fable2_runtime_shaders) || g_stopping.load())
    return false;
  auto &s = state();
  // Look at every shader first. A draw that cannot have all of them (one failed, one already ran its wait out,
  // one was never asked for) has nothing to wait for: it is skipped now, not after the others' translations.
  for (size_t i = 0; i < count; ++i) {
    const jobs::Peek p = s.board.Look(hashes[i]);
    if (p != jobs::Peek::Pending && p != jobs::Peek::Done)
      return false;
  }
  for (size_t i = 0; i < count; ++i)
    s.board.Prioritize(hashes[i]);  // together, so they translate side by side
  // One limit for the whole draw, shared by the shaders.
  const int cap_ms = std::clamp(int(REXCVAR_GET(fable2_shader_wait_max_ms)), 0, 600000);
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::milliseconds(cap_ms);
  bool ready = true;
  bool timed_out = false;
  uint64_t blamed = 0;
  for (size_t i = 0; i < count && ready; ++i) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    const jobs::WaitResult r = s.board.Wait(hashes[i], std::max(left, std::chrono::milliseconds(0)));
    ready = r == jobs::WaitResult::Done;
    timed_out = r == jobs::WaitResult::TimedOut;
    blamed = hashes[i];
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  if (ms >= 1.0) {
    std::lock_guard lock(s.wait_mutex);
    ++s.wait_stats.waits;
    s.wait_stats.total_ms += ms;
    s.wait_stats.max_ms = std::max(s.wait_stats.max_ms, ms);
    if (timed_out)
      ++s.wait_stats.gave_up;
  }
  if (ready) {
    if (ms >= 1.0)
      EOT_INFO("[shaders] a draw waited {:.0f} ms for the translation of {:016x}{}", ms, hashes[0],
               count > 1 ? std::format(" and {:016x}", hashes[1]) : std::string());
  } else if (timed_out && ms >= 1.0) {
    EOT_WARN("[shaders] gave up waiting for the translation of {:016x} after {:.0f} ms (fable2_shader_wait_max_ms): "
             "its draws are skipped until it lands",
             blamed, ms);
  }
  return ready;
}

RuntimeShaderWaitStats RuntimeShaderTakeWaitStats() {
  auto &s = state();
  std::lock_guard lock(s.wait_mutex);
  RuntimeShaderWaitStats out = s.wait_stats;
  s.wait_stats = RuntimeShaderWaitStats{};
  return out;
}

const ShaderCacheEntry *RuntimeShaderEntry(uint64_t hash) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.by_hash.find(hash);
  return it == s.by_hash.end() ? nullptr : it->second;
}

const uint8_t *RuntimeShaderBlob(const ShaderCacheEntry *e) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.by_entry.find(e);
  return it == s.by_entry.end() ? nullptr : it->second->spirv.data();
}

const uint32_t *RuntimeShaderLayouts(const ShaderCacheEntry *e) {
  auto &s = state();
  std::lock_guard lock(s.mutex);
  auto it = s.by_entry.find(e);
  return it == s.by_entry.end() ? nullptr : it->second->layouts.data();
}

}  // namespace eot::gpu
