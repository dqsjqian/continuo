// Core smoke tests: error model, Task, Buffer, and the stream seam.
//
// The stream test is the load-bearing one. `MemoryStream` is not a socket, yet
// `write_all` — written against the concept — drives it unchanged. That is the
// architectural claim of the whole project, asserted in code rather than in a
// design document.

#include "check.hpp"
#include "continuo/core/buffer.hpp"
#include "continuo/core/error.hpp"
#include "continuo/core/executor.hpp"
#include "continuo/core/stream.hpp"
#include "continuo/core/task.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace continuo;

namespace {

std::span<const std::byte> bytes_of(std::string_view text) {
    return std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string to_string(std::span<const std::byte> bytes) {
    return std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// ── error model ──────────────────────────────────────────────────────────────

void test_error_model() {
    test::section("error model");

    const Error eof = make_error_code(Errc::eof);
    CHECK(eof.category() == continuo_category());
    CHECK(eof.value() == static_cast<int>(Errc::eof));
    CHECK(eof.message() == "stream closed by peer");

    // Implicit conversion through is_error_code_enum.
    const Error timeout = Errc::timed_out;
    CHECK(timeout == Errc::timed_out);

    // Continuo conditions compare equal to their portable std::errc peers.
    CHECK(timeout == std::errc::timed_out);
    CHECK(make_error_code(Errc::would_block) == std::errc::operation_would_block);

    Result<int> ok{7};
    CHECK(ok.has_value());
    CHECK(static_cast<bool>(ok));
    CHECK(ok.value() == 7);

    Result<int> bad = fail(Errc::limit_exceeded);
    CHECK(!bad.has_value());
    CHECK(!static_cast<bool>(bad));
    CHECK(bad.error() == Errc::limit_exceeded);
    CHECK(bad.value_or(-1) == -1);

    Result<void> void_ok{};
    CHECK(void_ok.has_value());

    Result<void> void_bad = fail(Errc::cancelled);
    CHECK(!void_bad.has_value());
    CHECK(void_bad.error() == Errc::cancelled);
}

// ── Task ─────────────────────────────────────────────────────────────────────

Task<int> answer() {
    co_return 42;
}

Task<int> doubled(int input) {
    const int value = co_await answer();
    co_return value* input;
}

Task<void> no_value() {
    co_return;
}

Task<int> throws_inside() {
    throw std::runtime_error("boom");
    co_return 0;  // unreachable, keeps this a coroutine
}

Task<int> propagates_failure() {
    const int value = co_await throws_inside();
    co_return value;
}

Task<void> suspends_forever() {
    co_await std::suspend_always{};
    co_return;
}

void test_task() {
    test::section("Task");

    CHECK(answer().sync_get() == 42);
    CHECK(doubled(2).sync_get() == 84);

    // Lazy: constructing a task must not run its body.
    bool ran = false;
    auto record = [](bool& flag) -> Task<void> {
        flag = true;
        co_return;
    };
    Task<void> pending = record(ran);
    CHECK(!ran);
    std::move(pending).sync_get();
    CHECK(ran);

    no_value().sync_get();

    // A body exception surfaces at the awaiting frame, not at suspension.
    CHECK_THROWS(throws_inside().sync_get(), std::runtime_error);
    CHECK_THROWS(propagates_failure().sync_get(), std::runtime_error);

    // sync_get refuses to pretend a suspended task finished.
    CHECK_THROWS(suspends_forever().sync_get(), std::logic_error);

    // Move-only ownership: the moved-from task must be empty.
    Task<int> source = answer();
    CHECK(static_cast<bool>(source));
    Task<int> sink = std::move(source);
    CHECK(!static_cast<bool>(source));
    CHECK(std::move(sink).sync_get() == 42);
}

// ── Buffer ───────────────────────────────────────────────────────────────────

void test_buffer() {
    test::section("Buffer");

    Buffer buffer;
    CHECK(buffer.empty());

    buffer.append(bytes_of("GET / HTTP/1.1\r\n"));
    CHECK(buffer.size() == 16);
    CHECK(to_string(buffer.readable()) == "GET / HTTP/1.1\r\n");

    buffer.consume(6);
    CHECK(to_string(buffer.readable()) == "HTTP/1.1\r\n");

    // prepare/commit with a short read: only committed bytes become readable.
    std::span<std::byte> writable = buffer.prepare(64);
    CHECK(writable.size() == 64);
    std::memcpy(writable.data(), "Host: x\r\n", 9);
    buffer.commit(9);
    CHECK(to_string(buffer.readable()) == "HTTP/1.1\r\nHost: x\r\n");

    // Draining everything resets the cursors and keeps the allocation.
    const std::size_t capacity_before = buffer.capacity();
    buffer.consume(buffer.size());
    CHECK(buffer.empty());
    CHECK(buffer.capacity() == capacity_before);

    // consume() past the end clamps instead of underflowing.
    buffer.append(bytes_of("abc"));
    buffer.consume(99);
    CHECK(buffer.empty());

    // Reuse across many rounds must not grow without bound: the consumed
    // prefix is reclaimed rather than leaked.
    Buffer reused{128};
    const std::size_t steady_capacity = reused.capacity();
    for (int round = 0; round < 1000; ++round) {
        std::span<std::byte> chunk = reused.prepare(64);
        std::memset(chunk.data(), 'x', 64);
        reused.commit(64);
        reused.consume(64);
    }
    CHECK(reused.empty());
    CHECK(reused.capacity() <= steady_capacity * 4);
}

// ── stream seam ──────────────────────────────────────────────────────────────

/// In-memory stream that accepts at most `chunk_limit` bytes per write, so the
/// short-write path in `write_all` is actually exercised.
class MemoryStream {
public:
    explicit MemoryStream(std::size_t chunk_limit) : chunk_limit_(chunk_limit) {}

    Task<Result<std::size_t>> read_some(std::span<std::byte> destination) {
        if (read_pos_ >= written_.size()) {
            co_return fail(Errc::eof);
        }
        const std::size_t available = written_.size() - read_pos_;
        const std::size_t n = std::min({available, destination.size(), chunk_limit_});
        std::memcpy(destination.data(), written_.data() + read_pos_, n);
        read_pos_ += n;
        co_return n;
    }

    Task<Result<std::size_t>> write_some(std::span<const std::byte> source) {
        const std::size_t n = std::min(source.size(), chunk_limit_);
        written_.insert(
            written_.end(), source.begin(), source.begin() + static_cast<std::ptrdiff_t>(n));
        ++write_calls_;
        co_return n;
    }

    [[nodiscard]] std::string contents() const {
        return to_string(std::span<const std::byte>{written_.data(), written_.size()});
    }

    [[nodiscard]] int write_calls() const noexcept { return write_calls_; }

private:
    std::size_t chunk_limit_;
    std::vector<std::byte> written_{};
    std::size_t read_pos_{0};
    int write_calls_{0};
};

static_assert(AsyncReadStream<MemoryStream>);
static_assert(AsyncWriteStream<MemoryStream>);
static_assert(AsyncStream<MemoryStream>);

Task<Result<void>> round_trip(MemoryStream& stream, std::string_view payload) {
    Result<void> written = co_await write_all(stream, bytes_of(payload));
    if (!written) {
        co_return fail(written.error());
    }
    co_return Result<void>{};
}

void test_stream_seam() {
    test::section("stream seam");

    // write_all was written against the concept; it drives a non-socket
    // stream unchanged, looping over short writes.
    MemoryStream stream{4};
    Result<void> result = round_trip(stream, "hello continuo").sync_get();
    CHECK(result.has_value());
    CHECK(stream.contents() == "hello continuo");
    CHECK(stream.write_calls() == 4);  // 14 bytes / 4-byte chunks

    // Reading drains the same bytes, then reports a clean close.
    auto drain = [](MemoryStream& source) -> Task<Result<std::string>> {
        std::string out;
        std::byte scratch[8];
        for (;;) {
            Result<std::size_t> chunk = co_await source.read_some(std::span<std::byte>{scratch, 8});
            if (!chunk) {
                if (chunk.error() == Errc::eof) {
                    break;
                }
                co_return fail(chunk.error());
            }
            out.append(reinterpret_cast<const char*>(scratch), *chunk);
        }
        co_return out;
    };

    Result<std::string> drained = drain(stream).sync_get();
    CHECK(drained.has_value());
    CHECK(drained.value() == "hello continuo");
}

// ── executor seam ────────────────────────────────────────────────────────────

/// Executor that defers work until the host pumps it.
///
/// Only the queueing contract is asserted here. Driving a *coroutine* across a
/// deferred executor needs an owner that outlives the suspension — that is the
/// event loop's job, and it arrives with the event loop rather than being
/// faked here with a task whose frame `sync_get()` would tear down while a
/// queued resumption still points at it.
class QueuedExecutor {
public:
    void post(std::function<void()> work) { queue_.push_back(std::move(work)); }

    void pump() {
        // Swap before running: resumed work is free to post more, and mutating
        // the queue while iterating it would invalidate the iterators.
        std::vector<std::function<void()>> batch;
        batch.swap(queue_);
        for (auto& work : batch) {
            work();
        }
    }

    [[nodiscard]] std::size_t pending() const noexcept { return queue_.size(); }

private:
    std::vector<std::function<void()>> queue_{};
};

static_assert(Executor<QueuedExecutor>);

void test_executor_seam() {
    test::section("executor seam");

    // Inline: the host has chosen "run it now, on this thread".
    InlineExecutor inline_executor;
    bool ran = false;
    inline_executor.post([&ran] { ran = true; });
    CHECK(ran);

    // A coroutine hopping onto an inline executor completes synchronously,
    // and everything after the co_await observes the new stage.
    int stage = 0;
    auto hop = [](InlineExecutor& target, int& progress) -> Task<void> {
        progress = 1;
        co_await schedule_on(target);
        progress = 2;
        co_return;
    };
    hop(inline_executor, stage).sync_get();
    CHECK(stage == 2);

    // Queued: nothing runs until the host says so.
    QueuedExecutor queued;
    int calls = 0;
    queued.post([&calls] { ++calls; });
    CHECK(calls == 0);
    CHECK(queued.pending() == 1);
    queued.pump();
    CHECK(calls == 1);
    CHECK(queued.pending() == 0);
}

}  // namespace

int main() {
    test_error_model();
    test_task();
    test_buffer();
    test_stream_seam();
    test_executor_seam();
    return test::summary();
}
