// A TCP echo server, in the shape Mira intends.
//
// Three things here are the point, and none of them is the echoing:
//
//   * `run_until_complete` is how synchronous code starts a coroutine. There
//     is no detached-task type to declare and no loop to hand-pump.
//   * `TaskScope` owns the per-connection tasks. Connections outlive the
//     `accept` that produced them but not the scope, so nothing is detached
//     and nothing leaks.
//   * The scope's stop token is threaded into every read. That is what makes
//     `request_stop()` actually end the connections rather than merely ask.
//
// Usage: mira_echo_server [port] [connection-limit]
//   port              0 (the default) binds an ephemeral loopback port
//   connection-limit  total connections to accept, not a concurrency limit;
//                     0 (the default) serves until the process is interrupted
// A finite limit stops accepting, then waits for every peer to close its write
// side. Process interruption uses the default signal behavior, not graceful shutdown.

#include <mira/core/event_loop.hpp>
#include <mira/core/stream.hpp>
#include <mira/core/task.hpp>
#include <mira/core/task_scope.hpp>
#include <mira/transport/tcp.hpp>

#include <array>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <utility>

using Mira::EventLoop;
using Mira::Errc;
using Mira::Result;
using Mira::Task;
using Mira::TaskScope;
using Mira::transport::Endpoint;
namespace tcp = Mira::transport::tcp;

namespace {

template <typename Integer>
bool parse_number(std::string_view text, Integer& value) {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size();
}

/// Echo one connection until the peer closes it or the scope stops.
///
/// Takes the socket **by value**: this coroutine owns it for its whole life,
/// and the frame is what keeps it alive. A reference would dangle the moment
/// the accept loop went round again.
Task<void> echo(tcp::Socket socket, std::stop_token stop) {
    std::array<std::byte, 4096> buffer{};

    for (;;) {
        const Result<std::size_t> read = co_await socket.read_some(buffer, {.stop = stop});
        if (!read) {
            // `eof` is the peer hanging up, `cancelled` is us shutting down.
            // Neither is a failure worth reporting.
            if (read.error() != Errc::eof && read.error() != Errc::cancelled) {
                std::fprintf(stderr, "read: %s\n", read.error().message().c_str());
            }
            co_return;
        }

        const Result<void> written =
            co_await Mira::write_all(socket, std::span{buffer}.first(*read), {.stop = stop});
        if (!written) {
            if (written.error() != Errc::cancelled) {
                std::fprintf(stderr, "write: %s\n", written.error().message().c_str());
            }
            co_return;
        }
    }
}

Task<void> serve(tcp::Listener& listener, std::size_t limit) {
    TaskScope scope;
    std::size_t served = 0;

    for (;;) {
        Result<tcp::Socket> accepted = co_await listener.accept();
        if (!accepted) {
            std::fprintf(stderr, "accept: %s\n", accepted.error().message().c_str());
            scope.request_stop();
            break;
        }

        scope.spawn(echo(std::move(*accepted), scope.get_stop_token()));

        if (limit != 0 && ++served >= limit) {
            break;
        }
    }

    // Reaching the limit stops acceptance, not the connections just accepted.
    // Let them finish at peer EOF; only an accept failure cancels their I/O.
    // In both cases join waits for every child frame to be released.
    co_await scope.join();
}

}  // namespace

int main(int argc, char** argv) {
    std::uint16_t port = 0;
    std::size_t limit = 0;
    if (argc > 3 || (argc > 1 && !parse_number(argv[1], port)) ||
        (argc > 2 && !parse_number(argv[2], limit))) {
        std::fprintf(stderr, "usage: %s [port: 0..65535] [connection-limit: nonnegative integer]\n",
                     argv[0]);
        return 2;
    }

#ifdef SIGPIPE
    // Application policy only: disconnected peers must not terminate this example.
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        std::fprintf(stderr, "could not ignore SIGPIPE\n");
        return 1;
    }
#endif

    Result<EventLoop> created = EventLoop::create();
    if (!created) {
        std::fprintf(stderr, "event loop: %s\n", created.error().message().c_str());
        return 1;
    }
    EventLoop& loop = created.value();

    // Loopback rather than every interface: an example should not open a port
    // to the network just because someone ran it to see what it does.
    Result<tcp::Listener> listener = tcp::Listener::bind(loop, Endpoint::loopback(port));
    if (!listener) {
        std::fprintf(stderr, "bind: %s\n", listener.error().message().c_str());
        return 1;
    }

    std::printf("echo server listening on %s\n",
                listener->local_endpoint().to_string().c_str());
    std::fflush(stdout);

    const Result<void> ran = loop.run_until_complete(serve(*listener, limit));
    if (!ran) {
        std::fprintf(stderr, "run: %s\n", ran.error().message().c_str());
        return 1;
    }
    return 0;
}
