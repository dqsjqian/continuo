#pragma once

// Internal readiness backend — NOT a public header.
//
// One thin wrapper over whatever the platform uses to report "this descriptor
// is ready": kqueue on macOS/BSD, epoll on Linux. It reports readiness and
// nothing else — no buffers, no sockets, no protocol. That is what keeps the
// event loop reusable by every transport that arrives later.
//
// Registration is one-shot: an armed interest fires at most once and is
// automatically disarmed. This is a deliberate match for the coroutine model,
// where a suspended `co_await wait_readable(fd)` wants exactly one wakeup.
// Level-triggered re-arming would force the loop to track which waiters are
// still interested; one-shot pushes that decision back to the caller, which is
// where it belongs.

#include "Mira/core/error.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace Mira::detail {

/// What a waiter is waiting for.
enum class Interest : unsigned {
    none = 0u,
    read = 1u << 0,
    write = 1u << 1,
};

[[nodiscard]] constexpr Interest operator|(Interest a, Interest b) noexcept {
    return static_cast<Interest>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

[[nodiscard]] constexpr bool contains(Interest set, Interest flag) noexcept {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(flag)) != 0u;
}

/// A readiness notification for one descriptor.
struct ReadyEvent {
    int fd{-1};
    bool readable{false};
    bool writable{false};
    /// Peer hung up or the descriptor errored; the waiter is woken either way
    /// and discovers the reason from its next read/write.
    bool failed{false};
};

/// Thin, move-only handle over the platform readiness mechanism.
class Poller {
public:
    [[nodiscard]] static Result<Poller> create() noexcept;

    Poller(Poller&& other) noexcept : handle_(std::exchange(other.handle_, -1)) {}

    Poller& operator=(Poller&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, -1);
        }
        return *this;
    }

    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    ~Poller() { close(); }

    /// Register a one-shot interest in `fd`.
    [[nodiscard]] Result<void> arm(int fd, Interest interest) noexcept;

    /// Cancel any pending interest in `fd` — **both** directions.
    ///
    /// Callers that only want to drop one direction must `arm()` the remaining
    /// one instead, or they will silently cancel a waiter on the other.
    ///
    /// Not an error if nothing was armed: a waiter cancelled in the same tick
    /// its event fired is a normal race, not a failure.
    Result<void> disarm(int fd) noexcept;

    /// Wait for readiness, filling `out` with up to `out.size()` events.
    ///
    /// `timeout_ms < 0` waits indefinitely; `0` polls without blocking.
    /// Interruption by a signal yields zero events rather than an error.
    [[nodiscard]] Result<std::size_t> poll(std::span<ReadyEvent> out, int timeout_ms) noexcept;

private:
    Poller() = default;
    explicit Poller(int handle) noexcept : handle_(handle) {}

    void close() noexcept;

    int handle_{-1};
};

}  // namespace Mira::detail
