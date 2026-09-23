# Continuo — architecture

## What this library is

A coroutine-native networking library for C++20: a transport core, and
protocols that ride on it. HTTP/1.1 is the first protocol, not the purpose.

## What "complete" means here

Feature lists are a poor definition of completeness for a protocol library.
Continuo uses six layers instead, and the order matters — each one is only
worth building on top of a solid layer below.

| Layer | Concern | status |
|---|---|---|
| 1 Protocol correctness | RFC 9110 semantics, reject smuggling ambiguity, no guessing on malformed input | reserved |
| 2 Transport & concurrency | transport × protocol decoupling, I/O backends, backpressure | **backends done (kqueue / epoll / IOCP); sockets reserved** |
| 3 Execution model | coroutine-native API, host-owned thread policy | **done (`Task`, `Executor`, `EventLoop`)** |
| 4 API & abstraction | streaming bodies, value-based errors, composable helpers | **done (`Result`, `Buffer`, stream concepts)** |
| 5 Safety & robustness | TLS seam, limits closed by default, continuous fuzzing | reserved |
| 6 Engineering quality | conformance suites, interop benchmarks, ABI policy, docs | **CI across 3 backends + 4 discipline checks** |

The first principle behind layer 1 is worth stating plainly, because it drives
API shape everywhere else: **the value of a protocol library is concentrated in
its negative space** — the completeness with which it says *no* to malformed
input. Continuo does not guess on a `Content-Length`/`Transfer-Encoding`
conflict; it rejects. Limits are closed by default and opened by configuration,
never the reverse.

## The decision everything else follows from: completion, not readiness

Continuo targets macOS, Linux, Windows, iOS, and Android. Those platforms do
not agree on what an asynchronous I/O API *is*:

| | Model | Shape |
|---|---|---|
| kqueue (macOS, iOS, BSD) | **reactor** | "this handle is *ready*" — you then read |
| epoll (Linux, Android) | **reactor** | same |
| IOCP (Windows) | **proactor** | "your read has *completed*" — you submitted it earlier |

This forces a choice, and getting it backwards is how libraries end up with a
first-class POSIX path and a Windows path nobody can reason about:

- A **readiness-shaped** public API (`wait_readable(fd)`) cannot be implemented
  on IOCP without badly emulating it — IOCP never answers "is it ready?".
- A **completion-shaped** public API (`read(handle, buffer)`) maps onto IOCP
  *directly*, and is trivially emulated on a reactor: try the syscall, and on
  `EAGAIN` wait for readiness and retry.

So **the public API is completion-shaped on every platform**, and readiness is
an implementation detail of the POSIX backends. This is the conclusion asio
reached, and the direction io_uring has since taken Linux — a future io_uring
backend fits this API without changing a line of it.

```cpp
std::size_t n = (co_await loop.read(handle, buffer)).value();   // all platforms
```

Readiness is still exposed, but fenced: `wait_readable` / `wait_writable` exist
behind `#if CONTINUO_HAS_READINESS_API` for embedding a descriptor owned by
another library. Code that calls them does not compile on Windows — the honest
outcome, and better than an emulation whose semantics quietly differ.

## Layering

```
protocol   continuo::http   continuo::ws    continuo::h2   …   (each independent)
                  │               │               │
                  └───────────────┴───────────────┘
                                  ↓  reads/writes through stream concepts only
transport  continuo::tcp   continuo::udp   continuo::unix
                                  ↓
core       EventLoop · Buffer · Task · Executor seam · TLS seam · limits
                                  ↓
backend    kqueue (macOS/iOS/BSD) · epoll (Linux/Android) · IOCP (Windows)
```

Four invariants, all enforced by `tools/ci/check_layering.py` rather than by
convention:

1. **Dependencies point downwards only.** `core` must not include transport or
   protocol headers; transport must not include protocol headers. Reaching *up*
   a layer is precisely the move that makes a library unable to grow a second
   protocol later.
2. **No host framework dependency.** Continuo never includes `aria/…`. Hosts
   integrate through the executor and stream seams, so the library stays usable
   standalone.
3. **Platform detection has exactly one home.** Only `platform.hpp` may test
   `_WIN32`, `__linux__`, `__APPLE__` and friends; everything else asks it via
   `CONTINUO_*`. Scattered `#ifdef _WIN32` is how "supports Windows" decays
   into "compiles on Windows".
4. **Protocols are platform-agnostic.** A protocol module may not include OS
   headers. The moment a parser knows what a socket is, it can no longer be
   tested over an in-memory pipe or run over TLS.

The scripts exist because every one of these failure modes is *gradual*. Nobody
decides to weld the socket loop to the parser — it happens one include at a
time, and by the time it hurts, the fix is a rewrite.

### TCP and UDP are transport, not "more protocols"

A common way to phrase the roadmap is "HTTP first, then TCP/UDP". That
mis-files them. TCP and UDP are *transports* — the listener and connector that
HTTP already needs in order to accept a connection at all. They are not future
work; they are foundation work. The only open question is when they become
public API rather than internal plumbing. DNS and QUIC, by contrast, really are
protocols that ride on UDP.

## Core seams

Two concepts carry the whole design. Both live in `modules/core`.

### `Executor` — who resumes a coroutine

```cpp
template <typename E>
concept Executor = requires(E& e, void (*work)()) {
    { e.post(work) } -> std::same_as<void>;
};
```

One function, because `post` is the smallest thing every scheduler already has.
Continuo owns no thread policy: a GUI main loop, a per-core event loop, and a
thread pool are all valid hosts, and none needs to know about the others.
`co_await schedule_on(executor)` moves the rest of a coroutine onto a chosen
executor.

### `AsyncStream` — what a protocol reads and writes

```cpp
template <typename S>
concept AsyncReadStream = requires(S& s, std::span<std::byte> d) {
    { s.read_some(d) } -> std::same_as<Task<Result<std::size_t>>>;
};
```

Short-transfer semantics, matching the syscalls underneath. A protocol module
never names a socket type, so the same parser runs over TCP, a Unix socket, a
TLS session, or an in-memory pipe in tests. Richer operations (`write_all`,
`read_until`) are free functions composed on the minimal contract — never new
requirements on implementers.

The core test suite asserts this claim in code: `write_all` drives a
`MemoryStream` that is not a socket, unchanged.

## Decisions on record

| Decision | Choice | Why |
|---|---|---|
| **I/O model** | **completion-shaped public API** | The only shape that maps onto IOCP *and* reactors; see the section above |
| Platform support | macOS, Linux, Windows, iOS, Android — all first-class | Hosts ship on all of them; a compile-only platform is not supported |
| Readiness API | POSIX-only, behind an explicit macro | No IOCP equivalent; a fenced gap beats a misleading emulation |
| Execution model | coroutine-native, lazy `Task<T>` | The market gap: asio predates coroutines, cpp-httplib is thread-per-connection |
| Library form | compiled library, not header-only | 22k lines in one header costs every consumer compile time; a compiled target can carry an ABI policy |
| Error model | `std::error_code` + `Result<T>` | Standard, interoperable; failures are values, not exceptions |
| Standard floor | C++20, CI also builds C++23 | `std::expected` is C++23-only, so `Result<T>` has a documented C++20 subset |
| Buffer shape | single contiguous region | HTTP/1.1 header parsing wants unbroken `memchr`; h2 framing brings its own chained type |
| Framework coupling | zero — Aria depends on Continuo, never the reverse | Keeps the library usable by anyone; adapters live on the host side |
| Protocol scope | transport + HTTP/1.1 solid; ws/h2/h3 are slots, not promises | Breadth without depth is how protocol libraries get unsafe |

## Scope so far

**v0.1 — seams.** Error model, `Task<T>`, `Buffer`, executor and stream
concepts, warning policy, layering discipline, CI across C++20/C++23 and
sanitizers.

**v0.2 — the loop.** `EventLoop` with three backends (kqueue, epoll, IOCP),
completion-shaped `read`/`write`, timers, cross-thread `post`, and a
`platform.hpp` that is the single home for platform detection. CI now builds
and *runs* tests on all three backends, and cross-compiles for iOS and Android.

Verified locally on kqueue: 118 checks across C++20, C++23, and ASan/UBSan.
The epoll and IOCP backends are validated by CI only — no Linux or Windows
machine was available here, and saying otherwise would be a claim without
evidence.

**Deliberately absent.** Real sockets (`modules/transport`), TLS, HTTP parsing,
cancellation tokens, multi-threaded loops, and detached task launching. The
loop was worth building before sockets because the I/O model dictates what a
socket API can look like; building sockets first is how a poll loop ends up
welded to one protocol.
