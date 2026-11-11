# 架构

QoderZ 是一个单进程的本地网关：它读取 Qoder 账户，向上游说 Qoder 的私有协议，向下游暴露
OpenAI 兼容的 HTTP API。设计目标按优先级排序：**正确性、可维护性、低常驻内存、低延迟**。
C++23 的协程、RAII 与 `std::expected` 让异步路径既直白又不依赖垃圾回收。

## 1. 分层

依赖只能向下：

依赖主链：`cli → server → openai → qoder → config → common`。
`protocol` 被 `openai`/`qoder`/`server` 依赖，自身只依赖 `common`；`qoder` 也依赖 `protocol`。
`openai` 可以构造 `qoder` 的请求类型，但 `qoder` 不认识任何 OpenAI 概念。

| 层 | 职责 | 不做什么 |
| --- | --- | --- |
| `common` | 错误、日志、JSON 别名、base64/哈希/随机、时间、文件、HTTP/TLS 客户端 | 不读 Qoder 配置，不知道账户 |
| `protocol` | 纯编解码：BodyCodec、COSY、SSE 切行、文本工具调用、事件类型 | 不做 I/O、不读时钟（时间由调用方传入） |
| `config` | 站点、VPC、凭据文件、账户身份/默认/选择、路径 | 不联网 |
| `qoder` | 设备/PAT 登录、token 刷新、模型目录、用量、聊天客户端与流、错误映射 | 不知道 OpenAI 响应形状 |
| `openai` | OpenAI 请求校验与翻译、窗口/effort、响应/流翻译 | 不直接发 HTTP |
| `server` | 监听、路由、鉴权、请求上限、账户池、会话粘滞、错误响应、优雅关闭 | 不解析 Qoder 私有 JSON |
| `cli` | 参数解析、命令编排、输出格式、退出码 | 不组装 URL、不签名、不翻译协议 |

`protocol` 与 `openai` 必须能在没有网络、没有 io_context 的单元测试里运行。

## 2. 目录

```text
src/
  main.cpp                 组合根：argv -> cli::app
  common/
    error.hpp               error_kind / error / result
    log.hpp .cpp            level / logger
    json.hpp                nlohmann alias
    clock.hpp .cpp          clock 抽象与系统时钟
    strings.hpp             trim / 大小写 / 前缀
    codec/                  base64、hash、random
    net/                    http_client、url、connection、connection_pool、tls
  protocol/
    body_codec.hpp .cpp
    cosy_signer.hpp .cpp
    sse/                    line_reader、text_tool_splitter、chat_event
  config/
    site.hpp .cpp
    credentials.hpp .cpp
    accounts.hpp .cpp
    paths.hpp .cpp
  qoder/
    auth/                   device、pat、token（support/ 放浏览器与 JSON HTTP）
    catalog/                model、listing、parser、fallback、service
    usage/                  usage
    chat/                   request、client、stream
    errors.hpp .cpp
  openai/
    request/                request（read_request / prepare_request）、body、normalize、types
    response/               response、stream_frames
    selection/              selectors、session
  server/
    server.hpp .cpp
    router.hpp .cpp
    config.hpp
    api_key.hpp
    error_writer.hpp .cpp
    endpoints/              health、models、chat、chat_select
    http/                   chat_handler、connection_writer、response_sink
    pool/                   account_pool、health、affinity
  cli/
    app.hpp .cpp
    options.hpp .cpp
    output.hpp .cpp
    commands/               account、login、models、usage、serve/
tests/
  unit/<layer>/             与 src 同构
  integration/              进程内集成（server_core / chat_endpoint）与进程级端到端
  support/                  stub_server、temp_dir、server_harness、fake_clock、async_test
  stub_upstream/            独立的 loopback 假上游（qoderz_stub_upstream）
```

单目录代码文件不超过 8 个；超过时按职责再分子目录。文件有效行目标 ≤600，超过需要理由。

## 3. 一次聊天请求的数据流

```text
HTTP request
  │  server/router：鉴权、body 上限、按 path 分发
  ▼
openai/request：校验 + prepared_request（session id 先行）-> request_context（消息、工具、窗口）
  │
  ▼
server/pool：按 session 粘滞选账户；pinned 时只有一个
  │
  ▼
qoder/chat/client：取新鲜 token -> 组装 JSON -> BodyCodec -> COSY 签名
  │                         └─ busy 时同一份 body/headers 重试
  ▼
Qoder SSE ──► protocol/sse/line_reader ──► qoder/chat/stream ──► chat_event
  │
  ▼
openai/response：chat_event -> OpenAI chunk / 完整 JSON
  │
  ▼
server：写 SSE 帧或 JSON；成功后 MarkAnswered
```

关键边界：

- **首事件之前**才能换账户：`client.open()` 读到第一个事件并保留；认证/额度失败在这里暴露。
- **首事件之后**只写流错误帧，绝不重试。
- `MarkAnswered` 只在下游完整写完后调用；否则下一次同轮请求仍是 `stage: start`。
- 账户池只处理认证/额度冷却；busy、请求错误、上游错误不冷却。

## 4. 并发模型

- 一个 `asio::io_context`，N 个线程；N 默认 `min(hardware_concurrency, 4)`，可由 `--threads` 调整。
- 每个 TCP 连接一个协程，连接内请求顺序处理；SSE 按帧顺序写，不需要连接内锁。
- 共享状态只有账户池、会话粘滞表、任务表、模型缓存、日志；各自用互斥量保护。
- 协程里不持有互斥量跨 `co_await`；刷新 token 的单飞用异步门/定时等待实现。
- 取消：客户端断开时关闭 socket，协程链收到 `asio::error::operation_aborted`；上游流与
  定时器都随协程作用域销毁。禁止在取消路径写响应。
- 关闭：`signal_set` 捕获 SIGINT/SIGTERM，先关 acceptor，再给在途连接一个短宽限，
  最后 `io_context.stop()` 并 join。

## 5. 内存与性能

- 无 GC；所有缓冲、连接、JSON、流对象都由 RAII 管理。
- 请求体一次性读入 `std::string`，JSON DOM 的生命周期不超过请求；大 body 不复制多次。
- 上游请求体：先计算容量，写入一个字符串，BodyCodec 原地/预分配输出，签名直接对编码后字节做。
- SSE 读缓冲复用，行解析只做切片；事件里的字符串按需复制，避免整条流反复拼接。
- 上游连接复用：按 origin 维护空闲连接（上限小、TTL 短）；流式响应持有连接直到结束。
  连接失效时在“尚未写出任何响应”阶段换新连接重试一次。
- 日志默认 info；请求级细节在 debug，SSE 帧在 trace。日志格式化不做无谓分配。
- Release 构建走 CMake 默认的 `-O3 -DNDEBUG`；不开启 LTO，也不做 `-march=native`，保证 nix 产物可移植。
- 性能结论只报告实测：idle RSS/PSS、stub 负载下的 RSS/PSS、首字节延迟。测量脚本从外部
  发压，绝不在被测进程内造负载。

## 6. 错误模型

```cpp
enum class error_kind {
  configuration, auth, quota, busy, bad_request, upstream, io, protocol, canceled, internal,
};

struct error {
  error_kind m_kind;
  std::string m_message;
  int m_http_status = 0;
  std::string m_code;
  std::optional<int> m_retry_after;
};
```

- 可预期的失败返回 `std::expected<T, error>`；异常只用于构造/分配/第三方库的不可恢复错误，
  并在边界捕获一次。
- 协议解析返回 `result<T>` 或 `std::optional`，不用异常表达“这一帧坏掉”。
- 错误只在一个地方记录：CLI 边界或 HTTP 响应写出处；中间层只传播。
- 下游映射：configuration/internal → 500，auth → 401，quota → 429，busy → 503 + Retry-After，
  bad_request → 400，upstream → 502，canceled → 499（不写 body）。
- 所有错误响应都是 OpenAI 形状：`{"error":{"message","type","code"}}`。

## 7. 测试策略

- 单元测试：Catch2，覆盖纯编解码、选择器、解析、映射；固定向量来自 NoQoder 的测试。
- 服务测试：在 127.0.0.1 上启动进程内 stub HTTP 服务器（`http`，非 TLS），用
  `resolved_site` 直接指向它；不依赖环境变量。
- 集成测试：独立 `stub_upstream` 进程 + `qoderz serve` 子进程，走真实 HTTP/SSE 路径；
  凭据是临时文件，`QODERZ_UPSTREAM` 指向 loopback。
- 禁止任何测试访问真实 Qoder；`QODERZ_UPSTREAM` 的非 loopback 校验本身也要有测试。
- 时间相关的健康/过期逻辑注入 `clock`，测试推进假时钟，不 sleep。
- 每个修复先写能失败的测试；codec 变更必须保留旧算法/固定向量作为 oracle。

## 8. 安全

- 默认只监听 loopback；非 loopback 且无 API key 时拒绝启动（可用 `--allow-public` 显式覆盖）。
- API key 用常量时间比较；未配置 key 时 `/v1/*` 接受任意请求（本机使用场景），`/healthz` 与 404 本来就不鉴权。
- 凭据文件 0600、目录 0700、原子写；日志与 `--json` 绝不包含 token/PAT。
- `QODERZ_UPSTREAM` 只接受 loopback，防止凭据外流。
- 上游 TLS 验证系统 CA 与主机名；不提供关闭验证的开关。

## 9. 明确的非目标

- 不做 Anthropic Messages / Gemini API；只做 OpenAI chat completions。
- 不做每日签到、活动奖励、网页搜索替身。
- 不做 API-key 到账户的映射；一个本地密钥对应整个池。
- 不兼容 NoQoder 的凭据文件格式、环境变量前缀或 CLI 语法。
- 不追求“零拷贝到 TLS”；在正确性与可维护性之后才优化热点。
