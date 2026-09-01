#include "commands.h"

#include <iostream>
#include <set>

#include "output.h"

namespace {

int reportUnreachable() {
  std::cerr << "Error: could not reach playback-controld at 192.168.86.28:8080\n";
  return 1;
}

// body.value(...) throws if body isn't a JSON object (including null) -- found via real testing
// that a corrupted-Unicode request can produce a response gj can't parse (see http_client.cpp's
// toApiResult), which must report as an ordinary error, not crash the whole process.
bool isOk(const nlohmann::json& body) { return body.is_object() && body.value("ok", false); }

// Shared shape behind every control command (design.md §3.3): connected? ok? print + exit.
// `successMessage` is printed on ok:true; `showTrack` appends the response's current_track,
// for commands that return one (play/skip/previous) vs. those that don't (pause/resume/stop).
int reportResult(const ApiResult& result, const std::string& successMessage, bool showTrack) {
  if (!result.connected) return reportUnreachable();
  if (!isOk(result.body)) {
    std::cerr << formatError(result.body) << "\n";
    return 1;
  }
  std::cout << successMessage;
  if (showTrack && result.body.contains("current_track")) {
    std::cout << ": " << formatTrack(result.body["current_track"]);
  }
  std::cout << ".\n";
  return 0;
}

}  // namespace

void printUsage() {
  std::cerr << "usage: gj <command> [args]\n"
                "  gj listen\n"
                "  gj play <track|album|artist|playlist> <name>\n"
                "  gj pause\n"
                "  gj resume\n"
                "  gj stop\n"
                "  gj skip\n"
                "  gj previous\n"
                "  gj list <playlists|artists|albums|tracks>\n";
}

int commandPlay(HttpClient& client, int argc, char** argv) {
  // argv[argc] is guaranteed nullptr by the C standard, but argv[argc + 1] and beyond is
  // genuinely out of bounds -- check argc first, always, before touching argv[2]/argv[3]
  // (design.md §3.3).
  if (argc < 4) {
    printUsage();
    return 1;
  }
  std::string type = argv[2];
  std::string name = argv[3];

  static const std::set<std::string> kValidTypes = {"track", "album", "artist", "playlist"};
  if (kValidTypes.find(type) == kValidTypes.end()) {
    std::cerr << "Error: invalid type '" << type << "' -- must be track|album|artist|playlist\n";
    return 1;
  }

  auto result = client.post("/play?type=" + type + "&name=" + urlEncode(name));
  return reportResult(result, "Now playing", /*showTrack=*/true);
}

int commandPause(HttpClient& client) { return reportResult(client.post("/pause"), "Paused", false); }

int commandResume(HttpClient& client) {
  return reportResult(client.post("/resume"), "Playing", false);
}

int commandStop(HttpClient& client) { return reportResult(client.post("/stop"), "Stopped", false); }

int commandNext(HttpClient& client) {
  return reportResult(client.post("/next"), "Now playing", /*showTrack=*/true);
}

int commandPrevious(HttpClient& client) {
  return reportResult(client.post("/previous"), "Now playing", /*showTrack=*/true);
}

int commandList(HttpClient& client, int argc, char** argv) {
  if (argc < 3) {
    printUsage();
    return 1;
  }
  std::string type = argv[2];

  std::string path;
  std::string key;
  bool isTracks = false;
  if (type == "playlists") {
    path = "/playlists";
    key = "playlists";
  } else if (type == "artists") {
    path = "/artists";
    key = "artists";
  } else if (type == "albums") {
    path = "/albums";
    key = "albums";
  } else if (type == "tracks") {
    path = "/tracks";
    isTracks = true;
  } else {
    std::cerr << "Error: invalid list type '" << type
              << "' -- must be playlists|artists|albums|tracks\n";
    return 1;
  }

  auto result = client.get(path);
  if (!result.connected) return reportUnreachable();
  if (!isOk(result.body)) {
    std::cerr << formatError(result.body) << "\n";
    return 1;
  }

  if (isTracks) {
    std::cout << formatTrackList(result.body.value("tracks", nlohmann::json::array())) << "\n";
  } else {
    std::cout << formatCollectionList(result.body, key) << "\n";
  }
  return 0;
}
