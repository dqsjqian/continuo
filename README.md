# Continuo

Coroutine-native networking for C++20 — a transport core, and protocols that
ride on it.

> *Basso continuo*: the continuously played bass line that supplies the
> harmonic foundation a Baroque work is built over. Aria sings on top of it.

**Status: v0.2 — the loop runs.** Seams are settled and the event loop works on
all three I/O backends. No sockets and no HTTP parser yet. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the roadmap and the
reasoning behind each decision.

| Platform | Backend | State |
|---|---|---|
| macOS · iOS · BSD | kqueue | built + tested |
| Linux · Android | epoll | built + tested in CI |
| Windows | IOCP | built + tested in CI |

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
#include <continuo/core/event_loop.hpp>

continuo::Task<continuo::Result<void>> echo_once(continuo::EventLoop& loop,
                                                 continuo::NativeHandle handle) {
    std::array<std::byte, 4096> scratch{};

    // "tell me when this read finished" — identical on kqueue, epoll, IOCP.
    continuo::Result<std::size_t> read = co_await loop.read(handle, scratch);
    if (!read) {
        co_return continuo::fail(read.error());   // Errc::eof on a clean close
    }

    co_await loop.write(handle, std::span{scratch}.first(*read));
    co_return continuo::Result<void>{};
}
```

Failures are values (`Result<T>` over `std::error_code`), the host decides
which thread resumes a coroutine (`Executor`), and a protocol written against
the stream concepts runs over TCP, TLS, or an in-memory pipe unchanged.

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
modules/transport/   tcp / udp / unix          (reserved slot)
modules/http/        HTTP/1.1                  (reserved slot)
tools/ci/            architectural discipline scripts
docs/ARCHITECTURE.md what "complete" means, and every decision on record
```

## License

MIT — see [LICENSE](LICENSE).
