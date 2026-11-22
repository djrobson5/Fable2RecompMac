// Fable 2: the achievements ledger, <logs>/achievements.txt, kept for a launcher
// (Wayfarer) to read. The format is Wayfarer's ReportedAchievements, CRLF:
//
//   ; Achievements earned in Fable II (kept by the game; a launcher shows them).
//   [unlocked]    <id> = yyyy-MM-dd HH:mm      local time, only the unlocked ones
//   [counters]    (none for this game)
//   [gamerscore]  <id> = <points>              all of them
//   [list]        <id> = <label> | <description>   all of them, in the game's order
//
// Written to achievements.txt.new and renamed, at start and at each unlock.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <rex/ui/overlay/achievement_toast.h>

namespace rex::system {
class AchievementManager;
}

namespace fable2::app::ledger {

struct Item {
  uint32_t id = 0;
  std::string label;
  std::string description;  // the unlocked-state description
  uint32_t gamerscore = 0;
};

// FILETIME (100 ns since 1601) as local "yyyy-MM-dd HH:mm".
std::string FormatLocalTime(uint64_t filetime);

// The file's text. `unlocked` is (id, formatted time); entries whose id is not in
// `list` are left out. Pure: no clock, no files (the self-test compares its bytes).
std::string Format(const std::vector<Item>& list, const std::vector<std::pair<uint32_t, std::string>>& unlocked);

// True while a launcher holds <logs>/wayfarer.lock (it shows the pop-up itself).
// Never creates the file.
bool LauncherShowsNotes(const std::filesystem::path& logs);

// Writes the ledger now from the manager's list and unlocks; false (nothing
// written) while the list is empty.
bool Write(const rex::system::AchievementManager& achievements, const std::filesystem::path& logs);

// Writes the ledger, and rewrites it at every unlock (one log line each).
// `log` gets the text of each line. Call once, after the achievement list exists.
void Start(rex::system::AchievementManager& achievements, const std::filesystem::path& logs,
           void (*log)(const char* line));

// The built-in achievement toast, held back while a launcher shows the pop-up.
class QuietToast : public rex::ui::AchievementToastDialog {
 public:
  QuietToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer, rex::Runtime* runtime,
             std::filesystem::path logs)
      : AchievementToastDialog(drawer, immediate_drawer, runtime), logs_(std::move(logs)) {}
  void Push(const rex::system::AchievementEvent& event) override;

 private:
  std::filesystem::path logs_;
};

// Compares Format() and FormatLocalTime() with fixed expected bytes; prints one
// line per check to stdout. True when all pass. (FABLE2_LEDGER_SELFTEST=1 in the dry run.)
bool SelfTest();

}  // namespace fable2::app::ledger
