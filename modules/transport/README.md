# modules/transport — reserved slot

TCP, UDP, and Unix-domain transports live here: `Listener`, `Connector`,
`Socket`, and the readiness backends behind them (kqueue / epoll / IOCP).

Empty in v0.1 on purpose. A transport is only worth writing once the event loop
and the readiness abstraction in `modules/core` are settled — writing sockets
first is how a library ends up with its poll loop welded to one protocol.

Shape this will take, per `docs/ARCHITECTURE.md`:

```cpp
namespace continuo::tcp {

class Listener {
public:
    static Result<Listener> bind(const Endpoint& endpoint, ListenOptions options = {});
    Task<Result<Socket>> accept();
};

class Socket {  // models continuo::AsyncStream
public:
    Task<Result<std::size_t>> read_some(std::span<std::byte> destination);
    Task<Result<std::size_t>> write_some(std::span<const std::byte> source);
    void close();
};

}  // namespace continuo::tcp
```

`ListenOptions` is where the bind semantics that started this project get an
explicit home: exclusive binding is a documented choice with a platform-correct
default, not a per-OS surprise a consumer discovers in production.

This layer may include `continuo/core/…` and nothing above it.
