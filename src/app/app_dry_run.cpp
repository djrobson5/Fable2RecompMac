// Fable 2: FABLE2_APP_DRY_RUN=1 resolves everything Fable 2.app would use, prints it
// and exits, before main (so before SDL, a window or a Dock icon).
//
//   env -i HOME=$HOME FABLE2_APP_DATA_DIR=<scratch> FABLE2_APP_DRY_RUN=1 \
//       "Fable 2.app/Contents/MacOS/Fable2"
//
// Why this is before any window: the work is an __attribute__((constructor)) of
// the executable, which dyld runs before main. SDL is first touched in main
// (rex::ui::SDLWindowedAppContext::Initialize: SDL_InitSubSystem(VIDEO)), and
// nothing reachable from here, or from the static initializers of this
// executable or librexruntime, calls it. The code below only reads the
// environment and files; it uses no cvars and no logging.
//
// First-start work (the settings file, the one-time copies) is done for real
// only when FABLE2_APP_DATA_DIR names a scratch folder; otherwise the dry run
// reports what it would do and writes nothing. Outside an app bundle it writes
// nothing at all.
//
// Exit: 0 fine, 1 something required is missing or a first-start step failed.

#include "app/app_paths.h"

#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;
using fable2::app::Paths;

bool Exists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}

bool IsExecutable(const fs::path& p) {
  return access(p.c_str(), X_OK) == 0;
}

// A string value of the bundle's Info.plist ("" when there is none).
std::string PlistString(const fs::path& plist, const char* key) {
  std::string out;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(plist.c_str()),
                                                         CFIndex(plist.string().size()), false);
  if (!url)
    return out;
  CFDataRef data = nullptr;
  SInt32 error = 0;
  if (CFURLCreateDataAndPropertiesFromResource(nullptr, url, &data, nullptr, nullptr, &error) && data) {
    CFPropertyListRef list = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
    if (list && CFGetTypeID(list) == CFDictionaryGetTypeID()) {
      CFStringRef k = CFStringCreateWithCString(nullptr, key, kCFStringEncodingUTF8);
      CFTypeRef value = CFDictionaryGetValue(static_cast<CFDictionaryRef>(list), k);
      if (value && CFGetTypeID(value) == CFStringGetTypeID()) {
        char buffer[512];
        if (CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof buffer, kCFStringEncodingUTF8))
          out = buffer;
      }
      CFRelease(k);
    }
    if (list)
      CFRelease(list);
    CFRelease(data);
  }
  CFRelease(url);
  return out;
}

int g_missing = 0;

void Show(const char* label, const fs::path& p) {
  std::printf("  %-16s %s%s\n", label, p.c_str(), Exists(p) ? "" : "   (does not exist yet)");
}

// A file the app cannot run without.
void Require(const char* label, const fs::path& p, bool executable = false) {
  const bool ok = Exists(p) && (!executable || IsExecutable(p));
  std::printf("  %-16s %s   %s\n", label, p.c_str(), ok ? "ok" : "MISSING");
  if (!ok)
    ++g_missing;
}

int DryRun() {
  const Paths& p = fable2::app::GetPaths();
  std::printf("Fable2 dry run (pid %d)\n", int(getpid()));
  std::printf("app mode: %s\n", p.app_mode ? "yes" : "no");
  std::printf("  why              %s\n", p.mode_note.c_str());
  std::printf("  executable       %s\n", p.exe.c_str());
  if (!p.app_mode) {
    std::printf("dev mode: ReXGlue's defaults apply (everything beside the executable); nothing was written\n");
    return 0;
  }

  std::printf("bundle: %s\n", p.bundle.c_str());
  const fs::path plist = p.bundle / "Contents" / "Info.plist";
  const std::string commit = PlistString(plist, "Fable2SourceCommit");
  const std::string base = PlistString(plist, "Fable2UpstreamBase");
  std::printf("  Fable2SourceCommit   %s\n", commit.empty() ? "(none)" : commit.c_str());
  std::printf("  Fable2UpstreamBase   %s\n", base.empty() ? "(none)" : base.c_str());

  std::printf("folders (%s):\n", p.data_from_env ? "FABLE2_APP_DATA_DIR" : "the user's own");
  Show("data", p.data);
  Show("settings", p.config);
  Show("logs", p.logs);
  Show("user data", p.user);
  Show("caches", p.caches);
  Show("cache", p.cache);
  Show("shader_runtime", p.shader_runtime);
  Show("old saves root", p.old_root);

  std::printf("game files:\n");
  Require("assets/game", p.assets / "game" / "default.xex");
  std::printf("  %-16s %s%s\n", "assets/update", (p.assets / "update").c_str(),
              Exists(p.assets / "update" / "default.xexp") ? "   (default.xexp present)" : "   (no default.xexp)");

  std::printf("helpers:\n");
  Require("XenosRecomp", p.xenosrecomp, true);
  Require("shader_common.h", p.shader_common);
  Require("libdxcompiler", p.dxc_lib);
  Require("libvulkan", p.vulkan_lib_dir / "libvulkan.1.dylib");
  Require("libMoltenVK", p.vulkan_lib_dir / "libMoltenVK.dylib");
  Require("Vulkan ICD", p.vulkan_icd);
  Require("librexruntime", p.vulkan_lib_dir / "librexruntime.dylib");

  const bool write = p.data_from_env;
  std::printf("first start (%s):\n", write ? "done for real, in FABLE2_APP_DATA_DIR"
                                            : "dry: FABLE2_APP_DATA_DIR is not set, nothing is written");
  const fable2::app::StartReport report = fable2::app::FirstStart(p, write);
  for (const std::string& line : report.lines)
    std::printf("  %s\n", line.c_str());
  if (!report.ok) {
    std::printf("  FAILED\n");
    ++g_missing;
  }

  std::printf("result: %s\n", g_missing ? "something required is missing" : "ok");
  return g_missing ? 1 : 0;
}

__attribute__((constructor)) void MaybeDryRun() {
  const char* v = std::getenv("FABLE2_APP_DRY_RUN");
  if (!v || std::strcmp(v, "1") != 0)
    return;
  const int code = DryRun();
  std::fflush(stdout);
  std::fflush(stderr);
  _exit(code);
}

}  // namespace
