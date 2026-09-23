#include "continuo/core/error.hpp"

#include <string>

namespace continuo {
namespace {

class ContinuoCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "continuo"; }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<Errc>(value)) {
        case Errc::ok:
            return "success";
        case Errc::would_block:
            return "operation would block";
        case Errc::eof:
            return "stream closed by peer";
        case Errc::cancelled:
            return "operation cancelled";
        case Errc::timed_out:
            return "operation timed out";
        case Errc::limit_exceeded:
            return "configured limit exceeded";
        case Errc::invalid_argument:
            return "invalid argument";
        case Errc::not_supported:
            return "not supported on this platform";
        }
        return "unknown continuo error (" + std::to_string(value) + ")";
    }

    /// Map Continuo conditions onto the portable `std::errc` equivalents so
    /// that `ec == std::errc::timed_out` works across library boundaries.
    [[nodiscard]] std::error_condition default_error_condition(int value) const noexcept override {
        switch (static_cast<Errc>(value)) {
        case Errc::would_block:
            return std::make_error_condition(std::errc::operation_would_block);
        case Errc::cancelled:
            return std::make_error_condition(std::errc::operation_canceled);
        case Errc::timed_out:
            return std::make_error_condition(std::errc::timed_out);
        case Errc::invalid_argument:
            return std::make_error_condition(std::errc::invalid_argument);
        case Errc::not_supported:
            return std::make_error_condition(std::errc::not_supported);
        case Errc::limit_exceeded:
            return std::make_error_condition(std::errc::value_too_large);
        case Errc::ok:
        case Errc::eof:
            break;
        }
        return {value, *this};
    }
};

}  // namespace

const std::error_category& continuo_category() noexcept {
    static const ContinuoCategory category{};
    return category;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), continuo_category()};
}

}  // namespace continuo
