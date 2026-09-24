# modules/transport — TCP

`Socket` models `continuo::AsyncStream`, so a protocol written against the
concept accepts one without naming it. Implemented here:

- `Endpoint` — numeric IPv4 / IPv6 addresses. No name resolution: DNS is a
  protocol, and it belongs in its own module rather than inside the address
  type every transport depends on.
- `Listener` — `bind` with explicit `ListenOptions`, and `accept`.
- `Socket` — `read_some` / `write_some` with short-transfer semantics,
  `shutdown_send`, and `close`.
- `connect` — an outgoing connection with `ConnectOptions`.

This layer may include `continuo/core/…` and nothing above it; the layering
check in `tools/ci/check_layering.py` fails the build otherwise.

## Why `ListenOptions::exclusive` exists

This option is where the disagreement that started the project gets an explicit
home. `SO_REUSEADDR` means two different things: on POSIX it permits rebinding
a port left in `TIME_WAIT`, while on Windows the same constant lets a second
process **steal** a port another process is actively bound to — so two servers
both "successfully" listen on one port and split the incoming connections
between them.

Continuo therefore does not expose `SO_REUSEADDR` as a portable flag. It
exposes the *intent*, and each platform implements that intent with whatever
combination of socket options actually produces it. `tcp.hpp` carries the full
reasoning; this is a summary, not the specification.

## Not here, and why

**UDP and Unix-domain sockets.** A datagram is not a byte stream, so it needs
its own contract rather than being forced through `AsyncStream` — see the
transport section of `docs/ARCHITECTURE.md`. Writing those before that contract
exists is how a library ends up with a stream abstraction that quietly lies
about one of its transports.

**Per-operation cancellation and deadlines.** `OperationOptions` currently
stops at `continuo::EventLoop`; nothing in this layer forwards it yet.
