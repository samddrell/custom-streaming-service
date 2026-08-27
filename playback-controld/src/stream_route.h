#pragma once

#include <httplib.h>

#include "music_library.h"
#include "playback_session.h"

// GET /stream — the one long-lived, one-directional route (design.md §3.4).
class StreamRoute {
 public:
  StreamRoute(MusicLibrary& library, PlaybackSession& session);

  void registerRoutes(httplib::Server& server);

 private:
  void handleStream(const httplib::Request& req, httplib::Response& res);

  MusicLibrary& library_;
  PlaybackSession& session_;

  static constexpr size_t kChunkSize = 64 * 1024;
};
