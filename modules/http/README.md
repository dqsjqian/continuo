# modules/http — reserved slot

HTTP/1.1: message parsing, serialisation, `Server`, `Client`, and the limit
policy that governs them.

Empty in v0.1 on purpose. This module is written entirely against the stream
concepts in `modules/core` — it will never name a socket type, which is what
keeps the same parser usable over TCP, TLS, and an in-memory pipe in tests.

Non-negotiables for the first commit here, per `docs/ARCHITECTURE.md`:

- **Reject, don't guess.** A `Content-Length` / `Transfer-Encoding` conflict is
  an error, not a heuristic. Malformed chunked framing closes the connection.
- **Limits closed by default.** Header count, header size, URI length, and body
  size have conservative defaults that configuration opens, never the reverse.
- **Streaming bodies from day one.** A body is a stream, not a `std::string`
  that happens to be large.
- **Conformance as a test target.** Interop runs against curl and nginx, plus
  fuzzing on the parser in CI — before any feature breadth.

Sibling protocol modules (`ws`, `h2`, `h3`, `dns`) are separate slots. They
must not include each other: each talks to `core` only.
