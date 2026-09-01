#pragma once

#include <cstdio>

#include "http_client.h"

// The one long-running, stateful piece (design.md §3.5) -- deliberately its own class, mirroring
// playback-controld splitting StreamRoute out from ControlRoutes for the same reason: the "one
// long-lived stream" logic doesn't belong tangled up with short request/response commands.
class ListenCommand {
 public:
  explicit ListenCommand(HttpClient& client);

  // Blocks until playback genuinely ends (queue exhausted, /stop) or an unrecoverable error
  // occurs (daemon unreachable after one retry, mpv unusable).
  int run();

 private:
  HttpClient& client_;
  FILE* mpv_ = nullptr;
};
