// gpu/gpu_busy.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

// The arithmetic behind the Metal "gpu busy" figure: pure, so the offline self-test
// (tools/metal_selftest) can check it.

#pragma once

#include <algorithm>
#include <cstring>
#include <vector>

namespace eot::gpu {

struct BusyInterval {
  double start = 0.0;
  double end = 0.0;
};

// Seconds of [lo, hi] covered by at least one interval: the union, so command buffers that
// overlap on the GPU count once. Sorts the intervals; those outside the window add nothing.
inline double UnionBusySeconds(std::vector<BusyInterval> &intervals, double lo, double hi) {
  std::sort(intervals.begin(), intervals.end(),
            [](const BusyInterval &a, const BusyInterval &b) { return a.start < b.start; });
  double busy = 0.0;
  double open_start = 0.0, open_end = 0.0;
  bool open = false;
  for (const BusyInterval &i : intervals) {
    const double start = std::max(i.start, lo), end = std::min(i.end, hi);
    if (end <= start)
      continue;
    if (open && start <= open_end) {
      open_end = std::max(open_end, end);
      continue;
    }
    if (open)
      busy += open_end - open_start;
    open = true;
    open_start = start;
    open_end = end;
  }
  return open ? busy + (open_end - open_start) : busy;
}

// Drops the intervals that end at or before hi (their time is counted); what reaches past it is
// kept for the next window.
inline void DropIntervalsEndingBy(std::vector<BusyInterval> &intervals, double hi) {
  intervals.erase(std::remove_if(intervals.begin(), intervals.end(),
                                 [hi](const BusyInterval &i) { return i.end <= hi; }),
                  intervals.end());
}

// Whether a Metal command buffer's label (plume's MetalCommandList::debugLabel, or the label of
// a buffer plume commits itself) marks a buffer that does GPU work. The frame's lists carry
// plume's default label. Left out: the "Wait Command Buffer" that encodes the semaphore waits, the
// acquire and present buffers, and the lists that only carry a fence (the GPU-done marker, the
// queue-idle marker): they do no GPU work, and what span a buffer that only waits reports is up to
// Metal. Measured offline (tools/metal_selftest, a wait buffer blocked for 60 ms on an event from
// another queue): the wait buffer and the empty list behind it both report a zero-length span at
// the moment the wait ended, so they would not inflate the union; leaving them out makes the
// figure independent of that.
inline bool IsGpuWorkLabel(const char *label) { return std::strcmp(label, "RT64 Command List") == 0; }

}
