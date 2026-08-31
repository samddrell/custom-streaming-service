#include "stream_route.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include <json.hpp>

using json = nlohmann::json;

StreamRoute::StreamRoute(MusicLibrary& library, PlaybackSession& session)
    : library_(library), session_(session) {}

void StreamRoute::registerRoutes(httplib::Server& server) {
  server.Get("/stream", [this](const httplib::Request& req, httplib::Response& res) {
    handleStream(req, res);
  });
}

void StreamRoute::handleStream(const httplib::Request&, httplib::Response& res) {
  Track track;
  uint64_t generation;
  {
    std::lock_guard<std::mutex> lock(session_.mutex);
    if (!session_.current_track) {
      res.status = 404;
      res.set_content(json{{"ok", false}, {"error", "nothing playing"}}.dump(),
                      "application/json");
      return;
    }
    track = *session_.current_track;
    generation = session_.generation;
  }

  auto file = std::make_shared<std::ifstream>(library_.resolveAbsolutePath(track),
                                               std::ios::binary);
  if (!file->is_open()) {
    res.status = 404;
    res.set_content(json{{"ok", false}, {"error", "track file unavailable on disk"}}.dump(),
                    "application/json");
    return;
  }

  // Chunked, not the plain without-length variant: a connection close with no Content-Length
  // and no Transfer-Encoding is valid, complete HTTP framing (RFC 9112 §6), so curl treats an
  // interrupted plain stream as a *successful*, merely short, download — verified against a
  // real curl client, not just read from the spec. Chunked framing has an explicit terminator
  // ("0\r\n\r\n"); cutting it off before that is a detectable protocol violation, which is what
  // actually produces the transfer-error diagnostic requirements.md §4.3 describes.
  res.set_chunked_content_provider(
      "audio/mpeg",
      [this, file, generation](size_t /*offset*/, httplib::DataSink& sink) -> bool {
        {
          std::unique_lock<std::mutex> lock(session_.mutex);
          session_.cv.wait(lock, [&] {
            return session_.generation != generation ||
                   session_.status != PlaybackSession::Status::Paused;
          });
          if (session_.generation != generation) {
            // Superseded by /play, /stop, /next, or /previous — abrupt end, no clean
            // terminator. This is the interrupt-alert mechanism (design.md §3.2/§3.4):
            // the interrupted curl process surfaces its own transfer-error on stderr.
            return false;
          }
        }

        char buffer[kChunkSize];
        file->read(buffer, sizeof(buffer));
        std::streamsize n = file->gcount();
        if (n > 0 && !sink.write(buffer, static_cast<size_t>(n))) {
          return false;
        }
        if (n == 0 || file->eof()) {
          // Queue auto-advance (requirements.md §4.4, design.md §3.4): if there's another track
          // queued after this one, move to it and keep streaming on this same connection instead
          // of ending the response. Deliberately does not touch `generation` or notify `cv` —
          // this is the same playback session continuing, not an interrupt (§3.2), so the
          // interrupt check above is unaffected by this branch.
          bool advanced = false;
          std::filesystem::path next_path;
          {
            std::lock_guard<std::mutex> lock(session_.mutex);
            advanced = session_.advance(+1);
            if (advanced) {
              next_path = library_.resolveAbsolutePath(*session_.current_track);
            } else {
              // Queue exhausted: reset state the same way handleStop does, so a client
              // reconnecting after this point gets 404 instead of replaying the last track
              // forever. Still no generation bump / cv.notify_all() — this connection is ending
              // on its own, not being interrupted by another client.
              session_.current_track.reset();
              session_.current_queue.clear();
              session_.queue_position = 0;
              session_.status = PlaybackSession::Status::Stopped;
            }
          }
          if (!advanced) {
            sink.done();
            return true;
          }
          file->close();
          file->open(next_path, std::ios::binary);
          if (!file->is_open()) {
            std::cerr << "playback-controld: warning: auto-advance target file unavailable: "
                      << next_path << "\n";
            return false;
          }
        }
        return true;
      });
}
