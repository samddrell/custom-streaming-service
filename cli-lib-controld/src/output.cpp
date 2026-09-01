#include "output.h"

#include <iomanip>
#include <sstream>

std::string formatDuration(int seconds) {
  int minutes = seconds / 60;
  int secs = seconds % 60;
  std::ostringstream out;
  out << minutes << ':' << std::setfill('0') << std::setw(2) << secs;
  return out.str();
}

std::string formatTrack(const nlohmann::json& track) {
  return track.value("name", "") + " \xE2\x80\x94 " + track.value("artist", "") + " (" +
         track.value("album", "") + ")";
}

std::string formatCollectionList(const nlohmann::json& body, const std::string& key) {
  auto entries = body.value(key, nlohmann::json::array());
  if (entries.empty()) return "(none)";

  std::ostringstream out;
  for (const auto& entry : entries) {
    out << entry.value("name", "") << "\n";
    for (const auto& trackName : entry.value("tracks", nlohmann::json::array())) {
      out << "  " << trackName.get<std::string>() << "\n";
    }
  }
  std::string result = out.str();
  if (!result.empty()) result.pop_back();  // trailing newline
  return result;
}

std::string formatTrackList(const nlohmann::json& tracks) {
  if (tracks.empty()) return "(none)";

  std::ostringstream out;
  for (const auto& track : tracks) {
    out << formatTrack(track) << " [" << formatDuration(track.value("duration_seconds", 0))
        << "]\n";
  }
  std::string result = out.str();
  if (!result.empty()) result.pop_back();
  return result;
}

std::string formatError(const nlohmann::json& body) {
  // body can be null/non-object if the daemon's response couldn't be parsed as JSON at all
  // (found via real testing: a corrupted-Unicode request produced a response gj couldn't parse)
  // -- .value() throws on anything but an object, so guard it rather than crash on a bad reply.
  if (!body.is_object()) return "Error: unexpected (non-JSON) response from playback-controld";
  return "Error: " + body.value("error", "unknown error");
}
