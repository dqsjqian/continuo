# Continuo 一期协议交接（2026-09-25 更新）

## 先读结论

**一期目标已全面实码落地并通过本机验证：UDP、异步 DNS、HTTP/1 客户端、多协议 ALPN、HTTP/2、实验性 QUIC/HTTP/3。** 本机 macOS Debug 与 ASan+UBSan 全开（TLS/H2/H3）36/36 CTests 通过；但「本机通过」不等于「一期验收完成」——Windows/Linux 运行、真实 UDP 网络上的 QUIC、独立客户端互操作与性能实测仍未做，达成这些关卡前不得宣称生产就绪。

用户原话（2026-09-24）：

- 「HTTP/2、3还是非常有必要一期就支持的，是工作量很大吗？」——已按此执行。
- 「」「你赶紧弄吧，弄不完就写个交接 md」——本轮全程。

## 协作约束（仍然有效）

- 独立 C++23 网络库；不是 cpp-httplib 兼容层，不依赖 Aria。允许合理的破坏性 API 调整。
- 阶段一完善 Continuo；迁移下游、删除旧 HTTP 库、公开仓库属阶段二。当前仓库保持 PRIVATE。
- 只在 main 工作，不建 feature 分支、不 rebase/强推/amend、不绕过 hooks。
- 禁止加载  的技能或代理。
- 每个关键修复有变异验证。Windows 改动 mingw 只做类型检查，不能替代 MSVC/IOCP 运行测试。

## 本轮新增模块与能力（按代码不是宣传）

| 模块 | 内容 | 验证 |
|---|---|---|
| `modules/transport` UDP | `udp::Socket::bind/send_to/receive_from`，完成式数据报、零长包有效、截断报 `message_size` 并消费整包、同方向互斥、close 重入安全（generation token 防旧 fd 重试） | macOS 130 checks；8 项隔离变异全部杀死；Darwin 零缓冲 recvmsg 不消费包的缺陷被真实测试抓到并修复 |
| `modules/transport` resolver | 有界线程池 + 队列的 `getaddrinfo`，`Resolver::resolve` 返回去重 `vector<Endpoint>`（IPv6 scope 纳入），取消等待/绝对 deadline，EAI_SYSTEM 保留 errno；worker 不持 loop/协程指针 | macOS 93 checks；ASan/UBSan/TSan 全过；3 项隔离变异杀死 |
| `modules/http` H1 客户端 | `ResponseParser`（HEAD/1xx/204/304、CL/chunked/EOF、防走私）+ `ClientConnection<BoundedStream>`（start/read_body 零收集响应体、总 deadline、错误后不复用） | 723+720 checks；8 项隔离变异杀死；独立只读审查无新阻断 |
| `modules/tls` 多 ALPN | `Context::client_alpn/server_alpn(span<const string_view>)`，服务端优先选择、无交集 fatal、SSL_CTX ex_data 生命周期 | 真实回环握手回归；服务端优先变异杀死；只读审查无阻断 |
| `modules/http2`（可选 `CONTINUO_ENABLE_HTTP2`） | nghttp2 1.70.0 封装：Session/Connection、多流、HPACK、限额、消费驱动窗口、RST/GOAWAY；泛型 BoundedStream 适配 | in-memory 1320 checks + 真实 TCP/TLS(ALPN h2) 网络测试 788 checks（150KB 双向、碎片、GOAWAY）；3 项变异杀死；ASan 通过 |
| `modules/quic`+`modules/http3`（可选 `CONTINUO_ENABLE_HTTP3`，实验性） | ngtcp2 1.22.1 ossl + nghttp3 1.15.0：QUIC v1 数据报引擎（无 I/O）、TLS1.3 验证、PTO 重传、流控/取消/两阶段 GOAWAY；H3 请求/响应 + QPACK；错误统一 `continuo::Result`（ngtcp2/nghttp3 独立 category）；随机源下沉 `quic::fill_random`，HTTP3 不直接依赖 OpenSSL | 真实加密数据报 client/server：normal/chaos（丢包/乱序/重复）/bad-host/untrusted/bad-alpn/small-budget/priority 8 组；MAX_STREAMS 延迟归还专项回归 + 变异杀死；ASan+UBSan 7/7 |
| 根 CMake | `CONTINUO_ENABLE_HTTP2/HTTP3` 默认 OFF；TLS/H2/H3 各自独立 export set（Tls/Http2/Http3Targets）+ 依赖脚本（`continuo-http2-dependencies.cmake`、`continuo-http3-dependencies.cmake`）；显式 COMPONENTS 只加载所请求闭包 | 三个独立 consumer 实测：base（禁 OpenSSL 查找）、TLS、http2 缺失时清晰诊断 |
| `tools/ci/build_protocol_deps.py` | 固定 nghttp2 1.70.0 / nghttp3 1.15.0 / ngtcp2 1.22.1（SHA256 + MIT）的可复现静态构建脚本；--offline 模式；仅写 build/protocol-deps；Windows 拒绝运行（未验证） | 脚本就绪；本机依赖产物在 build/deps、build/phase1-deps 已验证 |
| core 修复 | POSIX `detach→fail_waiters→finalize` 同步恢复补 DispatchScope（destroy-during-detach / reentrant-run-once-during-detach 精确退出码 77/78 负测 + 去 guard 变异 ASan UAF 证实）；`tcp::connect`/`Endpoint::parse` NUL 与 IPv6 scope 数值/名称校验修复 + `address()` 保留 scope；lazy connect 拥有 Endpoint（UAF 变异杀死） | core 14/14；transport 180 checks |

## 本机验证汇总（macOS 15 / AppleClang 21 / kqueue / OpenSSL 3.6.2）

- **Debug 全开（TLS+H2+H3）**：36/36 CTests 通过。
- **ASan+UBSan 全开**：36/36 通过（`UBSAN_OPTIONS=halt_on_error=1`）。
- **MinGW-w64 交叉语法**：core/transport/http/quic/http3 源与相关测试 C++23 全警告通过（仅历史遗留 pragma/转换两处既有警告，非本轮引入）；**不是 Windows 运行证据**。
- **NDK29 arm64-v8a API26**：非 TLS 基础库（含 UDP/DNS/H1）+ `ClientConnection<tcp::Socket>` 显式实例化交叉编译通过；**未在设备/模拟器运行**。
- **安装消费**：TLS ON 安装后 base consumer（禁 OpenSSL）链接运行通过；TLS consumer 链接 OpenSSL 调用 `Context::client` 通过；请求未安装的 http2 得到精确诊断。H2 ON/TLS OFF 安装的 http2 consumer 由 H2 实现者验证通过。
- `git diff --check` 与 `tools/ci/check_layering.py` 通过。

复现（全开 Debug）：

```sh
python3 tools/ci/build_protocol_deps.py            # 下载并构建固定版本依赖（首次）
cmake -S . -B build/all -DCMAKE_BUILD_TYPE=Debug \
  -DCONTINUO_ENABLE_TLS=ON -DCONTINUO_ENABLE_HTTP2=ON -DCONTINUO_ENABLE_HTTP3=ON \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3) \
  -DCMAKE_PREFIX_PATH="$PWD/build/protocol-deps/prefix"
cmake --build build/all -j 8
ctest --test-dir build/all --output-on-failure
```

## 尚未完成 / 一期剩余验收关卡

1. **跨平台运行**：全部新代码（UDP/DNS/H1 client/H2/QUIC/H3）没有 Linux(epoll)、Windows(IOCP/MSVC) 运行证据；CI 矩阵已存在，推送后需确认全绿。
2. **QUIC 未接真实 UDP**：当前是数据报输入/输出的引擎 + 内存测试；需要 `quic::Engine` × `udp::Socket` 的调度入口（定时驱动 + 数据报收发循环）。
3. **独立互操作**：H2/H3 未与 curl/nghttp2 官方客户端互通测试。
4. **HTTP/1 服务端请求体仍是缓冲收集**；H2/H3 发送 body 为有界整块，非异步流式源。
5. 端到端背压/总内存上限、fuzz、性能实测未做。
6. ngtcp2 OpenSSL ossl 适配仍是上游 experimental；移动端 TLS/H2/H3 未交叉编译验证。

## 关键契约备忘

- Resolver 析构可能等待进行中的 `getaddrinfo`（系统调用不可中断）；取消只终止等待。
- H1 客户端连接复用保护只覆盖**已预读**的额外响应字节；BoundedStream 无非阻塞探测，不能声称全面防响应污染。`start` 的 Request/body span 借用契约：延迟 await 必须保活。
- H2 `Connection::flush/read/pump` 与 Session 调用不得跨挂起重叠；`output()` 返回拥有 vector。
- QUIC `cancel` 前置范围校验（<2^62、client-bidi %4==0、活跃未关闭）才进入 nghttp3。
- QUIC 关闭流但未 consume 时延迟归还对端流额度，由 `consume` 清空记录后补发（`quic_stream-limit` 回归锁定该行为）。
- ALPN 65535 是配置编码上限，接近上限的真实 ClientHello 可能因 TLS 总扩展限制握手失败。
