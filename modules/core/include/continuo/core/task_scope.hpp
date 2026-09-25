#pragma once

#include <continuo/core/task.hpp>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace continuo {

/// 单线程结构化任务所有权：子任务立即启动，join 等待其帧全部释放。
///
/// scope、子任务完成和 stop 回调必须在同一线程执行；不提供跨线程同步。
/// stop token 只是协作信号，不会自动取消事件循环 I/O。子任务引用的资源
/// 必须存活至 join 完成。传入协程 lambda 时，调用者仍须保持闭包存活。
///
/// join 只能调用一次，调用即关闭 spawn；返回的 Task 必须被驱动至完成。
/// 从未 spawn/join 的空 scope 可直接析构；其余必须在 join 完成后析构
/// （即使 join 重抛子任务异常）。提前析构
/// 或销毁正在等待的 join 会 terminate，而不会销毁仍被 I/O 引用的子帧。
class TaskScope final {
public:
    TaskScope() = default;
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    ~TaskScope() {
        if (!joined_ && (used_ || joining_)) {
            std::terminate();
        }
    }

    /// 接管一个未启动的非空 Task；完成的子帧不会积存在 scope 内。
    void spawn(Task<void> task) {
        if (joining_) {
            throw std::logic_error("continuo::TaskScope::spawn: scope is closed");
        }
        if (!task) {
            throw std::invalid_argument(
                "continuo::TaskScope::spawn: task holds no coroutine frame");
        }
        auto runner = run_child(this, std::move(task));
        used_ = true;
        ++pending_;
        runner.start(*this);
    }

    /// 调用时关闭接纳；等待所有子任务清理后重抛第一个异常。
    [[nodiscard]] Task<void> join() {
        if (joining_) {
            throw std::logic_error("continuo::TaskScope::join: join already requested");
        }
        auto task = wait_for_children(this);
        joining_ = true;
        return task;
    }

    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }

    [[nodiscard]] std::stop_token get_stop_token() const noexcept {
        return stop_source_.get_token();
    }

    bool request_stop() noexcept {
        // 回调可同步完成最后一个子任务，继而恢复父任务并销毁 scope。
        // 局部副本使 stop 状态独立存活，回调后不再访问 this。
        auto source = stop_source_;
        return source.request_stop();
    }

private:
    // 仅作 Task 的启动/完成桥接，不公开第二套异步任务 API。
    struct Runner {
        struct promise_type {
            TaskScope* scope{};

            Runner get_return_object() noexcept {
                return Runner{std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() const noexcept { return {}; }

            struct FinalAwaiter {
                bool await_ready() const noexcept { return false; }

                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<promise_type> self) const noexcept {
                    auto* owner = self.promise().scope;
                    // 已到最终挂起边界。必须先做计数与续体结算、再销毁
                    // runner 帧：MSVC 的优化器会把续体/owner 的读取落到
                    // 已销毁的帧里（heap-use-after-free，协程省略后更明
                    // 显）。结算结果与 owner 之外不再触碰本帧，销毁之后
                    // 仅作对称转移。
                    auto continuation = owner->child_completed();
                    self.destroy();
                    return continuation;
                }

                void await_resume() const noexcept {}
            };

            FinalAwaiter final_suspend() const noexcept { return {}; }
            void return_void() const noexcept {}
            void unhandled_exception() const noexcept { std::terminate(); }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;

        explicit Runner(Handle value) noexcept : handle(value) {}
        Runner(const Runner&) = delete;
        Runner& operator=(const Runner&) = delete;
        ~Runner() {
            if (handle) {
                handle.destroy();
            }
        }

        void start(TaskScope& scope) noexcept {
            handle.promise().scope = &scope;
            auto running = std::exchange(handle, {});
            // Defeat MSVC's coroutine frame fusion (HALO). run_child's only
            // suspend is the child task itself, so MSVC fuses the two frames
            // into one allocation; the Task awaiter then frees that block
            // while run_child is still executing on it. Storing the frame
            // address into the scope makes the allocation observable and
            // keeps the frames separate. Harmless on every other compiler.
            scope.frame_guard_ = running.address();
            running.resume();
        }
    };

    struct JoinAwaiter {
        TaskScope* scope;
        bool waiting{false};

        ~JoinAwaiter() {
            if (waiting) {
                std::terminate();
            }
        }

        bool await_ready() const noexcept { return scope->pending_ == 0; }

        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            waiting = true;
            scope->waiter_ = continuation;
        }

        void await_resume() noexcept { waiting = false; }
    };

    static Runner run_child(TaskScope* scope, Task<void> task) {
        try {
            // Task 的拥有型 awaiter 在完整表达式结束时释放子任务帧。
            co_await std::move(task);
        } catch (...) {
            if (!scope->failure_) {
                scope->failure_ = std::current_exception();
                scope->request_stop();
            }
        }
    }

    static Task<void> wait_for_children(TaskScope* scope) {
        co_await JoinAwaiter{scope};
        scope->joined_ = true;
        if (scope->failure_) {
            std::rethrow_exception(scope->failure_);
        }
    }

    std::coroutine_handle<> child_completed() noexcept {
        --pending_;
        if (pending_ == 0 && waiter_) {
            return std::exchange(waiter_, {});
        }
        return std::noop_coroutine();
    }

    std::stop_source stop_source_;
    std::exception_ptr failure_;
    void* frame_guard_ = nullptr;  // MSVC HALO escape hatch, see Runner::start
    std::coroutine_handle<> waiter_{};
    std::size_t pending_{0};
    bool used_{false};
    bool joining_{false};
    bool joined_{false};
};

}  // namespace continuo
