#pragma once

#include <httplib.h>

#include "music_library.h"
#include "playback_session.h"

// HTTP handlers for play/pause/resume/stop/next/previous and the browse routes (design.md
// §3.3). Registered on the same httplib::Server as StreamRoute — not a separate listener.
class ControlRoutes {
 public:
  ControlRoutes(MusicLibrary& library, PlaybackSession& session);

  void registerRoutes(httplib::Server& server);

 private:
  void handlePlay(const httplib::Request& req, httplib::Response& res);
  void handlePause(const httplib::Request& req, httplib::Response& res);
  void handleResume(const httplib::Request& req, httplib::Response& res);
  void handleStop(const httplib::Request& req, httplib::Response& res);
  void handleNext(const httplib::Request& req, httplib::Response& res);
  void handlePrevious(const httplib::Request& req, httplib::Response& res);

  void handleListPlaylists(const httplib::Request& req, httplib::Response& res);
  void handleListArtists(const httplib::Request& req, httplib::Response& res);
  void handleListAlbums(const httplib::Request& req, httplib::Response& res);
  void handleListTracks(const httplib::Request& req, httplib::Response& res);

  // Shared by /next and /previous; direction is +1 or -1.
  void advanceQueue(httplib::Response& res, int direction);

  MusicLibrary& library_;
  PlaybackSession& session_;
};
