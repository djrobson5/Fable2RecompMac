// gpu/shaders/runtime_stamp.h
// Fable 2: the stamp that says whether the translated shaders kept between runs
// (runtime_shaders.cpp) still fit the translator and shader_common.h in use.
//
// Header only, with nothing from the game in it, so a test can include it alone.

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

namespace eot::gpu::stamp {

// FNV-1a, 64 bit, over a stream of chunks (a file of tens of MB is hashed without being held in memory). The
// starting value is the one the first stamps used (FNV's offset basis with its last digit left off); it stays so that
// the hash of shader_common.h in a stamp of the earlier form still matches.
class Fnv {
 public:
  void Add(const uint8_t *bytes, size_t size) {
    for (size_t i = 0; i < size; ++i)
      h_ = (h_ ^ bytes[i]) * 1099511628211ull;
  }
  uint64_t Value() const { return h_; }

 private:
  uint64_t h_ = 1469598103934665603ull;
};

// The hash of a file's contents; the hash of no bytes when it cannot be read.
inline uint64_t FileHash(const std::filesystem::path &path) {
  Fnv h;
  std::ifstream in(path, std::ios::binary);
  std::vector<char> buffer(1 << 20);
  while (in.read(buffer.data(), std::streamsize(buffer.size())) || in.gcount() > 0)
    h.Add(reinterpret_cast<const uint8_t *>(buffer.data()), size_t(in.gcount()));
  return h.Value();
}

// "<shader_common.h> <translator> <shader compiler>", each the hash of the file's contents, as 16 hex digits.
// Judged by contents, not dates: a rebuild or reinstall that makes the same files keeps the translations (a dropped
// cache skips draws until they are translated again, and a draw the game makes only once, such as the hero's face
// texture at load, then stays wrong until the next load). A shader compiler that is not there counts as 0.
inline std::string Make(const std::filesystem::path &shader_common, const std::filesystem::path &translator,
                        const std::filesystem::path &compiler) {
  std::error_code ec;
  return std::format("{:016x} {:016x} {:016x}", FileHash(shader_common), FileHash(translator),
                     std::filesystem::exists(compiler, ec) ? FileHash(compiler) : 0ull);
}

// Writes the stamp through a temporary file and a rename, so a stop in the middle (the game is quit, the machine
// loses power) never leaves half a stamp: that would read as stale and drop every translation.
inline bool WriteAtomic(const std::filesystem::path &path, const std::string &text) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    out.flush();
    if (!out) {
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec)
    std::filesystem::remove(tmp, ec);
  return !ec;
}

enum class Verdict {
  Same,       // the translations fit
  CarryOver,  // the stamp of the form before 2026-11-22 for this shader_common.h: keep them once, write the new form
  Stale,      // drop them
};

// The earlier form was "<hash of shader_common.h> <translator's modification time>". With the same
// shader_common.h its translations are kept once (the translator itself did not change in any release up to
// then, except through the rebuilds that made the date change), and the stamp moves to the form above.
inline Verdict Judge(const std::string &old_stamp, const std::string &stamp) {
  if (old_stamp == stamp)
    return Verdict::Same;
  const bool older_form = std::count(old_stamp.begin(), old_stamp.end(), ' ') == 1 && old_stamp.size() > 17 &&
                          old_stamp[16] == ' ' && old_stamp.compare(0, 17, stamp, 0, 17) == 0 &&
                          std::all_of(old_stamp.begin() + 17, old_stamp.end(),
                                      [](char c) { return c >= '0' && c <= '9'; });
  return older_form ? Verdict::CarryOver : Verdict::Stale;
}

}  // namespace eot::gpu::stamp
