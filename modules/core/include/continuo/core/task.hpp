#pragma once

// continuo/core/task.hpp — the coroutine type every Continuo API speaks.
//
// `Task<T>` is a *lazy* coroutine: the body does not start until the task is
// awaited (or driven by `sync_get()`). Awaiting resumes the awaited task on the
// current thread via symmetric transfer, so a chain of `co_await`s costs no
// extra stack frames and no allocation beyond the coroutine frames themselves.
//
// Ownership: `Task<T>` is move-only and owns its coroutine frame. Awaiting a
// task consumes it (`operator co_await() &&` moves the handle into the
// awaiter), which is what keeps a temporary `Task` from destroying a frame
// that is still running.
//
// Deliberately absent from v0.1: `then()`-style continuations, detached
// launching, and cancellation plumbing. They belong with the executor and
// event loop, which land together in a later milestone.

#include <coroutine>
#include <exception>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace continuo {

template<typename T = void>
class Task;

namespace detail {

/// Shared promise state: the continuation to resume once the body finishes.
///
/// `FinalAwaiter::await_suspend` is a template so that a
/// `coroutine_handle<DerivedPromise>` can be accepted and up-cast to this base
/// — a non-template overload taking `coroutine_handle<TaskPromiseBase>` would
/// not be convertible from the derived handle.
struct TaskPromiseBase {
    std::coroutine_handle<> continuation{};

    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template<typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept {
            TaskPromiseBase& base = self.promise();
            return base.continuation ? base.continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
    [[nodiscard]] FinalAwaiter final_suspend() noexcept { return {}; }
};

template<typename T>
struct TaskPromise final : TaskPromiseBase {
    std::optional<T> value{};
    std::exception_ptr failure{};

    Task<T> get_return_object() noexcept;

    template<typename U = T>
        requires std::is_constructible_v<T, U&&>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }

    void unhandled_exception() noexcept { failure = std::current_exception(); }

    /// Hand the result to the awaiter, rethrowing a body exception if any.
    T&& result() && {
        if (failure) {
            std::rethrow_exception(failure);
        }
        return std::move(*value);
    }
};

template<>
struct TaskPromise<void> final : TaskPromiseBase {
    std::exception_ptr failure{};

    Task<void> get_return_object() noexcept;

    void return_void() noexcept {}
    void unhandled_exception() noexcept { failure = std::current_exception(); }

    void result() && {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
};

}  // namespace detail

/// Lazy, move-only coroutine handle used by every asynchronous Continuo API.
template<typename T>
class Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;
    using value_type = T;

    Task() noexcept = default;
    explicit Task(handle_type handle) noexcept : handle_(handle) {}

    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, {});
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { destroy(); }

    /// True when the task holds no frame, or its body has run to completion.
    [[nodiscard]] bool done() const noexcept { return !handle_ || handle_.done(); }

    /// True when the task owns a coroutine frame.
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handle_); }

    /// Await the task, consuming it.
    ///
    /// The awaiter takes ownership of the frame, so the awaited expression may
    /// be a temporary without the frame being destroyed while it still runs.
    auto operator co_await() && noexcept {
        class Awaiter {
        public:
            explicit Awaiter(handle_type handle) noexcept : handle_(handle) {}

            Awaiter(Awaiter&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
            Awaiter(const Awaiter&) = delete;
            Awaiter& operator=(const Awaiter&) = delete;
            Awaiter& operator=(Awaiter&&) = delete;

            ~Awaiter() {
                if (handle_) {
                    handle_.destroy();
                }
            }

            [[nodiscard]] bool await_ready() const noexcept { return !handle_ || handle_.done(); }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
                handle_.promise().continuation = awaiting;
                return handle_;
            }

            decltype(auto) await_resume() {
                if (!handle_) {
                    throw std::logic_error("continuo::Task: cannot await an empty task");
                }
                return std::move(handle_.promise()).result();
            }

        private:
            handle_type handle_;
        };

        return Awaiter{std::exchange(handle_, {})};
    }

    /// Drive a task that completes without ever suspending, and take its value.
    ///
    /// Intended for tests and for synchronous entry points. A task that
    /// suspends on real I/O will *not* complete here — that needs an event
    /// loop, and this function throws `std::logic_error` rather than silently
    /// returning a half-built result.
    decltype(auto) sync_get() && {
        handle_type handle = std::exchange(handle_, {});
        if (!handle) {
            throw std::logic_error("continuo::Task::sync_get: task holds no coroutine frame");
        }

        struct FrameGuard {
            handle_type handle;
            ~FrameGuard() {
                if (handle) {
                    handle.destroy();
                }
            }
        } guard{handle};

        handle.resume();
        if (!handle.done()) {
            throw std::logic_error(
                "continuo::Task::sync_get: task suspended; drive it on an event loop instead");
        }

        if constexpr (std::is_void_v<T>) {
            std::move(handle.promise()).result();
            return;
        } else {
            T value = std::move(handle.promise()).result();
            return value;
        }
    }

private:
    void destroy() noexcept {
        if (handle_) {
            handle_.destroy();
            handle_ = {};
        }
    }

    handle_type handle_{};
};

namespace detail {

template<typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}

}  // namespace detail

}  // namespace continuo
