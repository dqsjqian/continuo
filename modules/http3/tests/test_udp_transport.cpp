// HTTP/3 over a real UDP loopback: the in-memory engine tests prove the
// protocol state machines; this one proves the whole stack — nghttp3 over
// ngtcp2 over the kernel's UDP, driven through the event loop.

#include "continuo/core/task_scope.hpp"
#include "continuo/http3/connection.hpp"
#include "continuo/transport/udp.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace continuo;
using namespace std::chrono_literals;
using transport::Endpoint;
using quic::Bytes;
using H3Connection = http3::Connection<transport::udp::Socket>;

namespace {

template<class T>
T require(Result<T> value) {
    if (!value)
        throw std::runtime_error(value.error().message() + " (" +
                                 std::to_string(value.error().value()) + ")");
    return std::move(*value);
}

void require(Result<void> value) {
    if (!value) throw std::runtime_error(value.error().message());
}

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

Task<void> server_side(transport::udp::Socket& server_socket,
                       std::array<std::byte, 65536>& initial_buffer,
                       std::vector<std::uint8_t>& initial, quic::Options& server_options,
                       const Endpoint& server_address, std::optional<H3Connection>& server) {
        auto datagram =
            co_await server_socket.receive_from(initial_buffer, {.deadline = Clock::now() + 5s});
        check(datagram.has_value(), "服务端未收到 Initial");
        initial.assign(reinterpret_cast<const std::uint8_t*>(initial_buffer.data()),
                       reinterpret_cast<const std::uint8_t*>(initial_buffer.data()) +
                           datagram->size);
        // Engine::accept requires both endpoints: the local one we bound,
        // the remote learned from the Initial datagram's source address.
        server_options.local = server_address;
        server_options.remote = datagram->peer;
        auto accepted = co_await H3Connection::serve(std::move(server_socket), server_options,
                                                    http3::Limits{}, initial,
                                                    {.deadline = Clock::now() + 10s});
        check(accepted.has_value(), "服务端握手失败");
        if (!accepted) co_return;
        server.emplace(std::move(*accepted));

        // 等一个请求头，回显 body，再等流结束。
        for (;;) {
            auto head = co_await server->await_head(0, {.deadline = Clock::now() + 10s});
            check(head.has_value(), "服务端未收到请求头");
            if (!head) co_return;
            Bytes body;
            bool fin = false;
            while (!fin) {
                auto chunk = co_await server->read_body(0, {.deadline = Clock::now() + 10s});
                check(chunk.has_value(), "服务端读取 body 失败");
                if (!chunk) co_return;
                require(server->consume(0, chunk->data.size()));
                body.insert(body.end(), chunk->data.begin(), chunk->data.end());
                fin = chunk->fin;
            }
            require(co_await server->respond(
                0, {{":status", "200"}, {"content-length", std::to_string(body.size())}}, body,
                {.deadline = Clock::now() + 10s}));
            break;
        }
}

Task<void> client_side(EventLoop& loop, quic::Options& client_options,
                       std::optional<H3Connection>& client) {
        auto connected = co_await H3Connection::connect(loop, client_options, http3::Limits{},
                                                          {.deadline = Clock::now() + 10s});
        check(connected.has_value(), "客户端握手失败");
        if (!connected) co_return;
        client.emplace(std::move(*connected));

        Bytes payload(20000, 0x48);
        payload[11] = 0;  // 二进制安全
        const std::int64_t stream = require(co_await client->request(
            {{":method", "POST"},
             {":scheme", "https"},
             {":authority", "localhost"},
             {":path", "/echo"}},
            payload));

        auto head = co_await client->await_head(stream, {.deadline = Clock::now() + 10s});
        check(head.has_value(), "客户端未收到响应头");
        if (!head) co_return;
        bool status_ok = false;
        for (const auto& [name, value] : head->fields)
            if (name == ":status" && value == "200") status_ok = true;
        check(status_ok, "响应状态不是 200");

        std::uint64_t total = 0;
        bool fin = false;
        while (!fin) {
            auto chunk = co_await client->read_body(stream, {.deadline = Clock::now() + 10s});
            check(chunk.has_value(), "客户端读取 body 失败");
            if (!chunk) co_return;
            require(client->consume(stream, chunk->data.size()));
            check(std::all_of(chunk->data.begin(), chunk->data.end(),
                              [](std::uint8_t b) { return b == 0x48 || b == 0; }),
                  "回显数据损坏");
            total += chunk->data.size();
            fin = chunk->fin;
        }
        check(total == payload.size(), "回显字节数不符");
        require(co_await client->close(0, {.deadline = Clock::now() + 10s}));
}

Task<void> run(EventLoop& loop, const char* certificate, const char* key) {
    auto server_socket =
        require(transport::udp::Socket::bind(loop, Endpoint::loopback(0)));
    const Endpoint server_address = require(server_socket.local_endpoint());

    quic::Options client_options;
    client_options.local = Endpoint::loopback(0);
    client_options.remote = server_address;
    client_options.ca_file = certificate;
    client_options.peer_name = "localhost";
    client_options.alpn = "h3";

    quic::Options server_options;
    server_options.certificate_file = certificate;
    server_options.private_key_file = key;
    server_options.alpn = "h3";

    std::array<std::byte, 65536> initial_buffer{};
    std::vector<std::uint8_t> initial;

    TaskScope scope;
    std::optional<H3Connection> client;
    std::optional<H3Connection> server;


    scope.spawn(server_side(server_socket, initial_buffer, initial,
                          server_options, server_address, server));
    scope.spawn(client_side(loop, client_options, client));
    co_await scope.join();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    auto loop = EventLoop::create();
    if (!loop) return 2;
    try {
        loop->run_until_complete(run(*loop, argv[1], argv[2]));
        std::cout << "HTTP/3 over UDP loopback：握手、POST/回显 20KB、流控与关闭通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
