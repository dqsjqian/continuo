// Event loop — POSIX backend (kqueue / epoll).
//
// The public API is completion-shaped ("tell me when this read finished"), but
// POSIX only offers readiness ("this fd is readable"). This file bridges the
// two in the one place that is allowed to know the difference: try the
// syscall, and on EAGAIN suspend on readiness and retry. That is the whole
// emulation, and it is why the Windows backend can be a direct IOCP mapping
// instead of the other way around.

#include "continuo/core/platform.hpp"

#if CONTINUO_HAS_READINESS_API

    #include "continuo/core/event_loop.hpp"
    #include "loop_common.hpp"
    #include "poller.hpp"

    #include <algorithm>
    #include <array>
    #include <atomic>
    #include <cerrno>
    #include <fcntl.h>
    #include <mutex>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <unordered_map>
    #include <utility>
    #include <vector>

namespace continuo {
namespace {

[[nodiscard]] Error last_os_error() noexcept {
    return std::error_code{errno, std::system_category()};
}

/// Completions drained per iteration.
///
/// Bounded on purpose: an unbounded batch lets one busy descriptor starve
/// timers and posted work, which surfaces as latency nobody can attribute.
constexpr std::size_t kEventBatch = 64;

}  // namespace

class EventLoop::Impl {
public:
    struct FdWaiters {
        detail::VoidSuspension read{};
        detail::VoidSuspension write{};

        [[nodiscard]] bool empty() const noexcept { return !read.handle && !write.handle; }

        [[nodiscard]] detail::Interest interest() const noexcept {
            auto set = static_cast<detail::Interest>(0u);
            if (read.handle) {
                set = set | detail::Interest::read;
            }
            if (write.handle) {
                set = set | detail::Interest::write;
            }
            return set;
        }
    };

    Impl(detail::Poller poller, int wake_read, int wake_write) noexcept
        : poller_(std::move(poller)), wake_read_(wake_read), wake_write_(wake_write) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        shutdown();
        if (wake_read_ >= 0) ::close(wake_read_);
        if (wake_write_ >= 0) ::close(wake_write_);
    }

    void shutdown() {
        // Wake everything still suspended so those coroutine frames unwind
        // rather than leak. They observe `cancelled` and are expected to
        // return promptly — the loop is already unusable by then.
        if (shutting_down_.exchange(true, std::memory_order_acq_rel)) return;

        std::vector<detail::VoidSuspension> orphans;
        std::vector<std::function<void()>> discarded_work;
        {
            const std::lock_guard lock{mutex_};
            for (auto& [fd, waiters] : fd_waiters_) {
                if (waiters.read.handle) {
                    orphans.push_back(waiters.read);
                }
                if (waiters.write.handle) {
                    orphans.push_back(waiters.write);
                }
            }
            fd_waiters_.clear();
            timers_.extract_all(orphans);
            posted_.drain_into(discarded_work);
        }

        // Resume outside the lock: a resumed coroutine may call back in.
        for (const detail::VoidSuspension& orphan : orphans) {
            orphan.complete(fail(Errc::cancelled));
        }
    }

    // ── handle registration ─────────────────────────────────────────────────

    /// On POSIX the only per-handle setup is non-blocking mode, which the
    /// EAGAIN-retry loop depends on. A blocking descriptor would stall the
    /// whole loop inside a single `read()`.
    [[nodiscard]] Result<void> attach(int fd) noexcept {
        if (shutting_down()) return fail(Errc::cancelled);
        if (fd < 0) {
            return fail(Errc::invalid_argument);
        }
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0) {
            return fail(last_os_error());
        }
        if ((flags & O_NONBLOCK) == 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            return fail(last_os_error());
        }
        return Result<void>{};
    }

    void detach(int fd) noexcept {
        // Remove kernel interest before cancellation resumes user code, which
        // can register another operation. Never disarm that new registration.
        (void)poller_.disarm(fd);
        fail_waiters(fd, make_error_code(Errc::cancelled));
    }

    // ── suspension ──────────────────────────────────────────────────────────

    [[nodiscard]] Result<void>
    add_waiter(int fd, bool writable, std::coroutine_handle<> handle, Result<void>* result) {
        if (fd < 0) {
            return fail(Errc::invalid_argument);
        }
        if (shutting_down_.load(std::memory_order_acquire)) {
            return fail(Errc::cancelled);
        }

        detail::Interest combined{};
        {
            const std::lock_guard lock{mutex_};
            FdWaiters& waiters = fd_waiters_[fd];
            detail::VoidSuspension& slot = writable ? waiters.write : waiters.read;
            if (slot.handle) {
                // Two coroutines waiting on the same direction of the same
                // descriptor would race over a single wakeup. Refusing beats
                // silently picking a winner.
                return fail(Errc::invalid_argument);
            }
            slot = detail::VoidSuspension{handle, result};
            combined = waiters.interest();
        }

        // The syscall runs outside the lock; a failure must undo the slot.
        Result<void> armed = poller_.arm(fd, combined);
        if (!armed) {
            const std::lock_guard lock{mutex_};
            auto it = fd_waiters_.find(fd);
            if (it != fd_waiters_.end()) {
                detail::VoidSuspension& slot = writable ? it->second.write : it->second.read;
                slot = detail::VoidSuspension{};
                if (it->second.empty()) {
                    fd_waiters_.erase(it);
                }
            }
            return fail(armed.error());
        }
        return Result<void>{};
    }

    [[nodiscard]] bool shutting_down() const noexcept {
        return shutting_down_.load(std::memory_order_acquire);
    }

    Result<void> add_timer(detail::Clock::time_point deadline,
                           std::coroutine_handle<> handle,
                           Result<void>* result) {
        if (shutting_down()) return fail(Errc::cancelled);
        {
            const std::lock_guard lock{mutex_};
            timers_.add(deadline, detail::VoidSuspension{handle, result});
        }
        wake();  // a nearer deadline may shorten the current wait
        return Result<void>{};
    }

    void post(std::function<void()> work) {
        {
            const std::lock_guard lock{mutex_};
            if (shutting_down()) return;
            posted_.push(std::move(work));
        }
        wake();
    }

    void stop() {
        stop_requested_.store(true, std::memory_order_release);
        wake();
    }

    [[nodiscard]] bool stopped() const noexcept {
        return stop_requested_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t outstanding() const noexcept {
        const std::lock_guard lock{mutex_};
        std::size_t count = posted_.size() + timers_.size();
        for (const auto& [fd, waiters] : fd_waiters_) {
            count += (waiters.read.handle ? 1u : 0u) + (waiters.write.handle ? 1u : 0u);
        }
        return count;
    }

    [[nodiscard]] Result<void> arm_wakeup() noexcept {
        return poller_.arm(wake_read_, detail::Interest::read);
    }

    // ── driving ─────────────────────────────────────────────────────────────

    Result<void> run_once(Duration timeout) {
        if (shutting_down()) return fail(Errc::cancelled);
        std::array<detail::ReadyEvent, kEventBatch> events{};

        int timeout_ms = 0;
        {
            const std::lock_guard lock{mutex_};
            timeout_ms = detail::resolve_timeout_ms(
                timeout, timers_.earliest(), !posted_.empty() || stopped());
        }

        const Result<std::size_t> ready =
            poller_.poll(std::span<detail::ReadyEvent>{events}, timeout_ms);
        if (!ready) {
            return fail(ready.error());
        }

        std::vector<detail::VoidSuspension> to_resume;
        std::vector<std::function<void()>> to_run;
        std::vector<std::pair<int, detail::Interest>> to_rearm;
        bool wakeup_fired = false;

        {
            const std::lock_guard lock{mutex_};

            for (std::size_t i = 0; i < *ready; ++i) {
                const detail::ReadyEvent& event = events[i];

                if (event.fd == wake_read_) {
                    wakeup_fired = true;
                    continue;
                }

                auto it = fd_waiters_.find(event.fd);
                if (it == fd_waiters_.end()) {
                    continue;
                }

                // A failed descriptor wakes both directions: the waiter learns
                // what actually went wrong from its next read or write, which
                // is where the real error lives.
                const bool take_read = event.readable || event.failed;
                const bool take_write = event.writable || event.failed;

                if (take_read && it->second.read.handle) {
                    to_resume.push_back(std::exchange(it->second.read, detail::VoidSuspension{}));
                }
                if (take_write && it->second.write.handle) {
                    to_resume.push_back(std::exchange(it->second.write, detail::VoidSuspension{}));
                }

                if (it->second.empty()) {
                    fd_waiters_.erase(it);
                } else {
                    // One-shot registration is consumed; whatever still waits
                    // on this descriptor has to be re-armed.
                    to_rearm.emplace_back(event.fd, it->second.interest());
                }
            }

            timers_.extract_expired(detail::Clock::now(), to_resume);
            posted_.drain_into(to_run);
            wake_pending_.store(false, std::memory_order_release);
        }

        if (wakeup_fired) {
            drain_wakeup();
            Result<void> rearmed = arm_wakeup();
            if (!rearmed) {
                return rearmed;
            }
        }

        for (const auto& [fd, interest] : to_rearm) {
            Result<void> rearmed = poller_.arm(fd, interest);
            if (!rearmed) {
                // Surface it rather than leaving a coroutine suspended forever
                // with nothing armed on its behalf.
                fail_waiters(fd, rearmed.error());
            }
        }

        // Everything below runs outside the lock. Resumed coroutines may
        // submit more I/O, post work, or stop the loop.
        for (const detail::VoidSuspension& suspension : to_resume) {
            suspension.complete(Result<void>{});
        }
        for (auto& work : to_run) {
            work();
        }

        return Result<void>{};
    }

private:
    void fail_waiters(int fd, Error error) {
        std::vector<detail::VoidSuspension> broken;
        {
            const std::lock_guard lock{mutex_};
            auto it = fd_waiters_.find(fd);
            if (it == fd_waiters_.end()) {
                return;
            }
            if (it->second.read.handle) {
                broken.push_back(std::exchange(it->second.read, detail::VoidSuspension{}));
            }
            if (it->second.write.handle) {
                broken.push_back(std::exchange(it->second.write, detail::VoidSuspension{}));
            }
            fd_waiters_.erase(it);
        }
        for (const detail::VoidSuspension& suspension : broken) {
            suspension.complete(fail(error));
        }
    }

    /// Nudge a loop that may be blocked in `poll()`.
    ///
    /// The flag collapses bursts into a single byte, so a thousand posts do
    /// not fill the pipe.
    void wake() noexcept {
        if (wake_pending_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        const unsigned char byte = 1;
        ssize_t written = 0;
        do {
            written = ::write(wake_write_, &byte, 1);
        } while (written < 0 && errno == EINTR);
    }

    void drain_wakeup() noexcept {
        std::array<unsigned char, 64> scratch{};
        for (;;) {
            const ssize_t count = ::read(wake_read_, scratch.data(), scratch.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < static_cast<ssize_t>(scratch.size())) {
                break;
            }
        }
    }

    detail::Poller poller_;
    int wake_read_{-1};
    int wake_write_{-1};

    mutable std::mutex mutex_;
    std::unordered_map<int, FdWaiters> fd_waiters_{};
    detail::TimerQueue timers_{};
    detail::PostQueue posted_{};

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> wake_pending_{false};
    std::atomic<bool> shutting_down_{false};
};

// ── EventLoop surface ────────────────────────────────────────────────────────

EventLoop::EventLoop(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
EventLoop::EventLoop(EventLoop&&) noexcept = default;
EventLoop& EventLoop::operator=(EventLoop&& other) noexcept {
    if (this != &other) {
        if (impl_) impl_->shutdown();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
EventLoop::~EventLoop() {
    // Complete callbacks while impl_ still refers to the live implementation.
    if (impl_) impl_->shutdown();
}

Result<EventLoop> EventLoop::create() {
    Result<detail::Poller> poller = detail::Poller::create();
    if (!poller) {
        return fail(poller.error());
    }

    // Self-pipe rather than eventfd: one code path for every POSIX platform,
    // and the loop has no use for eventfd's counter semantics.
    std::array<int, 2> wake{-1, -1};
    if (::pipe(wake.data()) != 0) {
        return fail(last_os_error());
    }
    for (const int fd : wake) {
        const int flags = ::fcntl(fd, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
            ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            const Error error = last_os_error();
            ::close(wake[0]);
            ::close(wake[1]);
            return fail(error);
        }
    }

    auto impl = std::make_unique<Impl>(std::move(*poller), wake[0], wake[1]);
    Result<void> armed = impl->arm_wakeup();
    if (!armed) {
        return fail(armed.error());
    }
    return EventLoop{std::move(impl)};
}

Result<void> EventLoop::attach(NativeHandle handle) {
    return impl_->attach(handle);
}
void EventLoop::detach(NativeHandle handle) {
    impl_->detach(handle);
}

void EventLoop::post(std::function<void()> work) {
    impl_->post(std::move(work));
}
void EventLoop::stop() {
    impl_->stop();
}
bool EventLoop::stopped() const noexcept {
    return impl_->stopped();
}
std::size_t EventLoop::outstanding() const noexcept {
    return impl_->outstanding();
}
Result<void> EventLoop::run_once(Duration timeout) {
    return impl_->run_once(timeout);
}

Result<void> EventLoop::run() {
    // Returning when nothing is outstanding matches every other loop: with no
    // operations, timers, or queued work there is nothing left that could make
    // progress, and blocking forever would only hide the bug.
    while (!impl_->stopped() && impl_->outstanding() > 0) {
        Result<void> iteration = impl_->run_once(Duration::min());
        if (!iteration) {
            return iteration;
        }
    }
    return Result<void>{};
}

// ── portable completion API, emulated on readiness ───────────────────────────

Task<Result<std::size_t>> EventLoop::read(NativeHandle handle, std::span<std::byte> destination) {
    if (!impl_ || impl_->shutting_down()) co_return fail(Errc::cancelled);
    if (destination.empty()) {
        co_return std::size_t{0};
    }
    for (;;) {
        const ssize_t count = ::read(handle, destination.data(), destination.size());
        if (count > 0) {
            co_return static_cast<std::size_t>(count);
        }
        if (count == 0) {
            // A clean close is reported as eof, never as a zero-byte success:
            // "no more data" must not be mistakable for "nothing right now".
            co_return fail(Errc::eof);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return fail(last_os_error());
        }
        Result<void> ready = co_await wait_for(handle, /*writable=*/false);
        if (!ready) {
            co_return fail(ready.error());
        }
    }
}

Task<Result<std::size_t>> EventLoop::write(NativeHandle handle, std::span<const std::byte> source) {
    if (!impl_ || impl_->shutting_down()) co_return fail(Errc::cancelled);
    if (source.empty()) {
        co_return std::size_t{0};
    }
    for (;;) {
        const ssize_t count = ::write(handle, source.data(), source.size());
        if (count >= 0) {
            co_return static_cast<std::size_t>(count);
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return fail(last_os_error());
        }
        Result<void> ready = co_await wait_for(handle, /*writable=*/true);
        if (!ready) {
            co_return fail(ready.error());
        }
    }
}

Task<Result<NativeHandle>> EventLoop::accept(NativeHandle listener, int address_family) {
    if (!impl_ || impl_->shutting_down()) co_return fail(Errc::cancelled);
    // address_family is only needed by IOCP, which must pre-create the socket.
    // accept() reports the family itself, so POSIX ignores it.
    static_cast<void>(address_family);

    for (;;) {
        const int accepted = ::accept(listener, nullptr, nullptr);
        if (accepted >= 0) {
            // Attach before handing it over: a caller that has to remember
            // this would have code that works here and fails on Windows.
            Result<void> attached = impl_->attach(accepted);
            if (!attached) {
                ::close(accepted);
                co_return fail(attached.error());
            }
            co_return static_cast<NativeHandle>(accepted);
        }
        if (errno == EINTR) {
            continue;
        }
        // ECONNABORTED: the peer went away between the readiness notification
        // and the accept. Ordinary on a busy listener — retry rather than
        // failing the whole accept loop.
        if (errno == ECONNABORTED) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            co_return fail(last_os_error());
        }
        Result<void> ready = co_await wait_for(listener, /*writable=*/false);
        if (!ready) {
            co_return fail(ready.error());
        }
    }
}

Task<Result<void>> EventLoop::connect(NativeHandle handle, std::span<const std::byte> address) {
    if (!impl_ || impl_->shutting_down()) co_return fail(Errc::cancelled);
    if (address.empty()) {
        co_return fail(Errc::invalid_argument);
    }

    const auto* target = reinterpret_cast<const sockaddr*>(address.data());
    for (;;) {
        if (::connect(handle, target, static_cast<socklen_t>(address.size())) == 0) {
            co_return Result<void>{};
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EISCONN) {
            co_return Result<void>{};  // already established
        }
        if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) {
            co_return fail(last_os_error());
        }

        // A non-blocking connect reports completion by becoming writable, but
        // writability alone does not mean success: the actual outcome lives in
        // SO_ERROR and must be read, or a refused connection looks connected.
        Result<void> ready = co_await wait_for(handle, /*writable=*/true);
        if (!ready) {
            co_return fail(ready.error());
        }

        int pending = 0;
        socklen_t length = sizeof(pending);
        if (::getsockopt(handle, SOL_SOCKET, SO_ERROR, &pending, &length) < 0) {
            co_return fail(last_os_error());
        }
        if (pending != 0) {
            co_return fail(std::error_code{pending, std::system_category()});
        }
        co_return Result<void>{};
    }
}

// ── timers and scheduling ────────────────────────────────────────────────────

Task<Result<void>> EventLoop::wait_for(NativeHandle handle, bool writable) {
    Impl* impl = impl_.get();
    auto submit = [impl, handle, writable](std::coroutine_handle<> coroutine,
                                           Result<void>* result) {
        return impl->add_waiter(handle, writable, coroutine, result);
    };
    co_return co_await detail::OperationAwaiter<Result<void>, decltype(submit)>{submit};
}

Task<Result<void>> EventLoop::sleep_until(Clock::time_point deadline) {
    Impl* impl = impl_.get();
    auto submit = [impl, deadline](std::coroutine_handle<> coroutine, Result<void>* result) {
        return impl->add_timer(deadline, coroutine, result);
    };
    co_return co_await detail::OperationAwaiter<Result<void>, decltype(submit)>{submit};
}

Task<Result<void>> EventLoop::sleep_for(Duration delay) {
    return sleep_until(Clock::now() + delay);
}

Task<void> EventLoop::yield() {
    if (!impl_ || impl_->shutting_down()) co_return;
    Impl* impl = impl_.get();
    auto submit = [impl](std::coroutine_handle<> coroutine, Result<void>* result) {
        // A yield is a tracked suspension, not disposable posted work.
        // An already-due timer runs on the next pump and is cancelled/resumed
        // by shutdown, keeping child scopes joinable.
        return impl->add_timer(Clock::now(), coroutine, result);
    };
    (void)co_await detail::OperationAwaiter<Result<void>, decltype(submit)>{submit};
    co_return;
}

    #if CONTINUO_HAS_READINESS_API
Task<Result<void>> EventLoop::wait_readable(NativeHandle handle) {
    return wait_for(handle, /*writable=*/false);
}

Task<Result<void>> EventLoop::wait_writable(NativeHandle handle) {
    return wait_for(handle, /*writable=*/true);
}
    #endif

}  // namespace continuo

#endif  // CONTINUO_HAS_READINESS_API
