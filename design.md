# Jetson Music Streaming Service — Design Doc (v1)

Implements the requirements in `requirements.md`. Language: C++.

## 1. Technology Choices

| Concern | Choice | Rationale |
|---|---|---|
| Language / standard | C++17 | Broad compiler support, sufficient feature set (no need for C++20). |
| Build system | CMake, built natively on the Jetson | Standard tooling, avoids cross-compilation setup for a prototype. |
| HTTP | [cpp-httplib](https://github.com/yhirose/cpp-httplib) | Single-header, no external build dependency, supports chunked/streaming responses and early connection termination. |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) | Header-only, pairs naturally with cpp-httplib; used for the persisted library index and for parsing the hand-authored playlists file. |
| ID3 tag parsing | [TagLib](https://taglib.org/) | Mature, handles ID3v1/v2 edge cases; one real external dependency, worth it over hand-rolled parsing. |
| Testing | Catch2 (header-only) | Unit tests for library indexing/parsing logic; manual curl-driven testing for the HTTP layer. |
| Threading | `std::thread`, `std::mutex`, `std::condition_variable` | Sufficient for the concurrency model below; no async framework needed. |

## 2. Process Architecture

Single process, single binary: `playback-controld`. There is exactly **one** `httplib::Server`
instance — the single front door for every HTTP request, control or streaming, on one port.
Adding a future command means writing one more handler class and registering it on this same
server; nothing else in the architecture changes. `ControlRoutes` and `StreamRoute` (below) are
not separate listeners — they're handler objects the one server dispatches to, organized into
separate classes purely so the code for "request/response commands" and "the one long-lived,
one-directional stream" doesn't get tangled together.

```
                         all requests (one port)
                                  |
                                  v
                        +--------------------+
                        |   httplib::Server   |
                        +--------------------+
                         /        |         \
                        v         v          v
              +-------------+ +----------+ +---------------------+
              | ControlRoutes| | StreamRoute| |  (browse routes    |
              | (play/pause/ | | (GET       | |   share ControlRts)|
              |  stop/skip)  | | /stream)   | |                     |
              +-------------+ +----------+ +---------------------+
                     |               |
                     +-------+-------+
                             v
                  +-----------------------+
                  |   PlaybackSession      |  <- mutex-guarded shared state
                  |   (now playing, status,|
                  |    generation counter) |
                  +-----------------------+
                             |
                             v
                  +-----------------------+
                  |    MusicLibrary        |  <- internal module, not a
                  |  (index, playlists,    |     separate service/process
                  |   artists, albums)     |
                  +-----------------------+
                        |              |
                        v              v
              USB-C drive       Jetson-local storage
              (mp3 files only)  (library.json cache,
                                 playlists.json)
```

## 3. Components

### 3.1 `MusicLibrary`

Owns all knowledge of what music exists and where. Not a separate service — an internal class
`playback-controld` calls directly (matches the requirements doc's "internal module boundary"
decision).

Responsibilities:
- On startup, list `.mp3` files on the mounted USB drive (a cheap filesystem walk — no file
  contents read yet).
- For each file path not already present in the cached index, read ID3 tags via TagLib once to
  populate `name` (from the ID3 title tag), `artist`, `album`, `duration`, and add it to the
  index. If the title tag is missing or empty, `name` falls back to the filename with its
  extension stripped (e.g. `01 First.mp3` → `01 First`). A path already in the index
  is never re-tagged automatically — if a file's tags change in place, the fix is to delete
  `library.json` and let it rebuild from scratch. This keeps the indexing logic simple: no
  mtime bookkeeping, no change-detection, at the cost of not noticing in-place tag edits.
- If a file's `album` or `artist` ID3 tag is missing/empty, the index stores it as an empty
  string in `library.json` (the raw TagLib read is persisted as-is) — `MusicLibrary` does not
  fabricate a value at index time. The `"Unknown Album"` / `"Unknown Artist"` substitution
  described in §3.1.3 happens only when grouping tracks into Album/Artist collections, so a
  later real tag fix (after deleting `library.json` and rebuilding) is picked up normally rather
  than being masked by a previously-written placeholder.
- Persist the resulting index to `library.json` on Jetson-local storage after any change.
- Load `playlists.json` from Jetson-local storage (hand-authored by the user — see §3.1.1) and
  resolve each entry's track paths against the index. Any entry whose path isn't found in the
  index (typo, or the file was removed since the playlist was written) is logged as a warning
  to the console and dropped from that playlist — it's simply never in the resolved track list,
  so playing that playlist skips straight over it to the next valid track. This differs from a
  malformed `playlists.json` (§6): that's a whole-file parse failure, this is a single bad
  reference inside an otherwise-valid file.
- Expose lookups: by track name, by album, by artist, by playlist name, and full listings of
  each, for the browse routes.

Note: Artists and Albums are derived automatically from ID3 tags during the scan — there is no
manual file for those. Playlists are the one collection type that must be defined by hand,
since there's no ID3 concept of "playlist."

#### 3.1.1 `playlists.json` format

Hand-edited by the user directly on the Jetson's local filesystem
(e.g. `/var/lib/playback-controld/playlists.json`):

```json
{
  "Road Trip": ["Artist/Album/03 Track.mp3", "Artist2/Album2/01 Other.mp3"],
  "Chill": ["Artist3/Album3/05 Song.mp3"]
}
```

Order in each array is preserved as the playlist's track order.

#### 3.1.2 `library.json` format (index cache)

```json
{
  "tracks": [
    {
      "path": "Artist/Album/03 Track.mp3",
      "name": "Track",
      "artist": "Artist",
      "album": "Album",
      "duration_seconds": 214
    }
  ]
}
```

Updated incrementally: paths no longer present on disk are dropped; new paths are tagged and
added. An existing path already in the file is never re-read or re-tagged.

### 3.1.3 Collection representation (Album / Artist / Playlist)

Per requirements.md §5, these three share one structure — an ordered list of (name, Track)
pairs:

```cpp
using TrackCollection = std::vector<std::pair<std::string, Track>>; // ordered name -> track
```

`Album`, `Artist`, and `Playlist` are each just a named `TrackCollection`:

```cpp
struct Album    { std::string name; TrackCollection tracks; };
struct Artist   { std::string name; TrackCollection tracks; };
struct Playlist { std::string name; TrackCollection tracks; };
```

Albums and Artists are populated automatically from the library index, grouping tracks by their
`album`/`artist` field in the order encountered during the scan. A track whose `album` (or
`artist`) field is empty is grouped into a single synthetic `"Unknown Album"` (`"Unknown Artist"`)
collection rather than an empty-string-keyed bucket or being dropped from grouping entirely — so
`GET /albums`/`/artists` and `/play?type=album&name=Unknown Album` stay usable for untagged
tracks instead of silently hiding them. This substitution happens in-memory when building
`TrackCollection`s, not when writing `library.json` (§3.1). Playlists are populated by
resolving `playlists.json` entries against the index, in the file's array order (§3.1.1), with
unresolvable entries dropped and logged (§3.1, §6).

### 3.2 `PlaybackSession`

Shared mutable state, guarded by a single `std::mutex`. Represents "what's happening right now":

```cpp
struct PlaybackSession {
    std::mutex mutex;
    std::condition_variable cv;         // wakes a stream blocked by Paused, or by an interrupt
    enum class Status { Stopped, Playing, Paused } status = Status::Stopped;
    std::optional<Track> current_track;
    std::vector<Track> current_queue;   // resolved tracks for the active album/playlist/artist
    size_t queue_position = 0;
    uint64_t generation = 0;            // bumped on every play/stop/next/previous
};
```

`generation` is the mechanism behind the interrupt-alert design from the requirements doc: the
`StreamRoute`'s content-provider loop captures the generation at stream start and checks it on
every chunk; if it no longer matches, the stream ends immediately without a proper terminating
chunk, so the interrupted client's `curl` reports a transfer error on stderr. `Pause`/`Resume`
deliberately do **not** bump `generation` — pausing keeps the same stream connection alive
(§3.4), it doesn't interrupt it.

Every handler that bumps `generation` or changes `status` calls `cv.notify_all()` afterward, so
a stream currently blocked waiting out a pause wakes up immediately on any of: `/resume` (status
changes to `Playing`), or `/play`/`/stop`/`/next`/`/previous` (generation changes, superseding
it regardless of whether it happened to be paused at the time).

### 3.3 `ControlRoutes`

HTTP handlers registered on the shared server for:

| Method | Route | Behavior |
|---|---|---|
| POST | `/play?type=track\|album\|artist\|playlist&name=<name>` | Look up `name` in `MusicLibrary`, resolve to a queue of tracks, lock `PlaybackSession`, set it as current, bump `generation`, set status `Playing`. 404 if `name` not found — no state change. |
| POST | `/pause` | Lock session, set status `Paused` if currently `Playing`, else 409. Does not touch `generation` — the active `StreamRoute` connection stays open and simply stops sending further chunks (§3.4) until resumed or superseded. |
| POST | `/resume` | Lock session, set status `Playing` if currently `Paused`, else 409. Calls `cv.notify_all()` so a blocked stream immediately resumes sending from its current file position. |
| POST | `/stop` | Lock session, clear current track/queue, bump `generation`, set status `Stopped`, notify `cv`. |
| POST | `/next` | Lock session, advance `queue_position` if possible, set `current_track = current_queue[queue_position]`, bump `generation`, notify `cv`; 409 if nothing playing or already at the end. |
| POST | `/previous` | Same as `/next`, moving backward. |
| GET | `/playlists`, `/artists`, `/albums`, `/tracks` | Query `MusicLibrary`, return JSON listing. |

All responses are JSON: `{"ok": true, ...}` or `{"ok": false, "error": "..."}` with a
corresponding non-2xx HTTP status.

### 3.4 `StreamRoute`

`GET /stream` — the one long-lived, one-directional route. Handling:

1. Lock `PlaybackSession`, read `current_track` and `generation`. If nothing is playing, return
   404 immediately.
2. Register a chunked content provider (`httplib::ContentProviderWithoutLength`) that:
   - Opens the current track's file on the USB drive, keeping the file handle local to this
     request's callback (its read position is the stream's implicit playback position — no
     separate position field is kept in `PlaybackSession`).
   - Before each block (including the first), locks `PlaybackSession` and checks two things:
     1. If `generation` no longer matches the value captured at stream start, returns `false`
        immediately — ending the response mid-stream without a clean terminator, which is the
        abrupt disconnect that surfaces as a `curl` transfer error on the interrupted client.
     2. If `status == Paused`, waits on `cv` (releasing the lock while waiting, per
        `std::condition_variable::wait`) until `status != Paused` or `generation` changes.
        On waking, re-checks `generation` first — if it changed while paused, aborts the same
        way as (1); otherwise proceeds to read and send the next block from the still-open file
        handle, continuing exactly where it left off.
   - Streams in fixed-size blocks (e.g. 64 KB) between these checks.
   - If the file completes normally, ends cleanly.

No busy-polling loop is needed: the interrupt check rides on cpp-httplib's natural per-chunk
callback, and the pause check blocks efficiently on the condition variable instead of spinning.

## 4. Concurrency Model

cpp-httplib dispatches incoming requests onto a bounded internal thread pool, not literally one
thread per connection — the long-lived `/stream` connection occupies one pool worker for its
entire duration (including any time spent blocked on `cv` while paused). Since v1 only ever has
one active stream plus occasional, quick control requests, the pool must simply be sized to at
least 2–3 workers so the stream never starves control commands of a thread; this is a
configuration value on `httplib::Server`, not something requiring custom thread management. The
only shared mutable state is `PlaybackSession`, protected by its mutex; critical sections are
kept small (read/update a few fields, no I/O while holding the lock, and the one intentional
wait is on `cv` specifically to release the lock while blocked) so a paused or long-running
stream never blocks control requests or vice versa.

This gives the separation you were after — control logic and streaming logic are distinct
classes at runtime — without introducing IPC or a second process.

## 5. Storage & Persistence

- USB-C drive mounted at a fixed path, `/mnt/x10pro`, before `playback-controld` starts (no
  hot-plug handling in v1, per requirements). Mp3 files live under `/mnt/x10pro/music`, which is
  the path `MusicLibrary` actually scans — the drive also has unrelated top-level directories
  (e.g. `game-of-thrones`) that scanning only `/mnt/x10pro/music` avoids walking altogether. The
  drive holds only files the user put there — nothing generated by the daemon is written to it.
- `library.json` and `playlists.json` live on Jetson-local storage, e.g.
  `/var/lib/playback-controld/`, decoupled from the drive.
- No database — flat JSON files, matching the "lightweight, no fetch-latency dependency" goal.
  The in-memory index built from `library.json` is what's actually queried at runtime; the JSON
  file is only read at startup and written after a scan finds new files.

## 6. Error Handling

- Unknown `name` in `/play`: `404`, no state change (per requirements §10).
- Invalid state transition (`/pause` when not playing, `/next` at end of queue, etc.): `409`.
- USB drive missing/unreadable at startup: log an error and exit non-zero — no partial/degraded
  mode in v1 (matches "handled similar to the previous error" from requirements §10).
- Malformed `playlists.json`: log a warning, skip loading playlists, continue serving
  artists/albums/tracks (a syntax error in a hand-edited file shouldn't take down the daemon).
- Playlist entry referencing an unknown track path: log a warning naming the bad entry, drop
  just that entry, keep the rest of the playlist (§3.1).

## 7. Deployment

Run as a `systemd` service on the Jetson (`playback-controld.service`), so it starts on boot
and logs are captured by the journal (stdout/stderr, no separate logging framework needed for
v1).

systemd needs to be told to wait for the USB drive to actually be mounted before starting the
daemon — otherwise it could start on boot before the drive is available and find an empty
directory. This is done by referencing systemd's auto-generated mount unit for that path (the
mount at `/mnt/x10pro` corresponds to a unit named `mnt-x10pro.mount`) in the service file:

```ini
[Unit]
After=mnt-x10pro.mount

[Service]
ExecStart=/usr/local/bin/playback-controld

[Install]
WantedBy=multi-user.target
```

`After=` only controls startup ordering — start us once that mount has finished, so a normal
boot doesn't race the daemon against the drive appearing. Deliberately **not** using
`Requires=`: that directive would have systemd tear the daemon down the moment the mount unit
stops, which is disconnect handling — something requirements.md §2 explicitly defers, and §6
leaves disconnect-during-playback as genuinely undefined behavior rather than something actively
detected and reacted to, even at the systemd layer.

## 8. Source Layout

```
playback-controld/
  CMakeLists.txt
  src/
    main.cpp
    music_library.{h,cpp}
    playback_session.h
    control_routes.{h,cpp}
    stream_route.{h,cpp}
  third_party/
    httplib.h
    json.hpp
  tests/
    music_library_test.cpp
```

## 9. Open Items

- ~~Exact chunk size for streaming~~ — resolved: 64 KB. This is just how much file data is read
  and handed to the network at a time. Too small and you waste CPU on overhead per chunk; too
  large and you delay reacting to an interrupt (§3.4) by up to one chunk's read time. 64 KB is a
  common default for file-streaming code and is small enough that the delay is imperceptible;
  not worth tuning until/unless it's actually observed to matter.
- Whether `library.json`/`playlists.json` parsing errors should be surfaced via a control route
  (e.g. a `/status` or `/health` endpoint) — not currently in scope, no such route exists yet.
  Noted for later.
- ~~systemd unit file contents~~ — resolved, see §7.
