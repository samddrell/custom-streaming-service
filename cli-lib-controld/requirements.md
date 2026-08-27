# gj — CLI Controller for playback-controld — Requirements (v1)

## 1. Purpose

A command-line tool, `gj`, that controls and listens to the music streaming service
(`playback-controld`) running on the Jetson, over the same HTTP API that curl currently drives
manually. Built as the "real client" promised in playback-controld's requirements.md §2 (a
non-curl client was explicitly deferred there, not ruled out).

## 2. Scope (v1)

**In scope:**
- A single CLI binary, `gj`, written in C++ (same toolchain as `playback-controld`: CMake).
- Runs on Windows for v1.
- A `gj listen` command that opens the streaming endpoint, plays the audio by shelling out to
  `mpv` as a subprocess, and automatically reconnects when the stream is interrupted (see §5.2).
- Control commands (`play`, `pause`, `resume`, `stop`, `skip`, `previous`) that call
  `playback-controld`'s control endpoints.
- Browse commands (`list playlists|artists|albums|tracks`) that call the corresponding
  `playback-controld` GET endpoints and print every entry of that type.
- Human-readable output for every command; non-zero process exit code whenever the daemon
  responds with `"ok": false`.
- A hardcoded target address for `playback-controld`: `192.168.86.28:8080`.

**Explicitly out of scope for v1 (deferred):**
- Any playback control machine other than Windows.
- Decoding mp3 audio itself — `gj listen` always shells out to an external player (`mpv`); no
  in-process audio decode/output.
- Filtering `gj list` results by name server-side or client-side — `gj list <type>` always
  prints every entry of that type. (`playback-controld`'s list routes have no name-filter
  param; adding one, either there or client-side in `gj`, is future scope, not v1.)
- A `gj status` / "now playing" command. `playback-controld` has no read-only status route —
  status is only ever returned as a side effect of a mutating call — and adding one is a change
  to `playback-controld` itself, not just `gj`. Deferred until it's actually needed.
- A config file, CLI flag, or environment variable for the daemon's host/port. The address is a
  compile-time constant, mirroring `playback-controld`'s own single-deployment-target,
  no-config-file approach (`main.cpp`'s `kMusicDir`/`kStorageDir`/`kPort`).
- Collapsing the two-terminal model into one interactive process (see §3) — `gj listen` and the
  control commands stay separate invocations.
- Authentication/TLS — inherits `playback-controld`'s trusted-WiFi-VLAN-only posture.

## 3. Architecture — two-terminal model (unchanged from the original design)

```
Terminal 1 (stays open, blocking)          Terminal 2+ (short-lived commands)
        |                                            |
        v                                            v
  gj listen                              gj play <type> <name>
  - GET /stream                          gj pause / resume / stop
  - pipes bytes into `mpv -`             gj skip / previous
  - on stream interrupt (§5.2),          gj list <type>
    reconnects to /stream                       |
    automatically                                v
                                       POST/GET playback-controld
        ^                                        |
        |                                        v
        +-------- audio keeps playing, tracking whatever
                   playback-controld currently has "current_track" set to
```

Terminal 1 is where you hear the music. Terminal 2 (or any other terminal, any time) is where
you tell the Jetson what to play next. This mirrors `playback-controld`'s existing model of
"one long-lived GET /stream consumer, N short-lived control calls" — `gj` doesn't change that
shape, it just replaces `curl | mpv` and raw `curl -X POST ...` with named commands and
automatic reconnect-on-interrupt.

## 4. Commands

| Command | Calls | Behavior |
|---|---|---|
| `gj listen` | `GET /stream` (repeatedly) | Opens the stream, pipes bytes to `mpv` via stdin. On disconnect (whether from a deliberate interrupt via `/play`, `/stop`, `/next`, `/previous`, or a genuine network error — indistinguishable from the client side, see §5.2), immediately retries `GET /stream`. If the retry returns `404 nothing playing`, `gj listen` prints that and exits (does not poll/wait). Blocks the terminal until it exits. |
| `gj play <track\|album\|artist\|playlist> <name>` | `POST /play?type=...&name=...` | Starts/replaces playback. Prints the resulting current track on success; prints the daemon's error and exits non-zero on 400/404. |
| `gj pause` | `POST /pause` | Pauses. Non-zero exit + printed error on 409 (not currently playing). |
| `gj resume` | `POST /resume` | Resumes. Non-zero exit + printed error on 409 (not currently paused). |
| `gj stop` | `POST /stop` | Stops and clears the queue. |
| `gj skip` | `POST /next` | Advances to the next track in the queue. Non-zero exit + printed error on 409 (nothing playing / already at the end). |
| `gj previous` | `POST /previous` | Moves to the previous track in the queue. Same error behavior as `skip`. |
| `gj list playlists` | `GET /playlists` | Prints every playlist and its track names. |
| `gj list artists` | `GET /artists` | Prints every artist and their track names. |
| `gj list albums` | `GET /albums` | Prints every album and its track names. |
| `gj list tracks` | `GET /tracks` | Prints every track with name/artist/album/duration. |

`type` for `gj play` must be one of `track`, `album`, `artist`, `playlist`, matching
`playback-controld`'s `/play` validation exactly — `gj` does not re-validate beyond what the
daemon already rejects with a 400.

## 5. Behavior details

### 5.1 Output and exit codes

Every command prints a short human-readable line reflecting the daemon's JSON response (e.g.
`Now playing: Africa — Toto (Toto IV)`, `Paused.`, `Error: not currently playing`). The process
exits `0` when the daemon responds `"ok": true`, and non-zero (daemon's HTTP status, or a fixed
non-zero code) when it responds `"ok": false` or the request fails outright (connection
refused, timeout), so `gj` is usable in scripts/conditionals, not just interactively.

### 5.2 Reconnect-on-interrupt (`gj listen`)

`playback-controld` has no way to tell a connected `/stream` client *why* its connection ended
— an intentional interrupt (`generation` bumped by `/play`/`/stop`/`/next`/`/previous`) and a
genuine network failure both surface identically as an aborted chunked transfer
(`stream_route.cpp`). `gj listen` doesn't try to distinguish them: on any disconnect, it simply
retries `GET /stream`, which naturally picks up whatever `playback-controld` currently has as
`current_track` at reconnect time. This works because `/stream` reads `session_.current_track`
fresh on every new connection, not just once at startup.

- If the retry succeeds with a track streaming, playback continues seamlessly — this is what
  makes `gj skip`/`gj previous`/`gj play <new thing>` from another terminal "just work" while
  `gj listen` is running.
- If the retry gets `404 nothing playing` (e.g. someone ran `gj stop`), `gj listen` prints that
  and exits, rather than polling indefinitely for playback to resume.
- A real network failure (Jetson unreachable) will also cause the reconnect attempt to fail;
  v1 does not implement retry backoff/limits for this case beyond what's described above —
  behavior here is a single immediate retry, and if that also fails, `gj listen` reports the
  error and exits. (Distinguishing "transient network blip, keep retrying" from "daemon is
  genuinely down" is future scope if it turns out to matter in practice.)

### 5.3 Audio playback

`gj listen` shells out to `mpv` (must be installed and on `PATH`) as a subprocess, feeding it
the raw mp3 bytes read from `GET /stream`'s response body via stdin (`mpv -`). `gj` does not
decode mp3 or talk to any audio device itself — this matches `playback-controld`'s "raw mp3
passthrough" streaming design (its requirements.md §4.2) one layer further down the chain.

## 6. Configuration

`playback-controld`'s address is a hardcoded constant in `gj`, analogous to
`playback-controld`'s own `kMusicDir`/`kStorageDir`/`kPort` constants in `main.cpp`:

```cpp
constexpr const char* kDaemonHost = "192.168.86.28";
constexpr int kDaemonPort = 8080;
```

Note this is the Jetson's actual LAN address, not `0.0.0.0` — `playback-controld` binds
`0.0.0.0:8080` (accept on any interface), which is meaningful only on the listening side; a
client must connect to a real, reachable address.

## 7. Open questions

None currently blocking — design.md is the next step. Revisit if `192.168.86.28` changes (the
Jetson gets a new DHCP lease, a static reservation is set up, etc.) — since the address is a
hardcoded constant per §6, that would require a rebuild rather than a config change, which is
an accepted tradeoff for v1's simplicity but worth knowing about going in.
