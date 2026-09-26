#include "mira/tls/error.hpp"

#include <string>

namespace Mira::tls {
namespace {
class Category final : public std::error_category {
public:
    const char* name() const noexcept override { return "Mira.tls"; }
    std::string message(int value) const override {
        switch (static_cast<Errc>(value)) {
        case Errc::invalid_state:
            return "invalid TLS operation state";
        case Errc::operation_in_progress:
            return "another TLS operation is in progress";
        case Errc::protocol_error:
            return "TLS protocol failure";
        case Errc::certificate_verify_failed:
            return "TLS peer certificate verification failed";
        case Errc::truncated:
            return "TLS transport ended without close_notify";
        case Errc::configuration_error:
            return "TLS configuration failed";
        }
        return "unknown TLS error";
    }
};
}  // namespace

const std::error_category& tls_category() noexcept {
    static const Category category;
    return category;
}

Error make_error_code(Errc error) noexcept {
    return {static_cast<int>(error), tls_category()};
}
}  // namespace Mira::tls
