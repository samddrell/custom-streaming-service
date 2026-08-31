# Jetson Music Streaming Service — Test Plan (v1)

Covers `playback-controld` as specified in `requirements.md` and `design.md`. Each test case
below is traceable back to a requirement/design section so a passing plan is evidence the
implementation matches what was actually agreed, not just "seems to work."

## 1. Test Levels

| Level | Tool | What it covers |
|---|---|---|
| Unit | Catch2, run via `ctest` | `MusicLibrary` indexing/tagging/lookup logic — pure logic, no network, no real playback. |
| Integration (manual, curl-driven) | Shell scripts wrapping `curl` against a running `playback-controld` | The actual HTTP contract: control routes, the streaming route, interrupt behavior, error responses. |
| Deployment | Manual, on the Jetson | The systemd unit and its mount dependency (§7 of design.md). |

No end-to-end automation framework is introduced for v1 — matches the "stripped down"
philosophy from requirements.md §1. Integration tests are scripted curl sequences a person (or a
simple shell script) runs against a real running daemon and a fixture USB drive.

## 2. Test Environment & Fixtures

- A test USB drive (or a directory standing in for one during dev) mounted at a fixed path,
  containing a small fixture library:
  ```
  /mnt/x10pro/music/
    ArtistA/AlbumX/01 First.mp3
    ArtistA/AlbumX/02 Second.mp3
    ArtistB/AlbumY/01 Only.mp3
  ```
  Each fixture mp3 should be short (a few seconds of silence/tone is fine) and carry real ID3v2
  tags, so `TagLib` extraction is genuinely exercised, not mocked. `01 First.mp3` and
  `02 Second.mp3` should carry title tags "First"/"Second" respectively (matching the names
  used in test requests below); `01 Only.mp3` should be indexed with **no title tag set**, so
  its expected `name` is the filename with extension stripped (`01 Only`) — this is the fixture
  for the title-tag-fallback rule (design §3.1).
- A fixture `playlists.json` on Jetson-local storage referencing a mix of tracks across artists:
  ```json
  { "Mix": ["ArtistA/AlbumX/02 Second.mp3", "ArtistB/AlbumY/01 Only.mp3"] }
  ```
- Jetson-local storage (`/var/lib/playback-controld/`) must be cleared (`library.json` deleted)
  before any test run that's meant to exercise a cold start, since an existing index changes
  `MusicLibrary`'s behavior on purpose (§3.1 of design.md).
- A decoder for manual listening verification: `mpg123` (matches the example in requirements.md
  §4.2).

## 3. Traceability

| Doc section | What it commits to | Verified by |
|---|---|---|
| requirements §4.1 | Command surface: play/pause/resume/stop/next/previous/browse | IT-1 – IT-9, IT-14, IT-18 |
| requirements §4.2 | Raw mp3 passthrough streaming | IT-10, IT-11 |
| requirements §4.3 / design §3.2, §3.4 | Single active stream; interrupted stream drops abruptly; pause actually halts/resumes delivery | IT-12, IT-13, IT-19, IT-19b, IT-19c |
| requirements §4.4 / design §3.2, §3.4 | Queue auto-advance: progresses without manual `/next`, doesn't trigger the interrupt mechanism, stops cleanly at the end of the queue and resets state like `/stop` | IT-20, IT-21, IT-22, IT-23 |
| requirements §5 | Track has no stored position; Album/Playlist order preserved | UT-4, UT-5, IT-2 |
| requirements §6 / design §5 | Index + playlists on Jetson-local storage; drive holds only mp3s | IT-15, DT-1 |
| design §3.1 | Incremental indexing, no re-tagging of known paths, title-tag/filename name derivation | UT-1, UT-2, UT-3, UT-7 |
| design §3.1, §3.1.3 | Missing album/artist tag groups into synthetic "Unknown Album"/"Unknown Artist", not an empty-string bucket or dropped from grouping; `library.json` itself keeps the raw empty value | UT-8 |
| design §6 | Error handling: unknown name (404), invalid transition (409), missing drive (exit non-zero), malformed playlists.json (warn + continue), bad playlist entry (warn + skip) | IT-5, IT-8, DT-3, IT-16, IT-17 |
| design §7 | systemd starts after mount | DT-1 |
| requirements §2 (out of scope) | seek, volume, hot-plug, auth, multi-listener are NOT present | OT-1 – OT-5 |

## 4. Unit Tests — `MusicLibrary` (Catch2)

| ID | Case | Steps | Expected |
|---|---|---|---|
| UT-1 | Cold index build | Empty `library.json`, fixture drive present. Run indexing. | All 3 fixture tracks appear in the resulting index with correct `name`/`artist`/`album`/`duration`. |
| UT-2 | Warm index, no new files | Run indexing once to build `library.json`, record its exact contents, then modify the on-disk fixture mp3 files' tags (without changing their paths/filenames) and run indexing again. | The second run's index is byte-identical to the first — modified tags are NOT picked up, since a path already in the index is never re-read (design §3.1). Chosen as a black-box behavioral check rather than instrumenting/mocking TagLib calls, since the design has no seam for that and adding one would be scope creep. |
| UT-3 | New file added since last index | Start from UT-1's index, add a 4th fixture file, re-run indexing. | Only the new file is tagged; the original 3 entries are untouched (same field values as before, not merely equal by chance). |
| UT-4 | Track has no stored position field | Inspect the `Track` structure/serialized index. | No `track_number`/position field exists anywhere on a track — confirms requirements §5's explicit removal. |
| UT-5 | Playlist order preserved | Load the fixture `playlists.json`'s "Mix" playlist. | Resolved track list order exactly matches the JSON array order, not alphabetical or path order. |
| UT-6 | Playlist references unknown track path | `playlists.json` contains a path not present in the index. | Resolved playlist excludes that entry; the other entries are present and in their original relative order; a warning is logged for the dropped entry (design §3.1/§6). |
| UT-7 | Track name derivation | Index the fixture drive (see §2 — `First`/`Second` have title tags, `Only` does not). | `First` and `Second` tracks' `name` fields equal their ID3 title tag values; `Only`'s `name` field equals `"01 Only"` (filename with `.mp3` stripped, per the fallback rule in design §3.1). |
| UT-8 | Missing album/artist tag groups into "Unknown" bucket | Add a 4th fixture file with a title tag but no album/artist tags, re-run indexing, then inspect the resolved Album/Artist collections. | `library.json`'s stored `album`/`artist` for that track are empty strings (not fabricated at index time — design §3.1); the resolved Album/Artist groupings show it filed under `"Unknown Album"`/`"Unknown Artist"` alongside any other untagged tracks, not under an empty-string key and not silently dropped (design §3.1.3). |

## 5. Integration Tests — Control API (curl)

Assume a clean daemon start against the fixture drive/playlists before this block.

| ID | Case | Request | Expected |
|---|---|---|---|
| IT-1 | Play a track by name | `POST /play?type=track&name=First` | `200`, `{"ok":true,...}`; `/stream` subsequently serves `01 First.mp3`'s bytes. |
| IT-2 | Play an album | `POST /play?type=album&name=AlbumX` | Queue is First, Second in that order (per requirements §5). |
| IT-3 | Play an artist | `POST /play?type=artist&name=ArtistA` | Queue contains ArtistA's tracks. |
| IT-4 | Play a playlist | `POST /play?type=playlist&name=Mix` | Queue matches the "Mix" order from `playlists.json`. |
| IT-5 | Play an unknown name | `POST /play?type=track&name=DoesNotExist` | `404`; a subsequent `GET /tracks` or repeat of a prior `/play` shows state is unchanged (no partial state mutation — design §6). |
| IT-6 | Pause while playing | `/play` then `POST /pause` | `200`; status reflects `Paused`. See IT-19 (§6) for verifying this actually halts byte delivery, not just the status field. |
| IT-7 | Pause while already paused/stopped | `POST /pause` with nothing playing | `409`. |
| IT-8 | Resume while paused / invalid resume | Mirror of IT-6/IT-7 for `/resume` | `200` / `409` respectively. |
| IT-9 | Next / previous within and past queue bounds | `/play` an album, call `/next` repeatedly, checking `GET /stream`'s served bytes after each call | Advances through queue; after each `/next`, a fresh `/stream` request serves the *new current track's* bytes (not the previous one — confirms `current_track` is updated, not just `queue_position`); `409` when called past the last track (and symmetric case for `/previous` before the first). |
| IT-14 | Browse endpoints | `GET /playlists`, `/artists`, `/albums`, `/tracks` | Each returns JSON listing matching the fixture library exactly (names, no missing/extra entries). |
| IT-18 | Stop while playing (success case) | `/play` a track, then `POST /stop` | `200`; a subsequent status query reflects `Stopped`. Distinct from IT-13, which tests `/stop` as a stream-interrupting event — this checks the control command's own state-transition contract. |

## 6. Integration Tests — Streaming (curl)

| ID | Case | Steps | Expected |
|---|---|---|---|
| IT-10 | Full track streams cleanly | `/play` a track, then `curl http://<host>/stream \| mpg123 -` and let it finish | Full track audio plays; `curl` exits 0. |
| IT-11 | Stream reflects raw mp3 bytes | `curl http://<host>/stream -o out.mp3` while a known track plays, without interrupting it | `out.mp3`'s bytes are byte-for-byte identical to the source fixture file (confirms "raw passthrough, no transcoding" — requirements §4.2). |
| IT-12 | Interrupt via new `/play` | Start streaming track A to a background curl process; while it's mid-stream, issue `POST /play` for track B | The original curl process exits non-zero with a transfer-error message on stderr (e.g. `curl: (18) ...`); a *new* `GET /stream` call now serves track B's bytes. |
| IT-13 | Interrupt via `/stop` | Same as IT-12 but issue `POST /stop` instead of a new `/play` | Original stream aborts the same way; a subsequent `GET /stream` returns `404` (nothing playing). |
| IT-19 | Pause actually halts and resumes byte delivery | `/play` a track with a long-enough fixture file. Start `curl http://<host>/stream -o out.mp3 &` in the background. Poll `out.mp3`'s size until it's growing, then `POST /pause`. Keep polling the file size for several seconds. | The file's size stops growing within one chunk (≤64 KB) of the pause request and does not grow further while paused — confirms the content-provider actually blocks (design §3.4), not just that a status field changed. |
| IT-19b | Resume continues from the paused position | Immediately following IT-19, `POST /resume`, then let the background `curl` finish. | The file's size resumes growing; the completed `out.mp3` is byte-for-byte identical to the source fixture file (same check as IT-11) — confirms resume continues from exactly where it paused rather than restarting or corrupting the stream. |
| IT-19c | Interrupt while paused | Repeat IT-19's setup (pause mid-stream), then instead of `/resume`, issue `POST /play` for a different track. | The original (paused) curl process exits with a transfer-error on stderr, same as IT-12 — confirms a blocked/paused stream still reacts to `generation` changing, via `cv.notify_all()` (design §3.2). |
| IT-20 | Queue auto-advances without manual `/next` | `/play` an album (fixture `AlbumX`, tracks First/Second), then `curl http://<host>/stream -o out.mp3` in the foreground and let it run to completion with no `/next` calls in between. | `curl` exits 0; `out.mp3` is byte-for-byte identical to First's bytes immediately followed by Second's bytes concatenated (raw passthrough, no framing between them) — confirms the connection continued into track 2 on its own (requirements §4.4, design §3.4). |
| IT-21 | Auto-advance doesn't trigger the interrupt mechanism | Same setup as IT-20, streaming through the First→Second boundary. | `curl` exits 0 (not the transfer-error exit `curl: (18) ...` seen in IT-12/IT-13/IT-19c) — confirms crossing a queue boundary via auto-advance does **not** bump `generation` and does not abort the connection the way an external `/play`/`/stop`/`/next`/`/previous` does (design §3.2/§3.4). |
| IT-22 | Auto-advance stops cleanly at the end of the queue | Same as IT-20 but let `curl` run through both First and Second to the very end. | After Second's bytes, the stream ends cleanly (`curl` exits 0, no further data) — no attempt to auto-advance past the last track; `out.mp3`'s total size equals exactly First + Second's combined byte count, no more. |
| IT-23 | Reconnecting after queue exhaustion gets 404, not a replay | Immediately following IT-22 (queue just finished naturally, no `/stop` issued), call `GET /stream` again. | `404 nothing playing` — same response as after an explicit `/stop` (IT-13). Confirms `current_track`/`status` are reset when the queue runs out (design §3.4 case 2), not left pointing at the finished last track — a naive reconnect-on-any-disconnect client must not replay Second forever. |

## 7. Storage & Persistence Tests

| ID | Case | Steps | Expected |
|---|---|---|---|
| IT-15 | Drive holds no generated files | After running the daemon through several `/play` and browse calls, inspect the mounted drive's contents. | Drive contents are byte-identical to what was there before the daemon ran — no `library.json`/`playlists.json` written there (confirms requirements §6's Jetson-local decision). |
| IT-16 | Malformed `playlists.json` | Replace the fixture `playlists.json` with invalid JSON, restart the daemon. | Daemon starts successfully, logs a warning, `GET /playlists` returns an empty (or absent) list, `GET /artists`/`/albums`/`/tracks` still work normally (design §6). |
| IT-17 | Playlist entry with unknown track path | Add a bogus path (e.g. `ArtistZ/Fake/00 Nope.mp3`) into the "Mix" playlist's array, keeping the other two entries. Restart the daemon, then `POST /play?type=playlist&name=Mix`. | A warning naming the bad path appears in the daemon's console/journal output at load time. `GET /playlists` (or the resolved queue via `/play`) shows only the two valid tracks, in their original relative order. Playback proceeds through the valid tracks with no gap or error surfaced to the curl client — the bad entry is invisible at play time, only visible in the startup log. |

## 8. Deployment Tests (on the Jetson, manual)

| ID | Case | Steps | Expected |
|---|---|---|---|
| DT-1 | Starts after drive mount | Reboot the Jetson with the USB drive attached. | `playback-controld` is running only after `mnt-x10pro.mount` is active; `systemctl status playback-controld` shows it started after the mount in the journal timestamps. |
| DT-3 | Missing drive at startup | Start the Jetson (or manually start the service) with the drive not mounted at all. | Per design §6, the daemon logs an error and exits non-zero; systemd shows a failed unit rather than a silently-running-but-broken daemon. |

Note: the daemon is deliberately **not** torn down automatically if the mount disappears after
startup (design §7 uses `After=`, not `Requires=`, specifically to avoid this) — that scenario
is covered as an informational, non-gating check under OT-4 below, not as a deployment pass/fail
test.

## 9. Out-of-Scope Verification

These confirm v1 correctly does **not** support things explicitly deferred in requirements.md
§2 — worth a quick check so "not implemented yet" isn't confused with "broken":

| ID | Case | Expected |
|---|---|---|
| OT-1 | No seek endpoint | Any request to a seek-style route returns `404` (route doesn't exist) — no partial/broken seek behavior. |
| OT-2 | No volume endpoint | Same as OT-1 for volume control. |
| OT-3 | No auth required | Control/stream requests succeed with no credentials, confirming the "VLAN boundary is the only access control" decision (requirements §7) — not a bug if a stray unauthenticated request works, that's the design. |
| OT-4 | Drive disconnect mid-playback is unhandled | Physically remove the drive while streaming, and separately, run `systemctl stop mnt-x10pro.mount` while the daemon is running and idle. Documented as undefined behavior (requirements §6, design §7) — record what actually happens (likely a read error, or the daemon continuing to run with a now-missing drive) but this is informational, not a pass/fail gate for v1. |
| OT-5 | Multiple simultaneous `/stream` clients | Not a supported configuration (requirements §4.3) and not required to behave any particular way — not tested as pass/fail; only note in the report if something surprising (e.g. a crash) happens. |

## 10. Exit Criteria for v1

All UT and IT cases pass; DT cases pass on the actual Jetson hardware (not just dev machine);
OT cases confirm no accidental support for deferred features.
