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
| 1 Protocol correctness | RFC 9110 semantics, reject smuggling ambiguity, no guessing on malformed input | **HTTP/1.1 parse + serialise + connection loop done** |
| 2 Transport & concurrency | transport × protocol decoupling, I/O backends, backpressure | **backends + TCP done; UDP reserved** |
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

## Why HTTP/1.1 first, and not HTTP/2

A fair question in 2026: HTTP/2 is 51% of requests and HTTP/1.x is 28%
(Cloudflare Radar, mid-2026). Why build the old one?

Because those percentages describe *browser* traffic, and an embeddable server
library is mostly not talking to browsers.

1. **Non-browser clients speak HTTP/1.1.** Cloudflare's own breakdown notes
   that bots, `curl` invocations, language SDKs, CI pipelines and
   server-to-server API calls overwhelmingly default to HTTP/1.1 — filtering
   bots out moves HTTP/1.x from 28% to 9.7%, which is the measurement saying
   plainly where HTTP/1.1 lives. That machine-to-machine traffic is exactly
   what a library like this serves.
2. **HTTP/2 in practice requires TLS.** The spec permits cleartext `h2c`, but
   no browser implements it, so real HTTP/2 means TLS 1.2+ with ALPN
   negotiation. "Start at HTTP/2" therefore means "build a TLS stack first" —
   a larger project than the parser, and one that cannot be tested with a
   string literal.
3. **Reverse proxies terminate at the edge.** A CDN or nginx front-end speaks
   h2/h3 to the browser and HTTP/1.1 to the origin. The origin is precisely
   where an embedded C++ server sits.
4. **The semantics are shared, so the work is not wasted.** RFC 9110 defines
   HTTP semantics; RFC 9112 defines the HTTP/1.1 *syntax*. HTTP/2 (RFC 9113)
   reuses 9110 wholesale. Everything in `message.hpp` — methods, header
   handling, status codes, body-framing rules — is the semantic layer h2 and h3
   also need. Only the line-oriented parser in `parser.cpp` is 1.1-specific.

So the order is not nostalgia; it is dependency order. HTTP/2 lands after TLS,
and it lands on top of the semantic layer built here.

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

## What CI found that local testing could not

Both bugs below built cleanly and passed every test on the development machine.
Both would also have passed a Windows job that only compiled — which is the
argument for running tests on every platform rather than building on them.

1. **Winsock numbers are not Win32 numbers.** MSVC's `std::system_category()`
   maps part of the Winsock space: `WSAEADDRINUSE` is in the table, so the
   exclusive-bind test passed, but `WSAECONNREFUSED` is not. A refused
   connection therefore compared equal to `std::errc::connection_refused` on
   POSIX and to nothing at all on Windows. Fixed by `continuo::socket_error`,
   which translates the Winsock codes that have a portable equivalent.

2. **IOCP completions report NTSTATUS — a third numbering space.**
   `OVERLAPPED_ENTRY::Internal` holds an NTSTATUS
   (`STATUS_CONNECTION_REFUSED` is `0xC0000236`), not a Winsock error. So
   fixing (1) alone changed nothing: the value never reached the translation.
   `WSAGetOverlappedResult` is the documented way back to a Winsock number.

The pattern is worth naming, because it will recur: **the dangerous
portability bug is the one where every platform builds and runs, and one of
them silently fails to match the condition callers switch on.**

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

**v0.3 — the parser.** Incremental, strict HTTP/1.1 request parsing:
`message.hpp` (the RFC 9110 semantic layer, reusable by h2/h3), `limits.hpp`
(bounds closed by default), and a parser whose test suite is mostly published
smuggling vectors. 119 checks, and the byte-at-a-time tests assert that network
slicing cannot change the parse.

**v0.4 — TCP.** `Endpoint`, `Listener`, `Socket`, `connect`, and the bind
semantics that started this project: `ListenOptions::exclusive` makes a second
bind to a live port fail on *every* platform, because `SO_REUSEADDR` means
"reuse a dead socket" on POSIX and "steal a live one" on Windows. The option
exposes the intent; each backend picks whatever flag actually produces it.

This milestone also proved the Windows backend for the first time. The
event-loop tests used `socketpair()`, which Winsock lacks, so every socket case
had been skipped there and IOCP's read/write path had never run. Loopback TCP
runs everywhere, and it immediately found two bugs unreachable from a macOS
machine — see "What CI found" below.

**v0.5 — a working server.** Response serialisation and `serve_connection`,
generic over the stream. The stack runs end to end over a real socket. Three
rules live in the loop rather than in handlers, because breaking any of them
corrupts the *next* request rather than the current one: the body is always
drained, one response per request with one framing, and a HEAD response
carries its Content-Length but no bytes.

An API defect surfaced here and is worth recording, because it is the kind
only an integration reveals: `ParseStep::need_more` meant both "out of bytes"
and "state advanced, call me again". A caller cannot tell those apart, so the
connection loop read from the socket while the buffer still held a complete
request, hit eof, and closed. The parser now advances its own state machine
and `need_more` means exactly one thing. **A state machine must not export an
ambiguous "try again".**

**TLS/HTTPS foundation.** Optional `continuo::tls` (OpenSSL 3) depends on core,
not on TCP or HTTP. A bounded memory BIO pair separates synchronous TLS state
transitions from asynchronous ciphertext reads/writes. No SSL socket BIO or
`SSL_set_fd` is used; IOCP and POSIX therefore share the same TLS pump.

Client verification is mandatory: trusted chain plus DNS/IP identity; DNS
connections also send SNI. TLS 1.2 is the minimum. OpenSSL errors are classified
immediately on the calling thread before any suspension. Fatal errors poison
the session, and ciphertext EOF without close_notify is truncation. Shutdown
sends and flushes only the local close_notify; it does not certify a two-way
shutdown. Context lifetime is retained by SSL; the borrowed transport and spans
must remain alive. One outstanding operation per TLS stream is supported;
overlapping calls are rejected rather than racing the SSL state machine.

Tests generate fresh private CA/certificate/key fixtures at runtime; no private
keys are committed. HTTPS exercises the existing HTTP loop unchanged, with
trusted/untrusted chains, DNS and IP identity mismatches, short encrypted I/O,
large payloads, orderly close and truncated TCP. The optional TLS CI matrix is
separate from the dependency-free build. iOS/Android jobs currently compile all
non-TLS libraries only; target OpenSSL and mobile TLS runtime validation remain
outstanding. BSD also has no dedicated CI evidence.

This integration also fixes HTTP EOF before the end of a partial request head
being mistaken for idle disconnect, and rejects a zero read-chunk policy.
Current request bodies remain buffered, not streamed to handlers.

**Known lifecycle limits.** Pending tasks must not be destroyed while the event
loop/kernel still holds their handles or buffers. Cancellation, deadlines and
IOCP teardown with outstanding operations require further work. This milestone
must not be represented as production-ready or cancellation-safe.

**Deliberately absent.** UDP, routing, HTTP/2, a full HTTP client, native OS trust
store integration, mTLS policy, cancellation tokens, and multi-threaded loops.
