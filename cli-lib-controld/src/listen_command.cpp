#include "listen_command.h"

#include <iostream>
#include <thread>

#include "audio_buffer.h"

namespace {

// ~6 minutes at this library's observed average bitrate (~250kbps / ~31.4KB/s, measured from a
// real test track: 17.4MB over 553s), rounded to a clean cap (streaming-backpressure-fix.md §4).
// A fixed byte cap, not a per-track duration -- exact bitrate varies per file and isn't known
// before a connection opens, so this is an approximation, not an exact "6 minutes" guarantee.
constexpr size_t kBufferCapacityBytes = 12 * 1024 * 1024;

// Consumer side: drains the buffer into mpv's pipe at whatever pace mpv actually wants it,
// completely decoupled from how fast the producer thread is pulling off the network.
void consumeIntoMpv(AudioBuffer& buffer, FILE* mpv) {
  while (true) {
    auto chunk = buffer.pop();
    if (!chunk) return;  // producer finished and buffer fully drained -- normal exit
    if (fwrite(chunk->data(), 1, chunk->size(), mpv) != chunk->size()) {
      // mpv's pipe broke (crashed, or was never really running) -- tell the producer to stop
      // pushing rather than let it block forever waiting for room that will never free up.
      buffer.notifyConsumerStopped();
      return;
    }
  }
}

// Producer side: today's reconnect loop (design.md §3.5), now pushing into the bounded buffer
// instead of writing directly to mpv. Returns the process exit code once playback genuinely
// ends or an unrecoverable error occurs.
int runProducerLoop(HttpClient& client, AudioBuffer& buffer) {
  // Single-retry-per-genuine-failure. Keyed on whether the just-finished attempt actually moved
  // any audio bytes, not on which outcome it ended with -- an attempt that streamed real data
  // before getting interrupted still proves the connection is healthy, so the budget is
  // restored either way. (Fix for a bug found in review: an earlier draft only reset on a clean
  // StreamEnded, so a *second* legitimate interrupt in one session -- e.g. two `gj skip`s from
  // another terminal -- would have killed gj listen even though nothing had actually failed.)
  bool retry_available = true;

  while (true) {
    // Always flush before a new connection attempt, no exceptions -- the buffer may still hold
    // several minutes of a track that's about to be superseded (the network can deliver up to
    // ~180x faster than real playback), and draining that first would make gj skip/gj play feel
    // severely laggy. Safe to do unconditionally: TCP already absorbs genuine transient network
    // blips below the application layer, so a ConnectionFailed reaching here is essentially
    // always either a deliberate interrupt (flush is correct) or a sustained-enough outage that
    // the buffer's contents are already stale anyway (flush is harmless) -- never a case where
    // preserving the old buffer contents was actually the right call (streaming-backpressure-
    // fix.md §4).
    buffer.flush();

    auto result = client.stream("/stream", [&](const char* data, size_t len) {
      return buffer.push(data, len);
    });

    if (result.received_data) {
      retry_available = true;
    }

    switch (result.outcome) {
      case HttpClient::StreamResult::Outcome::NothingPlaying:
        std::cout << (result.message.empty() ? "Nothing is currently playing." : result.message)
                  << "\n";
        return 0;
      case HttpClient::StreamResult::Outcome::StreamEnded:
        // Queue finished cleanly. playback-controld has already reset its state to Stopped by
        // the time this returns (playback-controld design.md §3.4 case 2), so the next
        // reconnect is guaranteed to land in NothingPlaying above.
        continue;
      case HttpClient::StreamResult::Outcome::ConnectionFailed:
        if (!retry_available) {
          std::cerr << "gj: error: lost connection to playback-controld and the retry failed\n";
          return 1;
        }
        retry_available = false;  // one immediate retry per genuine failure
        continue;
    }
  }
}

}  // namespace

ListenCommand::ListenCommand(HttpClient& client) : client_(client) {}

int ListenCommand::run() {
  // --no-video: mpv opens a GUI window by default even for audio-only files (found via real
  // testing) -- --no-terminal alone only suppresses console/keyboard interaction, it doesn't
  // stop the window. --no-video disables video output entirely, so there's nothing to show.
  mpv_ = _popen("mpv --no-video --no-terminal -", "wb");
  if (!mpv_) {
    std::cerr << "gj: error: could not start mpv (is it installed and on PATH?)\n";
    return 1;
  }

  AudioBuffer buffer(kBufferCapacityBytes);
  std::thread consumer(consumeIntoMpv, std::ref(buffer), mpv_);

  int exit_code = runProducerLoop(client_, buffer);

  buffer.markProducerFinished();
  consumer.join();
  _pclose(mpv_);
  return exit_code;
}
