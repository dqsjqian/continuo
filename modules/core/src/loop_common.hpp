#pragma once

// Internal pieces shared by every event-loop backend — NOT a public header.
//
// Timers, the posted-work queue, operation identity, and the suspension
// awaiter behave identically whether completions arrive from kqueue, epoll, or
// IOCP. Keeping them here means the POSIX and Windows backends differ only
// where the platforms genuinely differ, instead of drifting apart in code that
// should be the same.

#include "continuo/core/error.hpp"
#include "continuo/core/operation.hpp"

#include <algorithm>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace continuo::detail {

using Clock = std::chrono::steady_clock;

/// Stable identity for one in-flight operation.
///
/// Monotonic and never reused. That single property is what makes every
/// indirect reference safe: a timer, a cancellation request, or a readiness
/// event naming an operation that has already resolved finds *nothing* rather
/// than finding a different operation that happens to occupy the same slot.
/// It is the generation counter other designs bolt on separately, except that
/// it cannot be forgotten at a comparison site — there is no slot to compare.
using OperationId = std::uint64_t;

/// Reserved: "no operation". Never handed out, so a zeroed field is
/// unambiguously empty rather than pointing at the first operation ever made.
inline constexpr OperationId kNoOperation = 0;

/// Which operation a timer belongs to, and what its firing means.
struct TimerTarget {
    OperationId operation{kNoOperation};

    /// `false`: the operation *succeeds* when this fires — a sleep reaching
    /// the time it asked for. `true`: the operation is cut short with
    /// `Errc::timed_out`. One queue holds both, because they are the same
    /// mechanism answering to different words.
    bool is_deadline{false};
};

/// Reference to a registered timer, or `{}` for "none".
struct TimerHandle {
    Clock::time_point deadline{};
    std::uint64_t serial{0};

    [[nodiscard]] bool valid() const noexcept { return serial != 0; }
};

/// Deadline-ordered timer queue with cancellable entries.
///
/// Not thread-safe on its own; the owning backend holds its lock.
///
/// Entries carry an `OperationId` rather than a coroutine handle. A timer that
/// outlives the operation it was registered for therefore cannot resume a
/// stale frame — it names something that is no longer in the table, and the
/// lookup fails harmlessly.
///
/// Keyed by `(deadline, serial)` so that cancellation is a single `erase` of a
/// key the caller already holds: no second index to keep in step, and
/// cancelling twice is naturally a no-op.
class TimerQueue {
public:
    [[nodiscard]] TimerHandle add(Clock::time_point deadline, TimerTarget target) {
        const std::uint64_t serial = ++serial_;  // from 1: 0 means "no timer"
        timers_.emplace(Key{deadline, serial}, target);
        return TimerHandle{deadline, serial};
    }

    /// Idempotent. Cancelling a timer that already fired, or was never
    /// registered, is the ordinary path for an operation that completed some
    /// other way.
    void cancel(TimerHandle handle) noexcept {
        if (handle.valid()) {
            timers_.erase(Key{handle.deadline, handle.serial});
        }
    }

    [[nodiscard]] bool empty() const noexcept { return timers_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return timers_.size(); }

    /// Nearest deadline, or `time_point::max()` when idle.
    [[nodiscard]] Clock::time_point earliest() const noexcept {
        return timers_.empty() ? Clock::time_point::max() : timers_.begin()->first.deadline;
    }

    /// Append every timer due at `now` to `out`, wake-ups before deadlines.
    ///
    /// The partition is the point, not an optimisation: when a sleep reaches
    /// its target in the same instant that some operation's deadline expires,
    /// the sleep must be seen to have succeeded. Insertion order would get
    /// that right by coincidence, since the wake-up is registered first; the
    /// partition says it on purpose.
    void extract_expired(Clock::time_point now, std::vector<TimerTarget>& out) {
        const auto appended = static_cast<std::ptrdiff_t>(out.size());
        while (!timers_.empty() && timers_.begin()->first.deadline <= now) {
            out.push_back(timers_.begin()->second);
            timers_.erase(timers_.begin());
        }
        std::stable_partition(std::next(out.begin(), appended),
                              out.end(),
                              [](const TimerTarget& target) noexcept { return !target.is_deadline; });
    }

private:
    struct Key {
        Clock::time_point deadline;
        std::uint64_t serial;

        friend auto operator<=>(const Key&, const Key&) = default;
    };

    std::map<Key, TimerTarget> timers_{};
    std::uint64_t serial_{0};
};

/// Convert a caller timeout, the nearest deadline, and queued work into the
/// milliseconds a backend should block for.
///
/// Returns a negative value to mean "block indefinitely" — the convention both
/// `epoll_wait` and `GetQueuedCompletionStatus` already use.
///
/// Invariant for callers: the wake-up byte a backend writes may be collapsed
/// or drained spuriously, so it is never the thing that guarantees progress.
/// **Every queue whose arrival triggers a wake must be reported through
/// `work_already_queued`**, or the loop can block with work sitting in it.
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

/// Awaiter that parks the caller until a backend resolves its operation.
///
/// `await_suspend` returns `bool` so that a submission failure can decline to
/// suspend: the coroutine continues immediately and observes the error, rather
/// than parking on an operation that was never started.
///
/// `Submit` yields the operation's `OperationId` on success. The id is what a
/// later cancellation names; it is deliberately not a pointer to this awaiter,
/// which lives in a coroutine frame and must not be reachable from anything
/// that could outlive the suspension.
template<typename T, typename Submit>
class OperationAwaiter {
public:
    explicit OperationAwaiter(Submit submit) noexcept : submit_(std::move(submit)) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle) {
        const auto submitted = submit_(handle, &result_);
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
