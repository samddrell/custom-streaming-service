#include "http_client.h"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace {

ApiResult toApiResult(const httplib::Result& res) {
  ApiResult result;
  if (!res) return result;
  result.connected = true;
  result.status = res->status;
  try {
    result.body = nlohmann::json::parse(res->body);
  } catch (const nlohmann::json::exception&) {
    // playback-controld always returns well-formed JSON on every response path (design.md §5)
    // -- leave body null rather than guessing at a fallback shape if this ever happens.
  }
  return result;
}

}  // namespace

std::string urlEncode(const std::string& value) {
  std::ostringstream out;
  out << std::hex << std::uppercase << std::setfill('0');
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out << static_cast<char>(c);
    } else {
      out << '%' << std::setw(2) << static_cast<int>(c);
    }
  }
  return out.str();
}

HttpClient::HttpClient(const std::string& host, int port) : client_(host, port) {
  // cpp-httplib defaults both to 300s (design.md §3.2) -- far too long for a LAN-local tool
  // talking to one fixed Jetson. Connection timeout stays short so an unreachable daemon fails
  // fast; read timeout stays long since it also covers however long a track sits paused
  // (playback-controld blocks the connection open and idle while paused, sending nothing).
  client_.set_connection_timeout(5, 0);
  client_.set_read_timeout(3600, 0);
}

ApiResult HttpClient::post(const std::string& path) { return toApiResult(client_.Post(path)); }

ApiResult HttpClient::get(const std::string& path) { return toApiResult(client_.Get(path)); }

HttpClient::StreamResult HttpClient::stream(const std::string& path, const ChunkSink& sink) {
  int status = 0;
  bool wrote_audio = false;
  std::string error_body;

  auto res = client_.Get(
      path,
      [&](const httplib::Response& response) {
        status = response.status;
        return true;  // never cancel here -- just capture status before the body starts
      },
      [&](const char* data, size_t len) {
        if (status == 404) {
          error_body.append(data, len);  // small JSON error body -- read, not played
          return true;
        }
        wrote_audio = wrote_audio || len > 0;
        return sink(data, len);  // false here also aborts httplib's read loop
      });

  if (!res) {
    return {StreamResult::Outcome::ConnectionFailed, wrote_audio, {}};
  }
  if (res->status == 404) {
    std::string message;
    try {
      message = nlohmann::json::parse(error_body).value("error", "");
    } catch (const nlohmann::json::exception&) {
      // Malformed body -- leave message empty, the caller has a sensible fallback string.
    }
    return {StreamResult::Outcome::NothingPlaying, wrote_audio, message};
  }
  return {StreamResult::Outcome::StreamEnded, wrote_audio, {}};
}
