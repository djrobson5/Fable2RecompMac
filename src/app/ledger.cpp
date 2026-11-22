// Fable 2: the achievements ledger (see ledger.h).

#include "app/ledger.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>

#include <rex/system/achievement_manager.h>

namespace fable2::app::ledger {

namespace fs = std::filesystem;

namespace {

std::mutex g_write_mutex;
void (*g_log)(const char*) = nullptr;
rex::system::AchievementListenerHandle g_listener = 0;

// One line in the file: no pipe (the separator) and no line break.
std::string Clean(std::string s) {
  for (char& c : s) {
    if (c == '|')
      c = '/';
    else if (c == '\r' || c == '\n')
      c = ' ';
  }
  return s;
}

void Say(const std::string& line) {
  if (g_log)
    g_log(line.c_str());
}

}  // namespace

std::string FormatLocalTime(uint64_t filetime) {
  constexpr int64_t kEpochDiffSeconds = 11644473600LL;  // 1601 to 1970
  const int64_t unix_seconds = int64_t(filetime / 10000000ULL) - kEpochDiffSeconds;
  const time_t t = time_t(unix_seconds);
  struct tm local {};
  localtime_r(&t, &local);
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M", &local);
  return buffer;
}

std::string Format(const std::vector<Item>& list, const std::vector<std::pair<uint32_t, std::string>>& unlocked) {
  std::map<uint32_t, std::string> when(unlocked.begin(), unlocked.end());
  std::string out = "; Achievements earned in Fable II (kept by the game; a launcher shows them).\r\n[unlocked]\r\n";
  for (const Item& item : list) {
    if (auto it = when.find(item.id); it != when.end())
      out += std::to_string(item.id) + " = " + it->second + "\r\n";
  }
  out += "\r\n[counters]\r\n\r\n[gamerscore]\r\n";
  for (const Item& item : list)
    out += std::to_string(item.id) + " = " + std::to_string(item.gamerscore) + "\r\n";
  out += "\r\n[list]\r\n";
  for (const Item& item : list)
    out += std::to_string(item.id) + " = " + Clean(item.label) + " | " + Clean(item.description) + "\r\n";
  return out;
}

bool LauncherShowsNotes(const fs::path& logs) {
  // Never created here: no file means no launcher has looked.
  const int fd = ::open((logs / "wayfarer.lock").c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return false;
  const bool taken = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
  ::close(fd);  // the lock, if it was got, goes with the handle
  return taken;
}

bool Write(const rex::system::AchievementManager& achievements, const fs::path& logs) {
  std::lock_guard lock(g_write_mutex);
  std::vector<Item> list;
  std::vector<std::pair<uint32_t, std::string>> unlocked;
  for (const auto& a : achievements.ListAchievements()) {
    list.push_back({a.id, a.label, a.description, a.gamerscore});
    if (achievements.IsUnlocked(a.id))
      unlocked.emplace_back(a.id, FormatLocalTime(achievements.GetUnlockTime(a.id)));
  }
  if (list.empty())
    return false;
  const std::string text = Format(list, unlocked);

  std::error_code ec;
  fs::create_directories(logs, ec);
  const fs::path file = logs / "achievements.txt";
  const fs::path tmp = logs / "achievements.txt.new";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();  // a write error shows only once the stream is flushed
    if (!out) {
      Say("[ledger] cannot write " + tmp.string());
      return false;
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    Say("[ledger] cannot replace " + file.string() + ": " + ec.message());
    return false;
  }
  return true;
}

void Start(rex::system::AchievementManager& achievements, const fs::path& logs, void (*log)(const char* line)) {
  g_log = log;
  const size_t count = achievements.ListAchievements().size();
  if (Write(achievements, logs))
    Say("[ledger] wrote " + (logs / "achievements.txt").string() + " (" + std::to_string(count) + " achievements)");
  else
    Say("[ledger] not written: the achievement list is empty or the file could not be written");
  if (g_listener)
    return;
  g_listener = achievements.RegisterUnlockCallback([&achievements, logs](const rex::system::AchievementEvent& event) {
    // Guest thread. The manager has recorded the unlock and calls us without its lock.
    const bool launcher = LauncherShowsNotes(logs);
    Say("[ledger] unlocked " + std::to_string(event.achievement.id) + " (" +
        std::to_string(event.achievement.gamerscore) + " G), shown by " + (launcher ? "launcher" : "game"));
    Write(achievements, logs);
  });
}

void QuietToast::Push(const rex::system::AchievementEvent& event) {
  if (LauncherShowsNotes(logs_))
    return;
  AchievementToastDialog::Push(event);
}

bool SelfTest() {
  bool ok = true;
  auto check = [&](const char* name, bool pass) {
    std::printf("  ledger selftest %-28s %s\n", name, pass ? "ok" : "FAILED");
    ok = ok && pass;
  };

  // Local time is pinned: FILETIME 2026-11-21 12:34:56 UTC, shown in UTC and in New York (EST, UTC-5).
  const uint64_t filetime = (uint64_t(1795264496ULL) + 11644473600ULL) * 10000000ULL;
  const char* old_tz = std::getenv("TZ");
  const std::string saved = old_tz ? old_tz : "";
  setenv("TZ", "UTC", 1);
  tzset();
  const std::string utc = FormatLocalTime(filetime);
  setenv("TZ", "America/New_York", 1);
  tzset();
  const std::string ny = FormatLocalTime(filetime);
  if (old_tz)
    setenv("TZ", saved.c_str(), 1);
  else
    unsetenv("TZ");
  tzset();
  check("time, UTC", utc == "2026-11-21 12:34");
  check("time, New York", ny == "2026-11-21 07:34");

  const std::vector<Item> list = {
      {10, "First Steps", "Complete the tutorial.", 15},
      {11, "A | Pipe", "Two\r\nlines.", 20},
      {83, "Last One", "Done.", 5},
  };
  const std::string expected =
      "; Achievements earned in Fable II (kept by the game; a launcher shows them).\r\n"
      "[unlocked]\r\n"
      "11 = 2026-11-21 12:34\r\n"
      "\r\n"
      "[counters]\r\n"
      "\r\n"
      "[gamerscore]\r\n"
      "10 = 15\r\n"
      "11 = 20\r\n"
      "83 = 5\r\n"
      "\r\n"
      "[list]\r\n"
      "10 = First Steps | Complete the tutorial.\r\n"
      "11 = A / Pipe | Two  lines.\r\n"
      "83 = Last One | Done.\r\n";
  check("format, one unlocked", Format(list, {{11, "2026-11-21 12:34"}, {99, "ignored"}}) == expected);

  const std::string none = Format({}, {});
  check("format, empty", none == "; Achievements earned in Fable II (kept by the game; a launcher shows them).\r\n"
                                 "[unlocked]\r\n\r\n[counters]\r\n\r\n[gamerscore]\r\n\r\n[list]\r\n");

  // The lock probe: no file, a free file, and a file held by another handle.
  const fs::path dir = fs::temp_directory_path() / ("fable2-ledger-selftest-" + std::to_string(getpid()));
  std::error_code ec;
  fs::create_directories(dir, ec);
  check("probe, no lock file", !LauncherShowsNotes(dir) && !fs::exists(dir / "wayfarer.lock", ec));
  const int held = ::open((dir / "wayfarer.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  check("probe, file not held", held >= 0 && !LauncherShowsNotes(dir));
  check("probe, file held", held >= 0 && ::flock(held, LOCK_EX | LOCK_NB) == 0 && LauncherShowsNotes(dir));
  if (held >= 0)
    ::close(held);
  fs::remove_all(dir, ec);
  return ok;
}

}  // namespace fable2::app::ledger
