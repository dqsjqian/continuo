#pragma once

// continuo/http/connection.hpp — serving requests over one connection.
//
// Generic over the stream type on purpose. `serve_connection` takes anything
// satisfying `continuo::AsyncStream`, so the same code serves a TCP socket, a
// TLS session once that lands, or an in-memory pipe in tests. This module
// links against `continuo::core` only — it has never seen a socket, and the
// layering check keeps it that way.
//
// Three correctness rules are enforced here rather than left to handlers,
// because getting any of them wrong corrupts the *next* request on the
// connection rather than the current one, which makes the bug look unrelated
// to its cause:
//
//   1. **The request body is always drained.** A handler that ignores the body
//      leaves those bytes in the stream, where they get parsed as the start of
//      the following request. This is the classic keep-alive desync.
//   2. **Exactly one response per request, with exactly one framing.** The
//      serializer owns Content-Length / Transfer-Encoding.
//   3. **A HEAD response carries headers but no body**, including its
//      Content-Length, which must describe what a GET *would* have returned.
//
// Usage:
//
//     auto handler = [](const Request& request, auto& writer) -> Task<Result<void>> {
//         Response response;
//         response.status = 200;
//         co_return co_await writer.send(response, body_bytes);
//     };
//     co_await serve_connection(socket, handler);

#include "continuo/core/buffer.hpp"
#include "continuo/core/error.hpp"
#include "continuo/core/stream.hpp"
#include "continuo/core/task.hpp"
#include "continuo/http/limits.hpp"
#include "continuo/http/message.hpp"
#include "continuo/http/parser.hpp"
#include "continuo/http/serializer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace continuo::http {

/// Per-connection policy.
struct ServerOptions {
    /// Parser bounds; see `limits.hpp`. Closed by default.
    Limits limits{};

    /// 外部取消透传到每次读取、响应写入及错误响应。
    std::stop_token stop{};

    /// Maximum requests served on one connection before closing it.
    ///
    /// Bounded because an unbounded keep-alive connection is a resource a
    /// single client can hold forever.
    std::uint32_t max_requests_per_connection = 100;

    /// Bytes read from the stream per syscall.
    std::size_t read_chunk = 8 * 1024;

    /// Send `Connection: close` and hang up after the current response.
    bool force_close = false;

    /// How long an established connection may sit between requests.
    ///
    /// Durations rather than a deadline, because a deadline cannot express what
    /// a server actually wants: one absolute time would cover the whole
    /// connection, so the hundredth keep-alive request would inherit whatever
    /// budget the first one left. `serve_connection` turns these into a fresh
    /// absolute deadline for each request, which is the form every layer below
    /// composes without arithmetic.
    ///
    /// Expiry here is **not** an error. A keep-alive connection going quiet and
    /// being closed is how it normally ends, so this returns success — the same
    /// as the peer having closed it politely.
    ///
    /// Zero disables it. A server exposed to the internet should not leave it
    /// that way: an idle connection costs a descriptor and a buffer, and a
    /// client can open many.
    Clock::duration idle_timeout = Clock::duration::zero();

    /// How long one request may take, from its first byte to its last
    /// response byte.
    ///
    /// Covers the handler as well as the I/O, because the point is to bound the
    /// exchange rather than to bound one syscall. Expiry **is** an error
    /// (`Errc::timed_out`) and ends the connection.
    ///
    /// No 408 is sent. Writing a response needs the stream, and the deadline
    /// that just expired is the same one the write would carry, so the attempt
    /// would fail immediately — announcing the timeout would take a second,
    /// separate budget that the caller never granted.
    ///
    /// Zero disables it, which leaves a slow peer bounded only by `limits` —
    /// a bound on one message's size, not on the time it may take to arrive.
    Clock::duration request_timeout = Clock::duration::zero();
};

/// Writes one response, and refuses to write two.
///
/// Templated on the stream rather than type-erased so that a handler's writes
/// go straight to the socket with no virtual dispatch and no allocation.
template<BoundedStream Stream>
class ResponseWriter {
public:
    ResponseWriter(Stream& stream,
                   bool head_request,
                   bool keep_alive,
                   OperationOptions io = {}) noexcept
        : stream_(&stream), io_(std::move(io)), head_request_(head_request),
          keep_alive_(keep_alive) {}

    ResponseWriter(const ResponseWriter&) = delete;
    ResponseWriter& operator=(const ResponseWriter&) = delete;

    /// Send a complete response with a known body. The common case.
    [[nodiscard]] Task<Result<void>> send(const Response& response,
                                          std::span<const std::byte> body = {}) {
        if (sent_head_) {
            co_return fail(SerializeError::framing_conflict);
        }

        Buffer out;
        const Framing framing =
            status_forbids_body(response.status) ? Framing::none : Framing::content_length;

        // Content-Length always describes the real body, even for HEAD, where
        // the bytes themselves are withheld: a HEAD response must announce
        // what a GET would have returned.
        Result<void> head = write_response_head(out, response, framing, body.size());
        if (!head) {
            co_return fail(head.error());
        }
        sent_head_ = true;

        if (!head_request_ && !body.empty() && !status_forbids_body(response.status)) {
            out.append(body);
        }

        finished_ = true;
        co_return co_await write_all(*stream_, out.readable(), io_);
    }

    /// Begin a streaming response whose size is not yet known.
    ///
    /// Bodies then go out via `write` and are terminated by `finish`.
    [[nodiscard]] Task<Result<void>> send_head_chunked(const Response& response) {
        if (sent_head_) {
            co_return fail(SerializeError::framing_conflict);
        }

        Buffer out;
        Result<void> head = write_response_head(out, response, Framing::chunked);
        if (!head) {
            co_return fail(head.error());
        }
        sent_head_ = true;
        chunked_ = !status_forbids_body(response.status);

        co_return co_await write_all(*stream_, out.readable(), io_);
    }

    /// Send one piece of a streaming body.
    [[nodiscard]] Task<Result<void>> write(std::span<const std::byte> piece) {
        if (!sent_head_ || finished_) {
            co_return fail(SerializeError::framing_conflict);
        }
        if (piece.empty()) {
            co_return Result<void>{};  // nothing to frame
        }
        // A HEAD response must not carry body bytes, but the handler should not
        // have to branch on it — swallow them here.
        if (head_request_ || !chunked_) {
            co_return Result<void>{};
        }

        Buffer out;
        Result<void> framed = write_chunk(out, piece);
        if (!framed) {
            co_return fail(framed.error());
        }
        co_return co_await write_all(*stream_, out.readable(), io_);
    }

    /// Terminate a streaming body.
    [[nodiscard]] Task<Result<void>> finish() {
        if (!sent_head_ || finished_) {
            co_return Result<void>{};
        }
        finished_ = true;
        if (head_request_ || !chunked_) {
            co_return Result<void>{};
        }

        Buffer out;
        write_last_chunk(out);
        co_return co_await write_all(*stream_, out.readable(), io_);
    }

    /// True once a head has gone out — the connection loop uses this to avoid
    /// sending an error response on top of a partial one.
    [[nodiscard]] bool sent_head() const noexcept { return sent_head_; }

    /// Whether the connection is expected to stay open after this response.
    [[nodiscard]] bool keep_alive() const noexcept { return keep_alive_; }

private:
    Stream* stream_;
    /// This request's budget, applied to every write the handler causes.
    OperationOptions io_{};
    bool head_request_{false};
    bool keep_alive_{true};
    bool sent_head_{false};
    bool chunked_{false};
    bool finished_{false};
};

namespace detail {

/// Send a minimal error response, used when a request cannot be understood.
///
/// Deliberately bare: a parse failure means the request is untrustworthy, so
/// the reply says as little as possible and the connection closes.
template<BoundedStream Stream>
Task<Result<void>> send_error(Stream& stream, unsigned status, OperationOptions io) {
    Response response;
    response.version = Version::http_1_1;
    response.status = status;
    response.headers.append("Connection", "close");

    Buffer out;
    Result<void> head = write_response_head(out, response, Framing::content_length, 0);
    if (!head) {
        co_return fail(head.error());
    }
    // Carries the request's own deadline: announcing a rejection must not
    // outlive the exchange it is rejecting.
    co_return co_await write_all(stream, out.readable(), std::move(io));
}

}  // namespace detail

/// Serve requests on `stream` until the connection ends.
///
/// Returns when the peer closes, a limit is reached, or the exchange decides to
/// close. A protocol error is answered with a 4xx where possible and then ends
/// the connection — continuing to parse a stream whose framing is already in
/// doubt is how one bad request becomes several.
template<BoundedStream Stream, typename Handler>
Task<Result<void>> serve_connection(Stream& stream, Handler handler, ServerOptions options = {}) {
    if (options.read_chunk == 0) {
        co_return fail(Errc::invalid_argument);
    }
    Buffer input;
    RequestParser parser{options.limits};

    // Turn a configured window into an absolute deadline, or nothing when the
    // window is disabled. Every layer below composes absolute deadlines
    // without arithmetic, which is why the conversion happens exactly here and
    // exactly once per window.
    const auto deadline_in = [](Clock::duration window) -> std::optional<Clock::time_point> {
        if (window == Clock::duration::zero()) {
            return std::nullopt;
        }
        return Clock::now() + window;
    };

    for (std::uint32_t served = 0; served < options.max_requests_per_connection; ++served) {
        parser.reset();

        bool head_ready = false;
        // Bytes left over from a pipelined request mean this one has already
        // begun, so it is on the request budget rather than the idle one.
        bool request_started = !input.empty();
        bool body_drained = false;
        Buffer body;  // accumulated only up to the configured limit

        // Whichever window applies right now. Recomputed when the first byte
        // arrives, so that a connection's hundredth request gets the same
        // budget as its first.
        OperationOptions io{.stop = options.stop,
                            .deadline = deadline_in(request_started ? options.request_timeout
                                                                    : options.idle_timeout)};

        // ── read and parse one request ──────────────────────────────────────
        while (!head_ready || !body_drained) {
            const Result<ParseStep> step = parser.parse(input);
            if (!step) {
                // The request is malformed or over budget. Answer once with
                // the status that matches — 413 for sizes, 400 for grammar —
                // then stop: the stream position is no longer trustworthy.
                const unsigned status = step.error() == Errc::limit_exceeded ? 413u : 400u;
                static_cast<void>(co_await detail::send_error(stream, status, io));
                co_return fail(step.error());
            }

            switch (*step) {
            case ParseStep::head:
                head_ready = true;
                if (parser.request().body_kind == BodyKind::none) {
                    body_drained = true;
                }
                break;

            case ParseStep::body:
                body.append(parser.body());
                break;

            case ParseStep::complete:
                body_drained = true;
                break;

            case ParseStep::need_more:
                if (parser.done()) {
                    body_drained = true;
                    break;
                }
                {
                    const std::span<std::byte> space = input.prepare(options.read_chunk);
                    Result<std::size_t> read = co_await stream.read_some(space, io);
                    if (!read) {
                        input.commit(0);
                        // Between requests, both a clean close and an idle
                        // timeout are how a keep-alive connection normally
                        // ends — neither is a failure to report. Mid-request,
                        // the same events are a truncated message and a peer
                        // that ran out of time, and both do fail.
                        if (!request_started &&
                            (read.error() == Errc::eof || read.error() == Errc::timed_out)) {
                            co_return Result<void>{};
                        }
                        co_return fail(read.error());
                    }
                    input.commit(*read);
                    if (*read == 0) {
                        // A non-empty read making no progress must not spin.
                        co_return fail(Errc::eof);
                    }
                    if (!request_started) {
                        request_started = true;
                        // The request's own budget starts at its first byte,
                        // not at whenever the connection happened to open.
                        io.deadline = deadline_in(options.request_timeout);
                    }
                }
                break;
            }
        }

        // ── run the handler ────────────────────────────────────────────────
        const Request& request = parser.request();
        const bool head_request = request.method == Method::head;
        const bool keep_alive = should_keep_alive(request) && !options.force_close &&
                                served + 1 < options.max_requests_per_connection;

        // The handler shares the request budget: a deadline that covered the
        // reading but not the responding would bound half an exchange.
        ResponseWriter<Stream> writer{stream, head_request, keep_alive, io};

        Result<void> handled = co_await handler(request, writer, body.readable());
        if (!handled) {
            // The handler failed before writing anything: a 500 is still
            // possible. If it already sent a head, the only honest option is
            // to close, because the response is half-written.
            if (!writer.sent_head()) {
                static_cast<void>(co_await detail::send_error(stream, 500, io));
            }
            co_return fail(handled.error());
        }

        // A handler that streamed but never terminated its body would leave the
        // connection mid-message; close it out on its behalf.
        Result<void> finished = co_await writer.finish();
        if (!finished) {
            co_return fail(finished.error());
        }

        if (!keep_alive) {
            co_return Result<void>{};
        }
    }

    co_return Result<void>{};
}

}  // namespace continuo::http
