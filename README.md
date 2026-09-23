# Continuo

Coroutine-native networking for C++20 — a transport core, and protocols that
ride on it.

> *Basso continuo*: the continuously played bass line that supplies the
> harmonic foundation a Baroque work is built over. Aria sings on top of it.

**Status: v0.1 — foundation.** The seams are settled, compile and run clean,
and are covered by tests. There is no event loop, no socket, and no HTTP parser
yet. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the roadmap and the
reasoning behind each decision.

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
#include <continuo/core/stream.hpp>

// A protocol never names a socket type — only the stream concept.
continuo::Task<continuo::Result<void>> greet(continuo::AsyncStream auto& stream) {
    constexpr std::string_view message = "hello\n";
    co_return co_await continuo::write_all(
        stream, std::as_bytes(std::span{message.data(), message.size()}));
}
```

Failures are values (`Result<T>` over `std::error_code`), the host decides
which thread resumes a coroutine (`Executor`), and the same parser runs over
TCP, TLS, or an in-memory pipe.

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

It fails the build if `core` reaches up into a transport or protocol layer, or
if anything in the library includes a host framework header.

## Layout

```
modules/core/        event loop, Buffer, Task, executor & stream seams, errors
modules/transport/   tcp / udp / unix          (reserved slot)
modules/http/        HTTP/1.1                  (reserved slot)
tools/ci/            architectural discipline scripts
docs/ARCHITECTURE.md what "complete" means, and every decision on record
```

## License

MIT — see [LICENSE](LICENSE).
