#pragma once

#include "continuo/core/error.hpp"

namespace continuo::tls {

enum class Errc {
    invalid_state = 1,
    operation_in_progress,
    protocol_error,
    certificate_verify_failed,
    truncated,
    configuration_error,
};

[[nodiscard]] const std::error_category& tls_category() noexcept;
[[nodiscard]] Error make_error_code(Errc error) noexcept;

}  // namespace continuo::tls

namespace std {
template<>
struct is_error_code_enum<continuo::tls::Errc> : true_type {};
}  // namespace std
