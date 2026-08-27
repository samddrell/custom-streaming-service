#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "music_library.h"

// Shared mutable state, guarded by a single mutex (design.md §3.2) — the only shared mutable
// state in the whole daemon. `generation` is bumped by /play, /stop, /next, /previous (never by
// /pause or /resume) and is what StreamRoute checks each chunk to detect an interrupt.
struct PlaybackSession {
  std::mutex mutex;
  std::condition_variable cv;  // wakes a stream blocked by Paused, or by an interrupt
  enum class Status { Stopped, Playing, Paused };
  Status status = Status::Stopped;
  std::optional<Track> current_track;
  std::vector<Track> current_queue;
  size_t queue_position = 0;
  uint64_t generation = 0;
};
