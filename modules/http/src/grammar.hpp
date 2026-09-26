#pragma once

// HTTP grammar primitives shared by the parser and the serialiser — NOT a
// public header.
//
// Both directions need the same character classes and the same list splitting,
// for symmetric reasons: the parser validates bytes arriving from a peer, and
// the serialiser validates bytes about to be sent. Same grammar (RFC 9110
// §5.6), opposite directions.
//
// They previously held a copy each. Because both copies lived in anonymous
// namespaces, nothing — not the linker, not a warning — would ever have said
// so, and a fix to one would have silently left the other behind. That is the
// same failure this module's design is otherwise preoccupied with avoiding:
// two parsers disagreeing about the same bytes is how request smuggling
// works, and a library disagreeing with itself is no better.

#include <cstddef>
#include <string_view>
#include <vector>

namespace Mira::http::grammar {

/// `tchar` — the characters allowed in a field name or a method token.
///
/// Notably excludes SP, HTAB, and the separators. A field name containing
/// anything outside this set is rejected, which is what closes the "header
/// name with a space" smuggling vector on the way in, and prevents writing an
/// unparseable field on the way out.
[[nodiscard]] constexpr bool is_tchar(unsigned char c) noexcept {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

/// Characters permitted in a field value: visible ASCII, SP, HTAB, plus the
/// obs-text range. Control characters are not — a bare CR or LF inside a value
/// is header injection inbound and response splitting outbound.
[[nodiscard]] constexpr bool is_field_vchar(unsigned char c) noexcept {
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

/// Optional whitespace, which surrounds a field value without being part of it.
[[nodiscard]] constexpr bool is_ows(char c) noexcept {
    return c == ' ' || c == '\t';
}

[[nodiscard]] constexpr std::string_view trim_ows(std::string_view text) noexcept {
    while (!text.empty() && is_ows(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ows(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// Split a comma-separated field value, trimming whitespace around each item
/// and dropping empty ones.
///
/// Empty items are dropped rather than reported: RFC 9110 §5.6.1 permits them
/// in a list, so `a,,b` means the same as `a,b` and refusing it would reject
/// traffic a conforming peer may send.
[[nodiscard]] inline std::vector<std::string_view> split_list(std::string_view text) {
    std::vector<std::string_view> items;
    while (!text.empty()) {
        const std::size_t comma = text.find(',');
        const std::string_view item = trim_ows(text.substr(0, comma));
        if (!item.empty()) {
            items.push_back(item);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    return items;
}

}  // namespace Mira::http::grammar
