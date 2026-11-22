// Fable 2: `Fable2 --describe-settings` prints the player settings as JSON and exits, for a launcher (Wayfarer) that
// draws them as a page. tools/mac/package_app.sh writes the output to Fable 2.app/Contents/Resources/settings.json.
//
//   {"settings": [ {section, key, type, default, label, help, page, advanced, min, max, choices, choice_labels}, ... ],
//    "start": {cvar: value text, ...}}
//
// The rows are curated here (which cvars, their labels, help, pages and ranges); each cvar's own type, default and
// allowed values come from the cvar registry (rex/cvar.h), so a default changed in the source changes here too.
// "start" is what the app writes into Fable2.toml at its first start (app/app_paths.h StartSettings(), the same table
// that makes the file's text). Fable2.toml is flat, so every row's section is "".
//
// When it runs: this is an __attribute__((constructor)) of the executable, so before main (which is the SDK's:
// rex::cvar::Init, logging, SDL, ReXApp) and before the SDK's command-line parsing. The cvars are registered by static
// initializers, and a constructor runs in link order with them: the registry is complete only if this file is linked
// after every file that defines a cvar of the list (CMakeLists.txt adds it last). A curated cvar missing from the
// registry is a hard failure, so a wrong order shows at packaging, never as a short settings.json.
//
// Nothing here opens a window, a log file or a settings file, and it doesn't touch app mode (app/app_paths.h): the
// flag starts with "-", so it is not a positional argument and AppMode() is unchanged for a normal start. Output goes
// to stdout, errors to stderr (exit 1), and the process leaves with _exit like the dry run (app_dry_run.cpp).

#include "app/app_paths.h"
#include "gpu/settings.h"

#include <rex/cvar.h>

#include <crt_externs.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rex::cvar::FlagEntry;
using rex::cvar::FlagType;

struct Choice {
  const char* value;
  const char* label;
};

// One setting of the page. Type, default and allowed values come from the cvar registry; `min`/`max` and `choices`
// here override it (a range inside the registry's, the labels for the registry's allowed values, or numbers that are
// shown as a choice). Every int and float row needs both min and max, from here or from the registry.
struct Row {
  const char* key;
  const char* label;
  const char* help;
  const char* page;
  bool advanced = false;
  std::optional<double> min = std::nullopt;
  std::optional<double> max = std::nullopt;
  std::vector<Choice> choices = {};
};

constexpr const char* kDisplay = "Display";
constexpr const char* kGraphics = "Graphics";
constexpr const char* kPicture = "Picture";
constexpr const char* kControls = "Controls";

// Only cvars the game reads. Left out (2026-11-21, design Q305): eot_bloom, eot_depth_of_field, eot_motion_blur,
// eot_color_grading, eot_fov_scale, eot_shadow_cascades and eot_shadow_distance_scale (Edge of Time settings whose
// getters nothing calls) and eot_aspect_ratio (ConfiguredAspectRatio() is a 16:9 stand-in). Add a row once its
// setting is wired.
std::vector<Row> CuratedRows() {
  return {
      // ---- Display ----
      {"fable2_gpu_backend", "Renderer",
       "Metal is the native Mac renderer. MoltenVK runs Vulkan on top of Metal; use it only if Metal gives trouble.",
       kDisplay, false, std::nullopt, std::nullopt,
       {{"metal", "Metal"}, {"vulkan", "MoltenVK (Vulkan)"}}},
      {"fullscreen", "Full screen", "Off plays in a window.", kDisplay},
      {"eot_fullscreen_mode", "Full screen mode",
       "Borderless is a full-screen window at the desktop's resolution. Exclusive switches the display to a mode "
       "close to the game's picture size. Only used with Full screen on.",
       kDisplay, false, std::nullopt, std::nullopt,
       {{"borderless", "Borderless"}, {"exclusive", "Exclusive"}}},
      {"eot_resolution", "Resolution",
       "How tall the picture is drawn in full screen, in pixels, before it is scaled to fit the screen. Higher is "
       "sharper and needs a faster Mac. In a window the game picks a size to suit your display.",
       kDisplay, false, std::nullopt, std::nullopt,
       {{"native", "Original (720p)"},
        {"720p", "720p"},
        {"1080p", "1080p"},
        {"1440p", "1440p"},
        {"2160p", "4K (2160p)"},
        {"display", "Match the display"}}},
      {"eot_render_scale", "Extra render scale",
       "Multiplies the resolution above. 1.0 leaves it as it is, 0.5 draws a quarter of the pixels, 2.0 four times as "
       "many.",
       kDisplay, false, 0.5, 2.0},
      {"eot_fps_limit", "Frame-rate cap",
       "0 means no cap. 60 uses less power and keeps the Mac cooler.", kDisplay, false, 0, 240},
      {"eot_vsync", "Vertical sync",
       "Waits for the screen's refresh, which stops tearing. Off lets the game run faster than the display.", kDisplay},
      {"fable2_title_fps", "Frame rate in the window title", "Shows the frames per second in the window's title bar.",
       kDisplay},

      // ---- Graphics ----
      {"eot_upscale", "Upscaling",
       "How the picture is enlarged to the screen when the game draws it at a lower resolution. MetalFX is Apple's "
       "upscaler and only works with the Metal renderer; with MoltenVK the game uses Bicubic instead.",
       kGraphics, false, std::nullopt, std::nullopt,
       {{"bilinear", "Bilinear (fast, soft)"},
        {"bicubic", "Bicubic (smooth)"},
        {"lanczos", "Lanczos (sharper)"},
        {"metalfx", "MetalFX (Apple)"}}},
      {"eot_host_aa", "Anti-aliasing",
       "Smooths jagged edges. TAA blends each frame with the ones before it.", kGraphics, false, std::nullopt,
       std::nullopt, {{"off", "Off"}, {"taa", "TAA"}}},
      {"eot_anisotropy", "Texture filtering",
       "Keeps textures sharp when they are seen at a slant. 16 is the sharpest, 1 is off, and 0 uses only what the "
       "game itself asks for.",
       kGraphics, false, 0, 16},

      // ---- Graphics, advanced ----
      {"eot_taa_feedback", "TAA history strength",
       "How much of the earlier frames TAA keeps. Higher is smoother but can leave trails behind movement. Only used "
       "with TAA on.",
       kGraphics, true, 0.3, 0.95},
      {"eot_metalfx_color", "MetalFX colour mode",
       "How MetalFX reads the picture's colours. Automatic is right for almost everyone. Only used with MetalFX.",
       kGraphics, true, std::nullopt, std::nullopt,
       {{"auto", "Automatic"}, {"perceptual", "Perceptual"}, {"linear", "Linear"}, {"hdr", "HDR"}}},
      {"eot_motion_vectors", "Motion vectors",
       "Lets TAA follow moving objects, which sharpens them. Costs some speed. Only used with TAA on.", kGraphics,
       true},
      {"eot_shadow_map_size", "Shadow map size",
       "How sharp shadows are. Automatic follows the resolution. Bigger needs more video memory.", kGraphics, true,
       std::nullopt, std::nullopt,
       {{"0", "Automatic"}, {"1024", "1024"}, {"2048", "2048"}, {"4096", "4096"}, {"8192", "8192"}}},
      {"eot_scene_copy_refresh", "Glass effect refresh",
       "Keeps the picture behind glass-like effects up to date. Off is a little faster and those effects can look "
       "stale.",
       kGraphics, true},
      {"eot_pip_scale", "Picture-in-picture size",
       "Resolution of the small extra views the game draws inside the scene, as a percent of the main picture. Lower "
       "is faster.",
       kGraphics, true, 25, 100},

      // ---- Picture ----
      {"eot_brightness", "Brightness", "Raises or lowers the whole picture. 0 is unchanged.", kPicture, false, -0.5,
       0.5},
      {"eot_contrast", "Contrast", "Difference between dark and bright. 1.0 is unchanged.", kPicture, false, 0.25,
       3.0},
      {"eot_saturation", "Saturation", "How strong the colours are. 0 is black and white, 1.0 is unchanged.", kPicture,
       false, 0.0, 3.0},
      {"eot_gamma", "Gamma", "Changes the mid-tones. Below 1.0 darkens them, above 1.0 lightens them.", kPicture,
       false, 0.4, 2.5},
      {"eot_present_gamma", "Console gamma ramp", "Uses the gamma ramp the game sets, as the Xbox 360 did. Off ignores "
       "it.", kPicture},

      // ---- Controls ----
      {"mnk_mode", "Keyboard and mouse",
       "Play with the keyboard and mouse in place of a controller. F4 in the game shows the key bindings.", kControls},
      {"mnk_mouse", "Mouse moves the camera",
       "The mouse steers the camera like the right stick. Needs Keyboard and mouse on.", kControls},
      {"mnk_sensitivity", "Mouse sensitivity", "How fast the mouse turns the camera.", kControls, false, 0.2, 3.0},
  };
}

// ---- output ----

// Function-local: this runs from a constructor, and nothing here may depend on another initializer having run.
std::vector<std::string>& Errors() {
  static std::vector<std::string> errors;
  return errors;
}

void Error(const std::string& what) { Errors().push_back(what); }

std::string JsonString(std::string_view s) {
  std::string out = "\"";
  for (const unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out + "\"";
}

// The shortest text that reads back as the same number, always with a point or exponent so a JSON reader takes it as
// a float.
std::string FloatText(double v) {
  char buf[40];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  std::string out(buf, r.ptr);
  if (out.find_first_of(".eE") == std::string::npos)
    out += ".0";
  return out;
}

std::string IntText(double v) { return std::to_string(static_cast<long long>(v)); }

std::optional<double> ParseNumber(std::string_view s) {
  double v = 0;
  if (!rex::cvar::ParseDouble(s, v) || !std::isfinite(v))
    return std::nullopt;
  return v;
}

enum class Kind { kBool, kInt, kFloat, kChoice, kString };

const char* KindName(Kind k) {
  switch (k) {
    case Kind::kBool: return "bool";
    case Kind::kInt: return "int";
    case Kind::kFloat: return "float";
    case Kind::kChoice: return "choice";
    case Kind::kString: return "string";
  }
  return "string";
}

// What one row decided, kept for checking "start" against it.
struct Built {
  Kind kind;
  std::optional<double> min, max;
  std::vector<std::string> choices;
};

// Builds one row's JSON from the row and the cvar it names; appends to Error() and returns "" on a problem.
std::string BuildRow(const Row& row, Built* built) {
  const std::string key = row.key;
  const FlagEntry* info = rex::cvar::GetFlagInfo(key);
  if (!info) {
    Error(key + ": not in the cvar registry (a typo, or this file is linked before the file that defines it)");
    return "";
  }
  const size_t errors_before = Errors().size();
  const auto& reg = info->constraints;

  Kind kind;
  switch (info->type) {
    case FlagType::Boolean: kind = Kind::kBool; break;
    case FlagType::Int32: kind = Kind::kInt; break;
    case FlagType::Double: kind = Kind::kFloat; break;
    case FlagType::String: kind = Kind::kString; break;
    default:
      Error(key + ": a cvar type the page does not handle");
      return "";
  }

  // Choices: the row's, else the registry's allowed values (a string cvar with them is a choice).
  std::vector<std::string> choices, labels;
  if (!row.choices.empty()) {
    for (const Choice& c : row.choices) {
      choices.push_back(c.value);
      labels.push_back(c.label);
    }
  } else if (reg.HasAllowedValues()) {
    choices = reg.allowed_values;
  }
  if (!choices.empty()) {
    if (kind == Kind::kBool || kind == Kind::kFloat)
      Error(key + ": choices on a bool or float cvar");
    if (std::set<std::string>(choices.begin(), choices.end()).size() != choices.size())
      Error(key + ": a choice is listed twice");
    if (!row.choices.empty() && reg.HasAllowedValues()) {
      const std::set<std::string> a(choices.begin(), choices.end()),
          b(reg.allowed_values.begin(), reg.allowed_values.end());
      if (a != b)
        Error(key + ": the row's choices are not the cvar's allowed values");
    }
    if (kind == Kind::kInt) {
      for (const std::string& c : choices) {
        const auto n = ParseNumber(c);
        if (!n || *n != std::floor(*n) ||
            (reg.min && *n < *reg.min) || (reg.max && *n > *reg.max))
          Error(key + ": choice \"" + c + "\" is not an int inside the cvar's range");
      }
    }
    kind = Kind::kChoice;
  }
  if (!labels.empty() && labels.size() != choices.size())
    Error(key + ": choice labels and choices differ in number");

  // Range for numbers: the row's, inside the registry's.
  std::optional<double> lo = row.min ? row.min : reg.min, hi = row.max ? row.max : reg.max;
  if (kind == Kind::kInt || kind == Kind::kFloat) {
    if (!lo || !hi)
      Error(key + ": an int or float row needs both min and max");
    else if (*lo >= *hi)
      Error(key + ": min is not below max");
    if (lo && reg.min && *lo < *reg.min)
      Error(key + ": min is below the cvar's own minimum");
    if (hi && reg.max && *hi > *reg.max)
      Error(key + ": max is above the cvar's own maximum");
    if (kind == Kind::kInt && ((lo && *lo != std::floor(*lo)) || (hi && *hi != std::floor(*hi))))
      Error(key + ": an int row's min and max must be whole numbers");
  } else if (row.min || row.max) {
    Error(key + ": min or max on a row that is not a number");
  }

  // The default: the cvar's own, as the type wants it.
  std::string def;
  const std::string& dv = info->default_value;
  switch (kind) {
    case Kind::kBool:
      if (dv != "true" && dv != "false")
        Error(key + ": the cvar's default \"" + dv + "\" is not a bool");
      def = dv;
      break;
    case Kind::kInt:
    case Kind::kFloat: {
      const auto n = ParseNumber(dv);
      if (!n) {
        Error(key + ": the cvar's default \"" + dv + "\" is not a number");
        break;
      }
      if (lo && hi && (*n < *lo || *n > *hi))
        Error(key + ": the default " + dv + " is outside min and max");
      def = kind == Kind::kInt ? IntText(*n) : FloatText(*n);
      break;
    }
    case Kind::kChoice: {
      std::string text = dv;
      if (info->type == FlagType::Int32) {
        if (const auto n = ParseNumber(dv))
          text = IntText(*n);
      }
      if (std::find(choices.begin(), choices.end(), text) == choices.end())
        Error(key + ": the default \"" + text + "\" is not one of the choices");
      def = JsonString(text);
      break;
    }
    case Kind::kString:
      def = JsonString(dv);
      break;
  }

  if (!row.label || !*row.label || !row.help || !*row.help || !row.page || !*row.page)
    Error(key + ": label, help and page are all needed");

  if (Errors().size() != errors_before)
    return "";

  std::string out = "{ \"section\": \"\", \"key\": " + JsonString(key) + ", \"type\": \"" + KindName(kind) +
                    "\", \"default\": " + def;
  if (kind == Kind::kChoice) {
    out += ", \"choices\": [";
    for (size_t i = 0; i < choices.size(); ++i)
      out += (i ? ", " : "") + JsonString(choices[i]);
    out += "]";
    if (!labels.empty()) {
      out += ", \"choice_labels\": [";
      for (size_t i = 0; i < labels.size(); ++i)
        out += (i ? ", " : "") + JsonString(labels[i]);
      out += "]";
    }
  }
  if (kind == Kind::kInt || kind == Kind::kFloat) {
    out += ", \"min\": " + (kind == Kind::kInt ? IntText(*lo) : FloatText(*lo));
    out += ", \"max\": " + (kind == Kind::kInt ? IntText(*hi) : FloatText(*hi));
  }
  // A preset gate set by hand makes the preset custom, as the overlay does (gpu/settings.cpp kGates): Wayfarer
  // writes this beside the row when the file holds another preset.
  if (eot::gpu::IsQualityPresetGate(key))
    out += ", \"also_set\": {\"eot_quality_preset\": \"custom\"}";
  out += ", \"label\": " + JsonString(row.label) + ", \"help\": " + JsonString(row.help) +
         ", \"page\": " + JsonString(row.page) + ", \"advanced\": " + (row.advanced ? "true" : "false") + " }";
  built->kind = kind;
  built->min = lo;
  built->max = hi;
  built->choices = choices;
  return out;
}

// Whether the row would take `value` (the text the file holds, without quotes).
bool Accepts(const Built& b, const std::string& value, bool is_string) {
  switch (b.kind) {
    case Kind::kBool: return !is_string && (value == "true" || value == "false");
    case Kind::kInt:
    case Kind::kFloat: {
      const auto n = ParseNumber(value);
      return !is_string && n && b.min && b.max && *n >= *b.min && *n <= *b.max &&
             (b.kind == Kind::kFloat || *n == std::floor(*n));
    }
    case Kind::kChoice: return is_string && std::find(b.choices.begin(), b.choices.end(), value) != b.choices.end();
    case Kind::kString: return is_string;
  }
  return false;
}

int Describe() {
  const std::vector<Row> rows = CuratedRows();
  std::vector<std::string> json_rows;
  std::vector<Built> built(rows.size());
  std::set<std::string> keys;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (!keys.insert(rows[i].key).second)
      Error(std::string(rows[i].key) + ": listed twice");
    json_rows.push_back(BuildRow(rows[i], &built[i]));
  }

  std::string start = "{ ";
  bool first = true;
  for (const fable2::app::StartSetting& s : fable2::app::StartSettings()) {
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const Row& r) { return std::strcmp(r.key, s.name) == 0; });
    if (it == rows.end()) {
      Error(std::string("start: ") + s.name + " is not a row");
      continue;
    }
    const Built& b = built[static_cast<size_t>(it - rows.begin())];
    if (!json_rows[static_cast<size_t>(it - rows.begin())].empty() && !Accepts(b, s.value, s.is_string))
      Error(std::string("start: ") + s.name + " = " + s.value + " is not a value the row accepts");
    start += (first ? "" : ", ") + JsonString(s.name) + ": " + JsonString(s.value);
    first = false;
  }
  start += " }";

  if (!Errors().empty()) {
    for (const std::string& e : Errors())
      std::fprintf(stderr, "Fable2 --describe-settings: %s\n", e.c_str());
    return 1;
  }

  std::string out = "{ \"settings\": [\n";
  for (size_t i = 0; i < json_rows.size(); ++i)
    out += "  " + json_rows[i] + (i + 1 < json_rows.size() ? ",\n" : "\n");
  out += "],\n\"start\": " + start + "\n}\n";
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

__attribute__((constructor)) void MaybeDescribeSettings() {
  const int argc = *_NSGetArgc();
  char** argv = *_NSGetArgv();
  bool asked = false;
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::strcmp(argv[i], "--describe-settings") == 0)
      asked = true;
  }
  if (!asked)
    return;
  const int code = Describe();
  std::fflush(stdout);
  std::fflush(stderr);
  _exit(code);
}

}  // namespace
