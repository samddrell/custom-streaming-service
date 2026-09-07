#include <cstdlib>
#include <iostream>

#include <httplib.h>

#include "control_routes.h"
#include "music_library.h"
#include "playback_session.h"
#include "stream_route.h"

namespace {
// Fixed paths, matching design.md §5/§7 — no config file, per the project's "simplest thing
// that satisfies requirements.md" preference (a single-Jetson deployment doesn't need one).
constexpr const char* kMusicDir = "/mnt/x10pro/music";
constexpr const char* kStorageDir = "/var/lib/playback-controld";
constexpr int kPort = 8080;
}  // namespace

int main() {
  MusicLibrary library(kMusicDir, kStorageDir);
  try {
    library.load();
  } catch (const std::exception& e) {
    // Missing/unreadable USB drive at startup: log and exit non-zero, no degraded mode
    // (design.md §6).
    std::cerr << "playback-controld: fatal: " << e.what() << "\n";
    return 1;
  }

  PlaybackSession session;

  httplib::Server server;
  // cpp-httplib defaults CPPHTTPLIB_SERVER_WRITE_TIMEOUT_SECOND to 5 -- far too short for a
  // client legitimately pacing itself to real playback speed (found via real testing: a slow,
  // paced consumer stalls a chunk write for multi-second stretches routinely, well within normal
  // operation, and the 5s default was killing those connections as if they'd failed). Matches
  // gj's own set_read_timeout(3600, 0) for the same reason on the client side
  // (streaming-backpressure-fix.md).
  server.set_write_timeout(3600, 0);
  ControlRoutes control_routes(library, session);
  control_routes.registerRoutes(server);
  StreamRoute stream_route(library, session);
  stream_route.registerRoutes(server);

  std::cout << "playback-controld: indexed " << library.allTracks().size()
            << " tracks, listening on 0.0.0.0:" << kPort << "\n";
  if (!server.listen("0.0.0.0", kPort)) {
    std::cerr << "playback-controld: fatal: failed to bind to port " << kPort << "\n";
    return 1;
  }
  return 0;
}
