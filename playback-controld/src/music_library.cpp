#include "music_library.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <json.hpp>

using json = nlohmann::json;

namespace {

bool hasMp3Extension(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".mp3";
}

}  // namespace

MusicLibrary::MusicLibrary(std::filesystem::path music_dir, std::filesystem::path storage_dir)
    : music_dir_(std::move(music_dir)), storage_dir_(std::move(storage_dir)) {}

std::filesystem::path MusicLibrary::resolveAbsolutePath(const Track& track) const {
  return music_dir_ / track.path;
}

void MusicLibrary::load() {
  std::error_code ec;
  if (!std::filesystem::is_directory(music_dir_, ec) || ec) {
    throw std::runtime_error("music directory not found or unreadable: " + music_dir_.string());
  }

  loadLibraryJson();
  scanAndTag();
  rebuildCollections();
  loadPlaylistsJson();
}

void MusicLibrary::loadLibraryJson() {
  tracks_.clear();
  index_by_path_.clear();
  index_by_name_.clear();

  std::filesystem::path library_json_path = storage_dir_ / "library.json";
  std::ifstream in(library_json_path);
  if (!in) return;  // no cache yet — first run

  json parsed;
  try {
    in >> parsed;
  } catch (const json::parse_error& e) {
    std::cerr << "warning: failed to parse " << library_json_path
              << ", starting from an empty index: " << e.what() << "\n";
    return;
  }

  for (const auto& t : parsed.value("tracks", json::array())) {
    Track track;
    track.path = t.value("path", "");
    track.name = t.value("name", "");
    track.artist = t.value("artist", "");
    track.album = t.value("album", "");
    track.duration_seconds = t.value("duration_seconds", 0);
    if (track.path.empty()) continue;
    tracks_.push_back(std::move(track));
  }

  for (size_t i = 0; i < tracks_.size(); ++i) {
    index_by_path_[tracks_[i].path] = i;
    auto [it, inserted] = index_by_name_.emplace(tracks_[i].name, i);
    if (!inserted) {
      std::cerr << "warning: track name '" << tracks_[i].name << "' at path '" << tracks_[i].path
                << "' collides with an existing entry at path '" << tracks_[it->second].path
                << "'; '/play?type=track&name=" << tracks_[i].name
                << "' will resolve to the first-indexed one\n";
    }
  }
}

void MusicLibrary::scanAndTag() {
  std::unordered_set<std::string> disk_paths;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           music_dir_, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& entry = *it;
    if (!entry.is_regular_file(ec) || ec) continue;
    if (!hasMp3Extension(entry.path())) continue;
    disk_paths.insert(std::filesystem::relative(entry.path(), music_dir_).generic_string());
  }

  std::vector<Track> kept;
  kept.reserve(tracks_.size());
  std::unordered_set<std::string> kept_paths;
  size_t removed_count = 0;
  for (auto& track : tracks_) {
    if (disk_paths.count(track.path)) {
      kept_paths.insert(track.path);
      kept.push_back(std::move(track));
    } else {
      ++removed_count;
    }
  }

  size_t added_count = 0;
  for (const auto& rel_path : disk_paths) {
    if (kept_paths.count(rel_path)) continue;

    std::filesystem::path abs_path = music_dir_ / rel_path;
    TagLib::FileRef file_ref(abs_path.string().c_str());

    Track track;
    track.path = rel_path;
    if (!file_ref.isNull() && file_ref.tag()) {
      TagLib::Tag* tag = file_ref.tag();
      track.name = tag->title().to8Bit(true);
      track.artist = tag->artist().to8Bit(true);
      track.album = tag->album().to8Bit(true);
    }
    if (track.name.empty()) {
      track.name = std::filesystem::path(rel_path).filename().stem().string();
    }
    if (!file_ref.isNull() && file_ref.audioProperties()) {
      track.duration_seconds = file_ref.audioProperties()->lengthInSeconds();
    }

    kept.push_back(std::move(track));
    ++added_count;
  }

  tracks_ = std::move(kept);

  index_by_path_.clear();
  index_by_name_.clear();
  for (size_t i = 0; i < tracks_.size(); ++i) {
    index_by_path_[tracks_[i].path] = i;
    auto [it, inserted] = index_by_name_.emplace(tracks_[i].name, i);
    if (!inserted) {
      std::cerr << "warning: track name '" << tracks_[i].name << "' at path '" << tracks_[i].path
                << "' collides with an existing entry at path '" << tracks_[it->second].path
                << "'; '/play?type=track&name=" << tracks_[i].name
                << "' will resolve to the first-indexed one\n";
    }
  }

  if (added_count > 0 || removed_count > 0) {
    saveLibraryJson();
  }
}

void MusicLibrary::saveLibraryJson() const {
  std::filesystem::create_directories(storage_dir_);

  json out;
  out["tracks"] = json::array();
  for (const auto& t : tracks_) {
    out["tracks"].push_back({{"path", t.path},
                              {"name", t.name},
                              {"artist", t.artist},
                              {"album", t.album},
                              {"duration_seconds", t.duration_seconds}});
  }

  std::filesystem::path library_json_path = storage_dir_ / "library.json";
  std::ofstream out_file(library_json_path);
  out_file << out.dump(2);
}

void MusicLibrary::loadPlaylistsJson() {
  playlists_.clear();
  playlist_by_name_.clear();

  std::filesystem::path playlists_json_path = storage_dir_ / "playlists.json";
  std::ifstream in(playlists_json_path);
  if (!in) return;  // no playlists defined — not an error

  json parsed;
  try {
    in >> parsed;
  } catch (const json::parse_error& e) {
    std::cerr << "warning: failed to parse " << playlists_json_path
              << ", continuing with no playlists: " << e.what() << "\n";
    return;
  }

  for (auto& [playlist_name, paths] : parsed.items()) {
    Playlist playlist;
    playlist.name = playlist_name;
    for (const auto& path_value : paths) {
      std::string rel_path = path_value.get<std::string>();
      auto it = index_by_path_.find(rel_path);
      if (it == index_by_path_.end()) {
        std::cerr << "warning: playlist '" << playlist_name << "' references unknown track path '"
                  << rel_path << "'; dropping that entry\n";
        continue;
      }
      const Track& track = tracks_[it->second];
      playlist.tracks.emplace_back(track.name, track);
    }
    playlist_by_name_[playlist.name] = playlists_.size();
    playlists_.push_back(std::move(playlist));
  }
}

void MusicLibrary::rebuildCollections() {
  albums_.clear();
  album_by_name_.clear();
  artists_.clear();
  artist_by_name_.clear();

  for (const auto& track : tracks_) {
    std::string album_key = track.album.empty() ? kUnknownAlbum : track.album;
    auto [album_it, album_inserted] = album_by_name_.try_emplace(album_key, albums_.size());
    if (album_inserted) albums_.push_back(Album{album_key, {}});
    albums_[album_it->second].tracks.emplace_back(track.name, track);

    std::string artist_key = track.artist.empty() ? kUnknownArtist : track.artist;
    auto [artist_it, artist_inserted] = artist_by_name_.try_emplace(artist_key, artists_.size());
    if (artist_inserted) artists_.push_back(Artist{artist_key, {}});
    artists_[artist_it->second].tracks.emplace_back(track.name, track);
  }
}

std::optional<Track> MusicLibrary::findTrack(const std::string& name) const {
  auto it = index_by_name_.find(name);
  if (it == index_by_name_.end()) return std::nullopt;
  return tracks_[it->second];
}

std::optional<Album> MusicLibrary::findAlbum(const std::string& name) const {
  auto it = album_by_name_.find(name);
  if (it == album_by_name_.end()) return std::nullopt;
  return albums_[it->second];
}

std::optional<Artist> MusicLibrary::findArtist(const std::string& name) const {
  auto it = artist_by_name_.find(name);
  if (it == artist_by_name_.end()) return std::nullopt;
  return artists_[it->second];
}

std::optional<Playlist> MusicLibrary::findPlaylist(const std::string& name) const {
  auto it = playlist_by_name_.find(name);
  if (it == playlist_by_name_.end()) return std::nullopt;
  return playlists_[it->second];
}
