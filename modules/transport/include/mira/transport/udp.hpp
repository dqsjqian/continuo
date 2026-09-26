#pragma once

#include "Mira/core/event_loop.hpp"
#include "Mira/transport/endpoint.hpp"

#include <memory>
#include <utility>

namespace Mira::transport::udp {

struct BindOptions {
    bool dual_stack{false};
};

struct Datagram {
    std::size_t size{0};
    Endpoint peer;
};

/// 单线程、完成式数据报传输，不是 AsyncStream。
/// 同方向只允许一个在途操作，收发可以同时进行；冲突返回 invalid_argument。
/// 零长包有效，零长接收缓冲也会消费一个包；截断返回 message_size 且丢弃尾部。
/// 调用者必须保持 Task 和借用缓冲到完成；close 取消但不同步排空 IOCP。
/// EventLoop 必须活得比 Socket 久。移动及销毁 wrapper 不影响已开始操作的状态。
class Socket {
public:
    Socket() = default;
    Socket(Socket&&) noexcept = default;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket();

    /// 独占绑定；不启用 SO_REUSEADDR/SO_REUSEPORT。port 0 由系统分配。
    [[nodiscard]] static Result<Socket>
    bind(EventLoop& loop, const Endpoint& endpoint, BindOptions options = {});
    [[nodiscard]] Result<Endpoint> local_endpoint() const;
    [[nodiscard]] NativeHandle native_handle() const noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    void close() noexcept;

    /// peer 按值捕获，Task 延迟启动或挂起时无需保留调用者的 Endpoint。
    [[nodiscard]] Task<Result<std::size_t>>
    send_to(std::span<const std::byte> source, Endpoint peer, OperationOptions options = {});
    [[nodiscard]] Task<Result<Datagram>> receive_from(std::span<std::byte> destination,
                                                      OperationOptions options = {});

private:
    struct State;
    static Task<Result<std::size_t>> send(std::shared_ptr<State> state,
                                          std::span<const std::byte> source,
                                          Endpoint peer,
                                          OperationOptions options);
    static Task<Result<Datagram>> receive(std::shared_ptr<State> state,
                                          std::span<std::byte> destination,
                                          OperationOptions options);
    std::shared_ptr<State> state_;
};

}  // namespace Mira::transport::udp
