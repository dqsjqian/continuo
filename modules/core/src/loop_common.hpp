#pragma once

// Internal pieces shared by every event-loop backend — NOT a public header.
//
// Timers, the posted-work queue, and the suspension awaiter behave identically
// whether completions arrive from kqueue, epoll, or IOCP. Keeping them here
// means the POSIX and Windows backends differ only where the platforms
// genuinely differ, instead of drifting apart in code that should be the same.

#include "continuo/core/error.hpp"

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace continuo::detail {

using Clock = std::chrono::steady_clock;

/// A suspended coroutine plus where to leave its outcome.
///
/// `result` points into the awaiter, which lives in the suspended coroutine's
/// frame — valid for exactly as long as the operation is registered.
template<typename T>
struct Suspension {
    std::coroutine_handle<> handle{};
    T* result{nullptr};

    void complete(T value) const {
        if (result) {
            *result = std::move(value);
        }
        handle.resume();
    }
};

/// Suspension whose outcome is success-or-error with no payload.
using VoidSuspension = Suspension<Result<void>>;

/// Deadline-ordered timer queue.
///
/// Not thread-safe on its own; the owning backend holds its lock. A multimap
/// keeps insertion cheap and "nearest deadline" O(1), which is all the loop
/// asks of it.
class TimerQueue {
public:
    void add(Clock::time_point deadline, VoidSuspension suspension) {
        timers_.emplace(deadline, suspension);
    }

    [[nodiscard]] bool empty() const noexcept { return timers_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return timers_.size(); }

    /// Nearest deadline, or `time_point::max()` when idle.
    [[nodiscard]] Clock::time_point earliest() const noexcept {
        return timers_.empty() ? Clock::time_point::max() : timers_.begin()->first;
    }

    /// Move every timer due at `now` into `out`.
    void extract_expired(Clock::time_point now, std::vector<VoidSuspension>& out) {
        while (!timers_.empty() && timers_.begin()->first <= now) {
            out.push_back(timers_.begin()->second);
            timers_.erase(timers_.begin());
        }
    }

    /// Move every timer into `out`, regardless of deadline (shutdown path).
    void extract_all(std::vector<VoidSuspension>& out) {
        for (const auto& [deadline, suspension] : timers_) {
            out.push_back(suspension);
        }
        timers_.clear();
    }

private:
    std::multimap<Clock::time_point, VoidSuspension> timers_{};
};

/// Convert a caller timeout, the nearest deadline, and queued work into the
/// milliseconds a backend should block for.
///
/// Returns a negative value to mean "block indefinitely" — the convention both
/// `epoll_wait` and `GetQueuedCompletionStatus` already use.
[[nodiscard]] inline int resolve_timeout_ms(Clock::duration caller_timeout,
                                            Clock::time_point earliest_deadline,
                                            bool work_already_queued) {
    using std::chrono::ceil;
    using std::chrono::milliseconds;

    if (work_already_queued) {
        return 0;
    }

    int limit = -1;
    if (caller_timeout != Clock::duration::min()) {
        const auto rounded = ceil<milliseconds>(caller_timeout).count();
        limit = rounded <= 0 ? 0 : static_cast<int>(rounded);
    }

    if (earliest_deadline != Clock::time_point::max()) {
        const Clock::duration remaining = earliest_deadline - Clock::now();
        const int until_deadline = remaining <= Clock::duration::zero()
                                       ? 0
                                       : static_cast<int>(ceil<milliseconds>(remaining).count());
        limit = limit < 0 ? until_deadline : (until_deadline < limit ? until_deadline : limit);
    }

    return limit;
}

/// Awaiter that parks the caller until a backend completes its operation.
///
/// `await_suspend` returns `bool` so that a submission failure can decline to
/// suspend: the coroutine continues immediately and observes the error, rather
/// than parking on an operation that was never started.
template<typename T, typename Submit>
class OperationAwaiter {
public:
    explicit OperationAwaiter(Submit submit) noexcept : submit_(std::move(submit)) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        Result<void> submitted = submit_(handle, &result_);
        if (!submitted) {
            result_ = fail(submitted.error());
            return false;
        }
        return true;
    }

    [[nodiscard]] T await_resume() const noexcept { return result_; }

private:
    Submit submit_;
    T result_{};
};

/// Work queued by `post()`, drained on the loop thread.
class PostQueue {
public:
    void push(std::function<void()> work) { queued_.push_back(std::move(work)); }

    [[nodiscard]] bool empty() const noexcept { return queued_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return queued_.size(); }

    void drain_into(std::vector<std::function<void()>>& out) noexcept { out.swap(queued_); }

    void clear() noexcept { queued_.clear(); }

private:
    std::vector<std::function<void()>> queued_{};
};

}  // namespace continuo::detail
