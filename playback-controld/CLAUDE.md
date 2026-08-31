# playback-controld

A C++ daemon for an NVIDIA Jetson Orin Nano that streams mp3 files (stored on an external USB-C
drive) to a client, controlled via curl. Built partly as a system-architecture learning exercise
for the user (see requirements.md §1) — implementation choices should be explained, not just made.

## Read first, in this order

1. `requirements.md` — what v1 must do. Source of truth for **scope**.
2. `design.md` — concrete C++ architecture implementing those requirements. Source of truth for **how**.
3. `test-plan.md` — test cases traceable back to both of the above.

These were produced through an iterative Q&A process with the user, then independently reviewed
by a second agent for scope creep and factual errors; the findings that survived review were
fixed directly in `design.md`/`test-plan.md`. If you spot another inconsistency between the
docs, treat requirements.md as authoritative for *what*, design.md for *how*, and flag the
conflict to the user rather than silently picking one side.

## Status

Docs are finished and internally consistent. **v1 is implemented**, builds, and has been smoke-
tested against the real drive (`playback-controld/` — `src/`, `tests/`, `CMakeLists.txt`,
`playback-controld.service`). `MusicLibrary`'s Catch2 unit tests (UT-1–UT-8) all pass, and a
manual pass of the curl-driven integration checks (play/stream byte-identity, pause/resume
mid-stream, interrupt-via-`/play`, interrupt-via-`/stop`, 404/409 error cases, next/previous
boundaries) confirmed against the real 65-track library — see test-plan.md §5–§6 for the case
list. Remaining test-plan.md work: IT-14/IT-15/IT-16/IT-17 (browse/storage/playlist edge cases —
`playlists.json` doesn't exist yet since it's hand-authored) and the DT-* deployment tests (need
an actual reboot + systemd install on the Jetson, not just running the binary manually).

Queue auto-advance (requirements.md §4.4, design.md §3.2/§3.4) has merged into `master` (PR #1)
— `PlaybackSession::advance()`, `StreamRoute`'s EOF handling. IT-20/21/22 still need running
against the real drive; don't treat them as passing until that's actually done.

**In progress (`auto-advance-stop-fix` branch, off `master`):** a follow-up gap found while
designing `cli-lib-controld`'s reconnect logic — when the queue runs out, `StreamRoute` needs to
reset session state the same way `/stop` does (see the "Queue auto-advance" load-bearing
decision below), or a reconnecting client replays the last track forever instead of getting
`404`. Docs are updated (requirements §4.4, design §3.4 case 2, test-plan IT-23); code change to
`stream_route.cpp`'s exhausted branch still needs implementing/testing before this merges.

## Load-bearing decisions (don't relitigate these without checking with the user first)

- **Single process, single `httplib::Server`, single port.** `ControlRoutes` and `StreamRoute`
  are handler classes registered on the *same* server — not separate services, not separate
  ports. This was deliberately walked back from an earlier two-port draft after the user found
  it confusing relative to what streaming actually needs (see design.md §2).
- **`PlaybackSession`** (mutex + `std::condition_variable`) is the only shared mutable state.
  A `generation` counter is bumped by `/play`, `/stop`, `/next`, `/previous` (never by
  `/pause`/`/resume`) and drives the interrupt mechanism: `StreamRoute`'s per-chunk loop aborts
  (returns `false`, causing an abrupt disconnect that surfaces as a `curl` transfer error on the
  interrupted client) when it detects a `generation` mismatch. This *is* the "alert the
  interrupted listener" behavior from requirements.md §4.3 — there is no separate notification
  endpoint, and there shouldn't be one added. **This must be registered via
  `Response::set_chunked_content_provider`, not `set_content_provider`** — verified against a
  real `curl` client, not just the spec: without `Transfer-Encoding: chunked`, an early
  connection close is valid, *complete* HTTP framing and `curl` exits 0 on a silently-truncated
  file. Only chunked encoding cut off before its terminator is a detectable error (design.md
  §3.4).
- **Queue auto-advance (requirements.md §4.4, design.md §3.2/§3.4)**: when a track finishes
  cleanly (EOF) and more tracks remain in `current_queue`, `StreamRoute` itself advances
  `queue_position`/`current_track` (via the new `PlaybackSession::advance()`, shared with
  `ControlRoutes::advanceQueue`/`/next`/`/previous`) and keeps streaming on the *same* connection
  — no `sink.done()`, no reconnect needed. **This must not bump `generation` or call
  `cv.notify_all()`** — it's the same playback session continuing, not an interrupt; bumping
  `generation` here would make ordinary track-to-track playback indistinguishable from an
  external `/play`/`/stop`/`/next`/`/previous`, breaking the interrupt mechanism above. Added
  after discovering, while designing `cli-lib-controld`, that a multi-track `/play` built a real
  queue but nothing ever advanced through it — a naive reconnecting client would just replay
  track 1 forever. **When the queue runs out** (last track finishes, nothing left to advance
  to), `StreamRoute` resets state the same way `handleStop` does (`current_track.reset()`,
  `current_queue.clear()`, `queue_position = 0`, `status = Stopped`) before ending the stream —
  still without bumping `generation`/notifying `cv`, same reasoning as above. This closes the
  same failure mode one level up: without it, a client reconnecting after the *whole queue*
  finishes (not just one track) would find `current_track` still set and replay the last track
  forever instead of getting `404`.
- **`/pause` is a real block, not a status flag.** `StreamRoute` blocks on the condition
  variable while `status == Paused`, holding its already-open file handle's read position, and
  resumes sending from exactly that position on `/resume` — no seeking involved. This was a
  design-review fix; an earlier draft only set a status field that nothing downstream read,
  which meant pause did nothing to an in-flight stream.
- **A track's `name`** (the lookup key for `/play?type=track&name=...`) comes from the ID3
  title tag, falling back to the filename with its extension stripped if no title tag is
  present. This is distinct from `artist`/`album`/`duration`, which are ID3-derived metadata.
- **A track with a missing/empty `album` or `artist` ID3 tag groups into a synthetic
  `"Unknown Album"`/`"Unknown Artist"` collection**, not an empty-string-keyed bucket and not
  dropped from grouping. This substitution happens only when building the in-memory Album/Artist
  `TrackCollection`s (design.md §3.1.3) — `library.json` itself keeps whatever TagLib actually
  read (including empty strings), so a later real tag fix is picked up normally after the usual
  delete-`library.json`-and-rebuild flow, instead of being masked by a placeholder that got
  persisted.
- **`library.json` (the tag-index cache) and `playlists.json` live on Jetson-local storage**
  (e.g. `/var/lib/playback-controld/`) — **not** on the USB drive, which holds only mp3 files.
  This was reversed from an earlier draft that put both on the drive for portability; the user
  chose Jetson-local instead. Don't move it back without checking.
- **Indexing never re-tags a path already in `library.json`** — no mtime tracking, no
  change-detection. If a file's tags change in place, the fix is deleting `library.json` and
  letting it rebuild. This was a deliberate simplification after the user called mtime-based
  re-tagging scope creep.
- **A `playlists.json` entry referencing a path not in the index is dropped and logged as a
  warning at load time** — not a fatal error for the whole file, not surfaced to the curl
  client, and the playlist just plays the remaining valid tracks in order.
- **The systemd unit uses `After=mnt-x10pro.mount`, deliberately not `Requires=`.** Hot-plug/
  disconnect handling is explicitly out of scope for v1 (requirements.md §2); `Requires=` would
  have systemd tear the daemon down automatically on mount loss, which is disconnect handling by
  another name. Don't add it back for "robustness" without checking — that's exactly the kind
  of scope creep this project has been pushing back on throughout.
- **No auth, no TLS.** Trusted WiFi VLAN only, by design (requirements.md §7), not an oversight.
- **Explicitly out of scope for v1**: seek, volume control, multiple simultaneous `/stream`
  listeners, USB hot-plug handling, any non-curl client, and a playlist-authoring API
  (`playlists.json` is hand-edited by the user directly).
- **Implementation-level decisions not in requirements.md/design.md, made during coding**:
  no separate `/status` route — every control response's JSON body carries the resulting
  `status` inline (e.g. `POST /pause` → `{"ok":true,"status":"Paused"}`); a track-name
  collision (two tracks resolving to the same `name`) is first-indexed-wins with a logged
  warning, matching the project's established warn-and-degrade pattern elsewhere; port `8080`,
  bound on `0.0.0.0`, hardcoded (no config file) alongside the fixed `/mnt/x10pro/music` and
  `/var/lib/playback-controld` paths.

## Collaboration notes

- The user (Sam) wants clarifying questions asked **before** non-trivial implementation or
  design decisions, not after. Throughout this project he's repeatedly caught and pushed back
  on scope creep (extra abstractions, defensive handling, config options beyond what was asked)
  and asked "why" whenever something looked more complex than necessary. Default to the simplest
  thing that satisfies `requirements.md`.
- He's building this to learn system architecture — prefer explaining the reasoning behind an
  implementation choice over silently making it.
- When a decision has real tradeoffs, present 2-4 concrete options with a recommendation (this
  is how essentially every decision in requirements.md/design.md got made) rather than picking
  unilaterally and moving on.
- Before any risky/hard-to-reverse action (force-push, resetting branches, deleting files),
  check first — same as any other project.

## Suggested next steps

v1 is implemented and smoke-tested (see Status above). What's left:

1. Write a `playlists.json` on the Jetson (`/var/lib/playback-controld/playlists.json`) and
   run IT-4, IT-16, IT-17 (playlist play, malformed-JSON handling, bad-entry handling).
2. `sudo cp build/playback-controld /usr/local/bin/`, install
   `playback-controld.service` (`sudo cp playback-controld.service
   /etc/systemd/system/ && sudo systemctl daemon-reload && sudo systemctl enable --now
   playback-controld`), then run the DT-* deployment tests (test-plan.md §8) — needs an actual
   reboot with the drive attached, not just running the binary by hand like this session did.
3. IT-15 (drive holds no generated files) and a final read-through of the remaining IT cases in
   test-plan.md §5–§7 not yet exercised.
