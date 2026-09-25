#include "check.hpp"
#include "continuo/core/stream.hpp"
#include "continuo/transport/udp.hpp"

#include <array>
#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

#if CONTINUO_HAS_READINESS_API
    #include <poll.h>
#endif

using namespace continuo;
using namespace continuo::transport;
using namespace std::chrono_literals;

namespace {
static_assert(!AsyncStream<udp::Socket>);

std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
OperationOptions budget() {
    return {.deadline = EventLoop::Clock::now() + 2s};
}

struct Detached {
    struct promise_type {
        Detached get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { std::terminate(); }
    };
};
template<class T>
Detached collect(Task<T> task, std::optional<T>& result) {
    result.emplace(co_await std::move(task));
}
void drain(EventLoop& loop) {
    const auto end = EventLoop::Clock::now() + 3s;
    while (loop.outstanding() && EventLoop::Clock::now() < end)
        CHECK(loop.run_once(10ms).has_value());
    CHECK(loop.outstanding() == 0);
}

Task<void> packets(EventLoop& loop, Family family) {
    auto a = udp::Socket::bind(loop, Endpoint::loopback(0, family));
    auto b = udp::Socket::bind(loop, Endpoint::loopback(0, family));
    CHECK(a.has_value());
    CHECK(b.has_value());
    if (!a || !b) co_return;
    const Endpoint peer = b->local_endpoint().value();
    CHECK(peer.port() != 0);
    CHECK(peer.family() == family);
    auto duplicate = udp::Socket::bind(loop, peer);
    CHECK(!duplicate);
    if (!duplicate) CHECK(duplicate.error() == std::errc::address_in_use);
    auto wildcard = udp::Socket::bind(loop, Endpoint::any(peer.port(), family));
    CHECK(!wildcard);

    // 延迟启动任务必须保留 peer 值，不能引用调用者的临时 endpoint。
    Endpoint mutable_peer = peer;
    auto delayed = a->send_to(bytes("first"), mutable_peer, budget());
    mutable_peer = Endpoint::loopback(0, family);
    CHECK((co_await std::move(delayed)).value() == 5);
    CHECK((co_await a->send_to(bytes("second"), peer, budget())).value() == 6);
    std::array<std::byte, 32> buffer{};
    auto one = co_await b->receive_from(buffer, budget());
    CHECK(one.has_value());
    if (one) {
        CHECK(one->size == 5);
        CHECK(one->peer.to_string() == a->local_endpoint()->to_string());
        CHECK(std::string_view(reinterpret_cast<char*>(buffer.data()), one->size) == "first");
    }
    auto two = co_await b->receive_from(buffer, budget());
    CHECK(two.has_value() && two->size == 6);
    CHECK(std::string_view(reinterpret_cast<char*>(buffer.data()), 6) == "second");

    CHECK((co_await a->send_to({}, peer, budget())).value() == 0);
    const auto empty = co_await b->receive_from(buffer, budget());
    CHECK(empty.has_value() && empty->size == 0);
    CHECK((co_await a->send_to({}, peer, budget())).value() == 0);
    const auto empty_buffer = co_await b->receive_from({}, budget());
    CHECK(empty_buffer.has_value() && empty_buffer->size == 0);

    for (const std::size_t capacity : {std::size_t{2}, std::size_t{0}}) {
        CHECK((co_await a->send_to(bytes("truncated"), peer, budget())).value() == 9);
        CHECK((co_await a->send_to(bytes("next"), peer, budget())).value() == 4);
        const auto short_packet =
            co_await b->receive_from(std::span{buffer}.first(capacity), budget());
        CHECK(!short_packet);
        if (!short_packet) CHECK(short_packet.error() == std::errc::message_size);
        const auto next = co_await b->receive_from(buffer, budget());
        CHECK(next.has_value() && next->size == 4);
        CHECK(std::string_view(reinterpret_cast<char*>(buffer.data()), 4) == "next");
    }

    std::stop_source stopped;
    stopped.request_stop();
    CHECK((co_await a->send_to(bytes("queued"), peer, budget())).has_value());
#if CONTINUO_HAS_READINESS_API
    pollfd queued{b->native_handle(), POLLIN, 0};
    CHECK(::poll(&queued, 1, 1000) == 1);
#endif
    const auto rejected = co_await b->receive_from(buffer, {.stop = stopped.get_token()});
    CHECK(!rejected && rejected.error() == Errc::cancelled);
    const auto expired =
        co_await b->receive_from(buffer, {.deadline = EventLoop::Clock::now() - 1ms});
    CHECK(!expired && expired.error() == Errc::timed_out);
    CHECK((co_await b->receive_from(buffer, budget())).value().size == 6);
    const auto no_send = co_await a->send_to({}, peer, {.stop = stopped.get_token()});
    CHECK(!no_send && no_send.error() == Errc::cancelled);
    const auto no_send_deadline =
        co_await a->send_to(bytes("x"), peer, {.deadline = EventLoop::Clock::now() - 1ms});
    CHECK(!no_send_deadline && no_send_deadline.error() == Errc::timed_out);
    const auto none = co_await b->receive_from(buffer, {.deadline = EventLoop::Clock::now() + 5ms});
    CHECK(!none && none.error() == Errc::timed_out);
    std::vector<std::byte> oversized(65536);
    const auto too_large = co_await a->send_to(oversized, peer, budget());
    CHECK(!too_large && too_large.error() == std::errc::message_size);
    b->close();
    CHECK(!b->is_open());
    CHECK(!b->local_endpoint());
    CHECK(!(co_await b->receive_from(buffer)));
    auto rebound = udp::Socket::bind(loop, peer);
    CHECK(rebound.has_value());
}

void concurrency_and_cancel() {
    test::section("同方向拒绝、全双工及取消排空");
    auto created = EventLoop::create();
    auto& loop = created.value();
    auto a = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    auto b = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    std::array<std::byte, 32> first{}, second{};
    std::optional<Result<udp::Datagram>> pending, conflict;
    std::stop_source stop;
    collect(a.receive_from(first, {.stop = stop.get_token()}), pending);
    // 数据已入队时新操作仍不可越过老操作，不能仅在 readiness 挂起时判忙。
    std::optional<Result<std::size_t>> sent;
    collect(b.send_to(bytes("keep"), a.local_endpoint().value(), budget()), sent);
#if CONTINUO_HAS_READINESS_API
    pollfd queued{a.native_handle(), POLLIN, 0};
    CHECK(::poll(&queued, 1, 1000) == 1);
#endif
#if CONTINUO_PLATFORM_WINDOWS
    while (!sent)
        CHECK(loop.run_once(10ms).has_value());
    // IOCP 可能同批完成接收；额外挂起一笔再验证方向限制。
    if (pending) {
        pending.reset();
        collect(a.receive_from(first, {.stop = stop.get_token()}), pending);
    }
#endif
    collect(a.receive_from(second, budget()), conflict);
    CHECK(conflict.has_value());
    CHECK(conflict && !*conflict && conflict->error() == Errc::invalid_argument);
    std::optional<Result<std::size_t>> outgoing;
    collect(a.send_to(bytes("duplex"), b.local_endpoint().value(), budget()), outgoing);
    std::optional<Result<udp::Datagram>> incoming;
    collect(b.receive_from(second, budget()), incoming);
    std::thread cancel([&] { stop.request_stop(); });
    cancel.join();
    drain(loop);
    CHECK(pending.has_value());
    // 同批已完成的包可以胜过取消；随后单独验证纯取消。
    CHECK(pending && (pending->has_value() || pending->error() == Errc::cancelled));
    CHECK(outgoing && outgoing->has_value() && **outgoing == 6);
    CHECK(incoming && incoming->has_value() && (*incoming)->size == 6);

    auto c = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    pending.reset();
    std::stop_source quiet;
    collect(c.receive_from(first, {.stop = quiet.get_token()}), pending);
    quiet.request_stop();
    drain(loop);
    CHECK(pending && !*pending && pending->error() == Errc::cancelled);
    pending.reset();
    collect(c.receive_from({}, {.deadline = EventLoop::Clock::now() + 5ms}), pending);
    drain(loop);
    CHECK(pending && !*pending && pending->error() == Errc::timed_out);

#if CONTINUO_PLATFORM_WINDOWS
    // IOCP 即使发送立即完成，也必须等完成包，方向占位不能提前释放。
    std::optional<Result<std::size_t>> send_one, send_two;
    collect(c.send_to(bytes("one"), b.local_endpoint().value(), budget()), send_one);
    collect(c.send_to(bytes("two"), b.local_endpoint().value(), budget()), send_two);
    CHECK(!send_one);
    CHECK(send_two && !*send_two && send_two->error() == Errc::invalid_argument);
    drain(loop);
    CHECK(send_one && send_one->has_value());
    pending.reset();
    send_one.reset();
    collect(c.receive_from(first), pending);
    collect(c.send_to(bytes("close"), b.local_endpoint().value()), send_one);
    c.close();
    // close 之后借用仍有效，排空后才能释放 first 与源 buffer。
    CHECK(!pending);
    drain(loop);
    CHECK(pending && !*pending && pending->error() == Errc::cancelled);
    CHECK(send_one && !*send_one && send_one->error() == Errc::cancelled);
#endif
}

Detached receive_and_destroy(Task<Result<udp::Datagram>> task,
                             std::unique_ptr<udp::Socket>& socket,
                             bool& done) {
    const auto result = co_await std::move(task);
    CHECK(!result && result.error() == Errc::cancelled);
    socket.reset();
    done = true;
}
Detached receive_then_close(Task<Result<udp::Datagram>> task,
                            udp::Socket& other,
                            int& success,
                            int& cancelled) {
    const auto result = co_await std::move(task);
    if (result) {
        ++success;
        other.close();
    } else {
        CHECK(result.error() == Errc::cancelled);
        ++cancelled;
    }
}

void close_ready_batch() {
    test::section("同批就绪时关闭另一数据报操作");
    auto created = EventLoop::create();
    auto& loop = created.value();
    auto a = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    auto b = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    auto sender_a = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    auto sender_b = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    std::array<std::byte, 16> first{}, second{};
    int success = 0, cancelled = 0;
    receive_then_close(a.receive_from(first, budget()), b, success, cancelled);
    receive_then_close(b.receive_from(second, budget()), a, success, cancelled);
    std::optional<Result<std::size_t>> sent_a, sent_b;
    collect(sender_a.send_to(bytes("a"), a.local_endpoint().value()), sent_a);
    collect(sender_b.send_to(bytes("b"), b.local_endpoint().value()), sent_b);
#if CONTINUO_HAS_READINESS_API
    // 不驱动 loop，先确认两 fd 都已就绪，保证测试真的覆盖同批关闭。
    pollfd readiness[2]{{a.native_handle(), POLLIN, 0}, {b.native_handle(), POLLIN, 0}};
    CHECK(::poll(&readiness[0], 1, 1000) == 1);
    CHECK(::poll(&readiness[1], 1, 1000) == 1);
#endif
    drain(loop);
#if CONTINUO_PLATFORM_WINDOWS
    // IOCP 已分类的完成可成功；关闭尚未分类的完成则返回 cancelled。
    CHECK(success >= 1);
    CHECK(success + cancelled == 2);
#else
    CHECK(success == 1);
    CHECK(cancelled == 1);
#endif
}

void close_lifetimes() {
    test::section("close、移动及 wrapper 重入销毁");
    auto created = EventLoop::create();
    auto& loop = created.value();
    auto socket =
        std::make_unique<udp::Socket>(udp::Socket::bind(loop, Endpoint::loopback(0)).value());
    std::array<std::byte, 32> buffer{};
    bool done = false;
    receive_and_destroy(socket->receive_from(buffer), socket, done);
    socket->close();
    drain(loop);
    CHECK(done);
    CHECK(!socket);

    auto original = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    const auto endpoint = original.local_endpoint().value();
    std::optional<Result<udp::Datagram>> received;
    collect(original.receive_from(buffer, budget()), received);
    auto moved = std::move(original);
    CHECK(!original.is_open());
    CHECK(moved.is_open());
    auto sender = udp::Socket::bind(loop, Endpoint::loopback(0)).value();
    std::optional<Result<std::size_t>> sent;
    collect(sender.send_to(bytes("moved"), endpoint, budget()), sent);
    drain(loop);
    CHECK(received && received->has_value() && (*received)->size == 5);

    // 普通入口立即复制状态，尚未启动的 Task 不会保存悬空 this。
    auto lazy = moved.receive_from(buffer);
    moved.close();
    received.reset();
    collect(std::move(lazy), received);
    CHECK(received && !*received && received->error() == Errc::invalid_argument);
    drain(loop);

    // loop 析构排空内核借用，再恢复协程，恢复路径可关闭并销毁 socket。
    auto shutdown_loop = std::make_unique<EventLoop>(EventLoop::create().value());
    auto shutdown_socket = std::make_unique<udp::Socket>(
        udp::Socket::bind(*shutdown_loop, Endpoint::loopback(0)).value());
    bool shutdown_done = false;
    receive_and_destroy(shutdown_socket->receive_from(buffer), shutdown_socket, shutdown_done);
    shutdown_loop.reset();
    CHECK(shutdown_done);
    CHECK(!shutdown_socket);
}
}  // namespace

int main() {
    for (const auto family : {Family::ipv4, Family::ipv6}) {
        test::section(family == Family::ipv4 ? "IPv4 数据报" : "IPv6 数据报");
        auto created = EventLoop::create();
        auto& loop = created.value();
        CHECK(loop.run_until_complete(packets(loop, family)).has_value());
        CHECK(loop.outstanding() == 0);
    }
    concurrency_and_cancel();
    close_lifetimes();
    close_ready_batch();
    return test::summary();
}
