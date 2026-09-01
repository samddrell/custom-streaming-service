#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include "output.h"

TEST_CASE("formatTrack formats name, artist, album") {
  nlohmann::json track = {{"name", "Second"},
                           {"artist", "ArtistA"},
                           {"album", "AlbumX"},
                           {"duration_seconds", 7}};
  REQUIRE(formatTrack(track) == "Second \xE2\x80\x94 ArtistA (AlbumX)");
}

TEST_CASE("formatDuration under a minute") { REQUIRE(formatDuration(7) == "0:07"); }

TEST_CASE("formatDuration over a minute") { REQUIRE(formatDuration(125) == "2:05"); }

TEST_CASE("formatDuration on a minute boundary") { REQUIRE(formatDuration(60) == "1:00"); }

TEST_CASE("formatError") {
  nlohmann::json body = {{"ok", false}, {"error", "not currently playing"}};
  REQUIRE(formatError(body) == "Error: not currently playing");
}

TEST_CASE("formatCollectionList includes names and tracks") {
  nlohmann::json body = {
      {"artists",
       nlohmann::json::array({{{"name", "ArtistA"}, {"tracks", {"First", "Second"}}}})}};
  auto out = formatCollectionList(body, "artists");
  REQUIRE(out.find("ArtistA") != std::string::npos);
  REQUIRE(out.find("First") != std::string::npos);
  REQUIRE(out.find("Second") != std::string::npos);
}

TEST_CASE("formatCollectionList handles an empty list without crashing") {
  nlohmann::json body = {{"artists", nlohmann::json::array()}};
  REQUIRE_NOTHROW(formatCollectionList(body, "artists"));
  REQUIRE(formatCollectionList(body, "artists") == "(none)");
}

TEST_CASE("formatTrackList") {
  nlohmann::json tracks = nlohmann::json::array(
      {{{"name", "Second"}, {"artist", "ArtistA"}, {"album", "AlbumX"}, {"duration_seconds", 7}}});
  auto out = formatTrackList(tracks);
  REQUIRE(out.find("Second") != std::string::npos);
  REQUIRE(out.find("ArtistA") != std::string::npos);
  REQUIRE(out.find("0:07") != std::string::npos);
}
