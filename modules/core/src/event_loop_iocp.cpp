// Event loop — Windows backend (I/O completion ports).
//
// This is where the completion-shaped public API pays for itself: `read()` is
// a `WSARecv` with an OVERLAPPED, and the loop hands the result back when the
// port reports it. No readiness emulation, no `select` fallback, no
// second-class Windows path.
//
// The OVERLAPPED trick: each in-flight operation allocates an `Operation`
// whose first member *is* an OVERLAPPED, so the pointer the kernel returns can
// be cast straight back to the owning operation. The operation is owned by the
// loop between submission and completion — a coroutine that is cancelled
// cannot free memory the kernel still writes into.
//
// NOT validated on the author's machine (macOS). CI's Windows job is the only
// thing that exercises this file; treat its first green run as the real
// verification, not this comment.

#include "continuo/core/platform.hpp"

#if CONTINUO_PLATFORM_WINDOWS

    #include "continuo/core/event_loop.hpp"

// clang-format off
#    include <winsock2.h>
#    include <windows.h>
#    include <ws2tcpip.h>
// clang-format on

    #include "loop_common.hpp"

    #include <array>
    #include <atomic>
    #include <memory>
    #include <mutex>
    #include <unordered_map>
    #include <utility>
    #include <vector>

    #pragma comment(lib, "ws2_32.lib")

namespace continuo {
namespace {

[[nodiscard]] Error last_os_error() noexcept {
    return std::error_code{static_cast<int>(::GetLastError()), std::system_category()};
}

[[nodiscard]] Error last_socket_error() noexcept {
    return std::error_code{::WSAGetLastError(), std::system_category()};
}

/// Completions drained per iteration — bounded so that one busy socket cannot
/// starve timers and posted work.
constexpr ULONG kEventBatch = 64;

/// Completion key used for `post()` wakeups, distinguishable from real I/O.
constexpr ULONG_PTR kWakeupKey = 1;

/// Winsock needs process-wide initialisation, exactly once.
[[nodiscard]] Result<void> ensure_winsock() {
    static Result<void> once = []() -> Result<void> {
        WSADATA data{};
        const int status = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (status != 0) {
            return fail(std::error_code{status, std::system_category()});
        }
        return Result<void>{};
    }();
    return once;
}

}  // namespace

class EventLoop::Impl {
public:
    /// One in-flight overlapped operation.
    ///
    /// `overlapped` must stay first: the kernel hands back its address, and
    /// the loop casts it straight back to this object.
    struct Operation {
        OVERLAPPED overlapped{};
        std::coroutine_handle<> handle{};
        Result<std::size_t>* result{nullptr};
        WSABUF buffer{};
    };

    explicit Impl(HANDLE port) noexcept : port_(port) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() {
        shutting_down_.store(true, std::memory_order_release);

        std::vector<detail::VoidSuspension> orphan_timers;
        std::vector<Operation*> orphan_ops;
        {
            const std::lock_guard lock{mutex_};
            timers_.extract_all(orphan_timers);
            posted_.clear();
            for (auto& [pointer, owned] : operations_) {
                orphan_ops.push_back(pointer);
            }
        }

        // Resume outside the lock so a resumed coroutine may call back in.
        for (const detail::VoidSuspension& timer : orphan_timers) {
            timer.complete(fail(Errc::cancelled));
        }
        for (Operation* operation : orphan_ops) {
            if (operation->result) {
                *operation->result = fail(Errc::cancelled);
            }
            std::coroutine_handle<> handle = operation->handle;
            release_operation(operation);
            handle.resume();
        }

        if (port_ != nullptr) {
            ::CloseHandle(port_);
        }
    }

    // ── handle registration ─────────────────────────────────────────────────

    /// Associate the handle with the completion port. Required exactly once
    /// per handle before any overlapped operation on it.
    [[nodiscard]] Result<void> attach(NativeHandle handle) noexcept {
        if (handle == invalid_handle) {
            return fail(Errc::invalid_argument);
        }
        if (::CreateIoCompletionPort(reinterpret_cast<HANDLE>(handle), port_, 0, 0) == nullptr) {
            return fail(last_os_error());
        }
        // Completions must not also signal the handle, or every operation
        // would pay for an event nobody waits on.
        ::SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(handle),
                                             FILE_SKIP_SET_EVENT_ON_HANDLE);
        return Result<void>{};
    }

    void detach(NativeHandle handle) noexcept {
        // A handle cannot be removed from a completion port; cancelling its
        // outstanding operations is the closest equivalent, and the
        // completions still arrive (as ERROR_OPERATION_ABORTED) so no
        // operation leaks.
        ::CancelIoEx(reinterpret_cast<HANDLE>(handle), nullptr);
    }

    // ── submission ──────────────────────────────────────────────────────────

    [[nodiscard]] Result<void> submit_read(NativeHandle handle,
                                           std::span<std::byte> destination,
                                           std::coroutine_handle<> coroutine,
                                           Result<std::size_t>* result) {
        if (shutting_down_.load(std::memory_order_acquire)) {
            return fail(Errc::cancelled);
        }

        Operation* operation = acquire_operation(coroutine, result);
        operation->buffer.buf = reinterpret_cast<CHAR*>(destination.data());
        operation->buffer.len = static_cast<ULONG>(destination.size());

        DWORD flags = 0;
        const int status = ::WSARecv(static_cast<SOCKET>(handle),
                                     &operation->buffer,
                                     1,
                                     nullptr,
                                     &flags,
                                     &operation->overlapped,
                                     nullptr);
        if (status == 0 || ::WSAGetLastError() == WSA_IO_PENDING) {
            // Even an inline completion is posted to the port, so the
            // resumption path stays identical either way.
            return Result<void>{};
        }

        const Error error = last_socket_error();
        release_operation(operation);
        return fail(error);
    }

    [[nodiscard]] Result<void> submit_write(NativeHandle handle,
                                            std::span<const std::byte> source,
                                            std::coroutine_handle<> coroutine,
                                            Result<std::size_t>* result) {
        if (shutting_down_.load(std::memory_order_acquire)) {
            return fail(Errc::cancelled);
        }

        Operation* operation = acquire_operation(coroutine, result);
        operation->buffer.buf = const_cast<CHAR*>(reinterpret_cast<const CHAR*>(source.data()));
        operation->buffer.len = static_cast<ULONG>(source.size());

        const int status = ::WSASend(static_cast<SOCKET>(handle),
                                     &operation->buffer,
                                     1,
                                     nullptr,
                                     0,
                                     &operation->overlapped,
                                     nullptr);
        if (status == 0 || ::WSAGetLastError() == WSA_IO_PENDING) {
            return Result<void>{};
        }

        const Error error = last_socket_error();
        release_operation(operation);
        return fail(error);
    }

    void add_timer(detail::Clock::time_point deadline,
                   std::coroutine_handle<> coroutine,
                   Result<void>* result) {
        {
            const std::lock_guard lock{mutex_};
            timers_.add(deadline, detail::VoidSuspension{coroutine, result});
        }
        wake();
    }

    void post(std::function<void()> work) {
        {
            const std::lock_guard lock{mutex_};
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
        return posted_.size() + timers_.size() + operations_.size();
    }

    // ── driving ─────────────────────────────────────────────────────────────

    Result<void> run_once(Duration timeout) {
        std::array<OVERLAPPED_ENTRY, kEventBatch> entries{};

        int timeout_ms = 0;
        {
            const std::lock_guard lock{mutex_};
            timeout_ms = detail::resolve_timeout_ms(
                timeout, timers_.earliest(), !posted_.empty() || stopped());
        }

        ULONG removed = 0;
        const BOOL ok = ::GetQueuedCompletionStatusEx(
            port_,
            entries.data(),
            kEventBatch,
            &removed,
            timeout_ms < 0 ? INFINITE : static_cast<DWORD>(timeout_ms),
            FALSE);

        if (ok == FALSE) {
            const DWORD error = ::GetLastError();
            if (error != WAIT_TIMEOUT) {
                return fail(std::error_code{static_cast<int>(error), std::system_category()});
            }
            removed = 0;
        }

        std::vector<std::pair<Operation*, Result<std::size_t>>> finished;
        finished.reserve(removed);

        for (ULONG i = 0; i < removed; ++i) {
            const OVERLAPPED_ENTRY& entry = entries[i];
            if (entry.lpCompletionKey == kWakeupKey || entry.lpOverlapped == nullptr) {
                continue;  // a post() nudge, not an I/O completion
            }

            auto* operation = reinterpret_cast<Operation*>(entry.lpOverlapped);
            const DWORD status = static_cast<DWORD>(entry.Internal);
            const DWORD transferred = entry.dwNumberOfBytesTransferred;

            Result<std::size_t> outcome = [&]() -> Result<std::size_t> {
                if (status == ERROR_OPERATION_ABORTED) {
                    return fail(Errc::cancelled);
                }
                if (status != 0) {
                    return fail(std::error_code{static_cast<int>(status), std::system_category()});
                }
                if (transferred == 0) {
                    // Zero bytes on a completed recv means the peer closed.
                    return fail(Errc::eof);
                }
                return static_cast<std::size_t>(transferred);
            }();

            finished.emplace_back(operation, outcome);
        }

        std::vector<detail::VoidSuspension> expired;
        std::vector<std::function<void()>> to_run;
        {
            const std::lock_guard lock{mutex_};
            timers_.extract_expired(detail::Clock::now(), expired);
            posted_.drain_into(to_run);
            wake_pending_.store(false, std::memory_order_release);
        }

        // Everything below runs outside the lock: resumed coroutines may
        // submit more I/O, post work, or stop the loop.
        for (auto& [operation, outcome] : finished) {
            if (operation->result) {
                *operation->result = outcome;
            }
            std::coroutine_handle<> handle = operation->handle;
            release_operation(operation);
            handle.resume();
        }
        for (const detail::VoidSuspension& timer : expired) {
            timer.complete(Result<void>{});
        }
        for (auto& work : to_run) {
            work();
        }

        return Result<void>{};
    }

private:
    /// Allocate an operation owned by the loop, not by the coroutine.
    ///
    /// The kernel writes into this memory until the completion arrives, so its
    /// lifetime cannot be tied to a frame that might unwind first.
    [[nodiscard]] Operation* acquire_operation(std::coroutine_handle<> coroutine,
                                               Result<std::size_t>* result) {
        auto owned = std::make_unique<Operation>();
        owned->handle = coroutine;
        owned->result = result;
        Operation* pointer = owned.get();

        const std::lock_guard lock{mutex_};
        operations_.emplace(pointer, std::move(owned));
        return pointer;
    }

    void release_operation(Operation* operation) noexcept {
        std::unique_ptr<Operation> owned;
        {
            const std::lock_guard lock{mutex_};
            auto it = operations_.find(operation);
            if (it != operations_.end()) {
                owned = std::move(it->second);
                operations_.erase(it);
            }
        }
        // `owned` frees here, outside the lock.
    }

    void wake() noexcept {
        if (wake_pending_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        ::PostQueuedCompletionStatus(port_, 0, kWakeupKey, nullptr);
    }

    HANDLE port_{nullptr};

    mutable std::mutex mutex_;
    std::unordered_map<Operation*, std::unique_ptr<Operation>> operations_{};
    detail::TimerQueue timers_{};
    detail::PostQueue posted_{};

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> wake_pending_{false};
    std::atomic<bool> shutting_down_{false};
};

// ── EventLoop surface ────────────────────────────────────────────────────────

EventLoop::EventLoop(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
EventLoop::EventLoop(EventLoop&&) noexcept = default;
EventLoop& EventLoop::operator=(EventLoop&&) noexcept = default;
EventLoop::~EventLoop() = default;

Result<EventLoop> EventLoop::create() {
    Result<void> winsock = ensure_winsock();
    if (!winsock) {
        return fail(winsock.error());
    }

    HANDLE port = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    if (port == nullptr) {
        return fail(last_os_error());
    }
    return EventLoop{std::make_unique<Impl>(port)};
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
    while (!impl_->stopped() && impl_->outstanding() > 0) {
        Result<void> iteration = impl_->run_once(Duration::min());
        if (!iteration) {
            return iteration;
        }
    }
    return Result<void>{};
}

// ── portable completion API, mapped straight onto IOCP ───────────────────────

Task<Result<std::size_t>> EventLoop::read(NativeHandle handle, std::span<std::byte> destination) {
    if (destination.empty()) {
        co_return std::size_t{0};
    }
    Impl* impl = impl_.get();
    auto submit = [impl, handle, destination](std::coroutine_handle<> coroutine,
                                              Result<std::size_t>* result) {
        return impl->submit_read(handle, destination, coroutine, result);
    };
    co_return co_await detail::OperationAwaiter<Result<std::size_t>, decltype(submit)>{submit};
}

Task<Result<std::size_t>> EventLoop::write(NativeHandle handle, std::span<const std::byte> source) {
    if (source.empty()) {
        co_return std::size_t{0};
    }
    Impl* impl = impl_.get();
    auto submit = [impl, handle, source](std::coroutine_handle<> coroutine,
                                         Result<std::size_t>* result) {
        return impl->submit_write(handle, source, coroutine, result);
    };
    co_return co_await detail::OperationAwaiter<Result<std::size_t>, decltype(submit)>{submit};
}

// ── timers and scheduling ────────────────────────────────────────────────────

Task<Result<void>> EventLoop::wait_for(NativeHandle, bool) {
    // No readiness concept on IOCP. The portable surface never calls this;
    // `wait_readable`/`wait_writable` are not declared on this platform.
    co_return fail(Errc::not_supported);
}

Task<Result<void>> EventLoop::sleep_until(Clock::time_point deadline) {
    Impl* impl = impl_.get();
    auto submit = [impl, deadline](std::coroutine_handle<> coroutine, Result<void>* result) {
        impl->add_timer(deadline, coroutine, result);
        return Result<void>{};
    };
    co_return co_await detail::OperationAwaiter<Result<void>, decltype(submit)>{submit};
}

Task<Result<void>> EventLoop::sleep_for(Duration delay) {
    return sleep_until(Clock::now() + delay);
}

Task<void> EventLoop::yield() {
    Impl* impl = impl_.get();
    auto submit = [impl](std::coroutine_handle<> coroutine, Result<void>* result) {
        impl->post([coroutine, result]() mutable {
            if (result) {
                *result = Result<void>{};
            }
            coroutine.resume();
        });
        return Result<void>{};
    };
    (void)co_await detail::OperationAwaiter<Result<void>, decltype(submit)>{submit};
    co_return;
}

}  // namespace continuo

#endif  // CONTINUO_PLATFORM_WINDOWS
