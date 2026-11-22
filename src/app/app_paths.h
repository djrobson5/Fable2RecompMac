// Fable 2: where the app (Fable 2.app, started with no arguments) keeps things.
//
// App mode = the executable sits in <name>.app/Contents/MacOS and no positional
// argument was given (-psn_* is ignored). Anything else (the build folder, a
// bench run, the bundle's executable with a game folder) keeps ReXGlue's
// defaults: everything beside the executable, saves under XDG_DATA_HOME.
//
// Nothing here may use cvars, logging or another translation unit's statics:
// the dry run (app_dry_run.cpp) calls it from a constructor, before main.

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace fable2::app {

struct Paths {
  bool app_mode = false;
  std::string mode_note;  // why app mode is on or off

  std::filesystem::path exe;     // the running executable
  std::filesystem::path bundle;  // <name>.app, app mode only

  // App mode only (empty otherwise).
  std::filesystem::path assets;          // <folder holding the .app>/assets (FABLE2_APP_ASSETS)
  std::filesystem::path data;            // FABLE2_APP_DATA_DIR or ~/Library/Application Support/Fable2
  std::filesystem::path caches;          // <data>/Caches with FABLE2_APP_DATA_DIR, else ~/Library/Caches/Fable2
  std::filesystem::path config;          // <data>/Fable2.toml
  std::filesystem::path logs;            // <data>/logs
  std::filesystem::path user;            // <data>/user (the SDK's user data root)
  std::filesystem::path cache;           // <caches>/cache (the SDK's cache root)
  std::filesystem::path shader_runtime;  // <caches>/shader_runtime
  std::filesystem::path xenosrecomp;     // Contents/Resources/XenosRecomp
  std::filesystem::path shader_common;   // Contents/Resources/shader_common.h
  std::filesystem::path dxc_lib;         // Contents/Resources/libdxcompiler.dylib
  std::filesystem::path vulkan_lib_dir;  // Contents/Frameworks (libvulkan.1.dylib, libMoltenVK.dylib)
  std::filesystem::path vulkan_icd;      // Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
  std::filesystem::path old_root;        // $XDG_DATA_HOME/Fable2 (default ~/.local/share/Fable2): the build folder's saves
  bool data_from_env = false;            // FABLE2_APP_DATA_DIR was set
};

// Resolved once, from the process's own argv and environment.
const Paths& GetPaths();
inline bool AppMode() { return GetPaths().app_mode; }

struct StartReport {
  std::vector<std::string> lines;  // what happened, for the log
  bool ok = true;                  // false when a step failed (the game can still start)
};

// The one-time work of an app's first start: the data and cache folders, the
// settings file (Metal backend, full screen, 1440p, MetalFX) when there is none,
// the build folder's saves and cache copied once. Copies are verified, never
// delete the source, and land by rename so a half copy is not taken for saves.
// With `write` false nothing is written; the lines say what would happen.
StartReport FirstStart(const Paths& paths, bool write);

// FirstStart(paths, true) once per process; the report is kept for the log.
const StartReport& EnsureFirstStart();

// One line of the settings file the first start writes: `name = "value"` for a string, `name = value` otherwise.
struct StartSetting {
  const char* name;
  const char* value;  // the text without quotes
  bool is_string;
};

// What the first start writes (Metal, full screen, 1440p, MetalFX). The one source of DefaultSettingsText() and of
// settings.json's "start" (app/settings_describe.cpp), so the two cannot drift.
std::span<const StartSetting> StartSettings();

// The settings file's text (first start), exposed for the dry run.
const char* DefaultSettingsText();

}  // namespace fable2::app
