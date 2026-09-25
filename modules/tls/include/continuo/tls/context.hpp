#pragma once

#include "continuo/core/error.hpp"

#include <memory>
#include <span>
#include <string_view>

namespace continuo::tls {

class Engine;

/// 不可变 TLS 配置。客户端始终验证证书链与对端身份，最低 TLS 1.2。
/// ca_file 为空时使用 OpenSSL 默认信任路径，不等同于原生系统钥匙串。
class Context {
public:
    /// protocol 为空时禁用 ALPN，否则为长度不超过 255 字节的单个二进制协议名。
    [[nodiscard]] static Result<Context> client(std::string_view ca_file = {},
                                                std::string_view protocol = {});
    /// 启用后，对端提供 ALPN 但不包含 protocol 时握手失败；未提供时允许无协商。
    [[nodiscard]] static Result<Context>
    server(std::string_view cert_file, std::string_view key_file, std::string_view protocol = {});

    /// protocols 按优先顺序排列；每项为 1..255 字节二进制名称，不允许重复。
    /// 长度前缀编码总长不得超过 65535 字节；空列表禁用 ALPN，输入会被复制。
    [[nodiscard]] static Result<Context>
    client_alpn(std::string_view ca_file, std::span<const std::string_view> protocols);
    /// 按服务器列表顺序选择共同协议；客户端提供 ALPN 却无共同协议时握手失败。
    /// 客户端未提供 ALPN 时允许无协商。调用方须检查 negotiated_protocol() 并显式
    /// 选择应用协议（例如 HTTP/1 回退）；TLS 不会自动实现 HTTP/1 与 HTTP/2 切换。
    /// 协议列表与 SSL_CTX 同寿命，已创建的流不依赖此 Context 继续存活。
    [[nodiscard]] static Result<Context> server_alpn(
        std::string_view cert_file,
        std::string_view key_file,
        std::span<const std::string_view> protocols);

    Context(Context&&) noexcept;
    Context& operator=(Context&&) noexcept;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context();

private:
    struct Impl;
    explicit Context(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
    friend class Engine;
};

}  // namespace continuo::tls
