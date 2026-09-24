#include "continuo/http/serializer.hpp"

#include "grammar.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace continuo::http {
namespace {

void append(Buffer& out, std::string_view text) {
    out.append(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(text.data()), text.size()});
}

[[nodiscard]] bool valid_header_name(std::string_view name) noexcept {
    if (name.empty()) {
        return false;
    }
    for (const char c : name) {
        if (!grammar::is_tchar(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_header_value(std::string_view value) noexcept {
    for (const char c : value) {
        if (!grammar::is_field_vchar(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    // Leading or trailing whitespace would be stripped by a recipient anyway,
    // and an all-whitespace value is almost always a bug in the caller.
    return true;
}

[[nodiscard]] std::string_view to_hex(std::uint64_t value, std::array<char, 20>& scratch) {
    static constexpr char digits[] = "0123456789abcdef";
    std::size_t index = scratch.size();
    if (value == 0) {
        scratch[--index] = '0';
    }
    while (value > 0) {
        scratch[--index] = digits[value % 16];
        value /= 16;
    }
    return std::string_view{scratch.data() + index, scratch.size() - index};
}

class SerializeCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "continuo.http.serialize"; }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<SerializeError>(value)) {
        case SerializeError::invalid_header:
            return "header name or value contains forbidden bytes";
        case SerializeError::framing_conflict:
            return "caller supplied a framing header that the serializer owns";
        case SerializeError::invalid_status:
            return "status code outside 100..999";
        }
        return "unknown http serialize error (" + std::to_string(value) + ")";
    }
};

}  // namespace

const std::error_category& serialize_category() noexcept {
    static const SerializeCategory category{};
    return category;
}

std::error_code make_error_code(SerializeError error) noexcept {
    return {static_cast<int>(error), serialize_category()};
}

bool status_forbids_body(unsigned status) noexcept {
    // RFC 9110 §6.4.1 / §15.3.5 / §15.4.5: informational, No Content, and Not
    // Modified responses have no body, and sending one desynchronises the
    // connection for every request that follows.
    return (status >= 100 && status < 200) || status == 204 || status == 304;
}

bool should_keep_alive(const Request& request) noexcept {
    const std::optional<std::string_view> connection = request.headers.get("Connection");

    // The token list is case-insensitive and may contain several values.
    const auto mentions = [&](std::string_view token) {
        if (!connection) {
            return false;
        }
        for (const std::string_view item : grammar::split_list(*connection)) {
            if (HeaderMap::names_equal(item, token)) {
                return true;
            }
        }
        return false;
    };

    if (request.version == Version::http_1_1) {
        return !mentions("close");
    }
    // HTTP/1.0 is close-by-default; persistence is opt-in.
    return mentions("keep-alive");
}

Result<void> write_response_head(Buffer& out,
                                 const Response& response,
                                 Framing framing,
                                 std::uint64_t body_size) {
    if (response.status < 100 || response.status > 999) {
        return fail(SerializeError::invalid_status);
    }

    // The serializer owns framing. A caller-supplied Content-Length or
    // Transfer-Encoding is refused rather than dropped: dropping it would make
    // the wire silently disagree with what the caller asked for.
    if (response.headers.contains("Content-Length") ||
        response.headers.contains("Transfer-Encoding")) {
        return fail(SerializeError::framing_conflict);
    }

    for (const auto& [name, value] : response.headers) {
        if (!valid_header_name(name) || !valid_header_value(value)) {
            return fail(SerializeError::invalid_header);
        }
    }

    // status-line = HTTP-version SP status-code SP [ reason-phrase ]
    append(out, to_string(response.version));
    append(out, " ");
    append(out, std::to_string(response.status));
    append(out, " ");
    if (!response.reason.empty()) {
        if (!valid_header_value(response.reason)) {
            return fail(SerializeError::invalid_header);
        }
        append(out, response.reason);
    } else {
        append(out, default_reason(response.status));
    }
    append(out, "\r\n");

    for (const auto& [name, value] : response.headers) {
        append(out, name);
        append(out, ": ");
        append(out, value);
        append(out, "\r\n");
    }

    // A status that forbids a body gets no framing header at all — writing
    // `Content-Length: 0` on a 304 is a common way to confuse caches.
    if (!status_forbids_body(response.status)) {
        switch (framing) {
        case Framing::content_length:
            append(out, "Content-Length: ");
            append(out, std::to_string(body_size));
            append(out, "\r\n");
            break;
        case Framing::chunked:
            append(out, "Transfer-Encoding: chunked\r\n");
            break;
        case Framing::none:
            // Deliberate: the caller is signalling "no body and no framing",
            // which is only valid when the connection closes afterwards.
            break;
        }
    }

    append(out, "\r\n");
    return Result<void>{};
}

Result<void> write_chunk(Buffer& out, std::span<const std::byte> chunk) {
    if (chunk.empty()) {
        // A zero-length chunk *is* the terminator; emitting one here would end
        // the message early and leave the caller writing into a closed body.
        return fail(SerializeError::framing_conflict);
    }

    std::array<char, 20> scratch{};
    append(out, to_hex(chunk.size(), scratch));
    append(out, "\r\n");
    out.append(chunk);
    append(out, "\r\n");
    return Result<void>{};
}

void write_last_chunk(Buffer& out) {
    append(out, "0\r\n\r\n");
}

}  // namespace continuo::http
