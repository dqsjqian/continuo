#pragma once

// continuo/core/stream.hpp — the seam between transport and protocol.
//
// This header is the reason Continuo can grow protocols without growing its
// core. A protocol module never names a socket type: it is written against
// `AsyncReadStream` / `AsyncWriteStream`, so the same parser runs over TCP, a
// Unix socket, a TLS session, or an in-memory pipe used by tests.
//
// The contract is intentionally thin — `read_some` / `write_some` with
// short-transfer semantics, exactly like the syscalls underneath:
//
//   * `read_some` resolves with the number of bytes read; 0 bytes means the
//     peer closed the stream, reported as `Errc::eof`.
//   * `write_some` resolves with the number of bytes accepted, which may be
//     fewer than offered. Full-write loops are the caller's business.
//
// Anything richer (read_exactly, read_until, write_all) is a free function
// composed on top, never a new requirement on implementers.

#include "continuo/core/error.hpp"
#include "continuo/core/task.hpp"

#include <concepts>
#include <cstddef>
#include <span>

namespace continuo {

/// A stream bytes can be read from.
template<typename S>
concept AsyncReadStream = requires(S& stream, std::span<std::byte> destination) {
    { stream.read_some(destination) } -> std::same_as<Task<Result<std::size_t>>>;
};

/// A stream bytes can be written to.
template<typename S>
concept AsyncWriteStream = requires(S& stream, std::span<const std::byte> source) {
    { stream.write_some(source) } -> std::same_as<Task<Result<std::size_t>>>;
};

/// A bidirectional stream — what a protocol server is handed per connection.
template<typename S>
concept AsyncStream = AsyncReadStream<S> && AsyncWriteStream<S>;

/// A stream that can be shut down in one direction and closed.
template<typename S>
concept ClosableStream = requires(S& stream) {
    { stream.close() } -> std::same_as<void>;
};

/// Write the whole buffer, looping over short writes.
///
/// Composed on the minimal contract so that no implementer has to provide it.
template<AsyncWriteStream S>
Task<Result<void>> write_all(S& stream, std::span<const std::byte> source) {
    std::size_t written = 0;
    while (written < source.size()) {
        Result<std::size_t> chunk = co_await stream.write_some(source.subspan(written));
        if (!chunk) {
            co_return fail(chunk.error());
        }
        const std::size_t n = *chunk;
        if (n == 0) {
            co_return fail(Errc::eof);
        }
        written += n;
    }
    co_return Result<void>{};
}

}  // namespace continuo
