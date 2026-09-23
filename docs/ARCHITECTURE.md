# Continuo — architecture

## What this library is

A coroutine-native networking library for C++20: a transport core, and
protocols that ride on it. HTTP/1.1 is the first protocol, not the purpose.

## What "complete" means here

Feature lists are a poor definition of completeness for a protocol library.
Continuo uses six layers instead, and the order matters — each one is only
worth building on top of a solid layer below.

| Layer | Concern | v0.1 status |
|---|---|---|
| 1 Protocol correctness | RFC 9110 semantics, reject smuggling ambiguity, no guessing on malformed input | reserved |
| 2 Transport & concurrency | transport × protocol decoupling, readiness backends, backpressure | seam defined |
| 3 Execution model | coroutine-native API, host-owned thread policy | **done (`Task`, `Executor`)** |
| 4 API & abstraction | streaming bodies, value-based errors, composable helpers | **done (`Result`, `Buffer`, stream concepts)** |
| 5 Safety & robustness | TLS seam, limits closed by default, continuous fuzzing | reserved |
| 6 Engineering quality | conformance suites, interop benchmarks, ABI policy, docs | **CI + discipline scripts in place** |

The first principle behind layer 1 is worth stating plainly, because it drives
API shape everywhere else: **the value of a protocol library is concentrated in
its negative space** — the completeness with which it says *no* to malformed
input. Continuo does not guess on a `Content-Length`/`Transfer-Encoding`
conflict; it rejects. Limits are closed by default and opened by configuration,
never the reverse.

## Layering

```
protocol   continuo::http   continuo::ws    continuo::h2   …   (each independent)
                  │               │               │
                  └───────────────┴───────────────┘
                                  ↓  reads/writes through stream concepts only
transport  continuo::tcp   continuo::udp   continuo::unix
                                  ↓
core       event loop · Buffer · Task · Executor seam · TLS seam · limits
```

Two invariants, both enforced by `tools/ci/check_layering.py` rather than by
convention:

1. **Dependencies point downwards only.** `core` must not include transport or
   protocol headers; transport must not include protocol headers. Reaching *up*
   a layer is precisely the move that makes a library unable to grow a second
   protocol later.
2. **No host framework dependency.** Continuo never includes `aria/…`. Hosts
   integrate through the executor and stream seams, so the library stays usable
   standalone.

The scripts exist because both failure modes are *gradual*. Nobody decides to
weld the socket loop to the parser — it happens one include at a time, and by
the time it hurts, the fix is a rewrite.

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
| Execution model | coroutine-native, lazy `Task<T>` | The market gap: asio predates coroutines, cpp-httplib is thread-per-connection |
| Library form | compiled library, not header-only | 22k lines in one header costs every consumer compile time; a compiled target can carry an ABI policy |
| Error model | `std::error_code` + `Result<T>` | Standard, interoperable; failures are values, not exceptions |
| Standard floor | C++20, CI also builds C++23 | `std::expected` is C++23-only, so `Result<T>` has a documented C++20 subset |
| Buffer shape | single contiguous region | HTTP/1.1 header parsing wants unbroken `memchr`; h2 framing brings its own chained type |
| Framework coupling | zero — Aria depends on Continuo, never the reverse | Keeps the library usable by anyone; adapters live on the host side |
| Protocol scope | transport + HTTP/1.1 solid; ws/h2/h3 are slots, not promises | Breadth without depth is how protocol libraries get unsafe |

## v0.1 scope

Shipped: error model, `Task<T>`, `Buffer`, executor and stream seams, warning
policy, layering discipline, CI across C++20/C++23 and sanitizers.

Deliberately absent: event loop and readiness backends, real sockets, TLS,
HTTP parsing, cancellation plumbing, detached task launching. Each needs the
seams above to be settled first — which is what v0.1 is for.
