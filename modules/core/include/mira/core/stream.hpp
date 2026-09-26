#pragma once

// Mira/core/stream.hpp — the seam between transport and protocol.
//
// This header is the reason Mira grow protocols without growing its
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

#include "Mira/core/error.hpp"
#include "Mira/core/operation.hpp"
#include "Mira/core/task.hpp"

#include <concepts>
#include <cstddef>
#include <span>
#include <utility>

namespace Mira {

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

// ── streams that can be cut short ───────────────────────────────────────────
//
// Cancellation cannot be composed onto a stream from the outside: only the
// layer that actually waits can stop waiting. So a stream either accepts
// `OperationOptions` or it cannot honour them, and pretending otherwise would
// mean a deadline that silently does nothing.
//
// These refine the concepts above rather than replacing them. An operation
// parameter with a default keeps the one-argument call valid, so every
// `BoundedStream` is also an `AsyncStream` and nothing that was written against
// the thin contract has to change. A stream that does not accept options stays
// perfectly usable — it just cannot be handed a deadline, which is the honest
// outcome rather than a quiet one.

/// A readable stream whose reads accept cancellation and a deadline.
template<typename S>
concept BoundedReadStream =
    AsyncReadStream<S> && requires(S& stream, std::span<std::byte> destination,
                                   OperationOptions options) {
        { stream.read_some(destination, options) } -> std::same_as<Task<Result<std::size_t>>>;
    };

/// A writable stream whose writes accept cancellation and a deadline.
template<typename S>
concept BoundedWriteStream =
    AsyncWriteStream<S> && requires(S& stream, std::span<const std::byte> source,
                                    OperationOptions options) {
        { stream.write_some(source, options) } -> std::same_as<Task<Result<std::size_t>>>;
    };

/// A bidirectional stream that can be cut short in both directions.
template<typename S>
concept BoundedStream = BoundedReadStream<S> && BoundedWriteStream<S>;

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

/// Write the whole buffer, applying `options` to **every** write.
///
/// The same absolute deadline on each write is what a deadline on the whole
/// transfer means — no layer has to subtract elapsed time, which is the
/// property that made absolute rather than relative the right choice.
///
/// `options` has no default on purpose. A defaulted third parameter would make
/// `write_all(stream, source)` ambiguous for a `BoundedWriteStream`, and the
/// loop is duplicated rather than factored for the same reason: the two forms
/// differ only in which `write_some` they call, and every way of sharing that
/// either reintroduces the ambiguity or hides the choice behind
/// `if constexpr`, which is how a deadline ends up silently ignored.
template<BoundedWriteStream S>
Task<Result<void>>
write_all(S& stream, std::span<const std::byte> source, OperationOptions options) {
    std::size_t written = 0;
    while (written < source.size()) {
        Result<std::size_t> chunk = co_await stream.write_some(source.subspan(written), options);
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

}  // namespace Mira
