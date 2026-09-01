#pragma once

#include <cstdio>
#include <string>

#include <httplib.h>
#include <json.hpp>

// Two response shapes gj needs (design.md §3.2): a plain request/response for control & browse
// routes, and a streaming call for /stream whose result must distinguish "no usable response at
// all" from an actual status code.
struct ApiResult {
  bool connected = false;  // false = couldn't get any HTTP response at all
  int status = 0;          // valid only if connected
  nlohmann::json body;     // valid only if connected and a body was returned
};

class HttpClient {
 public:
  HttpClient(const std::string& host, int port);

  ApiResult post(const std::string& path);  // control routes: /play, /pause, /resume, ...
  ApiResult get(const std::string& path);   // browse routes: /playlists, /artists, ...

  struct StreamResult {
    enum class Outcome { ConnectionFailed, NothingPlaying, StreamEnded } outcome;
    bool received_data = false;  // true if >=1 audio byte was written to `sink` this attempt
    std::string message;         // populated for NothingPlaying: the daemon's own "error" field
  };
  StreamResult stream(const std::string& path, FILE* sink);  // GET /stream, for gj listen

 private:
  httplib::Client client_;
};

// Percent-encodes everything outside A-Za-z0-9-_.~, for building /play's query string.
std::string urlEncode(const std::string& value);
