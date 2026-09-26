#pragma once

#include <Mira/core/task.hpp>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <stop_token>
#include <utility>
#include <vector>

namespace Mira {

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
        // 退休队列里只剩已完成的 runner 帧；此刻其执行链早已返回。
        for (auto runner : retired_) {
            runner.destroy();
        }
    }

    /// 接管一个未启动的非空 Task；完成的子帧不会积存在 scope 内。
    void spawn(Task<void> task) {
        if (joining_) {
            throw std::logic_error("Mira::TaskScope::spawn: scope is closed");
        }
        if (!task) {
            throw std::invalid_argument(
                "Mira::TaskScope::spawn: task holds no coroutine frame");
        }
        auto runner = run_child(this, std::move(task));
        used_ = true;
        ++pending_;
        runner.start(*this);
    }

    /// 调用时关闭接纳；等待所有子任务清理后重抛第一个异常。
    [[nodiscard]] Task<void> join() {
        if (joining_) {
            throw std::logic_error("Mira::TaskScope::join: join already requested");
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
            bool retired = false;

            Runner get_return_object() noexcept {
                return Runner{std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            std::suspend_always initial_suspend() const noexcept { return {}; }

            struct FinalAwaiter {
                bool await_ready() const noexcept { return false; }

                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<promise_type> self) const noexcept {
                    // 已到最终挂起边界。 runner 帧绝不能在这里销毁：本帧
                    // 里还驻留着 co_await 子任务时的唤醒机器码与局部状态，
                    // MSVC（含协程帧合并优化）会在销毁后的收尾路径上触碰
                    // 它们（ASan heap-use-after-free）。帧移交给 scope 的
                    // 退休队列，由 spawn 的 resume 返回后或 scope 析构时
                    // 在安全点销毁。
                    auto* owner = self.promise().scope;
                    self.promise().retired = true;
                    owner->retire(self);
                    return owner->child_completed();
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
            scope.frame_guard_ = running.address();
            running.resume();
            // 子任务同步完成时（帧已入退休队列），resume 已经返回，此处在
            // 栈上安全销毁；异步完成的帧由 scope 析构统一销毁。
            if (running && running.promise().retired) {
                scope.reclaim(running);
            }
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
    std::vector<std::coroutine_handle<>> retired_;

    /// runner 帧登记退休：不在自身执行链里销毁，由安全点统一回收。
    void retire(std::coroutine_handle<> runner) {
        retired_.push_back(runner);
    }

    /// start 的 resume 返回后回收同步完成的 runner 帧。
    void reclaim(std::coroutine_handle<> runner) {
        for (std::size_t i = retired_.size(); i-- > 0;) {
            if (retired_[i] == runner) {
                retired_.erase(retired_.begin() + static_cast<long>(i));
                runner.destroy();
                return;
            }
        }
        runner.destroy();  // 不在队列里（例如同步完成未入队），直接销毁
    }
    std::coroutine_handle<> waiter_{};
    std::size_t pending_{0};
    bool used_{false};
    bool joining_{false};
    bool joined_{false};
};

}  // namespace Mira
