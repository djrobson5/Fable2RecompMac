// Fable 2: the app's paths and first-start work (see app_paths.h).

#include "app/app_paths.h"

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <system_error>

namespace fable2::app {

namespace fs = std::filesystem;

namespace {

std::string Env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}

fs::path HomeDir() {
  if (const std::string home = Env("HOME"); !home.empty())
    return home;
  if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir)
    return pw->pw_dir;
  return {};
}

fs::path ExecutablePath() {
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  if (!size)
    return {};
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0)
    return {};
  buffer.resize(std::strlen(buffer.c_str()));
  std::error_code ec;
  fs::path canonical = fs::weakly_canonical(buffer, ec);
  return ec ? fs::path(buffer) : canonical;
}

// A positional argument is anything that is not an option; -psn_* (the old
// launch-services serial number) does not count.
bool HasPositionalArgument() {
  const int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!a || !*a)
      continue;
    if (a[0] == '-')
      continue;  // includes -psn_*
    return true;
  }
  return false;
}

Paths Resolve() {
  Paths p;
  p.exe = ExecutablePath();
  const fs::path exe_dir = p.exe.parent_path();
  const fs::path contents = exe_dir.parent_path();
  const fs::path bundle = contents.parent_path();
  const bool in_bundle = exe_dir.filename() == "MacOS" && contents.filename() == "Contents" &&
                         bundle.extension() == ".app";
  if (!in_bundle) {
    p.mode_note = "off: the executable is not in an .app bundle's Contents/MacOS";
    return p;
  }
  if (HasPositionalArgument()) {
    p.mode_note = "off: a positional argument was given";
    return p;
  }
  p.app_mode = true;
  p.mode_note = "on: inside " + bundle.filename().string() + " with no positional argument";
  p.bundle = bundle;

  const fs::path home = HomeDir();
  const std::string data_env = Env("FABLE2_APP_DATA_DIR");
  p.data_from_env = !data_env.empty();
  p.data = p.data_from_env ? fs::path(data_env) : home / "Library" / "Application Support" / "Fable2";
  p.caches = p.data_from_env ? p.data / "Caches" : home / "Library" / "Caches" / "Fable2";
  const std::string assets_env = Env("FABLE2_APP_ASSETS");
  p.assets = !assets_env.empty() ? fs::path(assets_env) : bundle.parent_path() / "assets";
  p.config = p.data / "Fable2.toml";
  p.logs = p.data / "logs";
  p.user = p.data / "user";
  p.cache = p.caches / "cache";
  p.shader_runtime = p.caches / "shader_runtime";

  const fs::path resources = contents / "Resources";
  p.xenosrecomp = resources / "XenosRecomp";
  p.shader_common = resources / "shader_common.h";
  p.dxc_lib = resources / "libdxcompiler.dylib";
  p.vulkan_lib_dir = contents / "Frameworks";
  p.vulkan_icd = resources / "vulkan" / "icd.d" / "MoltenVK_icd.json";

  const std::string xdg = Env("XDG_DATA_HOME");
  p.old_root = (!xdg.empty() ? fs::path(xdg) : home / ".local" / "share") / "Fable2";
  return p;
}

// ---- tree copy and check ----

struct TreeFacts {
  std::map<std::string, int64_t> entries;  // relative path -> size (-1 for a folder)
  uint64_t files = 0;
  uint64_t bytes = 0;
};

bool Scan(const fs::path& root, TreeFacts* out) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(root, fs::directory_options::none, ec), end; it != end;
       it.increment(ec)) {
    if (ec)
      return false;
    const fs::path rel = it->path().lexically_relative(root);
    if (it->is_directory(ec)) {
      out->entries[rel.string()] = -1;
    } else {
      const auto size = it->file_size(ec);
      if (ec)
        return false;
      out->entries[rel.string()] = static_cast<int64_t>(size);
      out->files++;
      out->bytes += size;
    }
  }
  return !ec;
}

bool SameContent(const fs::path& a, const fs::path& b) {
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  if (!fa || !fb)
    return false;
  char ba[65536], bb[65536];
  while (fa && fb) {
    fa.read(ba, sizeof ba);
    fb.read(bb, sizeof bb);
    if (fa.gcount() != fb.gcount() || std::memcmp(ba, bb, size_t(fa.gcount())) != 0)
      return false;
  }
  return fa.eof() == fb.eof();
}

// Same file list, sizes and contents.
bool Verify(const fs::path& a, const fs::path& b, TreeFacts* facts, std::string* why) {
  TreeFacts fa, fb;
  if (!Scan(a, &fa) || !Scan(b, &fb)) {
    *why = "cannot read a folder";
    return false;
  }
  if (fa.entries != fb.entries) {
    *why = "file lists or sizes differ";
    return false;
  }
  for (const auto& [rel, size] : fa.entries) {
    if (size >= 0 && !SameContent(a / rel, b / rel)) {
      *why = "contents differ: " + rel;
      return false;
    }
  }
  *facts = fa;
  return true;
}

// src -> dst through <dst>.copying: copy, verify, rename. The source stays.
bool CopyOnce(const fs::path& src, const fs::path& dst, StartReport* r) {
  std::error_code ec;
  const fs::path tmp = dst.parent_path() / (dst.filename().string() + ".copying");
  fs::remove_all(tmp, ec);
  fs::create_directories(dst.parent_path(), ec);
  ec.clear();
  fs::copy(src, tmp, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
  if (ec) {
    r->lines.push_back("copy failed " + src.string() + ": " + ec.message());
    r->ok = false;
    fs::remove_all(tmp, ec);
    return false;
  }
  TreeFacts facts;
  std::string why;
  if (!Verify(src, tmp, &facts, &why)) {
    r->lines.push_back("copy of " + src.string() + " did not verify (" + why + "); not used");
    r->ok = false;
    fs::remove_all(tmp, ec);
    return false;
  }
  fs::rename(tmp, dst, ec);
  if (ec) {
    r->lines.push_back("copy of " + src.string() + " could not be put in place: " + ec.message());
    r->ok = false;
    fs::remove_all(tmp, ec);
    return false;
  }
  r->lines.push_back("copied " + src.string() + " -> " + dst.string() + " (" + std::to_string(facts.files) +
                     " files, " + std::to_string(facts.bytes) + " bytes), verified");
  return true;
}

bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

bool SamePlace(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return fs::equivalent(a, b, ec);
}

}  // namespace

const char* DefaultSettingsText() {
  return "# Fable II: the app's settings, written once at its first start (Metal, full screen,\n"
         "# rendering at 1080p, MetalFX upscaling). The in-game settings overlay (F4) rewrites\n"
         "# this file, and what it saves wins from then on.\n"
         "fable2_gpu_backend = \"metal\"\n"
         "fullscreen = true\n"
         "eot_resolution = \"1080p\"\n"
         "eot_upscale = \"metalfx\"\n";
}

const Paths& GetPaths() {
  static const Paths paths = Resolve();
  return paths;
}

StartReport FirstStart(const Paths& p, bool write) {
  StartReport r;
  if (!p.app_mode) {
    r.lines.push_back("first start: not in app mode, nothing to do");
    return r;
  }
  const char* would = write ? "" : "would ";
  std::error_code ec;

  if (write) {
    for (const fs::path& dir : {p.data, p.caches, p.logs}) {
      fs::create_directories(dir, ec);
      if (ec) {
        r.lines.push_back("cannot create " + dir.string() + ": " + ec.message());
        r.ok = false;
        return r;
      }
    }
  }

  // Settings, once.
  if (Exists(p.config)) {
    r.lines.push_back("settings: " + p.config.string() + " exists, left alone");
  } else if (write) {
    const fs::path tmp = p.config.string() + ".new";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << DefaultSettingsText();
      if (!out) {
        r.lines.push_back("settings: cannot write " + tmp.string());
        r.ok = false;
      }
    }
    if (r.ok) {
      fs::rename(tmp, p.config, ec);
      if (ec) {
        r.lines.push_back("settings: cannot put " + p.config.string() + " in place: " + ec.message());
        r.ok = false;
      } else {
        r.lines.push_back("settings: wrote " + p.config.string() + " (Metal, full screen, 1080p, MetalFX)");
      }
    }
  } else {
    r.lines.push_back("settings: would write " + p.config.string() + " (Metal, full screen, 1080p, MetalFX)");
  }

  // The build folder's saves, once: only when the app has none and the old root has the hero save.
  static const char* const kSaveItems[] = {"B13EBABEBABEBABE", "4D5307F1", "achievements"};
  if (SamePlace(p.old_root, p.user)) {
    r.lines.push_back("saves: the old root is the user data root, nothing to copy");
  } else if (Exists(p.user / "B13EBABEBABEBABE")) {
    r.lines.push_back("saves: " + p.user.string() + " already has saves, nothing copied");
  } else if (!Exists(p.old_root / "B13EBABEBABEBABE")) {
    r.lines.push_back("saves: no hero save in " + p.old_root.string() + ", nothing to copy");
  } else {
    for (const char* item : kSaveItems) {
      const fs::path src = p.old_root / item, dst = p.user / item;
      if (!Exists(src))
        continue;
      if (Exists(dst)) {
        r.lines.push_back(std::string("saves: ") + item + " is already in " + p.user.string() + ", kept");
      } else if (write) {
        CopyOnce(src, dst, &r);
      } else {
        r.lines.push_back(std::string("saves: ") + would + "copy " + src.string() + " -> " + dst.string());
      }
    }
  }

  // The build folder's cache, once.
  const fs::path old_cache = p.old_root / "cache";
  if (Exists(p.cache)) {
    r.lines.push_back("cache: " + p.cache.string() + " exists, nothing copied");
  } else if (!Exists(old_cache)) {
    r.lines.push_back("cache: no cache in " + p.old_root.string() + ", nothing to copy");
  } else if (write) {
    CopyOnce(old_cache, p.cache, &r);
  } else {
    r.lines.push_back(std::string("cache: ") + would + "copy " + old_cache.string() + " -> " + p.cache.string());
  }
  return r;
}

const StartReport& EnsureFirstStart() {
  static const StartReport report = FirstStart(GetPaths(), true);
  return report;
}

}  // namespace fable2::app
