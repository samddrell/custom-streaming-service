#include "control_routes.h"

#include <json.hpp>

using json = nlohmann::json;

namespace {

const char* statusName(PlaybackSession::Status status) {
  switch (status) {
    case PlaybackSession::Status::Stopped:
      return "Stopped";
    case PlaybackSession::Status::Playing:
      return "Playing";
    case PlaybackSession::Status::Paused:
      return "Paused";
  }
  return "Unknown";
}

json trackJson(const Track& t) {
  return {{"name", t.name},
          {"artist", t.artist},
          {"album", t.album},
          {"duration_seconds", t.duration_seconds}};
}

json trackNamesJson(const TrackCollection& tracks) {
  json names = json::array();
  for (const auto& [name, track] : tracks) names.push_back(name);
  return names;
}

std::vector<Track> extractQueue(const TrackCollection& tracks) {
  std::vector<Track> queue;
  queue.reserve(tracks.size());
  for (const auto& [name, track] : tracks) queue.push_back(track);
  return queue;
}

void writeError(httplib::Response& res, int status, const std::string& message) {
  res.status = status;
  res.set_content(json{{"ok", false}, {"error", message}}.dump(), "application/json");
}

}  // namespace

ControlRoutes::ControlRoutes(MusicLibrary& library, PlaybackSession& session)
    : library_(library), session_(session) {}

void ControlRoutes::registerRoutes(httplib::Server& server) {
  server.Post("/play", [this](const httplib::Request& req, httplib::Response& res) {
    handlePlay(req, res);
  });
  server.Post("/pause", [this](const httplib::Request& req, httplib::Response& res) {
    handlePause(req, res);
  });
  server.Post("/resume", [this](const httplib::Request& req, httplib::Response& res) {
    handleResume(req, res);
  });
  server.Post("/stop", [this](const httplib::Request& req, httplib::Response& res) {
    handleStop(req, res);
  });
  server.Post("/next", [this](const httplib::Request& req, httplib::Response& res) {
    handleNext(req, res);
  });
  server.Post("/previous", [this](const httplib::Request& req, httplib::Response& res) {
    handlePrevious(req, res);
  });
  server.Get("/playlists", [this](const httplib::Request& req, httplib::Response& res) {
    handleListPlaylists(req, res);
  });
  server.Get("/artists", [this](const httplib::Request& req, httplib::Response& res) {
    handleListArtists(req, res);
  });
  server.Get("/albums", [this](const httplib::Request& req, httplib::Response& res) {
    handleListAlbums(req, res);
  });
  server.Get("/tracks", [this](const httplib::Request& req, httplib::Response& res) {
    handleListTracks(req, res);
  });
}

void ControlRoutes::handlePlay(const httplib::Request& req, httplib::Response& res) {
  if (!req.has_param("type") || !req.has_param("name")) {
    writeError(res, 400, "missing required query params 'type' and 'name'");
    return;
  }
  std::string type = req.get_param_value("type");
  std::string name = req.get_param_value("name");

  std::vector<Track> queue;
  if (type == "track") {
    auto track = library_.findTrack(name);
    if (!track) {
      writeError(res, 404, "no track named '" + name + "'");
      return;
    }
    queue = {*track};
  } else if (type == "album") {
    auto album = library_.findAlbum(name);
    if (!album) {
      writeError(res, 404, "no album named '" + name + "'");
      return;
    }
    queue = extractQueue(album->tracks);
  } else if (type == "artist") {
    auto artist = library_.findArtist(name);
    if (!artist) {
      writeError(res, 404, "no artist named '" + name + "'");
      return;
    }
    queue = extractQueue(artist->tracks);
  } else if (type == "playlist") {
    auto playlist = library_.findPlaylist(name);
    if (!playlist) {
      writeError(res, 404, "no playlist named '" + name + "'");
      return;
    }
    queue = extractQueue(playlist->tracks);
  } else {
    writeError(res, 400, "invalid type '" + type + "' — must be track|album|artist|playlist");
    return;
  }

  if (queue.empty()) {
    // An album/artist/playlist that resolves to zero playable tracks (e.g. every playlist
    // entry was unresolvable) — nothing to play, same shape of failure as "not found".
    writeError(res, 404, "'" + name + "' has no playable tracks");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(session_.mutex);
    session_.current_queue = std::move(queue);
    session_.queue_position = 0;
    session_.current_track = session_.current_queue.front();
    ++session_.generation;
    session_.status = PlaybackSession::Status::Playing;
  }
  session_.cv.notify_all();

  res.set_content(json{{"ok", true},
                       {"status", "Playing"},
                       {"current_track", trackJson(*session_.current_track)}}
                      .dump(),
                  "application/json");
}

void ControlRoutes::handlePause(const httplib::Request&, httplib::Response& res) {
  bool ok;
  {
    std::lock_guard<std::mutex> lock(session_.mutex);
    ok = session_.status == PlaybackSession::Status::Playing;
    if (ok) session_.status = PlaybackSession::Status::Paused;
  }
  if (!ok) {
    writeError(res, 409, "not currently playing");
    return;
  }
  res.set_content(json{{"ok", true}, {"status", "Paused"}}.dump(), "application/json");
}

void ControlRoutes::handleResume(const httplib::Request&, httplib::Response& res) {
  bool ok;
  {
    std::lock_guard<std::mutex> lock(session_.mutex);
    ok = session_.status == PlaybackSession::Status::Paused;
    if (ok) session_.status = PlaybackSession::Status::Playing;
  }
  if (!ok) {
    writeError(res, 409, "not currently paused");
    return;
  }
  session_.cv.notify_all();
  res.set_content(json{{"ok", true}, {"status", "Playing"}}.dump(), "application/json");
}

void ControlRoutes::handleStop(const httplib::Request&, httplib::Response& res) {
  {
    std::lock_guard<std::mutex> lock(session_.mutex);
    session_.current_track.reset();
    session_.current_queue.clear();
    session_.queue_position = 0;
    session_.status = PlaybackSession::Status::Stopped;
    ++session_.generation;
  }
  session_.cv.notify_all();
  res.set_content(json{{"ok", true}, {"status", "Stopped"}}.dump(), "application/json");
}

void ControlRoutes::handleNext(const httplib::Request&, httplib::Response& res) {
  advanceQueue(res, +1);
}

void ControlRoutes::handlePrevious(const httplib::Request&, httplib::Response& res) {
  advanceQueue(res, -1);
}

void ControlRoutes::advanceQueue(httplib::Response& res, int direction) {
  bool ok = false;
  bool nothing_playing = false;
  Track new_track;
  {
    std::lock_guard<std::mutex> lock(session_.mutex);
    if (session_.status == PlaybackSession::Status::Stopped || session_.current_queue.empty()) {
      nothing_playing = true;
    } else {
      long next_position = static_cast<long>(session_.queue_position) + direction;
      if (next_position < 0 || next_position >= static_cast<long>(session_.current_queue.size())) {
        ok = false;
      } else {
        session_.queue_position = static_cast<size_t>(next_position);
        session_.current_track = session_.current_queue[session_.queue_position];
        ++session_.generation;
        ok = true;
        new_track = *session_.current_track;
      }
    }
  }
  if (nothing_playing) {
    writeError(res, 409, "nothing playing");
    return;
  }
  if (!ok) {
    writeError(res, 409, direction > 0 ? "already at the end of the queue"
                                       : "already at the start of the queue");
    return;
  }
  session_.cv.notify_all();
  res.set_content(
      json{{"ok", true}, {"status", "Playing"}, {"current_track", trackJson(new_track)}}.dump(),
      "application/json");
}

void ControlRoutes::handleListPlaylists(const httplib::Request&, httplib::Response& res) {
  json playlists = json::array();
  for (const auto& p : library_.allPlaylists()) {
    playlists.push_back({{"name", p.name}, {"tracks", trackNamesJson(p.tracks)}});
  }
  res.set_content(json{{"ok", true}, {"playlists", playlists}}.dump(), "application/json");
}

void ControlRoutes::handleListArtists(const httplib::Request&, httplib::Response& res) {
  json artists = json::array();
  for (const auto& a : library_.allArtists()) {
    artists.push_back({{"name", a.name}, {"tracks", trackNamesJson(a.tracks)}});
  }
  res.set_content(json{{"ok", true}, {"artists", artists}}.dump(), "application/json");
}

void ControlRoutes::handleListAlbums(const httplib::Request&, httplib::Response& res) {
  json albums = json::array();
  for (const auto& a : library_.allAlbums()) {
    albums.push_back({{"name", a.name}, {"tracks", trackNamesJson(a.tracks)}});
  }
  res.set_content(json{{"ok", true}, {"albums", albums}}.dump(), "application/json");
}

void ControlRoutes::handleListTracks(const httplib::Request&, httplib::Response& res) {
  json tracks = json::array();
  for (const auto& t : library_.allTracks()) tracks.push_back(trackJson(t));
  res.set_content(json{{"ok", true}, {"tracks", tracks}}.dump(), "application/json");
}
