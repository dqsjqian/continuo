#pragma once

#include "mira/tls/context.hpp"
#include "mira/tls/error.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace Mira::tls {

/// 同步 memory BIO 状态机，不持有 socket，不执行任何异步 I/O。
/// 单实例调用须串行；每次 step 后先排空密文，再提供所需输入。
class Engine {
public:
    enum class Status { complete, want_input, want_output, eof };
    struct Step {
        Status status;
        std::size_t transferred = 0;
    };
    static constexpr std::size_t buffer_capacity = 64 * 1024;

    [[nodiscard]] static Result<Engine> create(const Context& context,
                                               std::string_view peer_name = {});
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

    [[nodiscard]] Result<Step> handshake();
    [[nodiscard]] Result<Step> read(std::span<std::byte> destination);
    [[nodiscard]] Result<Step> write(std::span<const std::byte> source);
    /// complete 仅表示本方 close_notify 已生成，调用方仍须排空密文。
    [[nodiscard]] Result<Step> shutdown();
    /// 握手完成前或未协商 ALPN 时为空；视图由本 Engine 持有，析构后失效。
    [[nodiscard]] std::string_view negotiated_protocol() const noexcept;

    [[nodiscard]] std::size_t input_capacity() const noexcept;
    [[nodiscard]] Result<std::size_t> feed(std::span<const std::byte> ciphertext);
    /// 失败后仍可取走已生成的密文（如 fatal alert），不会再次驱动 SSL I/O。
    [[nodiscard]] Result<std::size_t> drain(std::span<std::byte> ciphertext);
    /// 底层失败或驱动被取消后必须使状态机失效，不能重试应用数据。
    void invalidate() noexcept;

private:
    struct Impl;
    explicit Engine(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace Mira::tls
