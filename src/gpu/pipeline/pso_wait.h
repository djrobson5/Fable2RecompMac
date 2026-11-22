// gpu/pipeline/pso_wait.h
// Fable 2: a depth-tested draw whose host pipeline is still being built waits for it (a short while) instead of
// being skipped. The rules of that wait, apart from the renderer, so a test can drive them with a fake builder.
//
// The jobs (queued, running, done, failed; the urgent line; the wait with a deadline; "an earlier wait on this key
// ran out") are the board of src/gpu/shaders/runtime_jobs.h, keyed by the pipeline key. This file adds the
// policy around it:
//   - how long a draw may wait: `max_ms` in play, `loading_ms` while a loading screen is up;
//   - all the waits of one guest frame together may take `frame_ms` in play and `loading_frame_ms` while a loading
//     screen is up (two budgets: a loading frame does not spend the play budget, nor the other way round); a
//     budget of 0 means no waiting in that case;
//   - a key whose wait ran out is not waited for again (its draws are skipped until it lands);
//   - a key that failed is never waited for;
//   - a key pending for longer than `stale_ms` is built by the caller in place (the old rule);
//   - the numbers for one log line per window.
//
// Nothing here holds a lock while it waits (the board's condition variable releases its lock; the budget and the
// numbers have their own short locks that never nest with the board's), and no function calls out to other code
// under a lock.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

#include "gpu/shaders/runtime_jobs.h"

namespace eot::gpu::psowait {

struct Limits {
  int max_ms = 20;        // in play: the longest one draw waits
  int frame_ms = 33;      // in play: the longest all the waits of one guest frame take together
  int loading_ms = 500;   // while a loading screen is up: the longest one draw waits
  int loading_frame_ms = 1000;  // while a loading screen is up: the longest all the waits of one guest frame take
  int stale_ms = 2500;    // a key pending this long is built by the caller itself
};

enum class Outcome {
  Ready,          // the pipeline was built: look it up and go ahead
  Failed,         // its build failed: skip (the cache holds the failure)
  TimedOut,       // waited the full cap and it was not there: skip; later draws with the key do not wait
  GaveUpBefore,   // an earlier wait on the key ran out: skip at once
  BudgetSpent,    // the frame's wait budget is used up (skipped before waiting), or it cut the wait short: skip
  WaitsOff,       // waiting is switched off (or a cap or budget is 0): skip, as before the waits existed
  Stopped,        // shutdown: skip
  BuildHere,      // pending for too long: the caller builds it in place
};

struct Stats {
  uint64_t waits = 0;            // draws that waited
  double total_ms = 0, max_ms = 0;
  uint64_t gave_up = 0;          // of those, the wait ran out at the full cap
  uint64_t cut_short = 0;        // of those waits, the frame budget ended it before the cap (not a give-up)
  uint64_t budget_skipped = 0;   // draws skipped without waiting because the frame budget was already spent
};

template <class Payload>
class Waiter {
 public:
  using Clock = std::chrono::steady_clock;

  // `frame` identifies the guest frame (a frame budget starts over when it changes). `make_payload` is called
  // only when the key is new: the job's payload (what a worker needs to build it). `enabled` false: no waiting.
  template <class MakePayload>
  Outcome Gate(uint64_t key, MakePayload &&make_payload, bool enabled, bool loading, const Limits &limits,
               uint64_t frame) {
    if (board_.Stopped())   // shutdown: nothing waits and nothing is built in place while the render queue drains
      return Outcome::Stopped;
    std::chrono::milliseconds age{0};
    jobs::Peek p = board_.Look(key, &age);
    if (p == jobs::Peek::Unknown) {
      board_.Add(key, make_payload());
      p = board_.Look(key, &age);
    }
    switch (p) {
      case jobs::Peek::Done:
        return Outcome::Ready;
      case jobs::Peek::Failed:
        return Outcome::Failed;
      case jobs::Peek::Unknown:   // (cannot happen: just added)
        return Outcome::Failed;
      case jobs::Peek::Pending:
      case jobs::Peek::GaveUp:
        break;
    }
    // Built too slowly: the caller builds it here after all (even after a wait ran out).
    if (age > std::chrono::milliseconds(std::max(limits.stale_ms, 0)))
      return Outcome::BuildHere;
    if (!enabled)
      return Outcome::WaitsOff;
    if (p == jobs::Peek::GaveUp)
      return Outcome::GaveUpBefore;

    const int max_ms = std::max(loading ? limits.loading_ms : limits.max_ms, 0);
    const int frame_ms = std::max(loading ? limits.loading_frame_ms : limits.frame_ms, 0);
    if (max_ms == 0 || frame_ms == 0)
      return Outcome::WaitsOff;
    Budget &budget = loading ? loading_budget_ : play_budget_;
    const int cap_ms = std::min(max_ms, BudgetLeftMs(budget, frame, frame_ms));
    if (cap_ms < 1) {
      std::lock_guard lock(sm_);
      ++stats_.budget_skipped;
      return Outcome::BudgetSpent;
    }
    const bool full_cap = cap_ms == max_ms;
    const auto start = Clock::now();
    // Moves the job to the urgent line, then waits. A wait that the frame budget cut short does not count as a
    // wait that ran out: the build may be a millisecond away.
    const jobs::WaitResult r = board_.Wait(key, std::chrono::milliseconds(cap_ms), full_cap);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    {
      std::lock_guard lock(sm_);
      Spend(budget, frame, ms);
      ++stats_.waits;
      stats_.total_ms += ms;
      stats_.max_ms = std::max(stats_.max_ms, ms);
      if (r == jobs::WaitResult::TimedOut) {
        if (full_cap)
          ++stats_.gave_up;
        else
          ++stats_.cut_short;
      }
    }
    switch (r) {
      case jobs::WaitResult::Done:
        return Outcome::Ready;
      case jobs::WaitResult::Failed:
      case jobs::WaitResult::Unknown:
        return Outcome::Failed;
      case jobs::WaitResult::TimedOut:
        return full_cap ? Outcome::TimedOut : Outcome::BudgetSpent;
      case jobs::WaitResult::Stopped:
        return Outcome::Stopped;
    }
    return Outcome::Failed;
  }

  // Workers: see jobs::Board.
  jobs::Board<Payload> &board() { return board_; }
  void Stop() { board_.Stop(); }

  // The numbers since the last call (one log line per window).
  Stats TakeStats() {
    std::lock_guard lock(sm_);
    Stats out = stats_;
    stats_ = Stats{};
    return out;
  }

 private:
  struct Budget {
    uint64_t frame = ~uint64_t(0);
    double spent_ms = 0;
  };
  // Milliseconds of this frame's wait budget still unspent (a new frame starts a new budget).
  int BudgetLeftMs(Budget &b, uint64_t frame, int frame_ms) {
    std::lock_guard lock(sm_);
    if (frame != b.frame) {
      b.frame = frame;
      b.spent_ms = 0;
    }
    return static_cast<int>(std::max(0.0, std::min(double(frame_ms) - b.spent_ms, 1.0e6)));
  }
  void Spend(Budget &b, uint64_t frame, double ms) {   // sm_ held
    if (frame != b.frame) {
      b.frame = frame;
      b.spent_ms = 0;
    }
    b.spent_ms += ms;
  }

  jobs::Board<Payload> board_;
  std::mutex sm_;
  Stats stats_;
  Budget play_budget_, loading_budget_;
};

}  // namespace eot::gpu::psowait
