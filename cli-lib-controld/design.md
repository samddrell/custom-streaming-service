# gj — CLI Controller for playback-controld — Design Doc (v1)

Implements the requirements in `requirements.md`. Language: C++, same toolchain family as
`playback-controld` (CMake, cpp-httplib, nlohmann/json).

## 1. Technology Choices

| Concern | Choice | Rationale |
|---|---|---|
| Language / standard | C++17 | Matches `playback-controld`; no need for anything newer. |
| Build system | CMake | Same tooling as `playback-controld`, one build system to know for the whole project. |
| Platform | Windows (v1) | Per requirements §2. Built with MSVC or MinGW; needs `ws2_32` linked for `cpp-httplib`'s sockets (Windows-specific — `playback-controld` doesn't need this since it builds on Linux). |
| HTTP | [cpp-httplib](https://github.com/yhirose/cpp-httplib) — vendored copy, same header as `playback-controld/third_party/httplib.h` | Already proven in this project; used here in **client** mode instead of server mode. No TLS/OpenSSL needed (`playback-controld` doesn't use HTTPS), which keeps the build simple. |
| JSON | [nlohmann/json](https://github.com/nlohmann/json) — vendored copy | Matches `playback-controld`; parses every control-route response. |
| Audio playback | `mpv`, external, on `PATH` | Per requirements §5.3 — `gj` shells out, never decodes mp3 itself. |
| Testing | Catch2 (header-only), for pure logic only | Matches `playback-controld`'s split: Catch2 for logic with no network involved, manual testing against a real `playback-controld` + real `mpv` for everything else (test-plan.md). |
| Argument parsing | Hand-written `argv` dispatch, no library | The command surface is small and fixed (§4 of requirements.md) — a dispatch table keyed on `argv[1]` is simpler than pulling in a parsing library for ~10 subcommands with no optional flags. |

## 2. Process Architecture

Unlike `playback-controld` (one long-running server process handling many connections),
`gj` is a plain CLI tool: **each invocation is a separate, short-lived process**, except
`gj listen`, which is long-running but still single-purpose — it does exactly one thing
(stream audio) for its entire lifetime, not many things concurrently. There is no persistent
`gj` daemon and no shared state between invocations; every invocation starts cold, does its one
job, and exits. This is the mirror image of `playback-controld`'s architecture: the daemon holds
the shared mutable state (`PlaybackSession`) so many short client requests can coordinate through
it; `gj` has nothing to coordinate *between its own invocations* — coordination happens entirely
server-side, which is exactly why `gj skip` from one terminal can affect `gj listen` running in
another without either knowing about the other directly.

```
Terminal 1                              Terminal 2 (any time, any number of invocations)
+-------------------+                   +------------------------------------------+
|  gj listen         |                   |  gj play / pause / resume / stop /       |
|  (long-running)    |                   |  skip / previous / list                  |
|                     |                   |  (each: one HTTP call, then exit)        |
|  HttpClient::stream |                   |  HttpClient::post / ::get                |
|       |             |                   |         |                                |
+-------|-------------+                   +---------|--------------------------------+
        v                                           v
   GET /stream (repeated)              POST/GET playback-controld (§4 routes)
        |                                           |
        v                                           v
   mpv subprocess                    playback-controld's PlaybackSession
   (stdin pipe)                      (the only place any coordination happens)
```

## 3. Components

### 3.1 `main.cpp` — dispatch

Reads `argv[1]` as the subcommand name and dispatches to one function per command, matching the
table in requirements.md §4:

```cpp
int main(int argc, char** argv) {
  if (argc < 2) { printUsage(); return 1; }
  std::string cmd = argv[1];
  HttpClient client(kDaemonHost, kDaemonPort);

  if (cmd == "listen")   return ListenCommand(client).run();
  if (cmd == "play")     return commandPlay(client, argc, argv);
  if (cmd == "pause")    return commandPause(client);
  if (cmd == "resume")   return commandResume(client);
  if (cmd == "stop")     return commandStop(client);
  if (cmd == "skip")     return commandNext(client);
  if (cmd == "previous") return commandPrevious(client);
  if (cmd == "list")     return commandList(client, argc, argv);

  printUsage();
  return 1;
}
```

No abstraction beyond this — ten `if`s reads more plainly than a `std::map<std::string,
std::function<...>>` dispatch table would for a command count this small, and it's exactly as
easy to extend later.

### 3.2 `HttpClient` — thin wrapper around `httplib::Client`

Owns one `httplib::Client` instance pointed at `kDaemonHost:kDaemonPort` (§6 of requirements.md).
Two responsibilities: control-route calls, and the one streaming call `gj listen` needs — kept
separate because they have genuinely different result shapes and failure semantics.

```cpp
struct ApiResult {
  bool connected = false;     // false = couldn't get any HTTP response at all
  int status = 0;             // valid only if connected
  nlohmann::json body;        // valid only if connected and a body was returned
};

class HttpClient {
 public:
  HttpClient(const std::string& host, int port);

  ApiResult post(const std::string& path);   // control routes: /play, /pause, /resume, ...
  ApiResult get(const std::string& path);    // browse routes: /playlists, /artists, ...

  struct StreamResult {
    enum class Outcome { ConnectionFailed, NothingPlaying, StreamEnded } outcome;
    bool received_data = false;  // true if >=1 byte was written to `sink` this attempt —
                                  // see §3.5 for why this, not the outcome, drives retry logic
    std::string message;         // populated for NothingPlaying: the daemon's own "error" field
                                  // ("nothing playing" vs "track file unavailable on disk" are
                                  // both 404s from playback-controld — see stream_route.cpp —
                                  // and gj surfaces whichever one it actually was, not a single
                                  // hardcoded string that would mask the difference)
  };
  StreamResult stream(const std::string& path, FILE* sink);  // GET /stream, for gj listen

 private:
  httplib::Client client_;
};
```

`post`/`get` both collapse to the same `ApiResult` shape: `connected = false` means
`client_.Post(...)`/`Get(...)` returned a falsy `httplib::Result` (couldn't complete the request
at all — connection refused, timeout, etc.); otherwise `status` and `body` (parsed from the
response with `nlohmann::json::parse`) are populated. Every control/browse route always returns a
JSON body on any response it gives (§4 of `playback-controld`'s design.md), so there's no
"connected but no body" case to handle separately.

`stream()` is where the subtlety from requirements.md §5.2 lives — see §3.5 below for exactly
why it can't just expose the raw `httplib::Result` to callers.

**Connection timeout:** `cpp-httplib`'s default connection timeout is
`CPPHTTPLIB_CONNECTION_TIMEOUT_SECOND` = **300 seconds** (checked directly against the vendored
`third_party/httplib.h`, not assumed) — far too long for a LAN-local tool talking to one fixed
Jetson. `HttpClient`'s constructor calls `client_.set_connection_timeout(5, 0)` (5 seconds): if
`192.168.86.28:8080` isn't reachable (Jetson off, wrong network), every `gj` command should fail
fast with a clear error rather than appearing to hang for up to five minutes.

**Read timeout — deliberately left long, not tightened to match.** `cpp-httplib` separately
defaults `CPPHTTPLIB_CLIENT_READ_TIMEOUT_SECOND` to 300 seconds too, but this one governs how
long the client waits for *more bytes on an already-open connection* — which includes the entire
time a track sits paused (`playback-controld` blocks the connection open and idle while paused,
per its design.md §3.4, sending nothing). A 5-second read timeout would misfire on any pause
longer than 5 seconds and be indistinguishable from `ConnectionFailed`. `HttpClient`'s constructor
instead calls `client_.set_read_timeout(3600, 0)` (1 hour) — long enough that no realistic pause
trips it, while still eventually giving up rather than blocking forever on a truly dead
connection. Left as a round, generous number rather than "no timeout," since `cpp-httplib`'s
timeout fields are plain seconds counts with no documented sentinel for "infinite" in this
version — passing `0` risks meaning "immediately," not "never," and that's not a risk worth
taking without verifying it against the vendored header first.

**URL-encoding query parameters:** `cpp-httplib`'s own query-string builder
(`detail::append_query_params`) lives in an internal `detail` namespace, not something `gj` can
call directly. `commandPlay` (§3.3) needs to put a `name` that may contain spaces or other
special characters into `/play?type=...&name=...`'s query string, so `http_client.cpp` includes
a small standalone `std::string urlEncode(const std::string&)` (percent-encodes everything
outside `A-Za-z0-9-_.~`) — the only place `gj` needs this, since every other route takes no
parameters.

### 3.3 Control and browse commands (`commands.h`/`.cpp`)

One function per command (`commandPlay`, `commandPause`, `commandResume`, `commandStop`,
`commandNext`, `commandPrevious`, `commandList`), each following the same shape:

1. Build the request path (e.g. `commandPlay` reads `argv[2]`/`argv[3]` as `type`/`name` and
   URL-encodes `name` into the query string — the only command that needs request-building logic
   beyond a fixed path).
2. Call `client.post(...)` or `client.get(...)`.
3. Map the `ApiResult` to output + exit code (§3.6, §5).

`commandPlay` is the one with real argument-handling: `argv[2]` must be one of
`track|album|artist|playlist` (checked client-side only for a friendlier error message — the
daemon's own `400` on an invalid `type` is still the authoritative check, per requirements §4);
`argv[3]` is the `name`, taken as a single argument so multi-word names need quoting at the shell
(`gj play album "Toto IV"`), matching ordinary shell conventions rather than `gj` inventing its
own quoting rules.

`commandList` reads `argv[2]` (`playlists|artists|albums|tracks`) and maps it directly to the
matching route; an unrecognized value prints a usage error client-side (there's no daemon route
to fall back on to reject this one, unlike `commandPlay`'s `type`).

**Every `argv` read beyond `argv[1]` is bounds-checked against `argc` before dereferencing.**
`argv[argc]` is guaranteed `nullptr` by the C standard, but reading `argv[argc + 1]` or beyond
(e.g. `commandPlay` reading `argv[3]` when only `gj play track` was typed, `argc == 3`, so
`argv[3]` is the guaranteed-`nullptr` sentinel, but a bare `gj play` has `argc == 2` and
`argv[3]` is genuinely out of bounds) is undefined behavior, not just a `nullptr` to check for.
So each command that reads a fixed number of arguments checks `argc` is large enough *first* and
prints a usage error if not, before touching `argv[2]`/`argv[3]` at all — this was missing from
the pseudocode shown earlier in this section and needs to be explicit going into implementation.

### 3.4 Output formatting (`output.h`/`.cpp`)

Pure functions, each taking already-parsed `nlohmann::json` and returning a `std::string` — no
I/O in this file, so these are the only genuinely unit-testable pieces of `gj` without a real
network (§4 of test-plan.md):

```cpp
std::string formatTrack(const nlohmann::json& track);       // "Africa — Toto (Toto IV)"
std::string formatDuration(int seconds);                    // 295 -> "4:55"
std::string formatCollectionList(const nlohmann::json& arr, const std::string& key);
                                                              // /playlists, /artists, /albums
std::string formatTrackList(const nlohmann::json& tracks);  // /tracks
std::string formatError(const nlohmann::json& body);        // "Error: not currently playing"
```

`main.cpp`/`commands.cpp` are responsible for *printing* (`std::cout`/`std::cerr`) and choosing
*which* formatter applies to a given response — success output goes to `stdout`, error output to
`stderr` (so a caller piping `gj`'s stdout in a script doesn't see error text mixed into their
data), per requirements §5.1.

### 3.5 `ListenCommand` (`listen_command.h`/`.cpp`)

The one long-running, stateful piece — deliberately its own class, mirroring `playback-controld`
splitting `StreamRoute` out from `ControlRoutes` (design.md §2 there) for the same reason: the
"one long-lived stream" logic doesn't belong tangled up with short request/response commands.

```cpp
class ListenCommand {
 public:
  explicit ListenCommand(HttpClient& client);
  int run();  // blocks until playback genuinely ends or an unrecoverable error occurs

 private:
  HttpClient& client_;
  FILE* mpv_ = nullptr;
};
```

`run()`'s loop, precisely (this is the part that needed verifying against `httplib.h` directly —
see the note in §3.2):

```cpp
int ListenCommand::run() {
  mpv_ = _popen("mpv --no-terminal -", "wb");
  if (!mpv_) {
    std::cerr << "gj: error: could not start mpv (is it installed and on PATH?)\n";
    return 1;
  }

  bool retry_available = true;
  while (true) {
    auto result = client_.stream("/stream", mpv_);

    // Any real progress on this attempt (even one it ultimately failed on, e.g. streaming
    // half a track before a fresh /skip from another terminal interrupts it) fully restores
    // the retry budget — see the note below this block for why keying this on received_data,
    // not on which outcome came back, is what fixes the bug found in review.
    if (result.received_data) { retry_available = true; }

    switch (result.outcome) {
      case HttpClient::StreamResult::Outcome::NothingPlaying:
        std::cout << (result.message.empty() ? "Nothing is currently playing."
                                              : result.message) << "\n";
        _pclose(mpv_);
        return 0;
      case HttpClient::StreamResult::Outcome::StreamEnded:
        // Queue finished cleanly. playback-controld has already reset its state to Stopped
        // by the time this returns (playback-controld design.md §3.4 case 2), so the next
        // loop iteration's reconnect is guaranteed to land in NothingPlaying above — no
        // separate handling needed here.
        continue;
      case HttpClient::StreamResult::Outcome::ConnectionFailed:
        if (!retry_available) {
          std::cerr << "gj: error: lost connection to playback-controld and the retry failed\n";
          _pclose(mpv_);
          return 1;
        }
        retry_available = false;  // one immediate retry per genuine failure, per requirements §5.2
        continue;
    }
  }
}
```

**This is a fix, not the original design** — an earlier draft of this loop used a single
`first_attempt` flag that only ever reset on `StreamEnded`. That's broken: every external
interrupt (`/play`/`/stop`/`/next`/`/previous` from another terminal) produces `ConnectionFailed`,
not `StreamEnded`, so after the *first* interrupt was successfully retried, `first_attempt` never
became `true` again — a *second*, completely unrelated interrupt arriving later in the same
`gj listen` session (e.g. two `gj skip`s from another terminal, with real playback happening in
between) would hit the "already retried, give up" branch and kill `gj listen`, even though
nothing was actually wrong. Found in review before implementation started. The fix: key the
retry budget on whether the *just-finished* attempt actually moved any bytes
(`result.received_data`), not on which enum value it ended with — genuine forward progress on an
attempt is what proves the connection is healthy, whether that attempt's *own* ending was clean
(`StreamEnded`) or itself got interrupted partway through (`ConnectionFailed` with
`received_data == true`). Only an attempt that fails **without ever receiving a single byte**
leaves the budget spent, which is the correct signal for "this looks like a real outage, not a
routine interrupt." Covered by test-plan.md IT-19 (new).

**Why `stream()` can't just hand back the raw `httplib::Result`, precisely:** `cpp-httplib`'s
client `Result::operator bool()` (`httplib.h:2428`) is `res_ != nullptr` — and tracing
`ClientImpl::send_with_content_provider_and_receiver` (`httplib.h:15511-15521`), the populated
`Response` (which already has `status` read off the wire) is **discarded** — replaced with
`nullptr` — whenever the body read fails partway through:
`return send(req, *res, error) ? std::move(res) : nullptr;`. An interrupted `/stream` connection
(another client hit `/play`/`/stop`/`/next`/`/previous`, which is the *expected*, common case
here) fails the body read exactly this way. So `res->status` is only ever safe to read when
`bool(res)` is true — an aborted stream and a total connection failure are indistinguishable at
this layer (both come back falsy), which is exactly why `ConnectionFailed` covers both, and why
requirements.md §5.2 treats them the same (single retry, not an attempt to tell them apart).

`HttpClient::stream()` implementation. Note this uses `cpp-httplib`'s `Get` overload that takes
*both* a `ResponseHandler` (called once, right after headers/status arrive, before any body
bytes) and a `ContentReceiver` (`httplib.h:2627`) — needed so `gj` can inspect `status` before
deciding where body bytes go, since `/stream`'s `404` response is a small JSON error body, not
audio, and piping it into `mpv`'s stdin would feed it garbage right before closing the pipe
(review finding: the first draft used the plain single-callback `Get` and didn't account for
this):

```cpp
HttpClient::StreamResult HttpClient::stream(const std::string& path, FILE* sink) {
  int status = 0;
  bool wrote_audio = false;
  std::string error_body;

  auto res = client_.Get(
      path,
      [&](const httplib::Response& response) {
        status = response.status;
        return true;  // never cancel here — just capture status before the body starts
      },
      [&](const char* data, size_t len) {
        if (status == 404) {
          error_body.append(data, len);  // small JSON error body — read, not played
          return true;
        }
        wrote_audio = wrote_audio || len > 0;
        return fwrite(data, 1, len, sink) == len;  // false here also aborts httplib's read loop
      });

  if (!res) {
    return {StreamResult::Outcome::ConnectionFailed, wrote_audio, {}};
  }
  if (res->status == 404) {
    std::string message;
    try {
      message = nlohmann::json::parse(error_body).value("error", "");
    } catch (const nlohmann::json::exception&) {
      // Malformed body from something that isn't actually playback-controld — leave message
      // empty, ListenCommand's caller already has a sensible fallback string for this.
    }
    return {StreamResult::Outcome::NothingPlaying, wrote_audio, message};
  }
  return {StreamResult::Outcome::StreamEnded, wrote_audio, {}};
}
```

The content-receiver lambda returning `false` on a short `fwrite` (e.g. `mpv`'s pipe closed
because it crashed or was never actually running) also aborts the transfer the same way a server
interrupt does — from `HttpClient::stream()`'s perspective this is indistinguishable from
`ConnectionFailed` too, which is an acceptable simplification: a genuinely broken `mpv` pipe will
keep failing every `fwrite`, so `run()`'s existing "retry once, then give up" logic naturally
catches it within one extra loop iteration rather than needing a fourth outcome — and
`wrote_audio` will correctly be `false` in that case (nothing was ever successfully written), so
the retry-budget fix above doesn't mistake a broken pipe for real progress either.

**Process cleanup (Ctrl+C):** `_popen` (Windows CRT) spawns `mpv` attached to the same console as
`gj` by default (no `CREATE_NEW_PROCESS_GROUP`), so Windows' default `Ctrl+C` handling broadcasts
`CTRL_C_EVENT` to the whole console process group — both `gj` and `mpv` receive it and terminate,
with no custom signal handler needed in `gj` itself. This is standard Windows console behavior,
not something `gj`'s code has to implement, but it hasn't been empirically verified against a
real `mpv` process yet — worth confirming during testing (test-plan.md IT-11) rather than assumed
correct just because it's the documented default.

### 3.6 Exit codes

Resolves requirements §5.1's "HTTP status, or a fixed code" ambiguity: **fixed**, not the raw
HTTP status. `0` = success, `1` = any failure (`ok:false` from the daemon, a `4xx`/`5xx` the
daemon returned, or `ConnectionFailed`). Reasoning: `playback-controld`'s status codes (`400`,
`404`, `409`) don't carry meaningfully different *actionable* information to a shell script
checking `$?` — "did this succeed" is a boolean question, and a fixed code is simpler for a
script to check (`if gj play ...; then` rather than needing to know which of three non-zero
values might come back). `gj listen`'s clean finish (`NothingPlaying` after real playback, or a
cold start) exits `0` — it's an expected terminal state, not an error.

## 4. Concurrency Model

None needed. Every `gj` invocation is single-threaded: control/browse commands make one blocking
HTTP call and exit; `gj listen` runs one loop, entirely synchronous — `HttpClient::stream()`'s
content-receiver callback calls `fwrite()` directly from the same thread that's blocked inside
`client_.Get(...)`, no queue or background thread involved. This is a deliberate contrast with
`playback-controld`'s design.md §4, which needs a thread pool specifically because it's a
*server* handling a long-lived stream connection and short control connections *concurrently*;
`gj` never has two things happening in the same process at once, so there's nothing to
synchronize and no shared mutable state between threads to protect.

## 5. Error Handling

- **Daemon reachable, `ok: false`**: print `formatError(body)` to `stderr`, exit `1`.
- **Daemon unreachable** (`ApiResult::connected == false` / `StreamResult::Outcome::ConnectionFailed`
  after the one retry): print a clear "could not reach playback-controld at
  `192.168.86.28:8080`" message to `stderr`, exit `1`. The 5-second connection timeout (§3.2)
  ensures this happens quickly rather than after `cpp-httplib`'s 300-second default.
- **`mpv` missing or not on `PATH`**: `_popen` on Windows almost always "succeeds" (it launches
  `cmd.exe`, which then fails to find `mpv` and exits) rather than failing outright, so this
  can't be reliably detected at the `_popen` call itself. Detected instead the first time
  `fwrite` fails inside the content-receiver callback (§3.5) — surfaces as a `ConnectionFailed`
  outcome after the retry, same message path as a real connection failure. This is an accepted
  simplification: the error message won't specifically say "mpv is missing," just that streaming
  failed — good enough for v1, and distinguishing the two would need an upfront `mpv --version`
  probe that adds a startup delay for a check that's rarely going to fail in practice.
- **Malformed/unexpected JSON body**: not specially handled — `nlohmann::json::parse` throws on
  invalid JSON, which is allowed to propagate and terminate `gj` with an uncaught-exception
  message. `playback-controld` always returns well-formed JSON on every response path (verified
  by reading `control_routes.cpp`/`stream_route.cpp` directly), so this is only ever reachable if
  something *other* than `playback-controld` answers on `192.168.86.28:8080` — not worth
  defensive handling for.

## 6. Configuration

Covered fully in requirements.md §6 — `kDaemonHost`/`kDaemonPort` as `constexpr` constants,
consumed by `HttpClient`'s constructor call in `main.cpp` (§3.1). Nothing further to design here.

## 7. Deployment

`gj` is a plain console executable, not a service — built once (`cmake --build .`) and either run
directly from the build directory or copied somewhere on `PATH` (e.g. so `gj play ...` works from
any terminal without a path prefix). No installer, no background process, no systemd-equivalent:
it only ever runs in the foreground, for as long as a single command (or, for `gj listen`, a
single listening session) takes.

**External dependency:** `mpv` must be separately installed and on `PATH` — e.g.
`winget install mpv-player.mpv` or `scoop install mpv`. Verify with `mpv --version` from the same
terminal `gj` will run in before relying on `gj listen`.

## 8. Source Layout

```
cli-lib-controld/
  CMakeLists.txt
  src/
    main.cpp
    http_client.{h,cpp}
    commands.{h,cpp}
    output.{h,cpp}
    listen_command.{h,cpp}
  third_party/
    httplib.h
    json.hpp
  tests/
    output_test.cpp
    commands_test.cpp
```

## 9. Open Items

- ~~`ListenCommand::run()`'s retry bookkeeping~~ — resolved by an independent review before
  implementation started: the original `first_attempt`-only-resets-on-`StreamEnded` design was
  broken for repeated legitimate interrupts (§3.5 now documents both the bug and the fix —
  keying the retry budget on `received_data`, not on which outcome came back). Covered by
  test-plan.md IT-19.
- ~~Piping a `/stream` 404's JSON body into `mpv`'s stdin~~ — resolved in the same review pass;
  `HttpClient::stream()` (§3.5) now uses `cpp-httplib`'s `ResponseHandler`+`ContentReceiver` `Get`
  overload to route bytes based on status instead of writing everything to `sink` unconditionally.
- ~~No mechanism specified for URL-encoding `name` into `/play`'s query string~~ — resolved (§3.2,
  `urlEncode`).
- ~~`argv` bounds-checking~~ — resolved (§3.3) — was previously unstated and would have been
  undefined behavior, not just a crash, on a bare `gj play` with no further arguments.
- Whether `_popen`'s Windows console `Ctrl+C` propagation to `mpv` actually behaves as described
  is still unverified (§3.5) — the review pass confirmed the *reasoning* is a correct description
  of default Windows behavior as far as it goes, but flagged that `_popen` spawns `mpv` via an
  intermediate `cmd.exe /c` hop that the description doesn't account for; first real test is
  test-plan.md IT-18, not a code review.
- No plan yet for testing against something other than the real Jetson (requirements.md's
  hardcoded-host tradeoff, §6) — every integration test in test-plan.md needs
  `playback-controld` actually running and reachable at `192.168.86.28:8080`. Acceptable for a
  single-developer v1, but worth knowing this is a real testing friction point, not an oversight.
