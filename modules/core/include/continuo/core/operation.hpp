#pragma once

// continuo/core/operation.hpp — per-operation cancellation and deadlines.
//
// Every asynchronous `EventLoop` entry point takes one of these by value:
//
//     co_await loop.read(handle, buffer, {.deadline = Clock::now() + 5s});
//     co_await loop.read(handle, buffer, {.stop = scope.get_stop_token()});
//
// Cancellation belongs here rather than on `Task`, because a task does not
// know which operation it is suspended on — only the loop does. Destroying a
// task to "cancel" it is a contract violation that terminates; see `task.hpp`.

#include <chrono>
#include <optional>
#include <stop_token>

namespace continuo {

/// The clock every deadline in Continuo is expressed against.
///
/// Steady rather than system: a deadline must not move because something
/// adjusted the wall clock, and a timeout that can be lengthened by an NTP
/// correction is not a bound.
using Clock = std::chrono::steady_clock;

/// What may cut an operation short, besides the operation finishing.
///
/// A plain aggregate so that callers can name only what they need. **The
/// declaration order is part of the source contract**: designated initialisers
/// must be written in declaration order, so reordering these members would
/// break every `{.stop = ..., .deadline = ...}` call site at compile time.
///
/// Passed and stored **by value**. A reference parameter would dangle: a
/// reference bound to a `{...}` temporary dies at the end of the full
/// expression that creates the coroutine, and the coroutine first resumes
/// after that. Copying also means the stop state outlives whatever owned the
/// `std::stop_source` — a `TaskScope` may be destroyed while an operation it
/// started is still winding down, and the token stays valid.
struct OperationOptions {
    /// Cooperative cancellation. May be requested from any thread; the
    /// cancellation is *delivered* on the loop thread, so the coroutine is
    /// always resumed there.
    std::stop_token stop{};

    /// Absolute point in time after which the operation reports
    /// `Errc::timed_out`.
    ///
    /// Absolute, not a duration, on purpose. Two reasons, and the second is
    /// the one that matters more:
    ///
    ///   * An operation that retries internally (`EAGAIN`, `EINTR`, a partial
    ///     readiness wakeup) must not refresh its budget on every retry, or a
    ///     slow peer could hold it open indefinitely while every individual
    ///     wait stayed under the limit.
    ///   * It **composes**. One absolute deadline handed down through TLS to a
    ///     socket means "this whole handshake must finish by then" without any
    ///     layer subtracting elapsed time. A duration would require every
    ///     layer to do that arithmetic, and each one would get it slightly
    ///     wrong.
    std::optional<Clock::time_point> deadline{};
};

}  // namespace continuo
