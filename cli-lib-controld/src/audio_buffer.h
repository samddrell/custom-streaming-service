#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

// A bounded producer/consumer byte queue sitting between the network-reading side (producer)
// and the mpv-writing side (consumer) of `gj listen` (design.md §3.5/§4,
// streaming-backpressure-fix.md). Exists to absorb the natural mismatch between how fast the
// network can deliver data (near-instant on a LAN) and how fast mpv actually consumes it (real
// playback speed) -- without this, that mismatch propagates, via blocking I/O, all the way back
// through TCP to playback-controld's own write timeout.
//
// Bounded, not unbounded: an unbounded buffer would let the producer eagerly download an entire
// track in seconds, closing the /stream connection almost immediately -- which would break the
// interrupt mechanism (nothing left "live" to abort once the connection has already completed).
class AudioBuffer {
 public:
  explicit AudioBuffer(size_t capacity_bytes);

  // Producer side. Blocks while the buffer is at capacity. Appends `len` bytes from `data`.
  // Returns false (without appending) if the consumer has given up (its pipe to mpv broke) --
  // the producer should stop pushing and end its current connection attempt when this happens.
  bool push(const char* data, size_t len);

  // Consumer side. Blocks while the buffer is empty and the producer isn't finished yet.
  // Returns std::nullopt once the producer has signaled it's finished AND the buffer has been
  // fully drained -- the consumer's signal to stop.
  std::optional<std::string> pop();

  // Producer side. Discards whatever is currently queued but not yet consumed -- called at the
  // start of every new connection attempt (streaming-backpressure-fix.md §4: "always flush on
  // reconnect, no exceptions").
  void flush();

  // Producer side. Signals there is no more data coming (playback genuinely ended or an
  // unrecoverable error occurred) -- wakes a blocked consumer so it can drain whatever's left
  // and then exit.
  void markProducerFinished();

  // Consumer side. Signals mpv's pipe broke and the consumer is giving up -- wakes a blocked
  // producer (push()) so it doesn't wait forever for room that will never free up.
  void notifyConsumerStopped();

 private:
  std::mutex mutex_;
  std::condition_variable not_full_;
  std::condition_variable not_empty_;
  std::deque<std::string> chunks_;
  size_t size_bytes_ = 0;
  const size_t capacity_bytes_;
  bool producer_finished_ = false;
  bool consumer_stopped_ = false;
};
