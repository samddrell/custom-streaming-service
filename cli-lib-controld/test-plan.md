# gj — CLI Controller for playback-controld — Test Plan (v1)

Covers `gj` as specified in `requirements.md` and `design.md`. Mirrors `playback-controld`'s own
test-plan.md in structure: each case traces back to a requirement/design section, and — same
philosophy as that project — no end-to-end automation framework is introduced for v1. Unlike
`playback-controld`, almost everything here is manual: `gj` has no server-side logic of its own
to unit-test in isolation, only output formatting and argument handling.

## 1. Test Levels

| Level | Tool | What it covers |
|---|---|---|
| Unit | Catch2, run via `ctest` | `output.cpp`'s formatting functions and `commands.cpp`'s `argv`-bounds-checking — pure logic, no network, no subprocess. |
| Integration (manual) | Running the real `gj.exe` against a real, reachable `playback-controld` | Every control/browse command's actual HTTP contract, and `gj listen`'s reconnect/audio behavior. |
| Out-of-scope verification | Manual | Confirms deferred features are genuinely absent, not half-implemented. |

## 2. Test Environment & Fixtures

`gj`'s daemon address is a hardcoded constant (requirements.md §6) — `192.168.86.28:8080` — not
something a test run can point elsewhere without rebuilding. So unlike `playback-controld`'s own
test-plan (where a fixture drive is trivially swapped in via a local path), every integration
case here needs `playback-controld` **actually running and reachable at that address** with a
known library loaded. Use the same fixture library `playback-controld/test-plan.md §2` defines
(`ArtistA/AlbumX/01 First.mp3`, `02 Second.mp3`; `ArtistB/AlbumY/01 Only.mp3`, no title tag so its
name is `01 Only`; a fixture `playlists.json` with `"Mix": [Second, Only]`), deployed to the
Jetson for the duration of this test pass, rather than inventing separate names here.

Additional requirements for this test pass specifically:
- `mpv` installed and on `PATH` on the machine running `gj` (for every `gj listen` case except
  IT-10 and IT-17, which deliberately test the cold-start and `mpv`-missing paths).
- Fixture tracks should be a few seconds each (same rationale as `playback-controld`'s own
  fixtures) so a full IT-12 (auto-advance through a whole album, unattended) run doesn't take
  long, but long enough that IT-13/IT-14/IT-19/IT-20 (interrupt/pause from a second terminal
  mid-stream) have a real window to act in.
- A way to independently confirm what's actually playing (e.g. a `curl` terminal alongside `gj`,
  or just listening) — `gj` has no `status` command (requirements §2, deferred), so several
  cases below cross-check via a raw `curl` call or by ear rather than a `gj` command.

## 3. Traceability

| Doc section | What it commits to | Verified by |
|---|---|---|
| requirements §3 (required ordering) | `gj listen` requires something already playing; exits immediately on cold-start 404, doesn't poll | IT-10 |
| requirements §4 | Full command surface: play/pause/resume/stop/skip/previous/list | IT-1 – IT-9 |
| requirements §5.1 / design §3.6 | Human-readable output; fixed `0`/`1` exit codes | IT-1, IT-2, IT-4, IT-9 |
| requirements §5.2 / design §3.5 | Reconnect covers a single interrupt, a `/stop` interrupt specifically, and *repeated* interrupts in one session (the retry-budget fix) correctly | IT-12, IT-13, IT-19, IT-20 |
| design §3.4 (`playback-controld`, cross-referenced) | Pause blocks the connection rather than disconnecting it — must not be misread as an interrupt | IT-14, IT-15 |
| design §3.2 (connection timeout) | Unreachable daemon fails fast (~5s for one-shot commands, ~10s for `gj listen`'s retry), not `cpp-httplib`'s 300s default | IT-16 |
| design §3.2 (read timeout) | Long pauses shouldn't misfire the read timeout | Not directly tested (would require a 5+ minute manual pause) — noted as an accepted gap, see §8. |
| design §3.3 (`argv` bounds-checking) | Missing arguments produce a usage error, not undefined behavior | UT-9 – UT-11 |
| design §3.5 (404 message passthrough) | `gj listen` surfaces the daemon's actual `error` field, not a single hardcoded string that would mask "track file unavailable" behind "nothing is playing" | IT-10, IT-21 |
| design §3.5 (Ctrl+C propagation) | `mpv` doesn't survive `gj listen` being killed | IT-18 |
| requirements §2 (out of scope) | No `status` command, no `list` filtering, no config file, no non-Windows build | OT-1 – OT-4 |

## 4. Unit Tests (Catch2)

No network, no subprocess — pure functions given canned input.

### `output.cpp`

| ID | Case | Input | Expected |
|---|---|---|---|
| UT-1 | Format a track | `{"name":"Second","artist":"ArtistA","album":"AlbumX","duration_seconds":7}` | `"Second — ArtistA (AlbumX)"` |
| UT-2 | Format duration, under a minute | `7` | `"0:07"` (zero-padded seconds) |
| UT-3 | Format duration, over a minute | `125` | `"2:05"` |
| UT-4 | Format duration, exactly on a minute boundary | `60` | `"1:00"` |
| UT-5 | Format an error body | `{"ok":false,"error":"not currently playing"}` | `"Error: not currently playing"` |
| UT-6 | Format a collection list (playlists/artists/albums shape) | `{"artists":[{"name":"ArtistA","tracks":["First","Second"]}]}` | Output includes `ArtistA` and both track names, tracks visually grouped under their artist (exact layout is an implementation choice — this case checks content, not byte-exact formatting). |
| UT-7 | Format an empty collection list | `{"artists":[]}` | No crash; output indicates there's nothing to show rather than printing a blank line with no explanation. |
| UT-8 | Format the full track list (`/tracks` shape) | `{"tracks":[{"name":"Second","artist":"ArtistA","album":"AlbumX","duration_seconds":7}]}` | One line per track, includes name/artist/album/duration (reuses UT-1/UT-2's formatting). |

### `commands.cpp` — `argv` bounds-checking (design §3.3)

Call the command functions directly with hand-built `argc`/`argv` arrays; no `HttpClient` call
should ever be reached in these cases, since the bounds check must happen first.

| ID | Case | Input | Expected |
|---|---|---|---|
| UT-9 | `gj play` with no further arguments | `argc=2, argv={"gj","play"}` | Returns `1`, prints a usage error — does not read `argv[2]`/`argv[3]` at all. |
| UT-10 | `gj play track` with no name | `argc=3, argv={"gj","play","track"}` | Returns `1`, prints a usage error — does not read `argv[3]`. |
| UT-11 | `gj list` with no type | `argc=2, argv={"gj","list"}` | Returns `1`, prints a usage error — does not read `argv[2]`. |

## 5. Integration Tests — Control & Browse Commands (manual)

Assume `playback-controld` is freshly started against the fixture library (§2) before this block.

| ID | Case | Command | Expected |
|---|---|---|---|
| IT-1 | Play a track by name | `gj play track First` | Exit `0`; prints something like `Now playing: First — ArtistA (AlbumX)`; a raw `curl POST /play?type=track&name=First` afterward is redundant with state already set (sanity: `GET /tracks` unaffected, still lists all fixture tracks). |
| IT-2 | Play an unknown name | `gj play track DoesNotExist` | Exit `1`; prints `Error: no track named 'DoesNotExist'` (or whatever `playback-controld` actually returns) to `stderr`, nothing on `stdout`. |
| IT-3 | Play an album (multi-word name) | `gj play album "AlbumX"` | Exit `0`; confirms quoting a multi-word `name` at the shell works, and that it's correctly percent-encoded into the query string by `urlEncode` (design §3.2) — this fixture name is one word, but the case documents the pattern; repeat manually with a real multi-word/special-character name (e.g. one containing `&` or `#`) if the deployed library has one, since that's the case `urlEncode` actually exists for. |
| IT-4 | Invalid `type` | `gj play sometype First` | Exit `1` (either `gj`'s own client-side check or the daemon's `400`, per design §3.3 — either is acceptable, both must result in exit `1` and a printed error, not a crash). |
| IT-5 | Pause / resume / stop contract | `gj play track First`, then `gj pause`, `gj pause` again, `gj resume`, `gj resume` again, `gj stop` | First `pause` exits `0`; second `pause` exits `1` (`409`, already paused); first `resume` exits `0`; second `resume` exits `1` (`409`); `stop` exits `0`. |
| IT-6 | Skip / previous within and past bounds | `gj play album AlbumX`, then `gj skip`, `gj skip` again, `gj previous`, `gj previous` again | First `skip` moves to Second, exits `0`; second `skip` exits `1` (already at the end — only 2 tracks). First `previous` moves back to First, exits `0`; second `previous` exits `1` (already at the start). |
| IT-7 | Skip/previous with nothing playing | `gj stop` then `gj skip` | Exit `1` (`409`, nothing playing). |
| IT-8 | Browse: playlists/artists/albums/tracks | `gj list playlists`, `gj list artists`, `gj list albums`, `gj list tracks` | Each exits `0` and prints every fixture entry of that type — `Mix` playlist with its two tracks, `ArtistA`/`ArtistB`, `AlbumX`/`AlbumY`, all 3 fixture tracks individually. |
| IT-9 | Browse an unrecognized `list` type | `gj list songs` | Exit `1`, a client-side usage error (design §3.3 — `gj` itself rejects this, there's no matching daemon route to fall back on). |

## 6. Integration Tests — `gj listen` (manual, needs real `mpv` and real listening/verification)

| ID | Case | Steps | Expected |
|---|---|---|---|
| IT-10 | Cold start with nothing playing | `gj stop` (ensure nothing is playing), then `gj listen` | Prints `Nothing is currently playing.` (the daemon's own `"error":"nothing playing"` message, per design §3.5's `message` passthrough — not a string `gj` invented independently), exits `0` immediately — does not hang or poll (requirements §3's required-ordering note). |
| IT-11 | Play then listen: audio actually plays | `gj play track First`, then `gj listen` in the same terminal | Audio from `First` is actually audible through `mpv`. Confirms the whole pipe chain — `HttpClient::stream` → `fwrite` → `mpv`'s stdin → sound — works end to end, not just that bytes moved somewhere (mirrors `playback-controld` test-plan's own manual-listening philosophy for IT-10 there). |
| IT-12 | Auto-advance through a full queue, unattended | `gj play album AlbumX` (First, Second), then `gj listen`, and just wait without issuing any other command | Both First and Second play back to back with no manual `gj skip`. After Second finishes, `gj listen` itself prints `Nothing is currently playing.` and exits `0` — confirms `playback-controld`'s queue-exhaustion state reset (its design.md §3.4 case 2) is actually visible correctly from `gj`'s side, not just from raw `curl`. |
| IT-13 | Interrupted from another terminal via `/play` | `gj play track First`, `gj listen` in terminal 1, then from terminal 2: `gj play track "01 Only"` while First is still audibly playing | Audio switches to `01 Only` in terminal 1 without `gj listen` needing to be restarted — confirms the interrupt → `ConnectionFailed` → immediate-retry → new track path (design §3.5) actually works through `gj`, not just through raw `curl`. |
| IT-14 | Paused from another terminal | `gj play track First`, `gj listen` in terminal 1, then from terminal 2: `gj pause` | Audio stops within about one chunk, `gj listen` stays connected (no reconnect message, no exit) — confirms `playback-controld`'s pause-blocks-the-connection behavior (its design §3.4) doesn't get misread as a disconnect by `gj`. |
| IT-15 | Resumed after IT-14 | From terminal 2 (continuing IT-14): `gj resume` | Audio resumes from where it paused, in the same `gj listen` session, no reconnect. |
| IT-16 | Jetson unreachable vs. daemon not running | Two distinct sub-cases, since they're different failure modes with different latencies: **(a)** point at a host with nothing listening on the port (e.g. `playback-controld` not started) — this typically fails fast (connection refused, well under a second); **(b)** point at a genuinely unreachable host (Jetson physically off/disconnected) — this should take roughly the 5-second connection timeout (design §3.2) per attempt. Run both against a one-shot command (`gj play track First`) and against `gj listen`. | (a) fails near-instantly in both cases. (b) one-shot commands fail in ~5s; `gj listen` costs **~10s**, not ~5s, since its retry logic (design §3.5) makes a second connection attempt after the first `ConnectionFailed` — this is the detail most worth timing with a stopwatch, since it's easy to assume `gj listen` behaves identically to a one-shot command here. Either way, well under `cpp-httplib`'s 300-second default. Exit `1`, clear error message in both sub-cases. |
| IT-17 | `mpv` missing from `PATH` | Temporarily rename/remove `mpv` from `PATH`, then `gj play track First` + `gj listen` | Does not hang or crash; exits `1` with some error (design §3.5 accepts this won't specifically say "mpv is missing" — just needs to fail cleanly, not hang). Restore `mpv` on `PATH` afterward. |
| IT-18 | Ctrl+C kills the `mpv` child too | `gj listen` playing audio, then Ctrl+C in that terminal | Audio stops immediately; check with `tasklist` (or Task Manager) that no orphaned `mpv.exe` process is left running. This is the empirical check design §3.5 flags as unverified-by-reasoning-alone (including the `_popen`-spawns-via-`cmd.exe` wrinkle noted there). |
| IT-19 | Repeated interrupts in one session (retry-budget regression test) | `gj play album AlbumX`, `gj listen` in terminal 1; from terminal 2, once First is audibly playing: `gj skip` (interrupts to Second); wait until Second is audibly playing; then `gj skip` again (interrupts again — this second skip has nowhere to go since AlbumX only has 2 tracks, so use `gj play track "01 Only"` instead to force a second genuine interrupt) | **Both** interrupts succeed — audio changes both times, and `gj listen` is still running and connected after the second one. This is the case that would have caught the retry-bookkeeping bug found in review (design §3.5): a naive single-use retry flag would kill `gj listen` on the second interrupt even though nothing failed. |
| IT-20 | Interrupted from another terminal via `/stop` | `gj play track First`, `gj listen` in terminal 1, then from terminal 2: `gj stop` while First is still audibly playing | Audio stops; `gj listen` reconnects once (per design §3.5) and lands on `NothingPlaying` this time (not a new track, since nothing replaced it) — prints `Nothing is currently playing.` and exits `0`, same as IT-10/IT-12, without needing a third code path. Distinct from IT-13 (which reconnects into a *new* track) and IT-19 (which stays connected through *two* interrupts) — this one confirms an interrupt that leads straight to the terminal state still works correctly. |
| IT-21 | 404 message distinguishes "nothing playing" from a missing file | Play a track, then (from another machine or by temporarily renaming the file on the Jetson's drive) make the *currently playing* track's file unavailable on disk, then reconnect `gj listen` | `gj listen` prints the daemon's actual message (`"track file unavailable on disk"`, per `stream_route.cpp`), not the generic `"Nothing is currently playing."` — confirms design §3.5's `message` passthrough actually surfaces which 404 case occurred rather than collapsing both into one string. This case requires deliberately breaking the fixture setup; skip if not practical to safely reproduce, but don't mark it as passing without actually doing it. |

## 7. Out-of-Scope Verification

Confirms v1 correctly does **not** support things explicitly deferred in requirements.md §2:

| ID | Case | Expected |
|---|---|---|
| OT-1 | No `status`/"now playing" command | `gj status` (or similar) is not a recognized subcommand — usage error, not a crash or a silently-wrong answer. |
| OT-2 | No filtering on `list` | `gj list artists ArtistA` either errors as an unrecognized extra argument or is silently ignored and still prints *all* artists — either is acceptable, but it must not filter (requirements §2 explicitly defers this). |
| OT-3 | No config file / env var / flag for the daemon address | Setting an environment variable or passing an unrecognized flag intended to change the target host has no effect — `192.168.86.28:8080` is compiled in (requirements §6). |
| OT-4 | No non-Windows build attempted | Not tested as pass/fail for v1 — noted here only so "never tried" isn't later confused with "confirmed working elsewhere." |

## 8. Exit Criteria for v1

All UT cases pass. All IT cases pass against a real, reachable `playback-controld` with the
fixture library loaded (§2) — including IT-18's empirical Ctrl+C/process-cleanup check and IT-19's
retry-budget regression check, neither of which can be confirmed by code review alone. IT-21 may
be skipped only if genuinely impractical to safely reproduce, not skipped by default. OT cases
confirm no accidental support for deferred features.

One known, accepted gap: design §3.2's 1-hour read timeout (chosen so long pauses don't misfire
it) has no corresponding test case here, since exercising it properly would mean actually pausing
for 5+ minutes and confirming `gj listen` doesn't disconnect. Worth doing at least once before
calling v1 done, but not included as a required case given how long it takes to run.
