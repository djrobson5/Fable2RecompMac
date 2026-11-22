// gpu/shaders/runtime_child.h
// Fable 2: runs the shader translator as a child process that cannot hang a worker for good (runtime_shaders.cpp).
//
// Header only, with nothing from the game in it, so a test can drive it with ordinary programs (sleep, false).
// A child that runs past its time limit, or still runs when the game shuts down, is killed.

#pragma once

#include <fcntl.h>
#include <pthread/spawn.h>
#include <signal.h>
#include <spawn.h>
#include <sys/qos.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

namespace eot::gpu::child {

enum class Outcome {
  Ok,           // exited with status 0
  SpawnFailed,  // the program could not be started
  ExitFailed,   // exited with another status, or by a signal of its own
  Killed,       // still running after the time limit, so killed
  Cancelled,    // killed because `cancel` was set (shutdown)
};

struct Result {
  Outcome outcome = Outcome::Ok;
  std::string error;  // for the log, when not Ok
};

// Runs `argv` (argv[0] is the program), standard output and error to /dev/null, and waits at most `timeout`. The
// wait polls (every few milliseconds, backing off), so it can notice `cancel` and the limit: a blocking waitpid
// could do neither.
inline Result Run(const std::vector<std::string> &argv, bool urgent, std::chrono::milliseconds timeout,
                  const std::atomic<bool> *cancel = nullptr) {
  Result r;
  std::vector<char *> args;
  for (const std::string &a : argv)
    args.push_back(const_cast<char *>(a.c_str()));
  args.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
  // The child would inherit the calling thread's QoS, which can be background: macOS then keeps it on the
  // efficiency cores, throttled (a translation took 36 s instead of 5 s). Utility runs it at full speed while still
  // yielding to the game's threads. A draw that waits for the shader has nothing else to run: user-initiated.
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_set_qos_class_np(&attr, urgent ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY);
  pid_t pid = 0;
  const int rc = posix_spawn(&pid, argv[0].c_str(), &actions, &attr, args.data(), environ);
  posix_spawnattr_destroy(&attr);
  posix_spawn_file_actions_destroy(&actions);
  if (rc != 0) {
    r.outcome = Outcome::SpawnFailed;
    r.error = std::format("cannot start {} ({})", argv[0], std::strerror(rc));
    return r;
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto nap = std::chrono::milliseconds(1);
  int status = 0;
  for (;;) {
    const pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == pid)
      break;
    if (done < 0 && errno != EINTR) {
      r.outcome = Outcome::ExitFailed;
      r.error = std::format("{}: waitpid failed ({})", argv[0], std::strerror(errno));
      return r;
    }
    const bool cancelled = cancel && cancel->load(std::memory_order_acquire);
    if (cancelled || std::chrono::steady_clock::now() >= deadline) {
      kill(pid, SIGKILL);
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
      r.outcome = cancelled ? Outcome::Cancelled : Outcome::Killed;
      r.error = cancelled ? std::format("{} was stopped at shutdown", argv[0])
                          : std::format("{} ran over {} ms and was killed", argv[0], timeout.count());
      return r;
    }
    std::this_thread::sleep_for(nap);
    if (nap < std::chrono::milliseconds(20))
      nap *= 2;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    r.outcome = Outcome::ExitFailed;
    r.error = std::format("{} exited with {}", argv[0], WIFEXITED(status) ? WEXITSTATUS(status) : -1);
  }
  return r;
}

// Run() again once after a failure of any kind except shutdown (a start that failed, a non-zero exit, a kill):
// a child can fail once for reasons that pass (memory pressure, a full process table). Reports the last failure.
inline Result RunRetrying(const std::vector<std::string> &argv, bool urgent, std::chrono::milliseconds timeout,
                          const std::atomic<bool> *cancel = nullptr, int *attempts = nullptr) {
  Result r;
  int n = 0;
  do {
    ++n;
    r = Run(argv, urgent, timeout, cancel);
  } while (r.outcome != Outcome::Ok && r.outcome != Outcome::Cancelled && n < 2);
  if (attempts)
    *attempts = n;
  return r;
}

}  // namespace eot::gpu::child
