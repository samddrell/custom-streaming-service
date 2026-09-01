#include "listen_command.h"

#include <iostream>

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

  // Single-retry-per-genuine-failure (design.md §3.5). Keyed on whether the just-finished
  // attempt actually moved any audio bytes, not on which outcome it ended with -- an attempt
  // that streamed real data before getting interrupted still proves the connection is healthy,
  // so the budget is restored either way. This is the fix for a bug found in review: an earlier
  // draft only reset on a clean StreamEnded, so a *second* legitimate interrupt in one session
  // (e.g. two `gj skip`s from another terminal) would have killed gj listen even though nothing
  // had actually failed.
  bool retry_available = true;

  while (true) {
    auto result = client_.stream("/stream", mpv_);

    if (result.received_data) {
      retry_available = true;
    }

    switch (result.outcome) {
      case HttpClient::StreamResult::Outcome::NothingPlaying: {
        std::cout << (result.message.empty() ? "Nothing is currently playing." : result.message)
                  << "\n";
        _pclose(mpv_);
        return 0;
      }
      case HttpClient::StreamResult::Outcome::StreamEnded:
        // Queue finished cleanly. playback-controld has already reset its state to Stopped by
        // the time this returns (playback-controld design.md §3.4 case 2), so the next
        // reconnect is guaranteed to land in NothingPlaying above.
        continue;
      case HttpClient::StreamResult::Outcome::ConnectionFailed:
        if (!retry_available) {
          std::cerr << "gj: error: lost connection to playback-controld and the retry failed\n";
          _pclose(mpv_);
          return 1;
        }
        retry_available = false;  // one immediate retry per genuine failure
        continue;
    }
  }
}
