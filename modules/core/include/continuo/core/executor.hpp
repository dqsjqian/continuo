#pragma once

// continuo/core/executor.hpp — where coroutines get resumed.
//
// Continuo does not own a thread policy. The `Executor` concept is the single
// hook a host uses to decide *which thread* runs a resumption, which is what
// lets the library sit under a GUI framework's main loop, a per-core event
// loop, or a plain thread pool without any of them knowing about the others.
//
// The concept is one function — `post(callable)`, "run this soon, not now" —
// because that is the smallest thing every scheduler already has. Timers and
// readiness waits are separate interfaces owned by the event loop, so a host
// that only wants to control thread affinity does not have to implement them.
//
// `InlineExecutor` runs work immediately on the calling thread. It exists for
// tests and for single-threaded embedding; it is not a default, because
// silently running I/O completions inline is exactly the kind of surprise this
// seam is meant to prevent.

#include "continuo/core/task.hpp"

#include <concepts>
#include <coroutine>
#include <utility>

namespace continuo {

/// A scheduler Continuo can hand resumptions to.
template<typename E>
concept Executor = requires(E& executor, void (*work)()) {
    { executor.post(work) } -> std::same_as<void>;
};

/// Executor that runs posted work synchronously on the calling thread.
class InlineExecutor {
public:
    template<typename F>
    void post(F&& work) {
        std::forward<F>(work)();
    }
};

static_assert(Executor<InlineExecutor>);

/// Awaitable that reschedules the current coroutine onto `executor`.
///
/// Everything after the `co_await` runs on whatever thread that executor
/// dispatches on:
///
///     co_await schedule_on(ui_executor);
///     // ... now on the UI thread
template<Executor E>
[[nodiscard]] auto schedule_on(E& executor) noexcept {
    class Awaiter {
    public:
        explicit Awaiter(E& target) noexcept : target_(target) {}

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> awaiting) {
            // The handle is copied by value into the posted callable: capturing
            // by reference here would dangle as soon as this awaiter dies.
            target_.post([awaiting]() mutable { awaiting.resume(); });
        }

        void await_resume() const noexcept {}

    private:
        E& target_;
    };

    return Awaiter{executor};
}

}  // namespace continuo
