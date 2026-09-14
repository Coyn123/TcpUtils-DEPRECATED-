# TcpUtilsV2

A cross-platform TCP sockets library in C++, built around RAII ownership and explicit `Result<T>` error handling instead of exceptions or raw error codes.

## Status

Active work in progress. Development is currently focused on a small, self-contained core, `Listener` → `Connection` → `IStream` → `TaskBase`/`HttpTask`/`WSTask`, rather than the full library.

## Active core

- **[src/core/Platform.h](src/core/Platform.h)** — thin cross-platform shim over BSD sockets / Winsock: a common `socket_t` type, `close_socket` wrapper, one-time `WSAStartup`/`WSACleanup` via `ensure_started()`, and SIGPIPE-safe send flags. Two separate last-error accessors — `last_error()` (socket calls; `WSAGetLastError()` on Windows) and `last_file_error()` (everything else; plain `GetLastError()` on Windows) — since the two aren't interchangeable there.
- **[src/core/ResultType.h](src/core/ResultType.h)** — `tcp::Result<T>`, a minimal success-value-or-error-code type used in place of exceptions across the API.
- **[src/transport/Stream.h](src/transport/Stream.h) / [src/transport/Stream.cpp](src/transport/Stream.cpp)** — `tcp::IStream`, the `read_some` / `write_some` / `write_all` interface `Connection` implements, so higher-level code depends on an abstraction rather than a raw socket. `read_some`/`write_some` are pure virtual; `write_all` is a concrete loop over `write_some` that accumulates bytes written and retries on short writes.
- **[src/transport/Connection.h](src/transport/Connection.h) / [src/transport/Connection.cpp](src/transport/Connection.cpp)** — RAII wrapper around a connected socket: move-only, closes its descriptor on destruction, retries on `EINTR`, and reports peer EOF as `Result::ok(0)` rather than an error.
- **[src/transport/Listener.h](src/transport/Listener.h) / [src/transport/Listener.cpp](src/transport/Listener.cpp)** — RAII wrapper around a listening socket. `Listener::create(port)` is a factory returning `Result<Listener>`, so a partially-initialized listener can never escape into a live object; `accept()` hands back a `Connection`.
- **[src/transport/BufferedReader.h](src/transport/BufferedReader.h) / [src/transport/BufferedReader.cpp](src/transport/BufferedReader.cpp)** — buffered reads on top of an `IStream`: `read_until` for delimiter-based reads (request lines, headers), `read_exact` for fixed-size reads (a body sized by `Content-Length`). Both share a private chunk-reading helper.
- **[src/util/Base64.h](src/util/Base64.h) / [src/util/Base64.cpp](src/util/Base64.cpp)** and **[src/util/Sha1.h](src/util/Sha1.h) / [src/util/Sha1.cpp](src/util/Sha1.cpp)** — generic, dependency-free `base64_encode`/`sha1` byte-manipulation utilities, with no HTTP/WebSocket-specific knowledge. Used by the WebSocket handshake in `HttpTask`.
- **[src/http/HttpParser.h](src/http/HttpParser.h) / [src/http/HttpParser.cpp](src/http/HttpParser.cpp)** — the HTTP layer, composed over `BufferedReader` rather than a concrete socket:
  - `Http::build_request(reader)` — parses a request line, headers (merging duplicates, rejecting duplicate `Content-Length`/`Host`), and a `Content-Length`-sized body into an `HttpRequest`. Failures return a `Http::ParseError` (distinct from the propagated OS `errno` values used for actual socket failures) via `tcp::Result`.
  - `Http::route(url)` — maps a URL directly onto a file under `templates/` (bare `/` aliases to `index.html`), sanitized against path traversal: the joined candidate path is resolved with `std::filesystem::weakly_canonical` and checked that it's still nested under the canonicalized `templates/` root via `std::filesystem::relative` before it's trusted. Returns `tcp::Result<std::string>`, `err` on anything outside the root or a resolution failure.
  - `Http::build_response(request)` — reads the routed file off disk (rejecting anything that isn't a regular file, so a directory match 404s instead of downloading empty) and builds an `HttpResponse`, including a `Content-Type` resolved from the file extension against a small MIME table (falls back to `application/octet-stream` for anything unrecognized). A missing/rejected file is a normal `200`/`404`-style `Result::ok`, not an `err` — `err` is reserved for genuine I/O failures, reported via `last_file_error()`.
  - `Http::serialize_response(response)` — renders an `HttpResponse` into wire-format bytes, with a status-code → reason-phrase lookup (`200`, `404`, `101`, `400`, `500` today).
- **[src/websocket/WebSocketFrame.h](src/websocket/WebSocketFrame.h) / [src/websocket/WebSocketFrame.cpp](src/websocket/WebSocketFrame.cpp)** — the RFC 6455 frame layer, composed over `BufferedReader` the same way `HttpParser` is: `WebSocket::parse_frame(reader)` reads a frame off the wire (FIN/opcode, all three payload-length encodings, the mandatory client mask key, unmasking) into a `WsFrame`; `WebSocket::serialize_frame(frame)` renders one back to wire bytes for sending (server frames are never masked, per spec).
- **[src/tasks/TaskBase.h](src/tasks/TaskBase.h)**: `TaskBase`, an abstract one-shot unit of work, a virtual destructor (so ownership through a base handle cleans up correctly) plus a single pure virtual `run_task()`. No data of its own; each concrete task owns whatever it needs.
- **[src/tasks/HttpTask.h](src/tasks/HttpTask.h) / [src/tasks/HttpTask.cpp](src/tasks/HttpTask.cpp)**: `HttpTask`, the concrete `TaskBase` for one accepted connection. Takes ownership of a `Connection` at construction; `run_task()` runs `build_request`, then branches: a normal request goes through `build_response` → `serialize_response` → `write_all` once and returns; a WebSocket upgrade request computes the RFC 6455 handshake (`sha1`/`base64_encode` over the client's `Sec-WebSocket-Key`), sends the `101` response, then hands the connection off to a `WSTask` (moving `connection_` into it) instead of ending. A malformed request or a failed `build_response` now gets an actual `400`/`500` sent back before the connection closes, instead of silently dropping.
- **[src/tasks/WSTask.h](src/tasks/WSTask.h) / [src/tasks/WSTask.cpp](src/tasks/WSTask.cpp)**: `WSTask`, the concrete `TaskBase` that owns a connection for the lifetime of a WebSocket session (unlike `HttpTask`, not one-shot). `run_task()` loops `WebSocket::parse_frame` → branch on opcode (text/binary echoed back, ping answered with pong, close answered with close and the loop ends, pong ignored) → repeat.
- **[src/api/TcpUtil.h](src/api/TcpUtil.h) / [src/api/TcpUtil.cpp](src/api/TcpUtil.cpp)** — the harness that ties everything above together: `TcpUtil(port, factory, max_concurrent = 6)` takes a `TaskFactory` (`std::function<std::unique_ptr<TaskBase>(Connection)>`) so callers can plug in `HttpTask`, a future task type, or their own. Construction can't fail (it just stores config); `run()` does the actual `Listener::create` + accept loop and returns `tcp::Result<void>`, so startup failure (bad port, already in use) is reported through the same `Result` convention as everything else. Each accepted connection runs on its own detached `std::thread`, gated by a `std::counting_semaphore` sized to `max_concurrent` so the accept loop can't spawn unbounded threads under load; a thread releases its semaphore permit when its task finishes. `EMFILE`/`ENFILE` on `accept()` (file-descriptor exhaustion) get a short backoff-and-retry instead of spinning the CPU at 100% or killing the server outright. `make_http_task(Connection)` is the free-function factory that wraps `HttpTask` for this pattern.
- **[src/Main.cpp](src/Main.cpp)**: a 3-line usage example — constructs a `TcpUtil` on port `8080` with `make_http_task` and calls `run()`. Not required scaffolding; anyone using this as a library constructs their own `TcpUtil` with their own factory instead.

Together these give you a working, dependency-free HTTP **and WebSocket** server today, serving static files out of `templates/` with sanitized routing and MIME-typed responses, non-blocking thread-per-connection concurrency with a configurable cap, plus WebSocket upgrade + handshake + an echoing frame loop (verified end-to-end against a real client, not just unit-level). The transport core — `Listener`, `Connection`, `IStream`/`Stream`, `BufferedReader` — is feature-complete for basic reads and writes.

## Direction

Known gaps, not oversights:
- No HTTP keep-alive: each `HttpTask` runs its request/response sequence once and ends (a WebSocket upgrade is the one exception — that connection persists under `WSTask` for the session).
- The reason-phrase table in `serialize_response` covers `200`/`404`/`101`/`400`/`500`; anything else outside that set renders as `"Unknown"`.
- `TcpUtil::run()`'s accept loop currently retries on every `accept()` failure rather than ever breaking out — worth revisiting whether some errors (a genuinely dead listening socket, e.g. `EBADF`) should still end the loop instead of retrying forever.

Longer-term, an HTTPS layer should be able to slot in as a `TlsStream : IStream` without changing the HTTP code at all, since everything above is composed over `IStream`/`BufferedReader` rather than bound to a concrete transport. `TcpUtil`'s factory pattern is meant to support that the same way it would a `make_ws_task` or any other task type — a caller can construct `TcpUtil(port, their_factory)` without needing a new server class.

An earlier, inheritance-based HTTP server implementation (`TcpUtils`, `TcpHttpProtocol`, `TcpHttpServer`, `TcpHttpServerImplementation` — raw `SOCKET` handles, manual `closesocket` calls, no RAII) was removed from the working tree since it was never wired into the active core. It's still recoverable from git history if it's ever worth referencing again.

## Building

There's no build system wired up yet — compile the active core directly. Source lives under `src/`, organized by abstraction layer (`core/`, `transport/`, `http/`, `util/`, `tasks/`, `websocket/`, `api/`); `-Isrc` lets every file `#include` its dependencies by layer-qualified path (e.g. `#include "transport/Connection.h"`) without needing a flag per folder.

```bash
# Linux/macOS
g++ -std=c++20 -Isrc src/Main.cpp src/transport/Listener.cpp src/transport/Connection.cpp src/transport/Stream.cpp src/transport/BufferedReader.cpp src/http/HttpParser.cpp src/tasks/HttpTask.cpp src/tasks/WSTask.cpp src/websocket/WebSocketFrame.cpp src/util/Base64.cpp src/util/Sha1.cpp src/api/TcpUtil.cpp -o server -pthread

# Windows (MinGW)
g++ -std=c++20 -Isrc src/Main.cpp src/transport/Listener.cpp src/transport/Connection.cpp src/transport/Stream.cpp src/transport/BufferedReader.cpp src/http/HttpParser.cpp src/tasks/HttpTask.cpp src/tasks/WSTask.cpp src/websocket/WebSocketFrame.cpp src/util/Base64.cpp src/util/Sha1.cpp src/api/TcpUtil.cpp -o server.exe -lws2_32
```

`std::thread` is used now. If the Windows build fails to link, or `std::thread` throws at runtime, add `-pthread` (needed on some MinGW-w64 builds; a "win32 threads" MinGW distribution doesn't support `std::thread` at all and needs a different toolchain).

## Running

```bash
./server
```

Listens on port `8080` and serves requests indefinitely, up to `TcpUtil`'s default concurrency cap of 6 simultaneous connections. Visit `http://localhost:8080/` (or `curl -v`) to hit `templates/index.html`.
