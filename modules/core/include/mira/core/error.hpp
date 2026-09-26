#pragma once

// C++23 value-based errors: one standard result type, no compatibility fork.
#include <expected>
#include <system_error>
#include <type_traits>

#if !defined(__cpp_lib_expected) || __cpp_lib_expected < 202202L
    // Self-diagnosing: #error text is macro-expanded, so the reported values
    // tell a failing toolchain apart (old standard vs old library) at a glance.
    #define MIRA_DIAG_STR2(x) #x
    #define MIRA_DIAG_STR(x) MIRA_DIAG_STR2(x)
    #if !defined(__cpp_lib_expected)
        #error "Mira requires a C++23 standard library with std::expected; __cpp_lib_expected is undefined here (Mira diagnostic: __cplusplus=" MIRA_DIAG_STR(__cplusplus) ")"
    #else
        #error "Mira requires a C++23 standard library with std::expected; __cpp_lib_expected is too old (Mira diagnostic: __cplusplus=" MIRA_DIAG_STR(__cplusplus) " __cpp_lib_expected=" MIRA_DIAG_STR(__cpp_lib_expected) ")"
    #endif
#endif

namespace Mira {

enum class Errc {
    ok = 0,
    would_block,
    eof,
    cancelled,
    timed_out,
    limit_exceeded,
    invalid_argument,
    not_supported,
    /// A library boundary caught an exception it cannot attribute to a
    /// protocol event — e.g. a user handler that threw. Distinguishable from
    /// every I/O condition above so callers can tell "the connection broke"
    /// from "the code on this side broke".
    internal,
};

[[nodiscard]] const std::error_category& mira_category() noexcept;
[[nodiscard]] std::error_code make_error_code(Errc e) noexcept;
using Error = std::error_code;

/// Normalize native socket errors to portable error conditions where possible.
/// IOCP NTSTATUS values must first be translated through the socket API.
[[nodiscard]] Error socket_error(int native_code) noexcept;

template<typename T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline auto fail(Error e) noexcept {
    return std::unexpected<Error>(e);
}

[[nodiscard]] inline auto fail(Errc e) noexcept {
    return std::unexpected<Error>(make_error_code(e));
}

}  // namespace Mira

namespace std {
template<>
struct is_error_code_enum<Mira::Errc> : true_type {};
}  // namespace std
