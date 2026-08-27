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

Docs are finished and internally consistent. **No implementation exists yet** — no
`CMakeLists.txt`, no `src/`. Starting from `design.md` §8's source layout is the next step.

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
  endpoint, and there shouldn't be one added.
- **`/pause` is a real block, not a status flag.** `StreamRoute` blocks on the condition
  variable while `status == Paused`, holding its already-open file handle's read position, and
  resumes sending from exactly that position on `/resume` — no seeking involved. This was a
  design-review fix; an earlier draft only set a status field that nothing downstream read,
  which meant pause did nothing to an in-flight stream.
- **A track's `name`** (the lookup key for `/play?type=track&name=...`) comes from the ID3
  title tag, falling back to the filename with its extension stripped if no title tag is
  present. This is distinct from `artist`/`album`/`duration`, which are ID3-derived metadata.
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
- **The systemd unit uses `After=mnt-music.mount`, deliberately not `Requires=`.** Hot-plug/
  disconnect handling is explicitly out of scope for v1 (requirements.md §2); `Requires=` would
  have systemd tear the daemon down automatically on mount loss, which is disconnect handling by
  another name. Don't add it back for "robustness" without checking — that's exactly the kind
  of scope creep this project has been pushing back on throughout.
- **No auth, no TLS.** Trusted WiFi VLAN only, by design (requirements.md §7), not an oversight.
- **Explicitly out of scope for v1**: seek, volume control, multiple simultaneous `/stream`
  listeners, USB hot-plug handling, any non-curl client, and a playlist-authoring API
  (`playlists.json` is hand-edited by the user directly).

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

1. Scaffold the CMake project per `design.md` §8's source layout.
2. Vendor `cpp-httplib` and `nlohmann/json` as single headers under `third_party/`; link
   `TagLib` and `Catch2` as real build dependencies.
3. Implement `MusicLibrary` first (`design.md` §3.1) — it's unit-testable in isolation
   (`test-plan.md` §4, UT-1–UT-7) before any networking code exists.
4. Implement `PlaybackSession`, then `ControlRoutes` and `StreamRoute` (`design.md` §3.2–3.4).
5. Work through `test-plan.md`'s IT/DT cases as each piece lands; it's written to be run
   against a real fixture drive and a running daemon, not automated end-to-end.
