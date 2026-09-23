# Continuo

Coroutine-native networking for C++20 — a transport core, and protocols that
ride on it.

> *Basso continuo*: the continuously played bass line that supplies the
> harmonic foundation a Baroque work is built over. Aria sings on top of it.

**Status: TLS/HTTPS foundation.** Event loop on three I/O backends, TCP,
HTTP/1.1 parsing/serialisation and a connection loop, plus an optional OpenSSL 3
TLS stream. This is an experimental foundation, not a production-ready server:
cancellation/deadlines, concurrent connection ownership, routing and a full HTTP
client remain outstanding. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the roadmap and the
reasoning behind each decision.

| Platform | Backend | State |
|---|---|---|
| macOS | kqueue | local runtime tests, including TLS/HTTPS |
| Linux | epoll | desktop runtime CI, separate TLS matrix |
| Windows | IOCP | desktop loopback runtime CI, separate TLS matrix |
| iOS / Android | kqueue / epoll | cross-compile non-TLS modules only; no device runtime evidence |
| BSD | kqueue | backend intended to be portable; no dedicated CI evidence |

CI configuration describes the validation plan, not proof that an unrun change
passed. Mobile TLS requires a target-built OpenSSL 3 toolchain and is not yet
covered by this repository's CI.

One public API across all of them, because it is **completion-shaped** rather
than readiness-shaped — the only shape that maps onto IOCP as directly as onto
epoll. That decision is the backbone of the whole design; the reasoning is in
the architecture doc.

## Why

Existing options each give up something:

| | Coroutine-native API | Easy to embed | Grows new protocols |
|---|---|---|---|
| asio + Beast | retrofitted | heavy, verbose | yes |
| cpp-httplib | no (thread-per-connection) | yes | no — socket/parse/handler are welded |
| drogon | partly | it's a full framework | within the framework |
| nghttp2 | n/a (C) | yes | HTTP/2 only |

Continuo aims at the empty cell: asio's structural ability, cpp-httplib's
ergonomics, and an API that was coroutine-shaped from the first commit.

## What it looks like

```cpp
#include <continuo/http/connection.hpp>
#include <continuo/transport/tcp.hpp>

using namespace continuo;

Task<Result<void>> serve(transport::tcp::Listener& listener) {
    auto handler = [](const http::Request& request, auto& writer,
                      std::span<const std::byte> body) -> Task<Result<void>> {
        http::Response response;
        response.status = 200;
        response.headers.append("Content-Type", "text/plain");
        co_return co_await writer.send(response, body);   // echo it back
    };

    for (;;) {
        Result<transport::tcp::Socket> peer = co_await listener.accept();
        if (!peer) {
            co_return fail(peer.error());
        }
        // One connection, many requests: keep-alive, pipelining, and body
        // draining are handled by the loop, not by the handler.
        co_await http::serve_connection(*peer, handler);
    }
}
```

Failures are values (`Result<T>` over `std::error_code`), the host decides
which thread resumes a coroutine (`Executor`), and `serve_connection` is
generic over the stream — the same handler will serve TLS unchanged.

## Build

Requires CMake 3.20+ and a C++20 compiler. The default non-TLS build has no
third-party dependency. Enable TLS explicitly to require OpenSSL 3.

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

For TLS and HTTPS integration tests:

```sh
cmake -S . -B build/tls -DCONTINUO_ENABLE_TLS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/tls
ctest --test-dir build/tls --output-on-failure
```

Set `OPENSSL_ROOT_DIR` if CMake cannot locate your OpenSSL 3 installation.
`tls::Context::client(ca_file)` verifies both the certificate chain and the
DNS name/IP passed to `tls::Stream<T>::create`. Omitting the CA file uses
OpenSSL's default trust paths, not necessarily the operating system's native
trust store. There is no insecure verification bypass. Context factories accept
an optional final `protocol` argument for a single ALPN identifier; default is
no ALPN. HTTPS callers can opt into `"http/1.1"` and inspect
`stream.negotiated_protocol()`. The TLS module does not hardcode HTTP or imply
HTTP/2 support.

Create a stream over a live TCP socket, `co_await stream.handshake()`, then pass
it to the existing `http::serve_connection`. On normal completion call
`co_await stream.shutdown()` before closing TCP. Shutdown sends and flushes the
local `close_notify`; it does not wait for a peer reply. Bare TCP EOF during TLS
reads is reported as truncation, not a clean TLS close.

TLS currently serializes operations on each stream (overlap is rejected). The
stream, transport and borrowed buffers must outlive their pending tasks. Do not
destroy pending event-loop tasks to implement a timeout: cancellation-safe
lifetime management remains outstanding. HTTP request bodies are currently
buffered up to the configured limit, not streamed to the handler.

Architectural invariants are checked by a script, not by convention:

```sh
python3 tools/ci/check_layering.py
```

It fails the build when a layer reaches upwards, when anything includes a host
framework header, when a file tests a raw platform macro instead of asking
`platform.hpp`, or when a protocol module includes an OS header.

## Layout

```
modules/core/        EventLoop, Buffer, Task, executor & stream seams, errors
  include/…/platform.hpp   the only file that detects a platform
  src/event_loop_posix.cpp kqueue / epoll backend
  src/event_loop_iocp.cpp  Windows backend
modules/http/        HTTP/1.1
  include/…/message.hpp    RFC 9110 semantics — shared with h2/h3 later
  include/…/limits.hpp     bounds, closed by default
  src/parser.cpp           incremental, strict
modules/transport/   TCP
  include/…/endpoint.hpp   numeric addresses, no DNS
  include/…/tcp.hpp        Listener / Socket / connect
  src/socket_compat.hpp    the only OS-networking include in the module
tools/ci/            architectural discipline scripts
docs/ARCHITECTURE.md what "complete" means, and every decision on record
```

## License

MIT — see [LICENSE](LICENSE).
