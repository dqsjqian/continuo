#include "continuo/tls/context.hpp"

#include "context_impl.hpp"
#include "continuo/tls/error.hpp"

#include <openssl/err.h>
#include <string>

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
    const auto* protocol = static_cast<const std::string*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), protocol_index()));
    if (!protocol) return SSL_TLSEXT_ERR_ALERT_FATAL;
    std::size_t offset = 0;
    while (offset < offered_length) {
        const auto length = offered[offset++];
        if (length == 0 || length > offered_length - offset) return SSL_TLSEXT_ERR_ALERT_FATAL;
        const std::string_view candidate(reinterpret_cast<const char*>(offered + offset), length);
        if (candidate == *protocol) {
            *out = offered + offset;
            *out_length = length;
            return SSL_TLSEXT_ERR_OK;
        }
        offset += length;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}
}  // namespace

Context::Context(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Context::Context(Context&&) noexcept = default;
Context& Context::operator=(Context&&) noexcept = default;
Context::~Context() = default;

Result<Context> Context::client(std::string_view ca_file, std::string_view protocol) {
    if ((!ca_file.empty() && !valid_path(ca_file)) || protocol.size() > 255)
        return fail(continuo::Errc::invalid_argument);
    auto impl = std::make_unique<Impl>();
    impl->protocol = protocol;
    auto handle = make_context();
    if (!handle) return fail(handle.error());
    impl->handle = *handle;
    impl->client = true;
    if (!impl->protocol.empty()) {
        std::string wire(1, static_cast<char>(impl->protocol.size()));
        wire += impl->protocol;
        ERR_clear_error();
        if (SSL_CTX_set_alpn_protos(impl->handle,
                                    reinterpret_cast<const unsigned char*>(wire.data()),
                                    static_cast<unsigned int>(wire.size())) != 0)
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
    if (!valid_path(cert_file) || !valid_path(key_file) || protocol.size() > 255)
        return fail(continuo::Errc::invalid_argument);
    auto impl = std::make_unique<Impl>();
    impl->protocol = protocol;
    auto handle = make_context();
    if (!handle) return fail(handle.error());
    impl->handle = *handle;
    if (!impl->protocol.empty()) {
        ERR_clear_error();
        const int index = protocol_index();
        if (index < 0) return fail(make_error_code(Errc::configuration_error));
        // SSL 保留 SSL_CTX 的引用；协议副本随 SSL_CTX 释放，不依赖 Context 的生命周期。
        auto retained = std::make_unique<std::string>(impl->protocol);
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
