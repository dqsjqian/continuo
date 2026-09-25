#pragma once

#include "continuo/tls/context.hpp"

#include <openssl/ssl.h>

namespace continuo::tls {

struct Context::Impl {
    SSL_CTX* handle = nullptr;
    bool client = false;
    ~Impl() { SSL_CTX_free(handle); }
};

}  // namespace continuo::tls
