#pragma once

#include "Mira/core/stream.hpp"
#include "Mira/http2/session.hpp"

#include <array>
#include <utility>

namespace Mira::http2 {

// 适配器不拥有底层流；底层流和 Connection 必须存活且保持地址直到任务结束。
// 同一实例只允许一个未完成操作，包括挂起期间；此时不得访问 session()，
// 不得从其他任务重叠调用 read/flush/pump，也不可移动或析构 Connection。
// pump 一次执行输出、一次输入和协议应答输出。调用方轮询各 stream
// 并 take_body，驱动所有并发请求；不在单个请求上串行阻塞。
// output() 的 vector 由 flush 协程帧独立持有，跨 write_all 挂起不借用引擎内存。
// I/O 取消/deadline 终止连接；单流取消请用 Session::cancel。
template<BoundedStream Transport>
class Connection {
public:
    Connection(Transport& transport, Session session)
        : transport_(&transport), session_(std::move(session)) {}
    Session& session() noexcept { return session_; }
    const Session& session() const noexcept { return session_; }

    Task<Result<void>> flush(OperationOptions options = {}) {
        while (session_.wants_write()) {
            auto bytes = session_.output();
            if (!bytes) co_return fail(bytes.error());
            if (bytes->empty()) break;
            auto result = co_await write_all(*transport_, *bytes, options);
            if (!result) {
                session_.close(result.error());
                co_return fail(result.error());
            }
        }
        co_return Result<void>{};
    }

    Task<Result<void>> read(OperationOptions options = {}) {
        std::array<std::byte, 16384> bytes{};
        auto result = co_await transport_->read_some(bytes, options);
        if (!result || *result == 0) {
            auto error = result ? make_error_code(Errc::eof) : result.error();
            session_.close(error);
            co_return fail(error);
        }
        co_return session_.receive(std::span<const std::byte>(bytes.data(), *result));
    }

    Task<Result<void>> pump(OperationOptions options = {}) {
        auto result = co_await flush(options);
        if (!result) co_return result;
        result = co_await read(options);
        if (!result) co_return result;
        co_return co_await flush(options);
    }
private:
    Transport* transport_;
    Session session_;
};

} // namespace Mira::http2
