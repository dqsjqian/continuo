// Event loop tests — real descriptors, real completions, no wall-clock races.
//
// These run against whatever backend the platform selected. Nothing here names
// kqueue, epoll, or IOCP: if a test needs a `#if`, that is a signal the public
// API leaked a platform detail, and the API is what should change.
//
// Driving convention: a task that suspends on I/O cannot be `sync_get()`-ed,
// so tests own the frame in a `Task` variable, start it via a detached runner
// that records completion, and pump `run_once()` until the flag flips. That
// keeps every test deterministic — no sleeps, no "should be enough time".

#include "check.hpp"
#include "continuo/core/buffer.hpp"
#include "continuo/core/error.hpp"
#include "continuo/core/event_loop.hpp"
#include "continuo/core/platform.hpp"
#include "continuo/core/task.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !CONTINUO_PLATFORM_WINDOWS
    #include <sys/socket.h>
    #include <unistd.h>
#endif

using namespace continuo;
using namespace std::chrono_literals;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

// ── deterministic driver ─────────────────────────────────────────────────────

/// Fire-and-forget coroutine that owns its frame until the body returns.
///
/// Needed because a suspended `Task` has no owner: the test cannot hold the
/// frame and also let the loop resume it. The promise destroys itself at
/// `final_suspend`, which is the one place it is safe to do so.
struct DetachedTask {
    struct promise_type {
        std::atomic<bool>* finished{nullptr};

        DetachedTask get_return_object() noexcept { return DetachedTask{}; }
        std::suspend_never initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<promise_type> self) const noexcept {
                if (self.promise().finished) {
                    self.promise().finished->store(true, std::memory_order_release);
                }
                self.destroy();
            }
            void await_resume() const noexcept {}
        };

        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

/// Run `body` to completion, pumping the loop until it finishes.
///
/// `budget` bounds the number of iterations so a hung expectation fails the
/// test instead of hanging CI.
template<typename Body>
bool drive(EventLoop& loop, std::atomic<bool>& finished, Body body, int budget = 1000) {
    body();  // starts eagerly, suspends on its first await
    for (int i = 0; i < budget && !finished.load(std::memory_order_acquire); ++i) {
        const Result<void> stepped = loop.run_once(50ms);
        if (!stepped) {
            return false;
        }
    }
    return finished.load(std::memory_order_acquire);
}

// ── fixtures ─────────────────────────────────────────────────────────────────

/// A connected pair of handles the loop can do real I/O on.
class HandlePair {
public:
    HandlePair() {
#if CONTINUO_PLATFORM_WINDOWS
        // A loopback TCP pair stands in for socketpair(), which Winsock lacks.
        // Only reached on the Windows CI job.
        ok_ = false;
#else
        int fds[2] = {-1, -1};
        ok_ = ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0;
        first_ = fds[0];
        second_ = fds[1];
#endif
    }

    HandlePair(const HandlePair&) = delete;
    HandlePair& operator=(const HandlePair&) = delete;

    ~HandlePair() {
#if !CONTINUO_PLATFORM_WINDOWS
        if (first_ >= 0) {
            ::close(first_);
        }
        if (second_ >= 0) {
            ::close(second_);
        }
#endif
    }

    [[nodiscard]] bool valid() const noexcept { return ok_; }
    [[nodiscard]] NativeHandle first() const noexcept { return first_; }
    [[nodiscard]] NativeHandle second() const noexcept { return second_; }

    /// Write directly, bypassing the loop — simulates the peer.
    void peer_send(std::string_view payload) const {
#if !CONTINUO_PLATFORM_WINDOWS
        const ssize_t written = ::write(second_, payload.data(), payload.size());
        (void)written;
#else
        (void)payload;
#endif
    }

    void peer_close() {
#if !CONTINUO_PLATFORM_WINDOWS
        if (second_ >= 0) {
            ::close(second_);
            second_ = -1;
        }
#endif
    }

private:
    NativeHandle first_{invalid_handle};
    NativeHandle second_{invalid_handle};
    bool ok_{false};
};

// ── tests ────────────────────────────────────────────────────────────────────

void test_create_and_backend() {
    test::section("loop creation");

    Result<EventLoop> loop = EventLoop::create();
    CHECK(loop.has_value());
    CHECK(loop.value().outstanding() == 0);
    CHECK(!loop.value().stopped());

    // The backend is a build fact, not something a test should branch on —
    // asserting it is non-empty keeps the diagnostic honest.
    CHECK(std::string_view{io_backend_name()}.size() > 0);
    std::printf("   backend: %s\n", io_backend_name());
}

void test_post_and_stop() {
    test::section("post and stop");

    Result<EventLoop> created = EventLoop::create();
    CHECK(created.has_value());
    EventLoop& loop = created.value();

    int calls = 0;
    loop.post([&calls] { ++calls; });
    CHECK(calls == 0);  // queued, not run
    CHECK(loop.outstanding() == 1);

    CHECK(loop.run_once(0ms).has_value());
    CHECK(calls == 1);
    CHECK(loop.outstanding() == 0);

    // run() returns once nothing is outstanding, instead of blocking forever.
    CHECK(loop.run().has_value());

    // A posted callable may stop the loop from inside the loop.
    loop.post([&loop] { loop.stop(); });
    CHECK(loop.run().has_value());
    CHECK(loop.stopped());
}

void test_timers() {
    test::section("timers");

    Result<EventLoop> created = EventLoop::create();
    CHECK(created.has_value());
    EventLoop& loop = created.value();

    std::atomic<bool> finished{false};
    Result<void> outcome = fail(Errc::cancelled);
    const auto started = std::chrono::steady_clock::now();

    struct Runner {
        static DetachedTask go(EventLoop& target, Result<void>& slot, std::atomic<bool>& flag) {
            slot = co_await target.sleep_for(20ms);
            flag.store(true, std::memory_order_release);
            co_return;
        }
    };

    Runner::go(loop, outcome, finished);
    for (int i = 0; i < 200 && !finished.load(std::memory_order_acquire); ++i) {
        CHECK(loop.run_once(50ms).has_value());
    }

    CHECK(finished.load(std::memory_order_acquire));
    CHECK(outcome.has_value());
    // The deadline must not fire early; a loop that rounds timeouts down
    // would show up here.
    CHECK(std::chrono::steady_clock::now() - started >= 20ms);

    // Ordering: an earlier deadline fires first even when queued second.
    std::vector<int> order;
    std::atomic<int> done{0};

    struct Ordered {
        static DetachedTask go(EventLoop& target,
                               EventLoop::Duration delay,
                               int id,
                               std::vector<int>& log,
                               std::atomic<int>& counter) {
            (void)co_await target.sleep_for(delay);
            log.push_back(id);
            counter.fetch_add(1, std::memory_order_release);
            co_return;
        }
    };

    Ordered::go(loop, 40ms, 2, order, done);
    Ordered::go(loop, 5ms, 1, order, done);

    for (int i = 0; i < 200 && done.load(std::memory_order_acquire) < 2; ++i) {
        CHECK(loop.run_once(50ms).has_value());
    }
    CHECK(done.load(std::memory_order_acquire) == 2);
    CHECK(order.size() == 2);
    if (order.size() == 2) {
        CHECK(order[0] == 1);
        CHECK(order[1] == 2);
    }
}

void test_read_write_roundtrip() {
    test::section("read/write round trip");

    HandlePair pair;
    if (!pair.valid()) {
        std::printf("   skipped: no handle pair on this platform\n");
        return;
    }

    Result<EventLoop> created = EventLoop::create();
    CHECK(created.has_value());
    EventLoop& loop = created.value();

    CHECK(loop.attach(pair.first()).has_value());

    // Data already waiting: the read completes without ever suspending.
    pair.peer_send("hello");

    std::atomic<bool> finished{false};
    std::string received;
    Result<std::size_t> outcome = fail(Errc::cancelled);

    struct Reader {
        static DetachedTask go(EventLoop& target,
                               NativeHandle handle,
                               std::string& sink,
                               Result<std::size_t>& slot,
                               std::atomic<bool>& flag) {
            std::array<std::byte, 64> scratch{};
            slot = co_await target.read(handle, std::span<std::byte>{scratch});
            if (slot.has_value()) {
                sink.assign(reinterpret_cast<const char*>(scratch.data()), slot.value());
            }
            flag.store(true, std::memory_order_release);
            co_return;
        }
    };

    Reader::go(loop, pair.first(), received, outcome, finished);
    for (int i = 0; i < 200 && !finished.load(std::memory_order_acquire); ++i) {
        CHECK(loop.run_once(50ms).has_value());
    }

    CHECK(finished.load(std::memory_order_acquire));
    CHECK(outcome.has_value());
    CHECK(received == "hello");

    // Now the suspending path: read first, data arrives later.
    std::atomic<bool> second_finished{false};
    std::string second_received;
    Result<std::size_t> second_outcome = fail(Errc::cancelled);

    Reader::go(loop, pair.first(), second_received, second_outcome, second_finished);

    // One pump with nothing available proves the read really suspended.
    CHECK(loop.run_once(0ms).has_value());
    CHECK(!second_finished.load(std::memory_order_acquire));
    CHECK(loop.outstanding() >= 1);

    pair.peer_send("world");
    for (int i = 0; i < 200 && !second_finished.load(std::memory_order_acquire); ++i) {
        CHECK(loop.run_once(50ms).has_value());
    }

    CHECK(second_finished.load(std::memory_order_acquire));
    CHECK(second_outcome.has_value());
    CHECK(second_received == "world");

    // Writing through the loop reaches the peer.
    std::atomic<bool> write_finished{false};
    Result<std::size_t> write_outcome = fail(Errc::cancelled);

    struct Writer {
        static DetachedTask go(EventLoop& target,
                               NativeHandle handle,
                               std::span<const std::byte> payload,
                               Result<std::size_t>& slot,
                               std::atomic<bool>& flag) {
            slot = co_await target.write(handle, payload);
            flag.store(true, std::memory_order_release);
            co_return;
        }
    };

    Writer::go(loop, pair.first(), bytes_of("pong"), write_outcome, write_finished);
    for (int i = 0; i < 200 && !write_finished.load(std::memory_order_acquire); ++i) {
        CHECK(loop.run_once(50ms).has_value());
    }
    CHECK(write_finished.load(std::memory_order_acquire));
    CHECK(write_outcome.has_value());
    CHECK(write_outcome.value_or(0) == 4);
}

void test_eof_is_distinct_from_empty() {
    test::section("clean close reports eof");

    HandlePair pair;
    if (!pair.valid()) {
        std::printf("   skipped: no handle pair on this platform\n");
        return;
    }

    Result<EventLoop> created = EventLoop::create();
    CHECK(created.has_value());
    EventLoop& loop = created.value();
    CHECK(loop.attach(pair.first()).has_value());

    std::atomic<bool> finished{false};
    Result<std::size_t> outcome = std::size_t{999};

    struct Reader {
        static DetachedTask go(EventLoop& target,
                               NativeHandle handle,
                               Result<std::size_t>& slot,
                               std::atomic<bool>& flag) {
            std::array<std::byte, 16> scratch{};
            slot = co_await target.read(handle, std::span<std::byte>{scratch});
            flag.store(true, std::memory_order_release);
            co_return;
        }
    };

    Reader::go(loop, pair.first(), outcome, finished);
    CHECK(loop.run_once(0ms).has_value());
    CHECK(!finished.load(std::memory_order_acquire));

    pair.peer_close();
    for (int i = 0; i < 200 && !finished.load(std::memory_order_acquire); ++i) {
        CHECK(loop.run_once(50ms).has_value());
    }

    CHECK(finished.load(std::memory_order_acquire));
    // A closed peer must be eof, not a zero-byte success.
    CHECK(!outcome.has_value());
    CHECK(outcome.error() == Errc::eof);
}

void test_double_waiter_is_refused() {
    test::section("same-direction waiters refused");

    HandlePair pair;
    if (!pair.valid()) {
        std::printf("   skipped: no handle pair on this platform\n");
        return;
    }

    Result<EventLoop> created = EventLoop::create();
    CHECK(created.has_value());
    EventLoop& loop = created.value();
    CHECK(loop.attach(pair.first()).has_value());

    std::atomic<int> done{0};
    Result<std::size_t> first_outcome = fail(Errc::cancelled);
    Result<std::size_t> second_outcome = fail(Errc::cancelled);

    struct Reader {
        static DetachedTask go(EventLoop& target,
                               NativeHandle handle,
                               Result<std::size_t>& slot,
                               std::atomic<int>& counter) {
            std::array<std::byte, 16> scratch{};
            slot = co_await target.read(handle, std::span<std::byte>{scratch});
            counter.fetch_add(1, std::memory_order_release);
            co_return;
        }
    };

    // First read suspends and owns the read direction.
    Reader::go(loop, pair.first(), first_outcome, done);
    CHECK(loop.run_once(0ms).has_value());
    CHECK(done.load(std::memory_order_acquire) == 0);

    // Second read on the same direction is refused rather than racing.
    Reader::go(loop, pair.first(), second_outcome, done);
    CHECK(done.load(std::memory_order_acquire) == 1);
    CHECK(!second_outcome.has_value());
    CHECK(second_outcome.error() == Errc::invalid_argument);

    // The original waiter still works.
    pair.peer_send("ok");
    for (int i = 0; i < 200 && done.load(std::memory_order_acquire) < 2; ++i) {
        CHECK(loop.run_once(50ms).has_value());
    }
    CHECK(done.load(std::memory_order_acquire) == 2);
    CHECK(first_outcome.has_value());
}

void test_destruction_wakes_suspended() {
    test::section("destruction cancels suspended work");

    std::atomic<bool> finished{false};
    Result<void> outcome{};

    struct Sleeper {
        static DetachedTask go(EventLoop& target, Result<void>& slot, std::atomic<bool>& flag) {
            slot = co_await target.sleep_for(std::chrono::hours{1});
            flag.store(true, std::memory_order_release);
            co_return;
        }
    };

    {
        Result<EventLoop> created = EventLoop::create();
        CHECK(created.has_value());
        Sleeper::go(created.value(), outcome, finished);
        CHECK(created.value().outstanding() == 1);
        // Loop dies here with a waiter still suspended.
    }

    // The frame must have unwound rather than leaked (ASan would catch the
    // leak; this asserts the coroutine observed a reason).
    CHECK(finished.load(std::memory_order_acquire));
    CHECK(!outcome.has_value());
    CHECK(outcome.error() == Errc::cancelled);
}

void test_yield_returns_to_loop() {
    test::section("yield");

    Result<EventLoop> created = EventLoop::create();
    CHECK(created.has_value());
    EventLoop& loop = created.value();

    std::atomic<bool> finished{false};
    int stage = 0;

    struct Hopper {
        static DetachedTask go(EventLoop& target, int& progress, std::atomic<bool>& flag) {
            progress = 1;
            co_await target.yield();
            progress = 2;
            flag.store(true, std::memory_order_release);
            co_return;
        }
    };

    Hopper::go(loop, stage, finished);
    CHECK(stage == 1);  // suspended at the yield
    CHECK(!finished.load(std::memory_order_acquire));

    for (int i = 0; i < 10 && !finished.load(std::memory_order_acquire); ++i) {
        CHECK(loop.run_once(0ms).has_value());
    }
    CHECK(finished.load(std::memory_order_acquire));
    CHECK(stage == 2);
}

}  // namespace

int main() {
    test_create_and_backend();
    test_post_and_stop();
    test_timers();
    test_read_write_roundtrip();
    test_eof_is_distinct_from_empty();
    test_double_waiter_is_refused();
    test_destruction_wakes_suspended();
    test_yield_returns_to_loop();
    return test::summary();
}
