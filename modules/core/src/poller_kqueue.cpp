// kqueue readiness backend (macOS, iOS, FreeBSD).
//
// Platform detection lives in exactly one place — platform.hpp. Every other
// file asks it instead of testing compiler macros directly, and the layering
// check fails the build on any file that forgets.

#include "mira/core/platform.hpp"

#if defined(MIRA_IO_BACKEND_KQUEUE)

    #include "poller.hpp"

    #include <array>
    #include <cerrno>
    #include <ctime>
    #include <sys/event.h>
    #include <sys/types.h>
    #include <unistd.h>

namespace Mira::detail {
namespace {

/// Wrap the current `errno` as an error_code in the system category, so that
/// callers can compare against `std::errc` without a translation table.
[[nodiscard]] Error last_os_error() noexcept {
    return std::error_code{errno, std::system_category()};
}

constexpr std::size_t kMaxChanges = 2;

}  // namespace

Result<Poller> Poller::create() noexcept {
    const int handle = ::kqueue();
    if (handle < 0) {
        return fail(last_os_error());
    }
    return Poller{handle};
}

void Poller::close() noexcept {
    if (handle_ >= 0) {
        ::close(handle_);
        handle_ = -1;
    }
}

Result<void> Poller::arm(int fd, Interest interest) noexcept {
    std::array<struct kevent, kMaxChanges> changes{};
    std::size_t count = 0;

    // EV_ONESHOT: the filter is removed as soon as it fires, which is exactly
    // the lifetime a suspended coroutine wants.
    if (contains(interest, Interest::read)) {
        EV_SET(&changes[count++],
               static_cast<uintptr_t>(fd),
               EVFILT_READ,
               EV_ADD | EV_ONESHOT,
               0,
               0,
               nullptr);
    }
    if (contains(interest, Interest::write)) {
        EV_SET(&changes[count++],
               static_cast<uintptr_t>(fd),
               EVFILT_WRITE,
               EV_ADD | EV_ONESHOT,
               0,
               0,
               nullptr);
    }
    if (count == 0) {
        return fail(Errc::invalid_argument);
    }

    if (::kevent(handle_, changes.data(), static_cast<int>(count), nullptr, 0, nullptr) < 0) {
        return fail(last_os_error());
    }
    return Result<void>{};
}

Result<void> Poller::disarm(int fd) noexcept {
    std::array<struct kevent, kMaxChanges> changes{};
    EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);

    // ENOENT means the filter already fired (one-shot) or was never armed —
    // both are ordinary races for a cancelling waiter, not failures.
    if (::kevent(handle_, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr) <
            0 &&
        errno != ENOENT) {
        return fail(last_os_error());
    }
    return Result<void>{};
}

Result<std::size_t> Poller::poll(std::span<ReadyEvent> out, int timeout_ms) noexcept {
    if (out.empty()) {
        return fail(Errc::invalid_argument);
    }

    std::array<struct kevent, 64> events{};
    const std::size_t capacity = out.size() < events.size() ? out.size() : events.size();

    struct timespec timeout{};
    const struct timespec* timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000L;
        timeout_ptr = &timeout;
    }

    const int count =
        ::kevent(handle_, nullptr, 0, events.data(), static_cast<int>(capacity), timeout_ptr);
    if (count < 0) {
        // A signal during the wait is not a failure; report "nothing ready"
        // and let the caller loop.
        if (errno == EINTR) {
            return std::size_t{0};
        }
        return fail(last_os_error());
    }

    for (int i = 0; i < count; ++i) {
        const struct kevent& source = events[static_cast<std::size_t>(i)];
        ReadyEvent& target = out[static_cast<std::size_t>(i)];
        target.fd = static_cast<int>(source.ident);
        target.readable = source.filter == EVFILT_READ;
        target.writable = source.filter == EVFILT_WRITE;
        target.failed = (source.flags & EV_ERROR) != 0 || (source.flags & EV_EOF) != 0;
    }

    return static_cast<std::size_t>(count);
}

}  // namespace Mira::detail

#endif  // BSD-family
