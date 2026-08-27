#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// Basic ID3-derived metadata for one mp3 file (requirements.md §5).
struct Track {
  std::string path;  // relative to the music root, e.g. "Artist - Song.mp3"
  std::string name;
  std::string artist;
  std::string album;
  int duration_seconds = 0;
};

// Shared by Album/Artist/Playlist: an ordered name -> Track mapping (design.md §3.1.3).
using TrackCollection = std::vector<std::pair<std::string, Track>>;

struct Album {
  std::string name;
  TrackCollection tracks;
};

struct Artist {
  std::string name;
  TrackCollection tracks;
};

struct Playlist {
  std::string name;
  TrackCollection tracks;
};

// Owns all knowledge of what music exists and where (design.md §3.1). Not thread-safe on its
// own: load() is called once at startup before the HTTP server starts handling requests, and
// every lookup/listing method afterward is read-only, so no locking is needed here — the only
// mutable shared state in the whole daemon is PlaybackSession.
class MusicLibrary {
 public:
  // Tracks with no album/artist tag are grouped under these rather than an empty-string key,
  // so /albums and /artists stay browsable for untagged tracks (design.md §3.1.3).
  static constexpr const char* kUnknownAlbum = "Unknown Album";
  static constexpr const char* kUnknownArtist = "Unknown Artist";

  MusicLibrary(std::filesystem::path music_dir, std::filesystem::path storage_dir);

  // Scans music_dir for .mp3 files not already in library.json, tags new ones via TagLib,
  // drops entries for files no longer on disk, persists library.json if anything changed, then
  // loads playlists.json and resolves it against the index. Throws std::runtime_error if
  // music_dir doesn't exist/isn't readable (design.md §6: exit non-zero on missing drive).
  void load();

  std::filesystem::path resolveAbsolutePath(const Track& track) const;

  std::optional<Track> findTrack(const std::string& name) const;
  std::optional<Album> findAlbum(const std::string& name) const;
  std::optional<Artist> findArtist(const std::string& name) const;
  std::optional<Playlist> findPlaylist(const std::string& name) const;

  const std::vector<Track>& allTracks() const { return tracks_; }
  const std::vector<Album>& allAlbums() const { return albums_; }
  const std::vector<Artist>& allArtists() const { return artists_; }
  const std::vector<Playlist>& allPlaylists() const { return playlists_; }

 private:
  void scanAndTag();
  void loadLibraryJson();
  void saveLibraryJson() const;
  void loadPlaylistsJson();
  void rebuildCollections();

  std::filesystem::path music_dir_;
  std::filesystem::path storage_dir_;

  std::vector<Track> tracks_;
  std::unordered_map<std::string, size_t> index_by_path_;
  // First track to claim a given name wins; a later collision is logged and left unreachable
  // by name (design.md has no stated policy here — see the load() implementation comment).
  std::unordered_map<std::string, size_t> index_by_name_;

  std::vector<Album> albums_;
  std::unordered_map<std::string, size_t> album_by_name_;
  std::vector<Artist> artists_;
  std::unordered_map<std::string, size_t> artist_by_name_;
  std::vector<Playlist> playlists_;
  std::unordered_map<std::string, size_t> playlist_by_name_;
};
