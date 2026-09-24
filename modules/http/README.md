# modules/http — HTTP/1.1

Written entirely against the stream concepts in `modules/core`. This module
never names a socket type, which is what keeps one parser usable over TCP, over
TLS, and over an in-memory buffer in tests. The layering check enforces it.

Implemented here:

- `RequestParser` — incremental, with the framing rules in `decide_framing`.
- `write_response_head` / `write_chunk` / `write_last_chunk` — serialisation
  that owns framing, and refuses a caller-supplied `Content-Length` or
  `Transfer-Encoding` rather than silently dropping it.
- `ResponseWriter` and `serve_connection` — one connection's request loop,
  keep-alive and pipelining included.
- `Limits` and `ServerOptions` — the limit policy, closed by default.
- `grammar.hpp` — the RFC 9110 character classes and list splitting, shared by
  the parser and the serialiser. One definition on purpose: two copies of a
  `tchar` table is the same disagreement this module exists to prevent, except
  inside a single binary.

## Rules this module actually follows

- **Reject, don't guess.** A `Content-Length` / `Transfer-Encoding` conflict is
  an error, not a heuristic. So is whitespace before a colon, obsolete line
  folding, a bare LF where CRLF is required, a CR anywhere else, a non-`tchar`
  field name, and `chunked` appearing anywhere but last or more than once. Each
  of those is a place where two implementations could disagree about where a
  message ends, which is what request smuggling is made of.
- **Limits closed by default.** Header count, header and start-line size, body
  size, chunk size and chunk-extension size all have conservative defaults that
  configuration opens, never the reverse.

## Not here, and why

**Streaming request bodies.** A handler currently receives a body that has
already been collected, within the configured limit. This is the most visible
gap in the module, and it is a gap rather than a design choice: `Limits` bounds
one message, which is not the same as bounding a connection or a process.

**A client, routing, and static files.** Deliberately absent until the lifecycle
and resource contracts are settled. Breadth before contracts is how a protocol
library becomes hard to fix.

**Conformance evidence.** Interoperability runs against other implementations,
and parser fuzzing in CI, are acceptance work rather than something this module
can claim. Negative tests over crafted input are not the same as either.

**HTTP/2 and HTTP/3.** Separate slots when they come, and they will talk to
`core` only. Sibling protocol modules never include each other.

**A time bound that is on by default.** `ServerOptions::idle_timeout` and
`request_timeout` exist and are forwarded, but both default to zero, which is
off. A server exposed to the internet should set them; leaving them at the
default bounds a slow peer by message size only.
