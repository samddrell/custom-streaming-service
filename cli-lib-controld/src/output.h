#pragma once

#include <string>

#include <json.hpp>

// Pure formatting functions (design.md §3.4) -- no I/O, no network, just json in, string out.
std::string formatTrack(const nlohmann::json& track);       // "Africa — Toto (Toto IV)"
std::string formatDuration(int seconds);                    // 295 -> "4:55"

// `body` is a full response like {"ok":true,"artists":[{"name":...,"tracks":[...]}]} --
// `key` picks which top-level array to render ("playlists"/"artists"/"albums").
std::string formatCollectionList(const nlohmann::json& body, const std::string& key);

std::string formatTrackList(const nlohmann::json& tracks);  // the /tracks array itself
std::string formatError(const nlohmann::json& body);        // "Error: not currently playing"
