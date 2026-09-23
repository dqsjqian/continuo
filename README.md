# Continuo

Coroutine-native networking for C++20 — a transport core, and protocols that
ride on it.

> *Basso continuo*: the continuously played bass line that supplies the
> harmonic foundation a Baroque work is built over. Aria sings on top of it.

**Status: v0.5 — it serves HTTP.** Event loop on all three I/O backends, TCP
transport, a strict incremental HTTP/1.1 parser, response serialisation, and a
connection loop. A real client gets a real response over a real socket. No TLS
and no routing layer yet. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the roadmap and the
reasoning behind each decision.

| Platform | Backend | State |
|---|---|---|
| macOS · iOS · BSD | kqueue | built + tested |
| Linux · Android | epoll | built + tested in CI |
| Windows | IOCP | built + tested in CI, real loopback TCP |

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

Task<Result<void>> serve(tcp::Listener& listener) {
    auto handler = [](const http::Request& request, auto& writer,
                      std::span<const std::byte> body) -> Task<Result<void>> {
        http::Response response;
        response.status = 200;
        response.headers.append("Content-Type", "text/plain");
        co_return co_await writer.send(response, body);   // echo it back
    };

    for (;;) {
        Result<tcp::Socket> peer = co_await listener.accept();
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

Requires CMake 3.20+ and a C++20 compiler. No dependencies — including for the
test build.

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
```

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
