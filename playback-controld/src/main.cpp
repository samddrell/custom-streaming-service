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
