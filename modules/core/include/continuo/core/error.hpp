#pragma once

// continuo/core/error.hpp — error model.
//
// Continuo reports failures as values, not exceptions. Two standard pieces do
// the work; Continuo invents neither:
//
//   * `std::error_code` carries the failure (interoperable with every other
//     library that speaks the standard error protocol).
//   * `Result<T>` is `std::expected<T, std::error_code>` when the standard
//     library provides `<expected>` (C++23), and a minimal drop-in otherwise
//     so that C++20 remains a supported baseline.
//
// The portable subset is deliberately small: `has_value()`, `operator bool`,
// `value()`, `error()`, `value_or()`, and construction from either a value or
// an `Error`. Monadic helpers (`and_then`, `transform`, ...) exist only on the
// C++23 path — CI builds both standards, so accidental use of a C++23-only
// member on a C++20-supported path fails the build instead of the user.

#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#if __has_include(<expected>)
    #include <expected>
#endif

namespace continuo {

/// Failure conditions raised by Continuo itself.
///
/// Values map onto `std::error_code` through `continuo_category()`. Operating
/// system failures are *not* re-encoded here: those keep their native
/// `std::system_category()` code so that callers can compare against
/// `std::errc` / platform constants without translation tables.
enum class Errc {
    ok = 0,
    /// Operation would block; retry when the stream signals readiness.
    would_block,
    /// Peer closed the stream cleanly.
    eof,
    /// Operation abandoned because its cancellation token fired.
    cancelled,
    /// Deadline elapsed before the operation completed.
    timed_out,
    /// A configured limit (header size, body size, connection count) was hit.
    limit_exceeded,
    /// Input is syntactically or semantically unacceptable.
    invalid_argument,
    /// The requested capability is not implemented on this platform/build.
    not_supported,
};

/// Error category for `Errc`.
[[nodiscard]] const std::error_category& continuo_category() noexcept;

/// Build an `std::error_code` from `Errc` (also found by ADL).
[[nodiscard]] std::error_code make_error_code(Errc e) noexcept;

/// Failure type carried by `Result<T>`.
using Error = std::error_code;

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L

template<typename T>
using Result = std::expected<T, Error>;

/// Wrap an error so it can be returned where a `Result<T>` is expected.
[[nodiscard]] inline auto fail(Error e) noexcept {
    return std::unexpected<Error>(e);
}

/// Wrap an `Errc` so it can be returned where a `Result<T>` is expected.
[[nodiscard]] inline auto fail(Errc e) noexcept {
    return std::unexpected<Error>(make_error_code(e));
}

#else  // ── C++20 fallback ─────────────────────────────────────────────────────

namespace detail {

/// Tag returned by `fail()` on the C++20 path; converts into any `Result<T>`.
struct FailureTag {
    Error error;
};

struct VoidValue {};

}  // namespace detail

/// Minimal stand-in for `std::expected<T, Error>` on C++20 toolchains.
///
/// Only the portable subset documented at the top of this header is provided.
template<typename T>
class Result {
public:
    using value_type = T;
    using error_type = Error;

private:
    using storage_type = std::conditional_t<std::is_void_v<T>, detail::VoidValue, T>;

public:
    Result()
        requires(std::is_void_v<T> || std::is_default_constructible_v<T>)
        : storage_(storage_type{}) {}

    Result(detail::FailureTag f) noexcept : storage_(f.error) {}

    template<typename U = T>
        requires(!std::is_void_v<T> && std::is_constructible_v<T, U &&> &&
                 !std::is_same_v<std::remove_cvref_t<U>, Result> &&
                 !std::is_same_v<std::remove_cvref_t<U>, detail::FailureTag>)
    Result(U&& value) : storage_(storage_type(std::forward<U>(value))) {}

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] const Error& error() const noexcept { return std::get<1>(storage_); }

    decltype(auto) value() & {
        throw_if_failed();
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::get<0>(storage_);
        }
    }

    decltype(auto) value() const& {
        throw_if_failed();
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::get<0>(storage_);
        }
    }

    decltype(auto) value() && {
        throw_if_failed();
        if constexpr (std::is_void_v<T>) {
            return;
        } else {
            return std::move(std::get<0>(storage_));
        }
    }

    template<typename U>
        requires(!std::is_void_v<T>)
    [[nodiscard]] T value_or(U&& fallback) const& {
        return has_value() ? std::get<0>(storage_) : static_cast<T>(std::forward<U>(fallback));
    }

    decltype(auto) operator*() &
        requires(!std::is_void_v<T>)
    {
        return std::get<0>(storage_);
    }

    decltype(auto) operator*() const&
        requires(!std::is_void_v<T>)
    {
        return std::get<0>(storage_);
    }

private:
    void throw_if_failed() const {
        if (!has_value()) {
            throw std::system_error(std::get<1>(storage_));
        }
    }

    std::variant<storage_type, Error> storage_;
};

/// Wrap an error so it can be returned where a `Result<T>` is expected.
[[nodiscard]] inline detail::FailureTag fail(Error e) noexcept {
    return detail::FailureTag{e};
}

/// Wrap an `Errc` so it can be returned where a `Result<T>` is expected.
[[nodiscard]] inline detail::FailureTag fail(Errc e) noexcept {
    return detail::FailureTag{make_error_code(e)};
}

#endif  // __cpp_lib_expected

}  // namespace continuo

namespace std {
template<>
struct is_error_code_enum<continuo::Errc> : true_type {};
}  // namespace std
