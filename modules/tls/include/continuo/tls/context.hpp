#pragma once

#include "continuo/core/error.hpp"

#include <memory>
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
