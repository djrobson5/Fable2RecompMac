// gpu/shaders/runtime_jobs.h
// Fable 2: who is translating which shader (runtime_shaders.cpp), and who waits for it.
//
// Header only, with nothing from the game in it, so a test can include it alone and drive it with a fake
// translator.
//
// Every shader the renderer asks for is one job with a state: queued, running, done or failed. Workers take jobs;
// a draw that cannot go ahead without its shader waits for that shader's job (Wait), which first moves a queued job
// to the urgent line, where the urgent workers, who take nothing else, pick it up at once.
//
// The board has one lock, held only for bookkeeping: never while a job runs, never while a caller is blocked
// (the condition variables release it), and no function here calls out to other code under it, so it cannot take
// part in a lock cycle. A job's result is published by whoever ran it before it calls Finish; a waiter that
// sees Done can rely on it.

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace eot::gpu::jobs {

enum class State : uint8_t { Queued, Running, Done, Failed };

enum class WaitResult {
  Done,      // the job finished and published its result
  Failed,    // it finished without one (the shader cannot be translated)
  TimedOut,  // still not there when the wait ran out, or an earlier wait on it already ran out
  Unknown,   // nothing was ever asked for under that hash: there is nothing to wait for
  Stopped,   // the board was stopped (shutdown) before the job ended
};

// What a look at a job (Peek) finds, without waiting.
enum class Peek {
  Unknown,  // never asked for
  Pending,  // queued or running: a wait can end well
  Done,
  Failed,
  GaveUp,   // an earlier wait on it ran out: a wait would not wait
};

template <class Payload>
class Board {
 public:
  // Queues a job. False when the hash is known already (queued, running, done or failed): a hash is run once.
  bool Add(uint64_t hash, Payload payload) {
    std::lock_guard lock(m_);
    auto [it, added] = jobs_.try_emplace(hash);
    if (!added)
      return false;
    it->second.state = State::Queued;
    it->second.payload = std::move(payload);
    normal_.push_back(hash);
    wake_.notify_all();
    return true;
  }

  // Records a hash whose result exists without a job (a translation loaded from disk): Wait says Done at once.
  void MarkDone(uint64_t hash) {
    std::lock_guard lock(m_);
    auto [it, added] = jobs_.try_emplace(hash);
    if (added)
      it->second.state = State::Done;
  }

  // Blocks until there is a job for this kind of worker, and takes it (the hash and its payload; `urgent` says it
  // came from the urgent line: someone is waiting for it). Urgent jobs first; a worker that is `urgent_only` never
  // takes any other. False once Stop() was called.
  bool Take(bool urgent_only, uint64_t *hash, Payload *payload, bool *urgent) {
    std::unique_lock lock(m_);
    wake_.wait(lock, [&] { return stop_ || !urgent_.empty() || (!urgent_only && !normal_.empty()); });
    if (stop_)
      return false;
    std::deque<uint64_t> &line = !urgent_.empty() ? urgent_ : normal_;
    *urgent = &line == &urgent_;
    *hash = line.front();
    line.pop_front();
    Entry &e = jobs_.at(*hash);
    e.state = State::Running;
    *payload = std::move(e.payload);
    e.payload = Payload();
    return true;
  }

  // The job is over: Done when its result was published, Failed when it has none. Wakes every waiter.
  void Finish(uint64_t hash, bool ok) {
    std::lock_guard lock(m_);
    auto it = jobs_.find(hash);
    if (it == jobs_.end())
      return;
    it->second.state = ok ? State::Done : State::Failed;
    it->second.payload = Payload();
    done_.notify_all();
  }

  // Moves a queued job to the urgent line (no effect on a job that runs or is over).
  void Prioritize(uint64_t hash) {
    std::lock_guard lock(m_);
    PrioritizeLocked(hash);
  }

  // Waits up to `cap` for the job to end. A queued job is moved to the urgent line first. After one wait on a hash
  // ran out, later waits on it do not wait at all: the caller's old behaviour (skip the draw) applies until the
  // result lands.
  WaitResult Wait(uint64_t hash, std::chrono::milliseconds cap) {
    std::unique_lock lock(m_);
    auto it = jobs_.find(hash);
    if (it == jobs_.end())
      return WaitResult::Unknown;
    if (it->second.state == State::Done)
      return WaitResult::Done;
    if (it->second.state == State::Failed)
      return WaitResult::Failed;
    if (it->second.gave_up)
      return WaitResult::TimedOut;
    if (stop_)
      return WaitResult::Stopped;
    PrioritizeLocked(hash);
    done_.wait_for(lock, cap, [&] {
      const State s = jobs_.at(hash).state;
      return stop_ || s == State::Done || s == State::Failed;
    });
    Entry &e = jobs_.at(hash);
    if (e.state == State::Done)
      return WaitResult::Done;
    if (e.state == State::Failed)
      return WaitResult::Failed;
    if (stop_)
      return WaitResult::Stopped;
    e.gave_up = true;
    return WaitResult::TimedOut;
  }

  // Looks at a job without waiting or changing anything.
  Peek Look(uint64_t hash) {
    std::lock_guard lock(m_);
    auto it = jobs_.find(hash);
    if (it == jobs_.end())
      return Peek::Unknown;
    if (it->second.state == State::Done)
      return Peek::Done;
    if (it->second.state == State::Failed)
      return Peek::Failed;
    return it->second.gave_up ? Peek::GaveUp : Peek::Pending;
  }

  // Shutdown: ends every Take (the workers leave) and every Wait (the waiters go on), now and later.
  void Stop() {
    std::lock_guard lock(m_);
    stop_ = true;
    wake_.notify_all();
    done_.notify_all();
  }

  // For logs and tests: how many jobs wait in the normal and the urgent line.
  std::pair<size_t, size_t> Queued() {
    std::lock_guard lock(m_);
    return {normal_.size(), urgent_.size()};
  }

 private:
  struct Entry {
    State state = State::Queued;
    bool gave_up = false;
    Payload payload{};
  };

  void PrioritizeLocked(uint64_t hash) {
    auto it = jobs_.find(hash);
    if (it == jobs_.end() || it->second.state != State::Queued)
      return;
    for (auto q = normal_.begin(); q != normal_.end(); ++q)
      if (*q == hash) {
        normal_.erase(q);
        urgent_.push_back(hash);
        wake_.notify_all();   // (an urgent-only worker may be the one asleep: wake them all)
        return;
      }
  }

  std::mutex m_;
  std::condition_variable wake_;   // workers
  std::condition_variable done_;   // waiters
  std::unordered_map<uint64_t, Entry> jobs_;
  std::deque<uint64_t> normal_, urgent_;
  bool stop_ = false;
};

}  // namespace eot::gpu::jobs
