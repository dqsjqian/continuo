#include "Mira/core/error.hpp"

#include "Mira/core/platform.hpp"

#include <string>

#if MIRA_PLATFORM_WINDOWS
// clang-format off
    #include <winsock2.h>
// clang-format on
#endif

namespace Mira {
namespace {

class MiraCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "Mira"; }

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
        return "unknown Mira error (" + std::to_string(value) + ")";
    }

    /// Map Miraditions onto the portable `std::errc` equivalents so
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

const std::error_category& mira_category() noexcept {
    static const MiraCategory category{};
    return category;
}

std::error_code make_error_code(Errc e) noexcept {
    return {static_cast<int>(e), mira_category()};
}

Error socket_error(int native_code) noexcept {
#if MIRA_PLATFORM_WINDOWS
    // Translate the Winsock numbers that have a portable equivalent, so that
    // `ec == std::errc::connection_refused` means the same thing on every
    // platform. Anything unlisted keeps its native value — losing information
    // would be worse than an error a caller has to inspect by number.
    switch (native_code) {
    case WSAEINTR:
        return std::make_error_code(std::errc::interrupted);
    case WSAEACCES:
        return std::make_error_code(std::errc::permission_denied);
    case WSAEFAULT:
        return std::make_error_code(std::errc::bad_address);
    case WSAEINVAL:
        return std::make_error_code(std::errc::invalid_argument);
    case WSAEMFILE:
        return std::make_error_code(std::errc::too_many_files_open);
    case WSAEWOULDBLOCK:
        return std::make_error_code(std::errc::operation_would_block);
    case WSAEINPROGRESS:
        return std::make_error_code(std::errc::operation_in_progress);
    case WSAEALREADY:
        return std::make_error_code(std::errc::connection_already_in_progress);
    case WSAENOTSOCK:
        return std::make_error_code(std::errc::not_a_socket);
    case WSAEDESTADDRREQ:
        return std::make_error_code(std::errc::destination_address_required);
    case WSAEMSGSIZE:
        return std::make_error_code(std::errc::message_size);
    case WSAEPROTOTYPE:
        return std::make_error_code(std::errc::wrong_protocol_type);
    case WSAENOPROTOOPT:
        return std::make_error_code(std::errc::no_protocol_option);
    case WSAEPROTONOSUPPORT:
        return std::make_error_code(std::errc::protocol_not_supported);
    case WSAEOPNOTSUPP:
        return std::make_error_code(std::errc::operation_not_supported);
    case WSAEAFNOSUPPORT:
        return std::make_error_code(std::errc::address_family_not_supported);
    case WSAEADDRINUSE:
        return std::make_error_code(std::errc::address_in_use);
    case WSAEADDRNOTAVAIL:
        return std::make_error_code(std::errc::address_not_available);
    case WSAENETDOWN:
        return std::make_error_code(std::errc::network_down);
    case WSAENETUNREACH:
        return std::make_error_code(std::errc::network_unreachable);
    case WSAENETRESET:
        return std::make_error_code(std::errc::network_reset);
    case WSAECONNABORTED:
        return std::make_error_code(std::errc::connection_aborted);
    case WSAECONNRESET:
        return std::make_error_code(std::errc::connection_reset);
    case WSAENOBUFS:
        return std::make_error_code(std::errc::no_buffer_space);
    case WSAEISCONN:
        return std::make_error_code(std::errc::already_connected);
    case WSAENOTCONN:
        return std::make_error_code(std::errc::not_connected);
    case WSAETIMEDOUT:
        return std::make_error_code(std::errc::timed_out);
    case WSAECONNREFUSED:
        return std::make_error_code(std::errc::connection_refused);
    case WSAEHOSTUNREACH:
        return std::make_error_code(std::errc::host_unreachable);
    default:
        return std::error_code{native_code, std::system_category()};
    }
#else
    return std::error_code{native_code, std::system_category()};
#endif
}

}  // namespace Mira
