#include "continuo/tls/context.hpp"

#include "context_impl.hpp"
#include "continuo/tls/error.hpp"

#include <openssl/err.h>
#include <string>
#include <unordered_set>

namespace continuo::tls {
namespace {
Result<SSL_CTX*> make_context() {
    ERR_clear_error();
    SSL_CTX* handle = SSL_CTX_new(TLS_method());
    if (!handle) return fail(make_error_code(Errc::configuration_error));
    ERR_clear_error();
    if (SSL_CTX_set_min_proto_version(handle, TLS1_2_VERSION) != 1) {
        SSL_CTX_free(handle);
        return fail(make_error_code(Errc::configuration_error));
    }
    ERR_clear_error();
    SSL_CTX_set_options(handle, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    ERR_clear_error();
    SSL_CTX_set_mode(handle, SSL_MODE_ENABLE_PARTIAL_WRITE);
    return handle;
}

bool valid_path(std::string_view path) {
    return !path.empty() && path.find('\0') == std::string_view::npos;
}

Result<std::string> encode_protocols(std::span<const std::string_view> protocols) {
    std::string wire;
    std::unordered_set<std::string_view> seen;
    for (const auto protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255 ||
            protocol.size() + 1 > 65535 - wire.size() || !seen.insert(protocol).second)
            return fail(continuo::Errc::invalid_argument);
        wire += static_cast<char>(protocol.size());
        wire += protocol;
    }
    return wire;
}

int protocol_index() {
    static const int index = SSL_CTX_get_ex_new_index(
        0, nullptr, nullptr, nullptr, [](void*, void* value, CRYPTO_EX_DATA*, int, long, void*) {
            delete static_cast<std::string*>(value);
        });
    return index;
}

int select_protocol(SSL* ssl,
                    const unsigned char** out,
                    unsigned char* out_length,
                    const unsigned char* offered,
                    unsigned int offered_length,
                    void*) {
    const auto* protocols = static_cast<const std::string*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), protocol_index()));
    if (!protocols) return SSL_TLSEXT_ERR_ALERT_FATAL;
    // 先验证完整 offer，再按服务器优先顺序匹配，不能随客户端排序改变选择。
    for (std::size_t offset = 0; offset < offered_length;) {
        const auto length = offered[offset++];
        if (length == 0 || length > offered_length - offset) return SSL_TLSEXT_ERR_ALERT_FATAL;
        offset += length;
    }
    for (std::size_t preferred = 0; preferred < protocols->size();) {
        const auto length = static_cast<unsigned char>((*protocols)[preferred++]);
        const std::string_view protocol(protocols->data() + preferred, length);
        preferred += length;
        for (std::size_t offset = 0; offset < offered_length;) {
            const auto offered_size = offered[offset++];
            const std::string_view candidate(reinterpret_cast<const char*>(offered + offset),
                                             offered_size);
            if (candidate == protocol) {
                *out = offered + offset;
                *out_length = offered_size;
                return SSL_TLSEXT_ERR_OK;
            }
            offset += offered_size;
        }
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}
}  // namespace

Context::Context(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Context::Context(Context&&) noexcept = default;
Context& Context::operator=(Context&&) noexcept = default;
Context::~Context() = default;

Result<Context> Context::client(std::string_view ca_file, std::string_view protocol) {
    return client_alpn(ca_file,
                       protocol.empty() ? std::span<const std::string_view>{}
                                        : std::span<const std::string_view>{&protocol, 1});
}

Result<Context> Context::client_alpn(std::string_view ca_file,
                                     std::span<const std::string_view> protocols) {
    if (!ca_file.empty() && !valid_path(ca_file))
        return fail(continuo::Errc::invalid_argument);
    auto wire = encode_protocols(protocols);
    if (!wire) return fail(wire.error());
    auto impl = std::make_unique<Impl>();
    auto handle = make_context();
    if (!handle) return fail(handle.error());
    impl->handle = *handle;
    impl->client = true;
    if (!wire->empty()) {
        ERR_clear_error();
        if (SSL_CTX_set_alpn_protos(impl->handle,
                                    reinterpret_cast<const unsigned char*>(wire->data()),
                                    static_cast<unsigned int>(wire->size())) != 0)
            return fail(make_error_code(Errc::configuration_error));
    }
    ERR_clear_error();
    SSL_CTX_set_verify(impl->handle, SSL_VERIFY_PEER, nullptr);
    const std::string path(ca_file);
    ERR_clear_error();
    const int loaded = path.empty()
                           ? SSL_CTX_set_default_verify_paths(impl->handle)
                           : SSL_CTX_load_verify_locations(impl->handle, path.c_str(), nullptr);
    if (loaded != 1) return fail(make_error_code(Errc::configuration_error));
    return Context(std::move(impl));
}

Result<Context>
Context::server(std::string_view cert_file, std::string_view key_file, std::string_view protocol) {
    return server_alpn(cert_file, key_file,
                       protocol.empty() ? std::span<const std::string_view>{}
                                        : std::span<const std::string_view>{&protocol, 1});
}

Result<Context> Context::server_alpn(std::string_view cert_file,
                                     std::string_view key_file,
                                     std::span<const std::string_view> protocols) {
    if (!valid_path(cert_file) || !valid_path(key_file))
        return fail(continuo::Errc::invalid_argument);
    auto wire = encode_protocols(protocols);
    if (!wire) return fail(wire.error());
    auto impl = std::make_unique<Impl>();
    auto handle = make_context();
    if (!handle) return fail(handle.error());
    impl->handle = *handle;
    if (!wire->empty()) {
        ERR_clear_error();
        const int index = protocol_index();
        if (index < 0) return fail(make_error_code(Errc::configuration_error));
        // SSL 保留 SSL_CTX 的引用；协议副本随 SSL_CTX 释放，不依赖 Context 的生命周期。
        auto retained = std::make_unique<std::string>(std::move(*wire));
        ERR_clear_error();
        if (SSL_CTX_set_ex_data(impl->handle, index, retained.get()) != 1)
            return fail(make_error_code(Errc::configuration_error));
        retained.release();
        ERR_clear_error();
        SSL_CTX_set_alpn_select_cb(impl->handle, select_protocol, nullptr);
    }
    const std::string cert(cert_file);
    const std::string key(key_file);
    ERR_clear_error();
    if (SSL_CTX_use_certificate_chain_file(impl->handle, cert.c_str()) != 1)
        return fail(make_error_code(Errc::configuration_error));
    ERR_clear_error();
    if (SSL_CTX_use_PrivateKey_file(impl->handle, key.c_str(), SSL_FILETYPE_PEM) != 1)
        return fail(make_error_code(Errc::configuration_error));
    ERR_clear_error();
    if (SSL_CTX_check_private_key(impl->handle) != 1)
        return fail(make_error_code(Errc::configuration_error));
    return Context(std::move(impl));
}

}  // namespace continuo::tls
