#pragma once

#include "mira/http/parser.hpp"

namespace Mira::http {

/// 独立增量响应解析器。每个 1xx 也是一条完整消息；最终响应需 reset 后继续解析。
/// 101 和 CONNECT 的 2xx 返回 not_supported，不消费隧道字节。
/// 严格拒绝 obs-fold（即使 Limits 的兼容开关打开）、非 chunked 传输编码和歧义 framing。
/// limits 的 header count/total 在头部和 trailers 间共享。
class ResponseParser {
public:
    explicit ResponseParser(Method method = Method::get, Limits limits = {}) noexcept
        : limits_(limits), method_(method) {}

    /// eof=true 表示输入后不再有字节。固定长度/chunked 的提前 EOF 返回 Errc::eof。
    [[nodiscard]] Result<ParseStep> parse(Buffer& input, bool eof = false);
    /// 仅在完整消费 complete 后重置。不得在 body span 仍在使用时修改 input。
    void reset(Method method = Method::get);
    [[nodiscard]] const Response& response() const noexcept { return response_; }
    /// 指向 input；有效期至下次 parse/reset 或调用者修改 input。
    [[nodiscard]] std::span<const std::byte> body() const noexcept { return body_; }
    [[nodiscard]] const HeaderMap& trailers() const noexcept { return trailers_; }
    [[nodiscard]] std::uint64_t body_bytes_seen() const noexcept { return seen_; }
    [[nodiscard]] bool done() const noexcept { return state_ == State::done && pending_ == 0; }

private:
    enum class State {
        start,
        headers,
        length,
        chunk_size,
        chunk_data,
        chunk_end,
        trailers,
        eof_body,
        done
    };
    [[nodiscard]] Result<ParseStep> advance(Buffer& input, bool eof);
    [[nodiscard]] Result<void> framing();
    [[nodiscard]] Result<void> field(std::string_view text, bool trailer);
    Limits limits_;
    Method method_;
    State state_{State::start};
    Response response_{};
    HeaderMap trailers_{};
    std::span<const std::byte> body_{};
    std::uint64_t seen_{0};
    std::uint64_t remaining_{0};
    std::size_t pending_{0};
    std::size_t header_bytes_{0};
    Error error_{};
};

}  // namespace Mira::http
