# Jetson Music Streaming Service — Requirements (v1)

## 1. Purpose

A self-built music player service running on an NVIDIA Jetson Orin Nano, built primarily as a
system-architecture learning exercise. The system streams mp3 files stored on an external
USB-C drive, controlled and consumed via curl commands as a stand-in for a future proper client.

Design priority: a simple, stripped-down v1 with clearly-defined internal interfaces so that
individual pieces (control API, streaming format, client) can be swapped or expanded later
without a rewrite.

## 2. Scope (v1)

**In scope:**
- A single daemon, `playback-controld`, running on the Jetson.
- HTTP control endpoints (invoked via curl) to select and control playback.
- An HTTP endpoint that streams the currently-playing track as raw mp3 bytes.
- Automatic progression through a multi-track queue (album/artist/playlist) as each track
  finishes, without requiring a `/next` call per track (§4.4).
- A music management module (in-process, behind an internal interface) that indexes and
  serves music from a USB-C drive: playlists, artists, albums, tracks.
- Basic ID3-derived metadata per track: artist, album, duration.
- Persistence of the library index/playlists to the USB-C drive itself.
- Operation on a trusted WiFi VLAN, no authentication.

**Explicitly out of scope for v1 (deferred):**
- A non-curl client / UI.
- Transcoding or any format other than raw mp3 passthrough (no PCM, no protobuf framing).
- Seeking within a track.
- Multiple concurrent playback streams/listeners.
- USB drive hot-plug detection/handling (disconnects, remounts).
- Authentication, TLS, or exposure beyond the local WiFi VLAN.
- Uploading/ingesting new files via API (files are managed manually on the USB drive).

## 3. Architecture

Single process (`playback-controld`) for v1. Music management is **not** a separate service or
daemon — it is an internal module behind a clearly defined interface (e.g. a trait/interface
boundary in whatever language is chosen), so it can be extracted into its own service later
without changing callers. `playback-controld` owns this module directly rather than going
through a network API.

```
                 curl (control)                curl (stream GET)
                      |                              |
                      v                              v
              +-------------------------------------------+
              |            playback-controld               |
              |  +---------------------------------------+ |
              |  |  HTTP control handlers                 | |
              |  |  (play/pause/resume/stop/skip/browse)  | |
              |  +---------------------------------------+ |
              |  |  music management module (internal)    | |
              |  |  - library index (in-memory + persisted| |
              |  |    to Jetson-local storage)             | |
              |  |  - playlist/artist/album/track lookup  | |
              |  +---------------------------------------+ |
              |  |  playback engine / stream source        | |
              |  |  (reads raw mp3 bytes from USB drive)   | |
              |  +---------------------------------------+ |
              +-------------------------------------------+
                        |                        |
                        v                        v
              USB-C drive (mp3 files      Jetson-local storage
              only)                       (library index +
                                           playlist definitions)
```

## 4. Interfaces

### 4.1 Control API (curl, HTTP)

Endpoints to be finalized in the design doc, but must cover:
- Play by track / album / artist / playlist (select what plays next/now)
- Pause
- Resume
- Stop
- Skip to next track
- Skip to previous track
- Browse/list: enumerate available playlists, artists, albums, and tracks

Not included in v1: seek-to-position, volume control.

### 4.2 Streaming endpoint (curl, HTTP)

A single long-lived GET endpoint that streams the currently-playing track as raw, unmodified
mp3 bytes (no transcoding, no protobuf/other framing). A client consumes it by piping to an
external decoder/player, e.g.:

```
curl http://<jetson-host>/stream | mpg123 -
```

This resolves the earlier ambiguity between "stream mp3 files" and "stream uncompressed files"
in favor of streaming the original compressed mp3 bytes as-is — no PCM transcoding in v1.

### 4.3 Concurrency

Only one active playback session/stream is supported at a time. A new `play` command replaces
whatever is currently playing. The interrupted stream's connection is dropped abruptly (not a
clean end-of-stream), so `curl` on the interrupted listener's side surfaces its own transfer-error
diagnostic on stderr (e.g. `curl: (18) transfer closed with ... bytes remaining`) — this is the
alert, using the existing connection rather than a separate notification mechanism.
Multiple simultaneous listeners are out of scope for v1.

### 4.4 Queue auto-advance

When `/play` resolves to a multi-track queue (album/artist/playlist), playback advances through
that queue automatically as each track finishes — a client does not have to call `/next` between
every track for ordinary queue playback to work end to end on one long-lived `/stream` connection.
`/next`/`/previous` remain available for user-directed control (skip ahead, go back) and behave
exactly as before. Auto-advance only moves forward through the queue in order; it never loops
back to the start and never wraps past the end. When the last track in the queue finishes,
playback ends the same way a single track finishing already does today: the stream ends cleanly,
with nothing further to advance to.

This was identified as a gap while designing the CLI client (`cli-lib-controld`): the queue
resolved by `/play` was real, but nothing ever advanced through it automatically, so a client
naively reconnecting to `/stream` after a clean track end would just re-fetch the same
still-current track from byte 0, forever — never a real streaming *service*, just repeated
single-file playback gated by manual `/next` calls.

## 5. Data Model

A common base structure is shared by playlists, artists, and albums, differentiated by how
they're populated:

- **Track**: `name` (string) → `file path` (string, location on the USB drive), plus metadata:
  `artist`, `album`, `duration`. Track position/number is not stored on the track itself — it
  isn't inherent to the track (the same track can occupy different positions in different
  playlists/albums).
- **Album**, **Artist**, **Playlist**: each holds a collection of tracks as an ordered map
  (name → track); order is preserved for all three. Within Album/Playlist, position in this
  order is what stands in for "track number" in that context.

## 6. Storage & Persistence

- Source mp3 files live on an external USB-C drive, not copied onto the Jetson's own storage.
- The library index and playlist definitions are persisted on **Jetson-local storage**, not the
  USB drive — the drive holds only music files. This keeps the drive free of generated files
  and avoids depending on the drive's write speed/filesystem for index updates, at the cost of
  the library no longer being portable to a different Jetson without re-scanning (acceptable
  since this is a single fixed device, not a multi-device setup).
- The drive is assumed to be mounted at a fixed, known path before `playback-controld` starts.
  No hot-plug/mount-detection logic in v1; behavior on disconnect during playback is undefined
  for this iteration.

## 7. Networking & Security

- The service is reachable over WiFi on a dedicated VLAN — not the open local network, and not
  exposed beyond it.
- No authentication on control or streaming endpoints in v1; the VLAN boundary is the only
  access control. Revisit if exposure requirements change.

## 8. Non-Functional Requirements

- The music management module must be lightweight enough that playback start latency is not
  dependent on library lookup/fetch time (e.g. avoid re-scanning the whole drive on every
  request; keep an in-memory index backed by the persisted file).
- Resource usage should account for running on a Jetson Orin Nano, which may be shared with
  other workloads.

## 9. Resolved Questions (was UNSURE in earlier draft)

| Original question | Resolution |
|---|---|
| Should playback-controld use a library or call a separate music management API? | Neither exactly — it owns an internal module behind a clean interface, in the same process. |
| Does the music management system need to be its own service/daemon? | No, for v1. Single daemon, internal module boundary for future extraction. |
| What format to stream in — protobuf, uncompressed, etc.? | Raw mp3 bytes, unmodified, over HTTP. |
| How is the interrupted listener alerted when a new `play` replaces their stream? | Server drops the connection abruptly; curl's own transfer-error output on stderr is the alert. No new endpoint/mechanism. |
| Does track number belong on the Track itself? | No — position is contextual to the Album/Playlist containing it, represented by order in that collection's map, not stored on the Track. |

## 10. Open Items for Design Doc

- Exact HTTP route/method naming for control and streaming endpoints.
 - You can decide this. let me know if you have any questions.
- On-disk format for the persisted library index (flat files vs. embedded DB).
 - Perhaps the files could be flat and their organization could be maintained in a json, or something similar.
- Behavior when the requested track/playlist/album/artist doesn't exist.
 - It should fail to play the requested track and notify the user. This wouldn't change system state.
- Error handling scope (e.g. what happens if the USB drive is missing at startup).
 - This can be handled similar to the previous error.
- Concrete metadata extraction approach (ID3 tag library choice).
 - You can decide this. let me know if you have any questions.
