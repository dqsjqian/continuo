// QUIC over a real UDP loopback: the engine's in-memory datagram tests are
// replaced by an actual socket pair for this round. Same certificates, same
// payload discipline — but the packets now traverse the kernel's UDP stack
// and the event loop's datagram operations.

#include "continuo/core/task_scope.hpp"
#include "continuo/quic/connection.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <stop_token>

using namespace continuo;
using namespace std::chrono_literals;
using transport::Endpoint;
using quic::Connection;
using UdpConnection = Connection<transport::udp::Socket>;

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
                       const Endpoint& server_address, std::unique_ptr<UdpConnection>& server) {
        // Receive the Initial datagram, then hand the socket to the
        // connection — a QUIC listener routes by connection ID; this test
        // serves exactly one client per socket.
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
        auto accepted =
            co_await UdpConnection::serve(std::move(server_socket), server_options, initial,
                                           {.deadline = Clock::now() + 10s});
        if (!accepted) throw std::runtime_error(std::string("服务端握手失败: ") + accepted.error().message() + " (" + std::to_string(accepted.error().value()) + ")");
        if (!accepted) co_return;
        server = std::make_unique<UdpConnection>(std::move(*accepted));
        check(server->negotiated_protocol() == "h3", "服务端 ALPN 不符");

        // Echo the stream, then say goodbye on the same one.
        std::uint64_t total = 0;
        bool fin = false;
        quic::Bytes reply;
        while (!fin) {
            auto chunk = co_await server->read(0, {.deadline = Clock::now() + 10s});
            check(chunk.has_value(), "服务端读取失败");
            if (!chunk) co_return;
            require(server->consume(0, chunk->data.size()));
            total += chunk->data.size();
            fin = chunk->fin;
            reply.insert(reply.end(), chunk->data.begin(), chunk->data.end());
        }
        check(total == 200000, "服务端收到的字节数不符");
        require(co_await server->write(0, reply, true, {.deadline = Clock::now() + 10s}));

        // Keep pumping until the client closes the connection: destroying
        // the connection here would drop unacknowledged echo datagrams.
        while (!server->closed()) {
            auto round = co_await server->pump({.deadline = Clock::now() + 10s});
            if (!round) break;
        }
}

Task<void> client_side(EventLoop& loop, quic::Options& client_options,
                       std::unique_ptr<UdpConnection>& client) {
        auto connected =
            co_await UdpConnection::connect(loop, client_options, {.deadline = Clock::now() + 10s});
        if (!connected) throw std::runtime_error(std::string("客户端握手失败: ") + connected.error().message() + " (" + std::to_string(connected.error().value()) + ")");
        if (!connected) co_return;
        client = std::make_unique<UdpConnection>(std::move(*connected));
        check(client->negotiated_protocol() == "h3", "客户端 ALPN 不符");

        const std::int64_t stream = require(client->open_stream());
        quic::Bytes payload(200000, 0x5a);
        payload[7] = 0;  // 二进制安全：中间有零
        // FIN rides with the payload: the server echoes what it received
        // only after seeing the end of stream, so splitting them deadlocks.
        require(co_await client->write(stream, payload, true,
                                        {.deadline = Clock::now() + 10s}));

        std::uint64_t total = 0;
        bool fin = false;
        while (!fin) {
            auto chunk = co_await client->read(stream, {.deadline = Clock::now() + 10s});
            if (!chunk) throw std::runtime_error(std::string("客户端读取失败: ") + chunk.error().message() + " (" + std::to_string(chunk.error().value()) + ")");
            if (!chunk) co_return;
            require(client->consume(stream, chunk->data.size()));
            check(std::all_of(chunk->data.begin(), chunk->data.end(),
                              [](std::uint8_t b) { return b == 0x5a || b == 0; }),
                  "回显数据损坏");
            total += chunk->data.size();
            fin = chunk->fin;
        }
        check(total == payload.size(), "客户端回显字节数不符");
        require(co_await client->close(0, {.deadline = Clock::now() + 10s}));
}

Task<void> run(EventLoop& loop, const char* certificate, const char* key) {
    // Server: bind first so the client has somewhere to connect.
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
    std::unique_ptr<UdpConnection> client;
    std::unique_ptr<UdpConnection> server;


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
        static_cast<void>(loop->run_until_complete(run(*loop, argv[1], argv[2])));
        std::cout << "QUIC over UDP loopback：握手、200KB 双向流、流控与关闭通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
