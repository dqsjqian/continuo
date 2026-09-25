#pragma once

#include "continuo/core/event_loop.hpp"
#include "continuo/transport/endpoint.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace continuo::transport {

enum class ResolveTransport { tcp, udp };

struct ResolveQuery {
    std::string hostname;
    std::string service;
    std::optional<Family> family{};
    ResolveTransport transport{ResolveTransport::tcp};
};

struct ResolverOptions {
    std::size_t workers{2};
    std::size_t queue_capacity{64};
    std::size_t max_results{64};
};

/// getaddrinfo 的 EAI_* 错误独立于 errno/Winsock socket 错误。
/// POSIX 系统解析返回 EAI_SYSTEM 时，resolve 改为返回当时 errno 的 socket_error。
[[nodiscard]] const std::error_category& resolver_category() noexcept;
[[nodiscard]] Error resolver_error(int native_code) noexcept;

/// 固定线程池系统解析器；不执行 DNS 缓存、重试或端点连接选择。
/// 取消/超时只终止等待，不能中断已进入的系统 getaddrinfo。
/// 析构取消所有等待并 join 工作线程，可能等待系统调用返回；不 detach。
/// resolve 必须在相应 loop 线程调用；不得并发移动/析构和调用成员。
/// 已启动的等待持有独立状态，允许另一个线程析构 Resolver 并阻塞 join。
class Resolver {
public:
    using Endpoints = std::vector<Endpoint>;
    /// 可注入同步解析函数，在线程池调用；必须线程安全，不得访问 loop。
    /// max_results 为输出上限；框架仍会去重/验证/限制返回值。
    /// 自定义 Backend 的内部内存和执行时长由调用者负责约束。
    using Backend = std::function<Result<Endpoints>(const ResolveQuery&, std::size_t max_results)>;

    [[nodiscard]] static Result<Resolver> create(ResolverOptions options = {}, Backend backend = {});
    Resolver(Resolver&&) noexcept;
    Resolver& operator=(Resolver&&) noexcept;
    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;
    ~Resolver();

    /// 参数按值拥有；空 hostname/service、嵌入 NUL 或超过 4096 字节均拒绝。
    /// 无其他工作时仍计入 loop.outstanding()，完成仅在 loop 线程交付。
    /// 提交前 stop > deadline > 参数校验；交付时已发布结果 > 用户取消 >
    /// timer 超时 > resolver/loop 关闭（cancelled）。结果限制超出则 limit_exceeded。
    /// Task 可晚于 Resolver 销毁才启动，但 EventLoop 必须活到开始等待。
    [[nodiscard]] Task<Result<Endpoints>> resolve(EventLoop& loop, ResolveQuery query,
                                                 OperationOptions options = {});

private:
    class Impl;
    explicit Resolver(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace continuo::transport
