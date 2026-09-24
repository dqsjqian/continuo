# Continuo — architecture

## What this library is

An independent, coroutine-native C++23 networking library: a transport core,
and protocols that ride on it. HTTP is one protocol family, not the purpose.

### Design mandate (2026-09-23)

- Design from networking requirements, not cpp-httplib feature parity or an
  existing consumer's API. Neither cpp-httplib nor a host framework constrains
  the public interface. Consumers adapt later; breaking changes are permitted.
- C++23 is the minimum baseline. The build requires C++23 and `Result<T>`
  aliases `std::expected<T, Error>` directly. C++20 compatibility is removed;
  each target toolchain still needs explicit validation.
- Prioritize explicit ownership, structured task lifetimes, cancellation and
  deadlines, bounded buffering/backpressure, composable transports/protocols,
  and consistent cross-platform semantics. These are acceptance criteria to
  implement and verify, not claims that the current code already meets them.
- Evaluate correctness, API usability, performance and resource bounds through
  executable tests, interoperability checks and reproducible benchmarks.
  Existing libraries are comparison evidence, not the specification.
- Current phase develops Continuo only. Consumer migration, removal of old
  dependencies, and public release are later phases; keep this repository
  private for now.

## Acceptance criteria, not a completeness score

The existence of a type or a passing integration test is not evidence that its
lifetime, concurrency and resource contracts are complete. Current foundations
and remaining acceptance work must be described separately.

| Concern | Current foundation | Remaining acceptance work |
|---|---|---|
| Execution and ownership | Lazy, move-only `Task`, single-threaded `TaskScope` with immediate spawn and one-shot join, executor seam, single-threaded `EventLoop` | Explicit operation/buffer ownership across layers, safe I/O cancellation and continued join/drain validation |
| Cancellation and deadlines | Scope cooperative stop token, timers and loop stop are available; yield is tracked through shutdown | Propagated I/O cancellation and operation deadlines; deterministic outcomes for completion/close/timeout races; neither a stop token nor `stop()` automatically cancels I/O |
| Transport and composition | TCP and completion-shaped kqueue/epoll/IOCP implementations; stream concepts | Equivalent observable semantics across backends, verified teardown, bounded queues; datagram contracts before UDP expansion |
| Protocols and data flow | HTTP/1.1 parser, serializer and connection loop; buffered request bodies | Protocol conformance evidence, streamed bodies, slow-consumer backpressure and bounded aggregate memory |
| Security and robustness | Optional OpenSSL TLS stream, parser limits and negative-input tests | Lifecycle-safe TLS cancellation, broader fuzzing, failure injection and resource-exhaustion tests |
| Engineering evidence | C++23-only build, desktop runtime CI and mobile cross-compilation jobs exist; last confirmed passing desktop baseline is `eddfddb` | Fresh validation of current scope/task/yield changes, mobile runtime evidence, reproducible interop/performance/resource measurements; no current stable ABI promise |

Rejecting ambiguous or malformed input is part of protocol correctness, not a
substitute for the other contracts. The HTTP parser rejects conflicting
`Content-Length`/`Transfer-Encoding`; parser limits do not establish bounded
memory for every queue, task tree or complete connection. Such bounds need to
be specified and measured end to end.

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
  submission/completion, and onto a reactor by trying the syscall, waiting for
  readiness on `EAGAIN`, and retrying. Cancellation, close and buffer lifetime
  still require explicit backend-specific handling.

So **the public API is completion-shaped on every platform**, and readiness is
an implementation detail of the POSIX backends. Future backends such as
io_uring must satisfy the same ownership, cancellation and completion contract;
matching the I/O shape alone does not establish substitutability.

```cpp
std::size_t n = (co_await loop.read(handle, buffer)).value();   // all platforms
```

Readiness is still exposed, but fenced: `wait_readable` / `wait_writable` exist
behind `#if CONTINUO_HAS_READINESS_API` for embedding a descriptor owned by
another library. Code that calls them does not compile on Windows — the honest
outcome, and better than an emulation whose semantics quietly differ.

## HTTP/1.1 as the first protocol exercise

HTTP/1.1 is a concrete way to exercise incremental parsing, short transfers,
connection reuse and bounded request handling over the transport core. It is
not the product boundary, and another library's HTTP feature list is not the
acceptance plan.

RFC 9110 supplies shared HTTP semantics; RFC 9112 defines HTTP/1.1 framing.
Future HTTP/2 or HTTP/3 work may reuse semantic types where appropriate, but
must define its own framing, multiplexing, flow-control and transport needs.
In particular, HTTP/1.1 connection/body framing is not a generic HTTP contract.
These protocols are possible extensions, not completed modules or release
commitments. TLS integration tests are useful composition evidence, not a
reason to postpone the core lifetime and backpressure work.

## Layering

```
protocol   HTTP/1.1              future protocol modules
                  │                    │
                  └────────────────────┘
                      core I/O concepts
                  ┌───────────┴───────────┐
adapter       optional TLS          direct transport
                  │                       │
transport        TCP             future datagram/local transports
                  └───────────┬───────────┘
core       EventLoop · Buffer · Task · TaskScope · Executor · errors
                              │
backend                kqueue · epoll · IOCP
```

This diagram describes runtime composition, not concrete header dependencies.
Protocols and TLS depend on core concepts; application composition connects
TLS to a transport. Stream protocols must not assume every future transport is
a byte stream: datagrams need their own message-boundary and truncation
contracts. A protocol only composes with a transport whose semantics it needs.

Four dependency invariants are checked by `tools/ci/check_layering.py`.
These static checks do not prove lifetime safety or runtime substitutability:

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

TCP and UDP belong to the transport layer, not a checklist of HTTP features.
TCP currently supplies stream connections. UDP remains unimplemented and needs
a distinct datagram contract rather than a stream-shaped wrapper. DNS and QUIC
are examples of protocols that may use datagrams; their requirements must not
be imposed on the TCP API or treated as existing functionality.

## Core seams

The executor and stream concepts live in `modules/core`. They establish useful
composition seams but do not yet express the full lifetime, cancellation,
thread-affinity and backpressure contracts below.

### `Executor` — who resumes a coroutine

```cpp
template <typename E>
concept Executor = requires(E& e, void (*work)()) {
    { e.post(work) } -> std::same_as<void>;
};
```

One function, because `post` is the smallest thing every scheduler already has.
The host chooses scheduling policy; this does not make loop-bound objects
thread-safe. `EventLoop` is single-threaded except for its documented `post()`
and `stop()` entry points. `co_await schedule_on(executor)` schedules a
continuation on the chosen executor; subsequent scheduling may move it again.
Executor/loop lifetime and permitted thread transitions must be explicit before
claiming arbitrary GUI-loop or thread-pool integration is safe.

### `AsyncStream` — what a protocol reads and writes

```cpp
template <typename S>
concept AsyncReadStream = requires(S& s, std::span<std::byte> d) {
    { s.read_some(d) } -> std::same_as<Task<Result<std::size_t>>>;
};
```

Short-transfer semantics match the underlying I/O. Protocol code depends on a
stream contract rather than a concrete socket type; TCP, TLS and in-memory
streams exercise this seam. Unix-domain sockets are a possible future
transport, not an implemented capability. Richer operations can be composed
on the minimal contract; the existing `write_all` helper is tested with a
non-socket `MemoryStream`.

An asynchronous byte stream is not the same as a streamed HTTP body. Request
bodies are currently buffered before handler delivery. End-to-end streaming
must additionally define buffer ownership, incremental consumption, early
termination and the way slow consumers suspend producers.

### `TaskScope` — implemented single-threaded child ownership

`TaskScope` in `continuo/core/task_scope.hpp` owns child tasks, not their borrowed
resources. It is neither copyable nor movable. Scope operations, child completion
and stop callbacks must all execute on the same thread; the type is not a
cross-thread scheduler or a complete server-launch facility.

- `spawn(Task<void>)` accepts an unstarted, nonempty task, takes ownership and
  starts it immediately. Empty input throws `std::invalid_argument`; spawn after
  join has been requested throws `std::logic_error`. Completed child frames are
  reclaimed promptly instead of accumulating until scope destruction.
- `join()` can be called only once. The call immediately closes spawn intake,
  even though the returned `Task<void>` is lazy and has not yet been awaited.
  Drive that task to completion: join waits for every child and its frame cleanup.
  A second join throws `std::logic_error`.
- The first observed child exception is retained and requests cooperative stop.
  Join rethrows it only after every child has finished and released its frame;
  siblings are not abandoned when the first child fails. `Result` failures are
  values, not exceptions: a `Task<void>` adapter must handle them explicitly,
  for example by throwing `std::system_error`, as in the README example.
- `get_stop_token()` / `request_stop()` expose a `std::stop_token` signal.
  Children may inspect it or register callbacks, but pending I/O is not
  automatically cancelled and there is no deadline propagation. Requesting stop
  does not close spawn intake or replace join. Callback reentrancy can complete
  the last child and resume the join continuation synchronously.
- An untouched empty scope (neither spawn nor join used) may be destroyed.
  Every other scope must finish joining before destruction, even if all children
  completed synchronously and `pending() == 0`. Join that rethrows after cleanup
  still satisfies this requirement. Early scope destruction terminates; so does
  destroying a join task while it is waiting. Discarding an unstarted join still
  leaves the scope unjoined and causes termination at scope destruction.
  This fail-fast policy prevents silent release of child frames still referenced
  by I/O; it is not automatic asynchronous cleanup in a destructor.

Keep borrowed streams, buffers, coroutine-lambda closures and other child state
alive through join; normally keep the associated loop alive longer as well.
Do not destroy the parent task while it awaits join. Prefer free-function
coroutines for examples so a temporary lambda cannot leave a dangling closure.
Do not use `sync_get()` for real asynchronous I/O or a join that may suspend.
Awaiting an empty or consumed `Task<T>` now throws `std::logic_error` rather than
accessing a missing frame.

`EventLoop::yield()` uses an already-due timer rather than disposable posted
work. It counts as outstanding work, resumes on the next loop pump and is also
resumed by loop shutdown, allowing a waiting scope join to finish. Its public
return type is still `Task<void>`: shutdown's internal cancellation result is
not returned to the caller. Resumption does not authorize more work on a
shut-down loop, and `stop()` itself remains a stop-pumping request, not I/O
cancellation.

### Required lifetime and resource contracts

These are broader design/acceptance requirements, beyond the scope foundation:

- **Structured concurrency:** child operations belong to an explicit scope.
  Callers must explicitly stop when needed and join/drain before leaving that
  scope or releasing borrowed resources. `TaskScope` provides owned spawn/join
  and fail-fast misuse detection, not implicit destructor-driven I/O cancellation;
  silently detached work is not a default.
- **Ownership:** distinguish owned sockets, operation state and coroutine
  frames from borrowed streams and spans. Specify the destruction order of
  task, stream and loop, and which objects must survive kernel completion.
- **Cancellation and deadlines:** propagate a caller's cancellation request and
  monotonic deadline through composed I/O, including TLS. Define the outcome
  of close/completion/cancellation/timeout races, exactly-once completion, and
  resource reclamation. A timer API or `stop()` alone does not provide this.
- **Backpressure:** bound outstanding operations, buffered bytes and work
  queues; define whether reaching each limit suspends or rejects a producer.
  Test slow peers and stalled consumers. A parser size limit or bounded TLS
  BIO alone is not an end-to-end resource bound.
- **Thread affinity:** identify each operation's owner executor and permitted
  handoff points. Scheduling elsewhere must not leave callbacks able to resume
  a destroyed task or access a loop-bound object from the wrong thread.

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
| **I/O model** | **completion-shaped public API** | A common operation contract for IOCP and reactor implementations |
| Platform scope | macOS, Linux and Windows runtime targets; iOS/Android targets currently cross-compiled | Runtime correctness must be demonstrated per platform; BSD has no dedicated CI evidence |
| Readiness API | POSIX-only, behind an explicit macro | A platform extension, not part of the portable operation contract |
| Execution model | Lazy `Task<T>` with explicit single-threaded `TaskScope` spawn / join | Owned child frames and cooperative stop are implemented; full I/O cancellation and deadlines remain acceptance work |
| Library form | Compiled library with modular public headers | Keep implementation boundaries explicit; an ABI policy must be decided before stabilization |
| Error model | `std::error_code` + `Result<T>` for operational failures | `Task` can still propagate body exceptions; this is not a no-exceptions guarantee |
| Standard floor | C++23-only build; `Result<T>` directly aliases `std::expected<T, Error>` | Validate each toolchain and standard library; no C++20 compatibility mandate |
| Buffer shape | Current `Buffer` is contiguous | Future segmented or borrowed-buffer designs need measured benefit and an explicit ownership contract |
| Framework coupling | No host-framework dependency or old Aria API compatibility promise | Consumers adapt to the independent library after its contracts are established |
| Protocol scope | HTTP/1.1 and optional TLS exercise the core; later protocols remain proposals | Stabilize lifecycle and resource semantics before broadening scope |

## Staged acceptance

Stages are gates, not completion percentages or release dates. Each gate needs
recorded evidence identifying the tested revision, toolchains, platforms and
configuration; the historical milestones below do not certify these gates.

1. **C++23 foundation and contracts.** Move the build baseline and selected
   standard-library facilities to C++23. Document ownership, thread affinity,
   error and short-transfer semantics. Compile independent minimal consumers
   without Aria or compatibility adapters.
2. **Lifecycle and structured execution.** Validate the implemented scope-owned
   tasks and explicit join; add I/O cancellation/deadline propagation and safe
   cross-layer drain. Test destruction and
   close with pending read/write/accept/connect/timer work, nested task failure,
   repeated cancellation and simultaneous completion. Require exactly-once
   completion and no dangling kernel buffers, resumptions or resource leaks
   across kqueue, epoll and IOCP, using applicable sanitizers and fault injection.
3. **Bounded composable data flow.** Add incremental body consumption and
   explicit producer/consumer limits. Verify short transfers, partial failures,
   early termination and slow peers over memory streams, TCP and TLS; measure
   peak memory and outstanding work against configured bounds. Define separate
   datagram semantics before introducing UDP-based modules.
4. **Cross-platform and protocol correctness.** Run protocol negative cases,
   fuzzing and interoperability checks; test normalized error/EOF/cancel/close
   outcomes on each desktop backend. Obtain mobile runtime evidence before
   promoting cross-compilation to runtime support. Keep TLS/mobile/BSD gaps
   explicit rather than borrowing another platform's results.
5. **Performance and API validation.** Publish reproducible benchmark inputs,
   hardware, compiler/options and limits. Measure latency distributions,
   throughput, CPU, allocations and peak memory under steady load, overload,
   connection churn and cancellation. Compare identical workloads with suitable
   baselines; do not infer performance from coroutine syntax or API shape.
   Exercise independent client/server examples and review API clarity before
   considering stabilization or additional protocol families.

Consumer migration and public release require separate approval after these
contracts and evidence are established. This plan promises neither old Aria
interfaces nor another library's feature parity.

## Implementation history, not readiness certification

The following records implementation milestones. Test counts are historical
snapshots, not current totals or evidence of complete protocol/lifecycle safety.

**v0.1 — seams.** Error model, `Task<T>`, `Buffer`, executor and stream
concepts, warning policy, layering discipline, CI across C++20/C++23 and
sanitizers.

**v0.2 — the loop.** `EventLoop` with three backends (kqueue, epoll, IOCP),
completion-shaped `read`/`write`, timers, cross-thread `post`, and a
`platform.hpp` that is the single home for platform detection. CI now builds
and *runs* tests on all three backends, and cross-compiles for iOS and Android.

The historical local kqueue run recorded 118 checks across C++20, C++23 and
ASan/UBSan. Desktop epoll/IOCP runtime coverage comes from CI, not that local
run. Neither the check count nor the existence of a CI job establishes current
revision health or full backend lifecycle correctness.

**v0.3 — the parser.** Incremental, strict HTTP/1.1 request parsing:
`message.hpp` (HTTP message types, subject to review for future protocol reuse),
`limits.hpp` (parser bounds), and a parser whose test suite is mostly published
smuggling vectors. 119 checks, and the byte-at-a-time tests assert that network
slicing cannot change the parse.

**v0.4 — TCP.** `Endpoint`, `Listener`, `Socket`, `connect`, and an explicit
exclusive-bind policy. `ListenOptions::exclusive` expresses the intent to
reject a second bind to an occupied endpoint; each backend must implement and
test that intent using its platform's socket options. `SO_REUSEADDR` has
platform-specific semantics, so copying the option name is not a portable
contract. Existing loopback tests are evidence for their tested configurations,
not a proof covering every platform, address family and socket option mix.

This milestone first exercised the Windows socket path in these tests. The
event-loop tests used `socketpair()`, which Winsock lacks, so every socket case
had been skipped there and IOCP's read/write path had never run. Loopback TCP
runs everywhere, and it immediately found two bugs unreachable from a macOS
machine — see "What CI found" above.

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

**Structured task foundation.** `TaskScope` adds immediate owned spawn, one-shot
join, prompt child-frame reclamation, cooperative stop and first-exception
propagation after all children finish. Empty-task await is checked; tracked
`yield` resumes during loop shutdown. These changes are newer than the last
confirmed passing desktop CI baseline `eddfddb` and await fresh validation.

The current test registration has eight regular suites with TLS (`core`,
`event_loop`, `task_scope`, `transport`, `http_parser`, `http_server`,
`http_end_to_end`, `tls_https`), or seven without TLS. Four additional CTests
exercise fail-fast violations: `task_scope_pending-destruction`,
`task_scope_unobserved-failure`, `task_scope_abandoned-join` and
`task_scope_unstarted-join`. Totals are **12 CTests with TLS and 11 without**;
registration counts do not assert that this revision has passed them.

**Known lifecycle limits.** Pending tasks must not be destroyed while the event
loop/kernel still holds their handles or buffers. Scope misuse is fail-fast, not
implicit I/O cancellation or universal protection for arbitrary standalone
`Task` destruction. Full cancellation, deadlines and continued cross-backend
teardown validation remain work; this foundation is not production-readiness
or a complete cancellation-safety claim.

**Deliberately absent.** UDP, routing, HTTP/2, a full HTTP client, native OS trust
store integration, mTLS policy, I/O-integrated cancellation/deadline propagation,
and multi-threaded loops.
