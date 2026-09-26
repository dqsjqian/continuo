#pragma once

#include "mira/tls/context.hpp"

#include <openssl/ssl.h>

namespace Mira::tls {

struct Context::Impl {
    SSL_CTX* handle = nullptr;
    bool client = false;
    ~Impl() { SSL_CTX_free(handle); }
};

}  // namespace Mira::tls
