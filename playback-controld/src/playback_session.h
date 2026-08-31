#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "music_library.h"

// Shared mutable state, guarded by a single mutex (design.md §3.2) — the only shared mutable
// state in the whole daemon. `generation` is bumped by /play, /stop, /next, /previous (never by
// /pause or /resume, and never by queue auto-advance — see advance()) and is what StreamRoute
// checks each chunk to detect an interrupt.
struct PlaybackSession {
  std::mutex mutex;
  std::condition_variable cv;  // wakes a stream blocked by Paused, or by an interrupt
  enum class Status { Stopped, Playing, Paused };
  Status status = Status::Stopped;
  std::optional<Track> current_track;
  std::vector<Track> current_queue;
  size_t queue_position = 0;
  uint64_t generation = 0;

  // Moves queue_position by `direction` (+1 or -1) if the result is in bounds, updating
  // current_track to match. Returns false (no state change) if there's nowhere to move to.
  // Caller must already hold `mutex` — this does not lock internally. Shared by
  // ControlRoutes::advanceQueue (/next, /previous — which bump generation and notify cv
  // themselves after calling this) and StreamRoute's queue auto-advance (design.md §3.4 —
  // which deliberately does neither, since it isn't an interrupt).
  bool advance(int direction) {
    long next_position = static_cast<long>(queue_position) + direction;
    if (next_position < 0 || next_position >= static_cast<long>(current_queue.size())) {
      return false;
    }
    queue_position = static_cast<size_t>(next_position);
    current_track = current_queue[queue_position];
    return true;
  }
};
