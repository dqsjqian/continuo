#pragma once

// continuo/core/event_loop.hpp — asynchronous I/O, timers, and coroutine
// resumption, on every supported platform.
//
// The public API is **completion-shaped**:
//
//     std::size_t n = (co_await loop.read(handle, buffer)).value();
//
// "tell me when this read has finished", not "tell me when this handle is
// readable". That choice is what makes one API work across kqueue, epoll, and
// IOCP — see `platform.hpp` for why readiness-shaped APIs cannot be ported to
// IOCP without emulating it badly.
//
// Threading contract:
//   * `post()` and `stop()` are safe from any thread.
//   * everything else must be called on the thread running the loop.
//
// Resumption always happens outside the loop's internal lock, so a resumed
// coroutine may freely submit more I/O, post work, or stop the loop.

#include "continuo/core/error.hpp"
#include "continuo/core/platform.hpp"
#include "continuo/core/task.hpp"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>

namespace continuo {

/// Single-threaded I/O loop: completions, timers, and posted work.
class EventLoop {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;

    /// Create a loop, or report why the platform refused.
    [[nodiscard]] static Result<EventLoop> create();

    EventLoop(EventLoop&&) noexcept;
    EventLoop& operator=(EventLoop&&) noexcept;
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    ~EventLoop();

    // ── portable I/O (completion-shaped) ────────────────────────────────────

    /// Register `handle` with the loop before performing I/O on it.
    ///
    /// A no-op on reactor backends; on IOCP it associates the handle with the
    /// completion port, which must happen exactly once per handle. Transports
    /// call this when they create a socket, so protocol code never does.
    [[nodiscard]] Result<void> attach(NativeHandle handle);

    /// Stop tracking `handle`. Call before closing it.
    void detach(NativeHandle handle);

    /// Read once into `destination`, resolving with the byte count.
    ///
    /// Short reads are normal and are not an error. A clean peer close is
    /// reported as `Errc::eof` rather than a zero-length success, so that "no
    /// more data" cannot be mistaken for "nothing right now".
    [[nodiscard]] Task<Result<std::size_t>> read(NativeHandle handle,
                                                 std::span<std::byte> destination);

    /// Write once from `source`, resolving with the byte count accepted.
    ///
    /// Short writes are normal; looping is the caller's business (see
    /// `write_all` in `stream.hpp`).
    [[nodiscard]] Task<Result<std::size_t>> write(NativeHandle handle,
                                                  std::span<const std::byte> source);

    /// Accept one connection from a listening handle.
    ///
    /// The returned handle is already attached to the loop, because on IOCP an
    /// accepted socket is useless until it is associated with the completion
    /// port — leaving that to the caller would be a portability trap that only
    /// fires on one platform.
    ///
    /// `address_family` is the platform's own constant (`AF_INET`, `AF_INET6`,
    /// ...) passed straight through. The loop does not interpret it; IOCP
    /// simply needs to pre-create a socket of the right family before it can
    /// submit an accept.
    [[nodiscard]] Task<Result<NativeHandle>> accept(NativeHandle listener, int address_family);

    /// Connect `handle` to an already-encoded socket address.
    ///
    /// `address` is an opaque `sockaddr` blob: the loop memcpy's it into the
    /// syscall and never looks inside. That is what keeps `core` free of
    /// address-family knowledge while still being the only layer that talks to
    /// the OS — building the blob is the transport's job.
    [[nodiscard]] Task<Result<void>> connect(NativeHandle handle,
                                             std::span<const std::byte> address);

    // ── timers and scheduling (portable) ────────────────────────────────────

    /// Suspend for at least `delay`.
    [[nodiscard]] Task<Result<void>> sleep_for(Duration delay);

    /// Suspend until `deadline`.
    [[nodiscard]] Task<Result<void>> sleep_until(Clock::time_point deadline);

    /// Reschedule the caller onto the loop thread without waiting for I/O.
    [[nodiscard]] Task<void> yield();

    /// Queue `work` to run on the loop thread. Safe from any thread.
    void post(std::function<void()> work);

    // ── driving ─────────────────────────────────────────────────────────────

    /// Run until `stop()` is called or nothing is outstanding.
    Result<void> run();

    /// Run a single iteration: wait for completions (up to `timeout`), then
    /// dispatch expired timers, finished operations, and posted work.
    ///
    /// A default timeout blocks until something happens. Exposed because a
    /// host with its own main loop needs to interleave, and because tests
    /// should not depend on wall-clock races.
    Result<void> run_once(Duration timeout = Duration::min());

    /// Ask the loop to return from `run()`. Safe from any thread.
    void stop();

    /// True once `stop()` has been observed.
    [[nodiscard]] bool stopped() const noexcept;

    /// Suspended operations, pending timers, and queued callables. Zero means
    /// the loop has nothing left to do.
    [[nodiscard]] std::size_t outstanding() const noexcept;

    // ── readiness extension (POSIX only, NOT portable) ──────────────────────
    //
    // Deliberately fenced off. These exist to embed a descriptor owned by some
    // other library — a database client, a device fd — into the same loop.
    // They have no IOCP equivalent: code that calls them does not build on
    // Windows, which is the honest outcome, and better than an emulation whose
    // semantics quietly differ.

#if CONTINUO_HAS_READINESS_API
    /// Suspend until `handle` is readable. POSIX only.
    [[nodiscard]] Task<Result<void>> wait_readable(NativeHandle handle);

    /// Suspend until `handle` is writable. POSIX only.
    [[nodiscard]] Task<Result<void>> wait_writable(NativeHandle handle);
#endif

private:
    class Impl;

    explicit EventLoop(std::unique_ptr<Impl> impl) noexcept;

    [[nodiscard]] Task<Result<void>> wait_for(NativeHandle handle, bool writable);

    std::unique_ptr<Impl> impl_;
};

}  // namespace continuo
