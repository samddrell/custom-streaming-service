# Streaming Backpressure Fix — Sketch

A cross-cutting fix touching both `playback-controld` and `cli-lib-controld`, diagnosed together
as one bug during real hardware testing of `gj`. This is a first-pass sketch — the diagnosis
below is solid (reproduced and traced through actual code), but a few implementation parameters
are still open (marked explicitly) rather than guessed at here.

## 1. Observed symptoms

Found while testing `gj listen` against the real Jetson:

1. Playback restarts partway through a track — consistently within the first few minutes,
   regardless of the track's actual length (observed on a 9:13 track and others).
2. A multi-track queue (`gj play album ...`) never advances past the first track, even across
   closing and reopening `gj listen`.
3. Issuing `gj play`/`gj skip` from a second terminal while `gj listen` is running doesn't change
   the audio playing back until `gj listen` is manually closed and reopened.

## 2. Root cause, with evidence

**The core problem: neither `gj` nor `playback-controld` paces itself to real playback speed, and
nothing absorbs the resulting mismatch — so an accidental backpressure chain reaches all the way
back to a server-side timeout that was never meant to tolerate it.**

Evidence gathered directly against the real system (not theoretical):

- An **unthrottled** `curl` pull of a 9:13/17.4MB track completed in **2.575 seconds** — the full
  file, byte-identical, no truncation. Confirms `playback-controld` has no hard size/duration cap;
  the whole file is genuinely servable near-instantly if nothing is pacing consumption.
- A **throttled** pull of the same track (`curl --limit-rate 24k`, mimicking real playback pace)
  died at **61.5 seconds** with `curl: (18) transfer closed with outstanding read data remaining`
  — the exact abrupt-disconnect signature `playback-controld`'s interrupt mechanism produces, but
  nothing external interrupted it.
- `playback-controld/src/main.cpp:32` constructs `httplib::Server server;` with **no explicit
  write timeout** — so it runs on `cpp-httplib`'s default, `CPPHTTPLIB_SERVER_WRITE_TIMEOUT_SECOND
  = 5` (confirmed directly in the vendored `third_party/httplib.h`).

**The mechanism, traced end to end:**

1. `gj`'s content-receiver (`http_client.cpp`) does `fwrite(data, 1, len, sink)` immediately for
   every chunk that arrives over the network — no buffering, no pacing of its own.
2. `mpv` doesn't drain its stdin pipe steadily — it reads in **bursts**, filling its own internal
   demuxer cache to a target, then stops touching stdin entirely while it plays from that cache.
3. During those "coast" stretches, the tiny OS pipe between `gj` and `mpv` fills almost instantly
   (since `gj` pushes network-speed data with no pacing), and `fwrite()` **blocks** for the
   duration of the coast.
4. While blocked, `gj` isn't draining its TCP receive buffer either — so TCP's own flow control
   (the receive window, advertised automatically in every ACK) shrinks to zero, telling
   `playback-controld`'s kernel to stop sending.
5. `playback-controld`'s own kernel send buffer then fills too, and the `sink.write()` call inside
   `StreamRoute::handleStream`'s content-provider blocks at the OS level.
6. Once that block exceeds 5 seconds, `cpp-httplib` gives up and aborts the connection —
   indistinguishable, from `gj`'s side, from a real external interrupt.
7. `gj listen`'s reconnect logic (correctly, by design) treats this as "reconnect." Since nothing
   changed `PlaybackSession` server-side, the reconnect just re-opens the same `current_track`
   from byte 0 (`/stream` always starts fresh at byte 0 — no position tracking across
   connections) — this is symptom #1, the "restart."
8. Because the connection is aborted well before genuine EOF, `StreamRoute`'s auto-advance logic
   (which only runs on real end-of-file) never gets the chance to fire — this is symptom #2, the
   queue never advancing.
9. The `generation`-mismatch interrupt check only runs *between* chunk-send attempts. If the
   connection is mid-stall (steps 3–5 above), the server can't even reach the next check until the
   current stalled write clears one way or another — so an external `/play`/`/skip` can sit
   undetected for however long the stall lasts. This is symptom #3.

None of this is a TCP/HTTP design flaw — TCP's flow control is doing exactly what it's supposed
to (see the discussion this doc summarizes: TCP is still the right transport for byte-perfect,
non-interactive audio delivery; this isn't a case for UDP/WebRTC). The actual gap is that nothing
in our own code is willing to *absorb* backpressure locally before it propagates all the way back
to a timeout that has no idea a slow, paced consumer is expected and legitimate.

## 3. The fix — two parts

### 3a. `playback-controld`: raise the server write timeout

**File:** `src/main.cpp`. Add an explicit, generous write timeout on the `httplib::Server`,
mirroring the reasoning already written into `gj`'s own `set_read_timeout(3600, 0)` — a slow,
paced consumer is expected, legitimate behavior for a streaming endpoint, not a sign of a broken
connection.

```cpp
httplib::Server server;
server.set_write_timeout(3600, 0);  // exact value: open question, see §4
```

Necessary **regardless** of §3b below: even a well-sized bounded buffer will eventually exhaust
under some slow-enough consumption pattern, and the timeout needs to tolerate that rather than
treat it as failure. This is the cheap, already-fully-diagnosed half of the fix.

(Server-side *read* timeout — for reading incoming requests — is unrelated to this bug; streaming
is all outbound writes from the server's perspective. No change needed there.)

### 3b. `gj`: a bounded producer/consumer buffer between network and `mpv`

**Files:** likely `http_client.cpp`/`listen_command.cpp`, or a new dedicated component.

Add a small, **fixed-size** in-memory queue between two threads:
- **Producer thread**: reads off the `/stream` HTTP connection (today's content-receiver logic),
  pushes into the bounded queue. Blocks when the queue is full.
- **Consumer thread**: pulls from the queue, `fwrite()`s into `mpv`'s pipe (today's write logic).
  Blocks when the queue is empty.

**Must be bounded, not unbounded** — an unbounded buffer would let `gj` eagerly download an
entire track in a few seconds (per §2's evidence) and hold it all in memory. Beyond the memory
waste, this would **break the interrupt mechanism**: the `/stream` connection would complete and
close almost immediately, leaving nothing "live" for a later `/skip`/`/play` to abort. The buffer
needs to be sized to comfortably absorb `mpv`'s normal burst/coast jitter — on the order of
seconds of audio, not the whole track — while keeping the network connection genuinely paced
close to real playback speed.

This doesn't eliminate the backpressure chain from §2 — it relocates where blocking happens, from
an opaque OS pipe (whose stall propagates straight through TCP to a server timeout with no
context) to a queue *we* size and control. The two fixes are complementary: the buffer makes a
real stall rare; the longer server timeout makes sure that when one does happen anyway, it isn't
treated as a failure.

**Concurrency model change, worth flagging explicitly:** `cli-lib-controld/design.md §4`
currently states `gj` needs no concurrency at all ("every `gj` invocation is single-threaded").
This fix means `gj listen` specifically now does — a real, documented change to that section, not
just an implementation detail, once this moves past the sketch stage.

## 4. Decisions

- **`playback-controld` write timeout: `3600` seconds**, matching `gj`'s own `set_read_timeout`
  choice for symmetry. Worth knowing why this is generous rather than tight: `/pause` never
  touches this timeout at all, since the content-provider callback blocks in `cv.wait(...)`
  *before* attempting any `sink.write()` — no write in flight, nothing to time out. This value
  only protects against a stalled *active* write, which §3b's buffer should make rare regardless.
- **`gj` bounded-buffer capacity: the equivalent of ~6 minutes of playback, to start** — a fixed
  byte cap (not a per-track duration calculation), sized against this library's observed average
  bitrate (~250kbps, from the 17.4MB/9:13 test track = ~31.4KB/s): `6 * 60 * 31.4KB/s ≈ 11.3MB`,
  rounded to a clean **12MB** cap. Revisit if real-world testing shows it's poorly sized either
  direction.
- **Keep `ListenCommand`'s existing single-retry-per-failure logic as-is.** Reasoning: most
  reconnect triggers are user-initiated commands, not genuine network failures, so the existing
  policy (retry once immediately, keyed on whether the failed attempt received any data first)
  remains the right shape once the buffer is in place — it doesn't need to change to accommodate
  the buffer.
- **Always flush the buffer's queued-but-not-yet-consumed contents at the start of every new
  connection attempt (every reconnect, no exceptions, not conditional on the outcome that
  preceded it).** This was a gap the original sketch missed: since the network can deliver up to
  ~180x faster than real-time playback, the buffer could already hold several minutes of a track
  that's about to be interrupted — draining that into `mpv` before the new track's data arrives
  would make `gj skip`/`gj play` feel severely laggy, exactly the responsiveness the interrupt
  mechanism exists to provide.

  Why "always," rather than trying to distinguish "this reconnect is a real interrupt" from "this
  reconnect is a network blip" and only flushing for the former: **TCP already absorbs genuine
  transient blips — packet loss, brief WiFi drops, momentary congestion — entirely below the
  application layer**, via automatic retransmission (duplicate-ACK-triggered fast retransmit, or
  the OS's own retransmission-timeout-with-backoff). The application only ever sees a
  `ConnectionFailed` after something far more severe than a blip: either a *deliberate* interrupt
  (a real `generation` mismatch — flushing is obviously correct), or a sustained outage long
  enough to exhaust the OS's own multi-minute TCP retry budget (on the order of 15–30 minutes by
  default) — and if an outage lasted that long, the buffer's contents are already stale/exhausted
  by the time the connection recovers regardless of whether we flush. So there is no real "genuine
  blip, don't flush" case to design around — the two cases that actually reach `gj` as
  `ConnectionFailed` both call for the same response.

## 5. Implementation plan

1. **`playback-controld/src/main.cpp`**: `server.set_write_timeout(3600, 0)`, right after
   constructing `httplib::Server`. Small enough to also fold the corresponding
   `requirements.md`/`design.md`/`CLAUDE.md` updates into the same change.
2. **`cli-lib-controld`**: introduce a bounded `AudioBuffer` (producer/consumer queue, capacity
   from §4) between the network-reading side and the `mpv`-writing side. `HttpClient::stream()`'s
   `FILE* sink` parameter generalizes to a chunk-sink callback, so it no longer knows or cares
   that the destination is eventually `mpv` — that becomes `ListenCommand`'s concern.
   `ListenCommand::run()` becomes the producer (today's reconnect loop, now pushing into the
   buffer and flushing it at the top of every connection attempt), plus a new consumer thread
   draining the buffer into `mpv`'s pipe. Requires updating `cli-lib-controld/design.md §4`
   (currently states `gj` needs no concurrency at all — no longer true for `gj listen`
   specifically) and `CMakeLists.txt` (new source file).
3. Test-plan updates for both projects: deferred for now, per direct instruction — revisit once
   the implementation is in and can be tested against real hardware again.
