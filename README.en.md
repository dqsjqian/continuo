<div align="center">

# Continuo

**Independent C++23 coroutine networking · Transports first, protocols on top**

One completion-shaped I/O interface across kqueue, epoll and IOCP, without teaching protocols about sockets.

C++23 · TCP · Optional OpenSSL 3 · HTTP/1.1

[简体中文](README.md) | **English**

</div>

---

> *Basso continuo*: the continuously played bass line that provides a musical foundation.
> Continuo aims to be that foundation for networking software, not another all-in-one HTTP framework.

**Current stage: an experimental foundation, not a production-ready networking stack.** TCP, optional TLS and HTTP/1.1 have implementations and tests. Single-threaded `TaskScope` now provides explicit child-task ownership and joining, but complete I/O cancellation, deadlines and end-to-end backpressure remain unfinished. APIs may change as their contracts mature; there is no stable ABI promise yet.

## Why Continuo

- **A networking library, not a compatibility layer.** Independently designed, with no goal of cpp-httplib or Aria API compatibility and no host-framework dependency. HTTP is one protocol module above the transport and execution foundations.
- **Await completion, not readiness.** `co_await read_some(buffer)` returns the outcome of a read. The backends handle kqueue / epoll readiness and IOCP completion notifications.
- **Compose rather than couple.** Protocols depend on `AsyncStream`; applications choose TCP, TLS or an in-memory stream. TLS does not hardcode HTTP, and HTTP does not require OpenSSL.
- **Make errors and boundaries explicit.** `Result<T>` is a direct alias for `std::expected<T, std::error_code>`. Explicit ownership, resource bounds and reproducible tests are design criteria, not claims of completed work.

Quality is not a feature count or an unmeasured performance ranking. Establish lifetimes, cross-platform semantics and protocol correctness before expanding the surface.

## Module architecture

```mermaid
flowchart TB
    App[Application: composition and lifetime ownership]
    App -.-> HTTP[http · HTTP/1.1]
    App -.-> TLS[tls · Optional OpenSSL 3]
    App -.-> TCP[transport · TCP]
    HTTP --> Core[core · Task / TaskScope / Result / AsyncStream / Executor / Buffer / EventLoop]
    TLS --> Core
    TCP --> Core
    Core --> Backends[kqueue · epoll · IOCP]
```

Solid arrows show dependencies; dashed arrows show application composition. HTTP and TLS use core stream contracts and **do not depend directly on the TCP module**. Runtime compositions can include HTTP → TLS → TCP or a custom stream protocol → TCP.

| Directory / build target | Responsibility |
|---|---|
| `modules/core` · `continuo::core` | Coroutines and task scopes, errors, stream and executor interfaces, buffers, event loop and timers |
| `modules/transport` · `continuo::transport` | Numeric IP addresses, TCP listen / connect / read / write |
| `modules/tls` · `continuo::tls` | Optional TLS stream, certificate and hostname verification, ALPN |
| `modules/http` · `continuo::http` | HTTP/1.1 parsing, serialization and single-connection request handling |

`tools/ci/check_layering.py` checks dependency direction, rejects host-framework headers, centralizes platform detection in `platform.hpp`, and keeps OS headers out of protocols. Static layering checks do not establish runtime safety.

## Implemented, unverified and planned

| Area | Implemented foundation | Incomplete / still needs validation |
|---|---|---|
| Execution and lifetimes | Lazy, move-only `Task` that terminates rather than destroy a started, unfinished frame; single-threaded `TaskScope` spawn / join and cooperative stop token; single-threaded `EventLoop`, timers and posted work | Cross-layer join / drain contracts and continued lifetime validation; destroying the loop mid-dispatch is refused rather than supported |
| Cancellation and deadlines | Every `core` operation accepts a stop token and an absolute deadline (`read`/`write`/`accept`/`connect`/`sleep`/`wait_*`) on both backends, resolving exactly once through one point | **Transport, TLS and HTTP do not forward them yet**, so the stack above `core` cannot be said to support cancellation; the Windows semantics rest on CI alone |
| TCP | IPv4 / IPv6, listen, connect, short transfers, exclusive binding by default | Continued close / completion race validation; end-to-end operation and queue bounds |
| TLS (optional) | OpenSSL 3, certificate-chain and DNS-name / IP verification, one ALPN identifier, close notifications | Cancellation safety; mobile TLS; broader interoperability evidence |
| HTTP/1.1 | Incremental parsing, serialization, keep-alive, pipelined request handling, HEAD, chunked responses | Request bodies currently use bounded buffering; streaming requests, routing and a complete client remain unimplemented |
| Security and resources | Parser limits, malformed-input tests, bounded TLS BIO | End-to-end backpressure, aggregate memory bounds, broader fuzzing and failure injection |
| Future transports and protocols | TCP stream contracts as a starting point | UDP / datagrams, DNS, further protocols and backends; HTTP/2 and HTTP/3 are not implemented |

“Implemented” does not mean that an area has passed complete acceptance testing. `stop()` is still **not cancellation**: it asks `run()` to return. Per-operation cancellation is what `OperationOptions` is for, and it currently reaches `core` only.

### Platforms and evidence

| Platform | Backend | Validation scope |
|---|---|---|
| macOS | kqueue | Desktop runtime tests, including TLS / HTTPS |
| Linux | epoll | Desktop runtime CI, separate TLS matrix |
| Windows | IOCP | Desktop loopback runtime CI, separate TLS matrix |
| iOS / Android | kqueue / epoll | Non-TLS cross-compilation only; no device runtime evidence. Android needs **API level 30 or newer**, because the NDK's libc++ does not provide `std::stop_token` below it |
| BSD | kqueue | Backend portability direction; no dedicated CI evidence |

The latest confirmed passing three-desktop CI baseline is `a123370`. The IOCP cancellation semantics have **no runtime evidence at all** on the development machine: locally they are only cross-compiled through mingw-w64 as a type and lifetime check, and mingw is not MSVC — compiling is not running. A CI configuration is not proof that the current code passed.

## A look at the API

These are compilable coroutine functions, not a complete runnable server. The host must start and await the task while driving the associated `EventLoop`; a complete structured server-launch API is not available yet.

### TCP: read a chunk, write it back

```cpp
#include <continuo/core/stream.hpp>
#include <continuo/transport/tcp.hpp>

#include <array>
#include <cstddef>
#include <span>

continuo::Task<continuo::Result<void>>
echo_tcp(continuo::transport::tcp::Socket& socket) {
    std::array<std::byte, 4096> buffer{};
    for (;;) {
        auto read = co_await socket.read_some(buffer);
        if (!read) {
            if (read.error() == continuo::Errc::eof) {
                co_return continuo::Result<void>{};
            }
            co_return continuo::fail(read.error());
        }
        auto written = co_await continuo::write_all(
            socket, std::span<const std::byte>{buffer}.first(*read));
        if (!written) {
            co_return continuo::fail(written.error());
        }
    }
}
```

`read_some` permits short reads; `write_all` composes short writes until completion. The buffer lives in the coroutine frame and is not reused until the entire write finishes.

### HTTP: one handler, different streams

```cpp
#include <continuo/http/connection.hpp>
#include <continuo/transport/tcp.hpp>

#include <cstddef>
#include <span>

using namespace continuo;

template<AsyncStream Stream>
Task<Result<void>> echo_response(
    const http::Request&,
    http::ResponseWriter<Stream>& writer,
    std::span<const std::byte> body) {
    http::Response response;
    response.status = 200;
    response.headers.append("Content-Type", "application/octet-stream");
    co_return co_await writer.send(response, body);
}

Task<Result<void>> serve_http(transport::tcp::Socket& socket) {
    co_return co_await http::serve_connection(
        socket, echo_response<transport::tcp::Socket>);
}
```

`serve_connection` handles parsing, request-body consumption and the request loop on one connection. It does not schedule accepts or manage concurrent connections. The handler is a free function, avoiding temporary coroutine-lambda closure lifetime hazards. Substituting an already-handshaken `tls::Stream<T>` lets the same handler logic serve TLS.

### TaskScope: start children explicitly and await cleanup

```cpp
#include <continuo/core/event_loop.hpp>
#include <continuo/core/task_scope.hpp>

#include <chrono>
#include <stop_token>
#include <system_error>

continuo::Task<void> delayed_increment(
    continuo::EventLoop& loop, std::stop_token stop, int& count) {
    auto slept = co_await loop.sleep_for(std::chrono::milliseconds{1});
    if (!slept) {
        throw std::system_error(slept.error());
    }
    if (!stop.stop_requested()) {
        ++count;
    }
}

continuo::Task<int> count_after_delay(continuo::EventLoop& loop) {
    int count = 0;
    continuo::TaskScope scope;
    scope.spawn(delayed_increment(loop, scope.get_stop_token(), count));
    co_await scope.join();
    co_return count;
}
```

The host must drive `loop` and await `count_after_delay`; do not call `sync_get()` on it. `count` lives in the parent coroutine frame until join completes; the host must keep `loop` and the parent task alive. Free functions avoid dangling temporary coroutine-lambda closures. The `sleep_for` `Result` error is explicitly converted to an exception rather than discarded.

- `TaskScope` is neither copyable nor movable. `spawn(Task<void>)` takes ownership of an unstarted, nonempty task and starts it immediately. Completed child frames are reclaimed promptly, not retained until scope destruction.
- `join()` **may be called only once and closes spawn intake immediately**, at the call. Its lazy `Task<void>` must be driven to completion. A used scope still requires join even when `pending() == 0`.
- The first observed child exception triggers `request_stop()`. Join rethrows that exception only after all children and their frames have been cleaned up; it does not abandon siblings early.
- `get_stop_token()` / `request_stop()` provide only a cooperative signal, not automatic cancellation of pending I/O or deadlines. Scope operations, child completion and stop callbacks must run on the same thread.
- Only an empty scope on which neither spawn nor join has been used may be destroyed directly. Every other scope requires completed join (including rethrow after cleanup). Premature scope destruction or destruction of a waiting join calls `std::terminate()`. Discarding an unstarted join does not remove the obligation to finish joining before destruction. This is fail-fast, not implicit cancellation or background cleanup, and does not silently free child frames still referenced by I/O.
- Awaiting an empty or already-consumed `Task` throws `std::logic_error`; passing an empty task to the scope throws `std::invalid_argument`.

`EventLoop::yield()` registers an already-due timer, counts as outstanding work and resumes on the next loop pump. Loop shutdown also resumes it so a scope can finish joining. Its return type remains `Task<void>` and **does not expose cancellation status**. This neither permits further use of a shut-down loop nor changes the boundary that `stop()` is not cancellation.

**Lifetime obligations for callers**

- Sockets, TLS streams, borrowed handler state and buffers must outlive their corresponding operations; the associated event loop must live longer still.
- Except for `post()` / `stop()`, event-loop operations belong on the owner thread. An executor interface does not make sockets thread-safe.
- Do not implement timeouts by destroying tasks: pass `OperationOptions{.deadline = ...}`. Destroying a started, unfinished frame terminates, and so does calling `Task::sync_get()` on real asynchronous I/O — the event loop may still hold that frame's handle, its result slot and buffers it borrowed.
- Do not destroy, replace or re-enter the event loop while it is dispatching a batch, which includes doing so from a coroutine it just resumed. That terminates too: operations already taken out of its queues are waiting to be delivered, and on Windows an undrained completion batch still refers to loop state.
- The connection owner handles shutdown and closing. These functions neither transfer socket ownership nor provide cancellation or concurrent connection management.

### Cancellation and deadlines: a budget for one operation

```cpp
#include <continuo/core/event_loop.hpp>
#include <continuo/core/task_scope.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stop_token>

using namespace std::chrono_literals;

continuo::Task<continuo::Result<std::size_t>> read_with_budget(
    continuo::EventLoop& loop,
    continuo::NativeHandle handle,
    std::span<std::byte> into,
    std::stop_token stop) {
    co_return co_await loop.read(handle,
                                 into,
                                 {.stop = std::move(stop),
                                  .deadline = continuo::EventLoop::Clock::now() + 5s});
}

continuo::Task<void> read_until_stopped(continuo::EventLoop& loop,
                                        continuo::NativeHandle handle) {
    continuo::TaskScope scope;
    std::array<std::byte, 4096> buffer{};

    const continuo::Result<std::size_t> first =
        co_await read_with_budget(loop, handle, buffer, scope.get_stop_token());
    if (!first) {
        scope.request_stop();
    }
    co_await scope.join();
}
```

`OperationOptions` is a plain aggregate, filled in with designated initialisers. Two things about it are **contracts** rather than style:

- **The member order cannot be reordered** — designated initialisers must be written in declaration order, so reordering would break every `{.stop = ..., .deadline = ...}` call site at compile time.
- **It is passed by value** — a reference bound to a `{...}` temporary dies at the end of the expression that creates the coroutine, which is before the coroutine first resumes. Copying also lets the stop state outlive the `TaskScope` that owns the `stop_source`, which is exactly what an operation still winding down after its scope exits needs.

The deadline is an **absolute** time point rather than a duration: an internal retry (`EAGAIN`, `EINTR`, a partial readiness wake-up) must not refresh the budget, or a slow peer could hold the operation open indefinitely while every individual wait stayed under the limit.

Resolution rules:

| Situation | Outcome |
|---|---|
| Token already stopped at submission | `Errc::cancelled`, nothing submitted |
| Deadline already past at submission | `Errc::timed_out`, nothing submitted |
| Both | `cancelled` — an explicit request outranks an elapsed budget |
| Zero-length operation with either | The reason, not a 0-byte success it never performed |
| Completion and deadline / cancellation in the same `run_once` | The completion. It genuinely happened |
| Repeated cancellation | Resolved once; the extra requests find nothing |

**What cancellation does not do:** it does not roll back I/O that already happened; it does not undo a `connect` the kernel already started (the socket is left indeterminate and the caller must close it — the loop never closes a handle it was lent); and on IOCP a cancelled operation waits for its completion packet before the pinned reason is delivered, **even if that packet reports success** — so a cancelled read whose buffer the kernel had already filled leaves a hole in the stream, and that connection should be closed rather than reused. `timed_out` may also be delivered later than the deadline there.

A stop may be requested from **any thread**, but is always delivered on the loop thread: the callback records an operation id and nudges the loop. That avoids the re-entrancy trap of a `std::stop_callback` firing synchronously inside `await_suspend`, and is also what makes an off-thread request safe.

## Build and integrate

Requires **CMake 3.20+, a C++23 compiler and a standard library providing both `std::expected` and `std::stop_token`**. The default non-TLS build has no third-party dependency. Enabling TLS explicitly requires OpenSSL 3. Android needs **API level 30 or newer**: the NDK's libc++ does not provide `std::stop_token` below it.

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=23
cmake --build build/debug --config Debug
ctest --test-dir build/debug -C Debug --output-on-failure
```

Enable TLS and HTTPS integration tests:

```sh
cmake -S . -B build/tls -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_STANDARD=23 -DCONTINUO_ENABLE_TLS=ON
cmake --build build/tls --config Debug
ctest --test-dir build/tls -C Debug --output-on-failure
```

Set `OPENSSL_ROOT_DIR` if CMake cannot locate OpenSSL 3. C++20 is no longer supported.

For an existing CMake project, assuming the source is in `vendor/continuo` and `my_app` already exists:

```cmake
add_subdirectory(vendor/continuo)
target_link_libraries(my_app PRIVATE continuo::transport continuo::http)
```

For TCP only, link just `continuo::transport`. For TLS, enable `CONTINUO_ENABLE_TLS` and additionally link `continuo::tls`. `CONTINUO_BUILD_TESTS` defaults to on for a top-level build and off when included as a subdirectory.

### TLS boundaries

- `tls::Context::client(ca_file)` verifies the certificate chain; `tls::Stream<T>::create` takes the DNS name or IP to verify. Omitting the CA file uses OpenSSL's default trust paths, not necessarily the OS-native trust store. There is no insecure verification bypass switch.
- After creating the stream, `co_await stream.handshake()` before I/O. On normal completion, `co_await stream.shutdown()` before closing TCP.
- `shutdown()` sends and flushes the local `close_notify` without waiting for the peer's reply. TCP EOF without a close notification during reads is reported as truncation.
- Operations on each TLS stream are serialized; overlap is rejected. The optional final `protocol` parameter on context factories accepts one ALPN identifier. The default is no negotiation; HTTPS can explicitly select `"http/1.1"` and inspect `negotiated_protocol()`. This does not imply HTTP/2 support.

## Tests and roadmap

There are **8 regular test suites with TLS enabled**: `core`, `event_loop`, `task_scope`, `transport`, `http_parser`, `http_server`, `http_end_to_end` and `tls_https`, or 7 without TLS. **Eight** separate fail-fast CTests each commit one contract violation in its own process and demand the *exact* exit code the terminate handler installs, so that an ordinary crash cannot pass as a deliberate refusal. **The CTest total is 16 with TLS or 15 without it.** Regular suites exercise foundational types, the event loop — including the order in which per-operation cancellation and deadlines are decided, same-batch precedence, and per-direction readiness registration — scope frame reclamation / join / exceptions and cooperative stop, TCP loopback, HTTP parsing and connection handling, TLS / HTTPS composition and related negative cases. Counts are not proof of completeness.

```sh
python3 tools/ci/check_layering.py
```

Next priorities:

1. **Forward cancellation and deadlines through transport, TLS and HTTP:** only `core` supports them today, so the stack as a whole does not.
2. **Establish end-to-end resource contracts:** streaming request bodies, slow-consumer backpressure, queue and memory limits.
3. **Expand with evidence:** cross-platform negative tests, sanitizers, fuzzing, interoperability and reproducible benchmarks; then additional transports and protocols as needed.

See the [architecture document](docs/ARCHITECTURE.md) for design rationale and detailed acceptance criteria. The roadmap describes direction, not shipped functionality or release dates.

## Contributing

Start with a reproducible issue, an explicit contract or a focused test. Preserve one-way module dependencies, explain ownership and platform differences, and run relevant tests plus the layering check. Performance improvements need reproducible environments and measurements, not unverified rankings.

## License

[MIT](LICENSE)
