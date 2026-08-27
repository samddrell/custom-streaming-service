#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <type_traits>

#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <json.hpp>

#include "music_library.h"

namespace {

// RAII temp directory, unique per instance, removed on destruction.
class TempDir {
 public:
  explicit TempDir(const std::string& prefix) {
    std::string tmpl =
        (std::filesystem::temp_directory_path() / (prefix + "_XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (mkdtemp(buf.data()) == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = std::filesystem::path(buf.data());
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::filesystem::path fixturesDir() { return std::filesystem::path(FIXTURES_DIR); }

// Copies the standard 3-track fixture set (ArtistA/AlbumX/{01 First,02 Second}.mp3,
// ArtistB/AlbumY/01 Only.mp3 — matches test-plan.md §2) into dest.
void copyBaseFixtures(const std::filesystem::path& dest) {
  std::filesystem::copy(fixturesDir() / "ArtistA", dest / "ArtistA",
                        std::filesystem::copy_options::recursive);
  std::filesystem::copy(fixturesDir() / "ArtistB", dest / "ArtistB",
                        std::filesystem::copy_options::recursive);
}

void writePlaylistsJson(const std::filesystem::path& storage_dir, const nlohmann::json& content) {
  std::ofstream out(storage_dir / "playlists.json");
  out << content.dump(2);
}

std::optional<Track> trackNamed(const std::vector<Track>& tracks, const std::string& name) {
  for (const auto& t : tracks) {
    if (t.name == name) return t;
  }
  return std::nullopt;
}

// Compile-time enforcement of requirements.md §5: track position/number is not stored on
// Track — it's contextual to whichever Album/Playlist collection contains it.
template <typename T, typename = void>
struct HasTrackNumberMember : std::false_type {};
template <typename T>
struct HasTrackNumberMember<T, std::void_t<decltype(std::declval<T>().track_number)>>
    : std::true_type {};
static_assert(!HasTrackNumberMember<Track>::value,
             "Track must not store a position/track_number field (requirements.md §5)");

}  // namespace

TEST_CASE("UT-1: cold index build tags all fixture tracks correctly") {
  TempDir music_dir("pcd_music");
  TempDir storage_dir("pcd_storage");
  copyBaseFixtures(music_dir.path());

  MusicLibrary library(music_dir.path(), storage_dir.path());
  library.load();

  const auto& tracks = library.allTracks();
  REQUIRE(tracks.size() == 3);

  auto first = trackNamed(tracks, "First");
  REQUIRE(first.has_value());
  CHECK(first->artist == "ArtistA");
  CHECK(first->album == "AlbumX");
  CHECK(first->duration_seconds > 0);

  auto second = trackNamed(tracks, "Second");
  REQUIRE(second.has_value());
  CHECK(second->artist == "ArtistA");
  CHECK(second->album == "AlbumX");

  // UT-7: no title tag -> name falls back to the filename with .mp3 stripped.
  auto only = trackNamed(tracks, "01 Only");
  REQUIRE(only.has_value());
  CHECK(only->artist == "ArtistB");
  CHECK(only->album == "AlbumY");
}

TEST_CASE("UT-2: a path already in the index is never re-tagged") {
  TempDir music_dir("pcd_music");
  TempDir storage_dir("pcd_storage");
  copyBaseFixtures(music_dir.path());

  MusicLibrary first_library(music_dir.path(), storage_dir.path());
  first_library.load();
  std::vector<Track> first_run_tracks = first_library.allTracks();
  REQUIRE(first_run_tracks.size() == 3);

  // Modify "First"'s tags in place, at the same path, without touching library.json.
  std::filesystem::path first_path = music_dir.path() / "ArtistA" / "AlbumX" / "01 First.mp3";
  {
    TagLib::FileRef file_ref(first_path.string().c_str());
    REQUIRE_FALSE(file_ref.isNull());
    file_ref.tag()->setTitle("Retagged");
    file_ref.tag()->setArtist("SomeoneElse");
    file_ref.tag()->setAlbum("DifferentAlbum");
    file_ref.save();
  }

  MusicLibrary second_library(music_dir.path(), storage_dir.path());
  second_library.load();
  const auto& second_run_tracks = second_library.allTracks();

  REQUIRE(second_run_tracks.size() == first_run_tracks.size());
  // Byte-identical field values to the first run — the modified tags were not picked up.
  auto first_again = trackNamed(second_run_tracks, "First");
  REQUIRE(first_again.has_value());
  CHECK(first_again->artist == "ArtistA");
  CHECK(first_again->album == "AlbumX");
  CHECK_FALSE(trackNamed(second_run_tracks, "Retagged").has_value());
}

TEST_CASE("UT-3: a newly added file is tagged without disturbing existing entries") {
  TempDir music_dir("pcd_music");
  TempDir storage_dir("pcd_storage");
  copyBaseFixtures(music_dir.path());

  MusicLibrary first_library(music_dir.path(), storage_dir.path());
  first_library.load();
  std::vector<Track> original_tracks = first_library.allTracks();
  REQUIRE(original_tracks.size() == 3);

  std::filesystem::create_directories(music_dir.path() / "ArtistA" / "AlbumX");
  std::filesystem::copy_file(fixturesDir() / "extra" / "04 Fourth.mp3",
                             music_dir.path() / "ArtistA" / "AlbumX" / "04 Fourth.mp3");

  MusicLibrary second_library(music_dir.path(), storage_dir.path());
  second_library.load();
  const auto& tracks = second_library.allTracks();

  REQUIRE(tracks.size() == 4);
  for (const auto& original : original_tracks) {
    auto found = trackNamed(tracks, original.name);
    REQUIRE(found.has_value());
    CHECK(found->artist == original.artist);
    CHECK(found->album == original.album);
    CHECK(found->path == original.path);
  }
  auto fourth = trackNamed(tracks, "Fourth");
  REQUIRE(fourth.has_value());
  CHECK(fourth->artist == "ArtistA");
  CHECK(fourth->album == "AlbumX");
}

TEST_CASE("UT-4: Track has no stored position field") {
  // Enforced at compile time above (static_assert); this test case exists for 1:1
  // traceability with test-plan.md's UT-4.
  SUCCEED("Track has no track_number/position member (see static_assert above)");
}

TEST_CASE("UT-5: playlist track order matches playlists.json array order") {
  TempDir music_dir("pcd_music");
  TempDir storage_dir("pcd_storage");
  copyBaseFixtures(music_dir.path());
  writePlaylistsJson(storage_dir.path(),
                     {{"Mix", {"ArtistA/AlbumX/02 Second.mp3", "ArtistB/AlbumY/01 Only.mp3"}}});

  MusicLibrary library(music_dir.path(), storage_dir.path());
  library.load();

  auto mix = library.findPlaylist("Mix");
  REQUIRE(mix.has_value());
  REQUIRE(mix->tracks.size() == 2);
  CHECK(mix->tracks[0].first == "Second");
  CHECK(mix->tracks[1].first == "01 Only");
}

TEST_CASE("UT-6: a playlist entry with an unknown path is dropped, not the whole playlist") {
  TempDir music_dir("pcd_music");
  TempDir storage_dir("pcd_storage");
  copyBaseFixtures(music_dir.path());
  writePlaylistsJson(storage_dir.path(),
                     {{"Mix",
                       {"ArtistA/AlbumX/02 Second.mp3", "ArtistZ/Fake/00 Nope.mp3",
                        "ArtistB/AlbumY/01 Only.mp3"}}});

  std::ostringstream captured;
  std::streambuf* old_cerr = std::cerr.rdbuf(captured.rdbuf());
  MusicLibrary library(music_dir.path(), storage_dir.path());
  library.load();
  std::cerr.rdbuf(old_cerr);

  auto mix = library.findPlaylist("Mix");
  REQUIRE(mix.has_value());
  REQUIRE(mix->tracks.size() == 2);
  CHECK(mix->tracks[0].first == "Second");
  CHECK(mix->tracks[1].first == "01 Only");
  CHECK(captured.str().find("ArtistZ/Fake/00 Nope.mp3") != std::string::npos);
}

TEST_CASE("UT-7: track name derivation — title tag vs filename fallback") {
  TempDir music_dir("pcd_music");
  TempDir storage_dir("pcd_storage");
  copyBaseFixtures(music_dir.path());

  MusicLibrary library(music_dir.path(), storage_dir.path());
  library.load();

  CHECK(trackNamed(library.allTracks(), "First").has_value());
  CHECK(trackNamed(library.allTracks(), "Second").has_value());
  CHECK(trackNamed(library.allTracks(), "01 Only").has_value());
}

TEST_CASE("UT-8: missing album/artist groups into Unknown Album/Unknown Artist") {
  TempDir music_dir("pcd_music");
  TempDir storage_dir("pcd_storage");
  std::filesystem::create_directories(music_dir.path());
  std::filesystem::copy_file(fixturesDir() / "extra" / "05 NoTags.mp3",
                             music_dir.path() / "05 NoTags.mp3");

  MusicLibrary library(music_dir.path(), storage_dir.path());
  library.load();

  // library.json (the raw index) keeps the empty value as-is — no fabrication at index time.
  auto track = trackNamed(library.allTracks(), "NoTags");
  REQUIRE(track.has_value());
  CHECK(track->album.empty());
  CHECK(track->artist.empty());

  auto unknown_album = library.findAlbum(MusicLibrary::kUnknownAlbum);
  REQUIRE(unknown_album.has_value());
  bool found_in_album = false;
  for (const auto& [name, t] : unknown_album->tracks) {
    if (name == "NoTags") found_in_album = true;
  }
  CHECK(found_in_album);

  auto unknown_artist = library.findArtist(MusicLibrary::kUnknownArtist);
  REQUIRE(unknown_artist.has_value());
  bool found_in_artist = false;
  for (const auto& [name, t] : unknown_artist->tracks) {
    if (name == "NoTags") found_in_artist = true;
  }
  CHECK(found_in_artist);
}
