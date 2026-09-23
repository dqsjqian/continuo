#include "continuo/http/parser.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace continuo::http {
namespace {

// ── character classification (RFC 9110 §5.6.2) ───────────────────────────────

/// `tchar` — the characters allowed in a field name or method token.
///
/// Notably excludes SP, HTAB, and the separators. A field name containing
/// anything outside this set is rejected, which is what closes the "header
/// name with a space" smuggling vector.
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
/// is header injection.
[[nodiscard]] constexpr bool is_field_vchar(unsigned char c) noexcept {
    return c == '\t' || (c >= 0x20 && c != 0x7F);
}

[[nodiscard]] constexpr bool is_ows(char c) noexcept {
    return c == ' ' || c == '\t';
}

[[nodiscard]] std::string_view trim_ows(std::string_view text) noexcept {
    while (!text.empty() && is_ows(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_ows(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] std::string_view as_text(std::span<const std::byte> bytes) noexcept {
    return std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// A line located inside the buffer, plus how many bytes to consume for it.
struct Line {
    std::string_view text;    // without the terminator
    std::size_t consumed{0};  // including the terminator
};

/// Find the next line, honouring the CRLF policy.
///
/// Returns nullopt when the buffer holds no complete line yet. Fails when the
/// terminator is malformed: a bare LF where CRLF is required, or a CR not
/// followed by LF. Both are line-ending differentials — the raw material of
/// request smuggling, where a front-end and a back-end disagree about where a
/// message ends.
[[nodiscard]] Result<std::optional<Line>> next_line(std::span<const std::byte> window,
                                                    bool allow_bare_lf) {
    const std::string_view text = as_text(window);
    const std::size_t lf = text.find('\n');
    if (lf == std::string_view::npos) {
        return std::optional<Line>{};
    }

    if (lf == 0) {
        if (!allow_bare_lf) {
            return fail(ParseError::bad_line_ending);
        }
        return std::optional<Line>{Line{text.substr(0, 0), 1}};
    }

    const bool has_cr = text[lf - 1] == '\r';
    if (!has_cr && !allow_bare_lf) {
        return fail(ParseError::bad_line_ending);
    }

    const std::size_t content = has_cr ? lf - 1 : lf;

    // A CR anywhere other than immediately before the LF is injection.
    if (text.substr(0, content).find('\r') != std::string_view::npos) {
        return fail(ParseError::bad_line_ending);
    }

    return std::optional<Line>{Line{text.substr(0, content), lf + 1}};
}

/// Parse an unsigned decimal integer with overflow detection.
///
/// Rejects signs, whitespace, and any non-digit — `Content-Length: +10` and
/// `Content-Length: 10 ` are both errors, because tolerating either means
/// disagreeing with some other parser about the body length.
[[nodiscard]] std::optional<std::uint64_t> parse_decimal(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::uint64_t>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;  // overflow
        }
        value = value * 10 + digit;
    }
    return value;
}

/// Parse an unsigned hexadecimal integer with overflow detection.
[[nodiscard]] std::optional<std::uint64_t> parse_hex(std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        std::uint64_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 16) {
            return std::nullopt;
        }
        value = value * 16 + digit;
    }
    return value;
}

/// Split a comma-separated list, trimming optional whitespace around items.
[[nodiscard]] std::vector<std::string_view> split_list(std::string_view text) {
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

[[nodiscard]] bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    return HeaderMap::names_equal(a, b);
}

class ParseCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "continuo.http"; }

    [[nodiscard]] std::string message(int value) const override {
        switch (static_cast<ParseError>(value)) {
        case ParseError::malformed_start_line:
            return "malformed start line";
        case ParseError::unsupported_version:
            return "unsupported HTTP version";
        case ParseError::malformed_header:
            return "malformed header field";
        case ParseError::whitespace_before_colon:
            return "whitespace between field name and colon";
        case ParseError::obsolete_line_folding:
            return "obsolete line folding is not accepted";
        case ParseError::framing_conflict:
            return "Content-Length conflicts with Transfer-Encoding";
        case ParseError::inconsistent_content_length:
            return "repeated Content-Length values disagree";
        case ParseError::malformed_content_length:
            return "malformed Content-Length";
        case ParseError::invalid_transfer_encoding:
            return "invalid Transfer-Encoding";
        case ParseError::malformed_chunk:
            return "malformed chunked encoding";
        case ParseError::bad_line_ending:
            return "malformed line ending";
        case ParseError::limit_exceeded:
            return "configured limit exceeded";
        }
        return "unknown http parse error (" + std::to_string(value) + ")";
    }
};

}  // namespace

const std::error_category& parse_category() noexcept {
    static const ParseCategory category{};
    return category;
}

std::error_code make_error_code(ParseError error) noexcept {
    return {static_cast<int>(error), parse_category()};
}

// ── RequestParser ────────────────────────────────────────────────────────────

void RequestParser::reset() {
    state_ = State::start_line;
    request_.clear();
    trailers_.clear();
    body_ = {};
    body_seen_ = 0;
    body_remaining_ = 0;
    chunk_remaining_ = 0;
    headers_total_ = 0;
    pending_consume_ = 0;
    saw_last_chunk_ = false;
}

Result<ParseStep> RequestParser::parse(Buffer& input) {
    // Release the previous body slice now rather than when it was handed out.
    // Consuming it immediately would shrink the buffer while the caller still
    // held a span into it — ASan flags that as a container overflow, and it is
    // right to.
    if (pending_consume_ > 0) {
        input.consume(pending_consume_);
        pending_consume_ = 0;
    }
    body_ = {};

    // Drive the state machine until it either produces something for the
    // caller or genuinely runs out of bytes.
    //
    // This loop is the reason `need_more` means exactly one thing: "read more
    // from the stream". An earlier version also returned it after an internal
    // step succeeded, which left callers unable to tell "I advanced, call me
    // again" from "I am blocked" — so a connection loop would read on a buffer
    // that still had a complete request in it, and treat the resulting eof as
    // a closed connection. Collapsing that ambiguity here keeps every caller
    // from having to rediscover it.
    for (;;) {
        switch (state_) {
        case State::start_line: {
            const Result<bool> parsed = parse_start_line(input);
            if (!parsed) {
                return fail(parsed.error());
            }
            if (!*parsed) {
                return ParseStep::need_more;
            }
            state_ = State::headers;
            continue;
        }

        case State::headers: {
            const Result<bool> parsed = parse_headers(input);
            if (!parsed) {
                return fail(parsed.error());
            }
            if (!*parsed) {
                return ParseStep::need_more;
            }

            const Result<void> framing = decide_framing();
            if (!framing) {
                return fail(framing.error());
            }

            switch (request_.body_kind) {
            case BodyKind::none:
                state_ = State::done;
                break;
            case BodyKind::length:
                body_remaining_ = request_.content_length;
                state_ = body_remaining_ == 0 ? State::done : State::body_length;
                break;
            case BodyKind::chunked:
                state_ = State::body_chunk_header;
                break;
            }
            return ParseStep::head;
        }

        case State::body_length: {
            const Result<ParseStep> step = read_length_body(input);
            if (!step) {
                return fail(step.error());
            }
            if (*step == ParseStep::need_more) {
                return ParseStep::need_more;
            }
            return *step;
        }

        case State::body_chunk_header: {
            const Result<Progress> step = read_chunk_header(input);
            if (!step) {
                return fail(step.error());
            }
            if (*step == Progress::need_data) {
                return ParseStep::need_more;
            }
            continue;  // advanced into chunk data or the trailer section
        }

        case State::body_chunk_data: {
            const Result<Progress> step = read_chunk_data(input);
            if (!step) {
                return fail(step.error());
            }
            if (*step == Progress::need_data) {
                return ParseStep::need_more;
            }
            if (*step == Progress::emitted_body) {
                return ParseStep::body;
            }
            continue;  // consumed the chunk's trailing CRLF
        }

        case State::body_chunk_trailer: {
            const Result<Progress> step = read_chunk_trailer(input);
            if (!step) {
                return fail(step.error());
            }
            if (*step == Progress::need_data) {
                return ParseStep::need_more;
            }
            if (*step == Progress::finished) {
                return ParseStep::complete;
            }
            continue;  // a trailer field was recorded
        }

        case State::done:
            return ParseStep::complete;
        }
    }
}

Result<bool> RequestParser::parse_start_line(Buffer& input) {
    Result<std::optional<Line>> located = next_line(input.readable(), limits_.allow_bare_lf);
    if (!located) {
        return fail(located.error());
    }
    if (!located->has_value()) {
        // Refuse to buffer an unbounded start line while waiting for its end.
        if (input.size() > limits_.max_start_line) {
            return fail(ParseError::limit_exceeded);
        }
        return false;
    }

    const Line line = **located;
    if (line.text.size() > limits_.max_start_line) {
        return fail(ParseError::limit_exceeded);
    }

    // RFC 9112 §3: request-line = method SP request-target SP HTTP-version.
    // Exactly one space in each position — tolerating runs of spaces is
    // another way two parsers end up disagreeing about the target.
    const std::size_t first_sp = line.text.find(' ');
    if (first_sp == std::string_view::npos || first_sp == 0) {
        return fail(ParseError::malformed_start_line);
    }
    const std::size_t second_sp = line.text.find(' ', first_sp + 1);
    if (second_sp == std::string_view::npos || second_sp == first_sp + 1) {
        return fail(ParseError::malformed_start_line);
    }

    const std::string_view method = line.text.substr(0, first_sp);
    const std::string_view target = line.text.substr(first_sp + 1, second_sp - first_sp - 1);
    const std::string_view version = line.text.substr(second_sp + 1);

    if (version.find(' ') != std::string_view::npos) {
        return fail(ParseError::malformed_start_line);
    }
    for (const char c : method) {
        if (!is_tchar(static_cast<unsigned char>(c))) {
            return fail(ParseError::malformed_start_line);
        }
    }
    if (target.empty()) {
        return fail(ParseError::malformed_start_line);
    }
    for (const char c : target) {
        // The target must be printable ASCII without whitespace; anything
        // else has to be percent-encoded by the client.
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte == 0x7F) {
            return fail(ParseError::malformed_start_line);
        }
    }

    if (version == "HTTP/1.1") {
        request_.version = Version::http_1_1;
    } else if (version == "HTTP/1.0") {
        request_.version = Version::http_1_0;
    } else {
        // Includes HTTP/0.9 and HTTP/2.0 written into a 1.x start line.
        return fail(ParseError::unsupported_version);
    }

    request_.method = method_from_token(method);
    request_.target.assign(target);

    input.consume(line.consumed);
    return true;
}

Result<bool> RequestParser::parse_headers(Buffer& input) {
    for (;;) {
        Result<std::optional<Line>> located = next_line(input.readable(), limits_.allow_bare_lf);
        if (!located) {
            return fail(located.error());
        }
        if (!located->has_value()) {
            if (input.size() > limits_.max_header_line) {
                return fail(ParseError::limit_exceeded);
            }
            return false;
        }

        const Line line = **located;

        // An empty line ends the header section.
        if (line.text.empty()) {
            input.consume(line.consumed);
            return true;
        }

        if (line.text.size() > limits_.max_header_line) {
            return fail(ParseError::limit_exceeded);
        }

        // A continuation line (obs-fold). Rejected by default: RFC 9112 §5.2
        // tells recipients to reject it, and accepting it while a peer does
        // not is a smuggling differential.
        if (is_ows(line.text.front())) {
            if (!limits_.allow_obsolete_line_folding) {
                return fail(ParseError::obsolete_line_folding);
            }
            if (request_.headers.empty()) {
                return fail(ParseError::malformed_header);
            }
            input.consume(line.consumed);
            continue;
        }

        const std::size_t colon = line.text.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return fail(ParseError::malformed_header);
        }

        const std::string_view name = line.text.substr(0, colon);

        // "Header : value" — whitespace before the colon. RFC 9112 §5.1 says
        // a server MUST reject this; it is one of the best-known smuggling
        // vectors, because intermediaries disagree about whether the field
        // name includes the space.
        if (is_ows(name.back())) {
            return fail(ParseError::whitespace_before_colon);
        }
        for (const char c : name) {
            if (!is_tchar(static_cast<unsigned char>(c))) {
                return fail(ParseError::malformed_header);
            }
        }

        const std::string_view raw_value = line.text.substr(colon + 1);
        for (const char c : raw_value) {
            if (!is_field_vchar(static_cast<unsigned char>(c))) {
                return fail(ParseError::malformed_header);
            }
        }
        const std::string_view value = trim_ows(raw_value);

        if (request_.headers.size() + 1 > limits_.max_header_count) {
            return fail(ParseError::limit_exceeded);
        }
        headers_total_ += line.text.size();
        if (headers_total_ > limits_.max_headers_total) {
            return fail(ParseError::limit_exceeded);
        }

        request_.headers.append(std::string{name}, std::string{value});
        input.consume(line.consumed);
    }
}

Result<void> RequestParser::decide_framing() {
    // RFC 9112 §6.1–6.3. The order of these checks is the whole point: the
    // conflict case is resolved by rejecting, never by preferring one header.
    const bool has_te = request_.headers.contains("Transfer-Encoding");
    const std::size_t cl_count = request_.headers.count("Content-Length");

    if (has_te && cl_count > 0) {
        // The classic CL.TE / TE.CL smuggling setup. Some stacks honour
        // Content-Length, others Transfer-Encoding; a request that carries
        // both is an attack or a bug, and either way it is not answerable.
        return fail(ParseError::framing_conflict);
    }

    if (has_te) {
        const std::vector<std::string_view> raw = request_.headers.get_all("Transfer-Encoding");
        std::vector<std::string_view> codings;
        for (const std::string_view field : raw) {
            for (const std::string_view coding : split_list(field)) {
                codings.push_back(coding);
            }
        }
        if (codings.empty()) {
            return fail(ParseError::invalid_transfer_encoding);
        }

        // `chunked` must be the final coding, and may appear only once:
        // otherwise the message length is undeterminable (RFC 9112 §6.3).
        std::size_t chunked_count = 0;
        for (const std::string_view coding : codings) {
            if (equals_ignore_case(coding, "chunked")) {
                ++chunked_count;
            }
        }
        if (chunked_count != 1 || !equals_ignore_case(codings.back(), "chunked")) {
            return fail(ParseError::invalid_transfer_encoding);
        }
        // Any other coding (gzip, deflate) is a content transform this parser
        // does not apply; refusing beats silently handing back compressed
        // bytes that the caller will treat as plaintext.
        if (codings.size() != 1) {
            return fail(ParseError::invalid_transfer_encoding);
        }

        request_.body_kind = BodyKind::chunked;
        return Result<void>{};
    }

    if (cl_count > 0) {
        const std::vector<std::string_view> values = request_.headers.get_all("Content-Length");
        std::optional<std::uint64_t> agreed;
        for (const std::string_view field : values) {
            // A single field may itself be a list: "Content-Length: 5, 5".
            for (const std::string_view item : split_list(field)) {
                const std::optional<std::uint64_t> parsed = parse_decimal(item);
                if (!parsed) {
                    return fail(ParseError::malformed_content_length);
                }
                if (agreed && *agreed != *parsed) {
                    return fail(ParseError::inconsistent_content_length);
                }
                agreed = parsed;
            }
        }
        if (!agreed) {
            return fail(ParseError::malformed_content_length);
        }
        if (*agreed > limits_.max_body_size) {
            return fail(ParseError::limit_exceeded);
        }

        request_.body_kind = BodyKind::length;
        request_.content_length = *agreed;
        return Result<void>{};
    }

    request_.body_kind = BodyKind::none;
    return Result<void>{};
}

Result<ParseStep> RequestParser::read_length_body(Buffer& input) {
    if (input.empty()) {
        return ParseStep::need_more;
    }

    const std::span<const std::byte> available = input.readable();
    const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(body_remaining_, static_cast<std::uint64_t>(available.size())));

    body_ = available.first(take);
    body_seen_ += take;
    body_remaining_ -= take;
    pending_consume_ = take;

    if (body_remaining_ == 0) {
        state_ = State::done;
    }
    return ParseStep::body;
}

Result<RequestParser::Progress> RequestParser::read_chunk_header(Buffer& input) {
    Result<std::optional<Line>> located = next_line(input.readable(), limits_.allow_bare_lf);
    if (!located) {
        return fail(located.error());
    }
    if (!located->has_value()) {
        if (input.size() > limits_.max_chunk_extension + 32) {
            return fail(ParseError::limit_exceeded);
        }
        return Progress::need_data;
    }

    const Line line = **located;

    // chunk-size [ chunk-ext ] CRLF
    const std::size_t semicolon = line.text.find(';');
    const std::string_view size_text = line.text.substr(0, semicolon);
    if (semicolon != std::string_view::npos) {
        const std::string_view extension = line.text.substr(semicolon + 1);
        if (extension.size() > limits_.max_chunk_extension) {
            return fail(ParseError::limit_exceeded);
        }
    }

    // No leading/trailing whitespace, no sign, hex only.
    const std::optional<std::uint64_t> size = parse_hex(size_text);
    if (!size) {
        return fail(ParseError::malformed_chunk);
    }
    if (*size > limits_.max_chunk_size) {
        return fail(ParseError::limit_exceeded);
    }
    if (body_seen_ + *size > limits_.max_body_size) {
        return fail(ParseError::limit_exceeded);
    }

    input.consume(line.consumed);

    if (*size == 0) {
        saw_last_chunk_ = true;
        state_ = State::body_chunk_trailer;
        return Progress::advanced;
    }

    chunk_remaining_ = *size;
    state_ = State::body_chunk_data;
    return Progress::advanced;
}

Result<RequestParser::Progress> RequestParser::read_chunk_data(Buffer& input) {
    if (chunk_remaining_ > 0) {
        if (input.empty()) {
            return Progress::need_data;
        }
        const std::span<const std::byte> available = input.readable();
        const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(
            chunk_remaining_, static_cast<std::uint64_t>(available.size())));

        body_ = available.first(take);
        body_seen_ += take;
        chunk_remaining_ -= take;
        pending_consume_ = take;
        return Progress::emitted_body;
    }

    // Chunk data is followed by its own CRLF, which must be present.
    Result<std::optional<Line>> located = next_line(input.readable(), limits_.allow_bare_lf);
    if (!located) {
        return fail(located.error());
    }
    if (!located->has_value()) {
        return Progress::need_data;
    }
    if (!(*located)->text.empty()) {
        return fail(ParseError::malformed_chunk);
    }
    input.consume((*located)->consumed);
    state_ = State::body_chunk_header;
    return Progress::advanced;
}

Result<RequestParser::Progress> RequestParser::read_chunk_trailer(Buffer& input) {
    for (;;) {
        Result<std::optional<Line>> located = next_line(input.readable(), limits_.allow_bare_lf);
        if (!located) {
            return fail(located.error());
        }
        if (!located->has_value()) {
            if (input.size() > limits_.max_header_line) {
                return fail(ParseError::limit_exceeded);
            }
            return Progress::need_data;
        }

        const Line line = **located;
        if (line.text.empty()) {
            input.consume(line.consumed);
            state_ = State::done;
            return Progress::finished;
        }

        if (line.text.size() > limits_.max_header_line) {
            return fail(ParseError::limit_exceeded);
        }

        const std::size_t colon = line.text.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            return fail(ParseError::malformed_header);
        }
        const std::string_view name = line.text.substr(0, colon);
        if (is_ows(name.back())) {
            return fail(ParseError::whitespace_before_colon);
        }
        for (const char c : name) {
            if (!is_tchar(static_cast<unsigned char>(c))) {
                return fail(ParseError::malformed_header);
            }
        }

        if (trailers_.size() + 1 > limits_.max_header_count) {
            return fail(ParseError::limit_exceeded);
        }

        trailers_.append(std::string{name}, std::string{trim_ows(line.text.substr(colon + 1))});
        input.consume(line.consumed);
        return Progress::advanced;
    }
}

}  // namespace continuo::http
