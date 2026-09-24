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
    #include <exception>
    #include <memory>
    #include <mswsock.h>
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
    return socket_error(::WSAGetLastError());
}

/// Completions drained per iteration — bounded so that one busy socket cannot
/// starve timers and posted work.
constexpr ULONG kEventBatch = 64;

/// Completion key used for `post()` wakeups, distinguishable from real I/O.
constexpr ULONG_PTR kWakeupKey = 1;

/// Per-endpoint slot size AcceptEx writes addresses into: a sockaddr plus the
/// 16 bytes of padding the API mandates.
constexpr DWORD kAddressSlot = sizeof(SOCKADDR_STORAGE) + 16;

/// AcceptEx and ConnectEx are not exported from a link library: they must be
/// fetched per-socket through WSAIoctl. Resolved once and cached, which is what
/// Microsoft's own documentation prescribes.
template<typename Fn>
[[nodiscard]] Result<Fn> resolve_extension(SOCKET socket, GUID guid) {
    Fn function = nullptr;
    DWORD written = 0;
    if (::WSAIoctl(socket,
                   SIO_GET_EXTENSION_FUNCTION_POINTER,
                   &guid,
                   sizeof(guid),
                   &function,
                   sizeof(function),
                   &written,
                   nullptr,
                   nullptr) == SOCKET_ERROR) {
        return fail(last_socket_error());
    }
    return function;
}

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

        /// Accept-only state. AcceptEx requires the socket to exist *before*
        /// the operation is submitted, and writes both endpoint addresses into
        /// a caller-supplied buffer that must stay alive until completion —
        /// hence both living here, owned by the loop.
        /// The socket this operation was submitted on. Needed because an IOCP
        /// completion reports an NTSTATUS, and only WSAGetOverlappedResult can
        /// turn that back into a Winsock error number.
        SOCKET socket{INVALID_SOCKET};
        SOCKET accepted{INVALID_SOCKET};
        SOCKET listener{INVALID_SOCKET};
        /// Two sockaddr slots plus the 16-byte padding AcceptEx demands.
        std::array<std::byte, 2 * (sizeof(SOCKADDR_STORAGE) + 16)> address_scratch{};
        bool is_accept{false};
        bool is_connect{false};
        bool cancel_requested{false};
    };

    explicit Impl(HANDLE port) noexcept : port_(port) {}

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ~Impl() { shutdown(); }

    void shutdown() noexcept {
        if (shutting_down_.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        stop_requested_.store(true, std::memory_order_release);

        std::vector<detail::VoidSuspension> orphan_timers;
        std::vector<Operation*> orphan_ops;
        std::vector<std::function<void()>> discarded_work;
        {
            const std::lock_guard lock{mutex_};
            timers_.extract_all(orphan_timers);
            posted_.drain_into(discarded_work);
            for (auto& [pointer, owned] : operations_) {
                orphan_ops.push_back(pointer);
            }
        }

        // CancelIoEx 只提出取消请求；完成包到达前，内核仍可访问
        // OVERLAPPED、AcceptEx 地址缓冲和调用方的读写缓冲。
        for (Operation* operation : orphan_ops) {
            ::CancelIoEx(reinterpret_cast<HANDLE>(operation->socket), &operation->overlapped);
        }

        // 即使取消返回 ERROR_NOT_FOUND，操作也可能已完成但尚未出队。
        // 排空全部 I/O 后才能释放 posted 捕获或恢复任何可能回收缓冲的协程。
        std::size_t remaining = orphan_ops.size();
        while (remaining != 0) {
            DWORD transferred = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* overlapped = nullptr;
            const BOOL ok =
                ::GetQueuedCompletionStatus(port_, &transferred, &key, &overlapped, INFINITE);
            if (overlapped == nullptr) {
                if (ok == FALSE) {
                    // 无法确认内核已结束访问时，不能继续析构并造成 UAF。
                    std::terminate();
                }
                continue;
            }
            if (key == kWakeupKey) {
                continue;
            }
            // 失败的 I/O 同样返回非空 OVERLAPPED，必须计入已排空数量。
            --remaining;
        }

        // 在锁外恢复；shutdown 标记阻止恢复后的协程再次提交操作。
        for (Operation* operation : orphan_ops) {
            if (operation->accepted != INVALID_SOCKET) {
                ::closesocket(operation->accepted);
            }
            if (operation->result) {
                *operation->result = fail(Errc::cancelled);
            }
            std::coroutine_handle<> handle = operation->handle;
            release_operation(operation);
            handle.resume();
        }
        for (const detail::VoidSuspension& timer : orphan_timers) {
            timer.complete(fail(Errc::cancelled));
        }
        discarded_work.clear();

        if (port_ != nullptr) {
            ::CloseHandle(std::exchange(port_, nullptr));
        }
    }

    // ── handle registration ─────────────────────────────────────────────────

    /// Associate the handle with the completion port. Required exactly once
    /// per handle before any overlapped operation on it.
    [[nodiscard]] Result<void> attach(NativeHandle handle) noexcept {
        if (shutting_down_.load(std::memory_order_acquire)) {
            return fail(Errc::cancelled);
        }
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
        {
            const std::lock_guard lock{mutex_};
            for (auto& [pointer, owned] : operations_) {
                if (owned->socket == static_cast<SOCKET>(handle)) {
                    owned->cancel_requested = true;
                }
            }
        }
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
        operation->socket = static_cast<SOCKET>(handle);
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
        operation->socket = static_cast<SOCKET>(handle);
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

    [[nodiscard]] Result<void> submit_accept(NativeHandle listener,
                                             int address_family,
                                             std::coroutine_handle<> coroutine,
                                             Result<std::size_t>* result) {
        if (shutting_down_.load(std::memory_order_acquire)) {
            return fail(Errc::cancelled);
        }

        const auto listening = static_cast<SOCKET>(listener);
        if (accept_ex_ == nullptr) {
            GUID guid = WSAID_ACCEPTEX;
            Result<LPFN_ACCEPTEX> resolved = resolve_extension<LPFN_ACCEPTEX>(listening, guid);
            if (!resolved) {
                return fail(resolved.error());
            }
            accept_ex_ = *resolved;
        }

        // AcceptEx needs the receiving socket to exist before submission —
        // unlike accept(), which manufactures one on return.
        const SOCKET accepted =
            ::WSASocketW(address_family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
        if (accepted == INVALID_SOCKET) {
            return fail(last_socket_error());
        }

        Operation* operation = acquire_operation(coroutine, result);
        operation->is_accept = true;
        operation->socket = listening;
        operation->accepted = accepted;
        operation->listener = listening;

        DWORD received = 0;
        const BOOL ok = accept_ex_(listening,
                                   accepted,
                                   operation->address_scratch.data(),
                                   /*dwReceiveDataLength=*/0,
                                   kAddressSlot,
                                   kAddressSlot,
                                   &received,
                                   &operation->overlapped);

        if (ok == TRUE || ::WSAGetLastError() == ERROR_IO_PENDING) {
            return Result<void>{};
        }

        const Error error = last_socket_error();
        ::closesocket(accepted);
        release_operation(operation);
        return fail(error);
    }

    [[nodiscard]] Result<void> submit_connect(NativeHandle handle,
                                              std::span<const std::byte> address,
                                              std::coroutine_handle<> coroutine,
                                              Result<std::size_t>* result) {
        if (shutting_down_.load(std::memory_order_acquire)) {
            return fail(Errc::cancelled);
        }

        const auto socket = static_cast<SOCKET>(handle);
        if (connect_ex_ == nullptr) {
            GUID guid = WSAID_CONNECTEX;
            Result<LPFN_CONNECTEX> resolved = resolve_extension<LPFN_CONNECTEX>(socket, guid);
            if (!resolved) {
                return fail(resolved.error());
            }
            connect_ex_ = *resolved;
        }

        const auto* target = reinterpret_cast<const sockaddr*>(address.data());

        // ConnectEx requires an already-bound socket; POSIX connect() binds
        // implicitly, so this step has no counterpart in the other backend.
        SOCKADDR_STORAGE local{};
        local.ss_family = target->sa_family;
        const int local_length = target->sa_family == AF_INET6
                                     ? static_cast<int>(sizeof(sockaddr_in6))
                                     : static_cast<int>(sizeof(sockaddr_in));
        if (::bind(socket, reinterpret_cast<const sockaddr*>(&local), local_length) ==
                SOCKET_ERROR &&
            ::WSAGetLastError() != WSAEINVAL) {
            return fail(last_socket_error());
        }

        Operation* operation = acquire_operation(coroutine, result);
        operation->is_connect = true;
        operation->socket = socket;

        const BOOL ok = connect_ex_(socket,
                                    target,
                                    static_cast<int>(address.size()),
                                    nullptr,
                                    0,
                                    nullptr,
                                    &operation->overlapped);
        if (ok == TRUE || ::WSAGetLastError() == ERROR_IO_PENDING) {
            return Result<void>{};
        }

        const Error error = last_socket_error();
        release_operation(operation);
        return fail(error);
    }

    [[nodiscard]] Result<void> add_timer(detail::Clock::time_point deadline,
                                         std::coroutine_handle<> coroutine,
                                         Result<void>* result) {
        {
            const std::lock_guard lock{mutex_};
            if (shutting_down_.load(std::memory_order_acquire)) {
                return fail(Errc::cancelled);
            }
            timers_.add(deadline, detail::VoidSuspension{coroutine, result});
        }
        wake();
        return Result<void>{};
    }

    [[nodiscard]] Result<void> post(std::function<void()> work) {
        {
            const std::lock_guard lock{mutex_};
            if (shutting_down_.load(std::memory_order_acquire)) {
                return fail(Errc::cancelled);
            }
            posted_.push(std::move(work));
        }
        wake();
        return Result<void>{};
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
        if (shutting_down_.load(std::memory_order_acquire)) {
            return fail(Errc::cancelled);
        }
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
                // detach may have closed the socket before its completion
                // arrived. Do not query Winsock with a stale/reused handle.
                if (operation->cancel_requested) {
                    return fail(Errc::cancelled);
                }
                if (status != 0) {
                    // `entry.Internal` is an NTSTATUS (STATUS_CONNECTION_REFUSED
                    // is 0xC0000236), which is a third numbering space on top of
                    // Win32 and Winsock. Only WSAGetOverlappedResult maps it back
                    // to the Winsock number a caller can reason about — feeding
                    // the raw NTSTATUS to system_category() produces an error
                    // that matches no std::errc at all.
                    DWORD ignored_bytes = 0;
                    DWORD ignored_flags = 0;
                    if (operation->socket != INVALID_SOCKET &&
                        ::WSAGetOverlappedResult(operation->socket,
                                                 &operation->overlapped,
                                                 &ignored_bytes,
                                                 FALSE,
                                                 &ignored_flags) == FALSE) {
                        return fail(socket_error(::WSAGetLastError()));
                    }
                    return fail(std::error_code{static_cast<int>(status), std::system_category()});
                }
                if (operation->is_accept) {
                    // An accept completes with zero bytes transferred — that is
                    // success, not eof. The accepted socket also does not
                    // inherit the listener's state unless told to, and skipping
                    // this leaves getsockname/shutdown broken in ways that only
                    // show up much later.
                    ::setsockopt(operation->accepted,
                                 SOL_SOCKET,
                                 SO_UPDATE_ACCEPT_CONTEXT,
                                 reinterpret_cast<const char*>(&operation->listener),
                                 sizeof(operation->listener));
                    return static_cast<std::size_t>(operation->accepted);
                }
                if (operation->is_connect) {
                    // A connect also completes with zero bytes. Only a recv
                    // may read zero as "peer closed".
                    return std::size_t{0};
                }
                if (transferred == 0) {
                    // Zero bytes on a completed recv means the peer closed.
                    return fail(Errc::eof);
                }
                return static_cast<std::size_t>(transferred);
            }();

            // A failed accept must not leak the socket it pre-created.
            if (operation->is_accept && !outcome.has_value() &&
                operation->accepted != INVALID_SOCKET) {
                ::closesocket(operation->accepted);
            }

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

    LPFN_ACCEPTEX accept_ex_{nullptr};
    LPFN_CONNECTEX connect_ex_{nullptr};

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> wake_pending_{false};
    std::atomic<bool> shutting_down_{false};
};

// ── EventLoop surface ────────────────────────────────────────────────────────

EventLoop::EventLoop(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
EventLoop::EventLoop(EventLoop&&) noexcept = default;
EventLoop& EventLoop::operator=(EventLoop&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            impl_->shutdown();
        }
        impl_ = std::move(other.impl_);
    }
    return *this;
}
EventLoop::~EventLoop() {
    // 恢复回调可能经由 socket.close() 再调用 loop.detach()；
    // 必须在 unique_ptr 开始销毁并清空 impl_ 之前完成 shutdown。
    if (impl_) {
        impl_->shutdown();
    }
}

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
    (void)impl_->post(std::move(work));
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

Task<Result<std::size_t>> EventLoop::read(NativeHandle handle,
                                          std::span<std::byte> destination,
                                          OperationOptions options) {
    if (destination.empty()) {
        co_return std::size_t{0};
    }
    Impl* impl = impl_.get();
    static_cast<void>(options);
    auto submit = [impl, handle, destination](std::coroutine_handle<> coroutine,
                                              Result<std::size_t>* result) {
        return impl->submit_read(handle, destination, coroutine, result);
    };
    co_return co_await detail::OperationAwaiter<Result<std::size_t>, decltype(submit)>{submit};
}

Task<Result<std::size_t>> EventLoop::write(NativeHandle handle,
                                           std::span<const std::byte> source,
                                           OperationOptions options) {
    if (source.empty()) {
        co_return std::size_t{0};
    }
    Impl* impl = impl_.get();
    static_cast<void>(options);
    auto submit = [impl, handle, source](std::coroutine_handle<> coroutine,
                                         Result<std::size_t>* result) {
        return impl->submit_write(handle, source, coroutine, result);
    };
    co_return co_await detail::OperationAwaiter<Result<std::size_t>, decltype(submit)>{submit};
}

Task<Result<NativeHandle>>
EventLoop::accept(NativeHandle listener, int address_family, OperationOptions options) {
    Impl* impl = impl_.get();
    static_cast<void>(options);
    auto submit = [impl, listener, address_family](std::coroutine_handle<> coroutine,
                                                   Result<std::size_t>* result) {
        return impl->submit_accept(listener, address_family, coroutine, result);
    };

    // The accepted socket travels back through the size_t slot; the loop has
    // already associated it with the completion port.
    Result<std::size_t> accepted =
        co_await detail::OperationAwaiter<Result<std::size_t>, decltype(submit)>{submit};
    if (!accepted) {
        co_return fail(accepted.error());
    }
    Result<void> attached = impl->attach(static_cast<NativeHandle>(*accepted));
    if (!attached) {
        ::closesocket(static_cast<SOCKET>(*accepted));
        co_return fail(attached.error());
    }
    co_return static_cast<NativeHandle>(*accepted);
}

Task<Result<void>> EventLoop::connect(NativeHandle handle,
                                      std::span<const std::byte> address,
                                      OperationOptions options) {
    if (address.size() < sizeof(sockaddr)) {
        co_return fail(Errc::invalid_argument);
    }
    Impl* impl = impl_.get();
    static_cast<void>(options);
    auto submit = [impl, handle, address](std::coroutine_handle<> coroutine,
                                          Result<std::size_t>* result) {
        return impl->submit_connect(handle, address, coroutine, result);
    };
    Result<std::size_t> connected =
        co_await detail::OperationAwaiter<Result<std::size_t>, decltype(submit)>{submit};
    if (!connected) {
        co_return fail(connected.error());
    }
    // ConnectEx leaves the socket in a half-initialised state until told.
    ::setsockopt(static_cast<SOCKET>(handle), SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
    co_return Result<void>{};
}

// ── timers and scheduling ────────────────────────────────────────────────────

Task<Result<void>> EventLoop::wait_for(NativeHandle, bool, OperationOptions) {
    // No readiness concept on IOCP. The portable surface never calls this;
    // `wait_readable`/`wait_writable` are not declared on this platform.
    co_return fail(Errc::not_supported);
}

Task<Result<void>> EventLoop::sleep_until(Clock::time_point deadline, OperationOptions options) {
    Impl* impl = impl_.get();
    static_cast<void>(options);
    auto submit = [impl, deadline](std::coroutine_handle<> coroutine, Result<void>* result) {
        return impl->add_timer(deadline, coroutine, result);
    };
    co_return co_await detail::OperationAwaiter<Result<void>, decltype(submit)>{submit};
}

Task<Result<void>> EventLoop::sleep_for(Duration delay, OperationOptions options) {
    return sleep_until(Clock::now() + delay, std::move(options));
}

Task<void> EventLoop::yield() {
    Impl* impl = impl_.get();
    auto submit = [impl](std::coroutine_handle<> coroutine, Result<void>* result) {
        // Keep yield in the tracked timer queue so shutdown resumes it after
        // draining kernel I/O, instead of discarding a continuation in post().
        return impl->add_timer(Clock::now(), coroutine, result);
    };
    (void)co_await detail::OperationAwaiter<Result<void>, decltype(submit)>{submit};
    co_return;
}

}  // namespace continuo

#endif  // CONTINUO_PLATFORM_WINDOWS
