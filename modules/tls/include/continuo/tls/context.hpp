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
    /// 服务端完整配置：证书链 + 私钥，可选 mTLS 与最低协议版本。
    ///
    /// string_view 仅需存活到 Context::server 返回；所有输入都会被复制或
    /// 在构造时一次性加载，Context 不保留任何视图。
    struct ServerConfig {
        std::string_view cert_file;
        std::string_view key_file;
        /// 非空时加载该 CA 并强制验证客户端证书（mTLS）；未出示证书的
        /// 客户端握手失败。空 = 不要求客户端证书。
        std::string_view client_ca_file{};
        /// 最低协议版本："1.2"（默认）或 "1.3"；其他值拒绝。
        std::string_view min_version{"1.2"};
        /// 单协议 ALPN，空禁用。语义同 server(cert, key, protocol)。
        std::string_view protocol{};
    };

    /// 客户端完整配置：可选自定义信任锚与客户端证书链（mTLS 客户端侧）。
    struct ClientConfig {
        /// 空 = OpenSSL 默认信任路径。
        std::string_view ca_file{};
        /// 单协议 ALPN，空禁用。语义同 client(ca_file, protocol)。
        std::string_view protocol{};
        /// 非空时随握手出示客户端证书链；cert/key 必须成对出现。
        std::string_view cert_file{};
        std::string_view key_file{};
    };

    /// protocol 为空时禁用 ALPN，否则为长度不超过 255 字节的单个二进制协议名。
    [[nodiscard]] static Result<Context> client(std::string_view ca_file = {},
                                                std::string_view protocol = {});
    /// 启用后，对端提供 ALPN 但不包含 protocol 时握手失败；未提供时允许无协商。
    [[nodiscard]] static Result<Context>
    server(std::string_view cert_file, std::string_view key_file, std::string_view protocol = {});

    /// 完整配置重载：在基本形态之上增加 mTLS 与最低协议版本控制。
    /// min_version 只放宽到 "1.2"：无论配置如何，TLS 1.0/1.1 永远被拒绝。
    [[nodiscard]] static Result<Context> server(ServerConfig config);
    [[nodiscard]] static Result<Context> client(ClientConfig config);

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
