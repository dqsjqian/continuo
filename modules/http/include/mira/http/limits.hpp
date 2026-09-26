#pragma once

// Mira/http/limits.hpp — the bounds a parser refuses to exceed.
//
// Every field here is closed by default and opened by configuration, never
// the reverse. An unbounded parser is a denial-of-service primitive: a peer
// that sends an endless header line costs the server memory until it dies, and
// nothing in the protocol stops it.
//
// The defaults are deliberately close to what mainstream servers enforce
// (nginx's 8k header buffer, Apache's 8190-byte line limit), so that a service
// running with defaults is conservative but not incompatible.

#include <cstddef>
#include <cstdint>

namespace Mira::http {

/// Parser bounds. All sizes are in bytes.
struct Limits {
    /// Longest request line (`GET /... HTTP/1.1`) or status line.
    std::size_t max_start_line = 8 * 1024;

    /// Longest single header field line, including name and value.
    std::size_t max_header_line = 8 * 1024;

    /// Maximum number of header fields in one message.
    std::size_t max_header_count = 100;

    /// Maximum total bytes across all header fields.
    std::size_t max_headers_total = 64 * 1024;

    /// Maximum body size the parser will emit for one message.
    ///
    /// Conservative on purpose: a service that accepts large uploads should
    /// say so explicitly. Set to `SIZE_MAX` for unbounded.
    std::uint64_t max_body_size = 1024 * 1024;

    /// Maximum size of a single chunk in a chunked body.
    std::uint64_t max_chunk_size = 1024 * 1024;

    /// Maximum number of chunk-extension bytes tolerated per chunk header.
    std::size_t max_chunk_extension = 256;

    /// Accept a lone LF as a line terminator instead of requiring CRLF.
    ///
    /// Off by default. Parsers that disagree about line endings are a classic
    /// request-smuggling differential: if a front-end sees CRLF where a
    /// back-end sees LF, the two disagree about where a message ends.
    bool allow_bare_lf = false;

    /// Permit obsolete line folding (a header value continued on a line
    /// starting with SP or HTAB).
    ///
    /// Off by default: RFC 9112 §5.2 tells recipients to reject it, and it is
    /// another smuggling differential.
    bool allow_obsolete_line_folding = false;
};

}  // namespace Mira::http
