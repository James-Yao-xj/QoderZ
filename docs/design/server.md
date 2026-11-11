# HTTP 服务与账户池

本文定义 `qoderz serve` 的对外行为、请求编排、账户池与健康策略。协议翻译的细节见
[OpenAI 兼容翻译](../protocol/openai-compat.md)，上游流见
[Qoder 聊天协议](../protocol/qoder-chat.md)。

## 1. 监听与安全

- 默认监听 `http://127.0.0.1:8788`；`--listen` 或 `QODERZ_LISTEN` 可覆盖。
- 只支持 HTTP/1.1。默认不启用 HTTP/2、不做 TLS 终止；需要 TLS 时放在反向代理后面。
- 非 loopback 监听且没有 API key 时**拒绝启动**；确实要公开时用 `--allow-public` 显式覆盖，
  启动日志会打一条醒目警告。
- API key 来自 `--api-key`、`--api-key-stdin` 或 `QODERZ_API_KEY`。配置后：
  - 只检查 `Authorization: Bearer <key>`，scheme 大小写不敏感；
  - 比较使用常量时间；
  - `/healthz` 与 404 不鉴权；`/v1/models`、`/v1/chat/completions` 必须先鉴权。
- 未配置 key 时，`/v1/*` 接受任意请求（本机使用场景）；非 loopback 的启动限制仍然生效。
- 请求体上限默认 32 MiB（`--max-body-bytes`）。超限返回 413，`type=invalid_request_error`，
  `code` 为数字 413，并关闭连接。
- 请求 `Content-Type` 不做强制校验；body 必须是合法 JSON。

## 2. 路由

| 方法 | 路径 | 鉴权 | 行为 |
| --- | --- | --- | --- |
| GET | `/healthz` | 否 | `{"status":"ok"}`，200 |
| GET | `/v1/models` | 是 | 健康账户目录并集 |
| POST | `/v1/chat/completions` | 是 | 流式/非流式聊天 |
| 其它方法 | 已存在的路径 | 仅 `/v1/*` | 405 + `Allow` 头，OpenAI 错误体 |
| 任意方法 | 未定义路径 | 否 | 404 OpenAI 错误体 |

404 文案：

```text
unknown endpoint; use /v1/models or /v1/chat/completions
```

405 是 QoderZ 有意的改进：NoQoder 把所有方法不符都落进 404，客户端很难定位问题。
QoderZ 对已知路径返回 405，并在 `Allow` 里列出正确方法。

## 3. 错误响应

统一形状：

```json
{"error":{"message":"...","type":"...","code":<数字状态码>}}
```

`code` 恒为数字状态码，与 NoQoder 一致；Qoder 自己的错误码（例如 `10605`）只出现在
`message` 里，不占用 `code` 字段。`Content-Type: application/json`。只有 503 且
`retry_after > 0` 时写 `Retry-After`。

| 分类 | HTTP | type |
| --- | --- | --- |
| 请求 JSON/字段非法 | 400 | `invalid_request_error` |
| 配置错误 | 500 | `configuration_error` |
| 认证失败 | 401 | `authentication_error` |
| 额度耗尽 | 429 | `insufficient_quota` |
| 忙碌队列 | 503 | `server_busy` + `Retry-After` |
| 上游失败 | 502 | `upstream_error` |
| 客户端取消 | 499 | 不写 body |
| 其它 | 500 | `internal_error` |

本地 I/O 失败（读写 socket、启动浏览器等）归入 `io`，按 `common/error.hpp` 的默认映射是
500 `internal_error`；只有真正来自上游的失败才用 502 `upstream_error`。

错误只在一个地方写：路由层。中间层返回 `result<T, error>`，不自己拼 HTTP。

## 4. `/v1/models`

1. 鉴权。
2. 取该站点的账户候选（不轮转、不改变绑定）。
3. 对每个候选：确保 token 新鲜 → 取模型目录（缓存 5 分钟）→ 成功记一次 success；
   auth/quota 失败记 failure 并跳过；其它失败只记 warning，不冷却，继续下一个。
4. 按账户名排序（忽略大小写）合并目录，用模型 key 忽略大小写去重，先到先得。
5. 并集非空 → 200；为空时抛最后一个错误；没有账户 → 401 `not signed in; run: qoderz login`；
   全部冷却 → 全 auth 则 401，否则 429，消息固定
   `all Qoder accounts are temporarily unavailable; retry later`。

响应：

```json
{"object":"list","data":[{"id":"<key>","object":"model","created":<unix>,
  "owned_by":"qoder","context_window":1000000,
  "supports_images":true,"supports_tools":true,"reasoning":true}]}
```

`created` 是本次响应生成时刻，整个列表共用；不写 `name`。

HTTP 与 CLI 的模型对象**有意不同**，并不是同一个序列化器。`/v1/models` 面向 OpenAI 客户端，
用 OpenAI 的模型对象形状：`reasoning` 是 bool，表示“这个模型支不支持推理”，并带上 `created`。
`qoderz models --json` 面向人和脚本：`reasoning` 直接给出 effort 数组（形如
`["low","medium","high"]`），也不写 `created`。两边只在字段名上对齐——
`id`、`context_window`、`supports_images`、`supports_tools`——使用者不必为两种形状各学一遍。

目录缓存按 `(site, vpc, uid)` 存 5 分钟、最多 64 条；空目录返回站点 fallback，但**不缓存**，
下一次仍请求上游。`qoderz models` 始终强制刷新。

## 5. `/v1/chat/completions`

### 5.1 解析

顺序固定：

1. 鉴权；
2. 读 body（上限 32 MiB），解析 JSON；失败 → 400 `the request body is not valid JSON`；
3. 读 session 头：`X-Qoder2Api-Session`，回退 `X-Session-Id`；
4. 校验 `messages` 是数组；否则 400 `messages is required`；
5. 读 `model` 并 trim；空 → 400 `model is required`；去掉可选的 `"{site}/"` 前缀；
6. 按目录 key 忽略大小写匹配；找不到 → 400 `unknown or disabled model "<name>"`；
7. 请求翻译与会话任务准备。

### 5.2 账户选择与 failover

候选来自账户池（见 §6），按顺序尝试：

1. 确保 token 新鲜；auth/quota 失败 → 冷却并跳过；
2. 取模型目录（先缓存）；如果该账户没有请求的模型，把它记进“未见到”列表，尝试下一个；
3. 翻译请求、打开上游、peek 第一个 SSE 事件；
4. 首事件是 auth/quota → 冷却并跳过；busy → 503，不换号；其它错误 → 直接映射，不换号；
5. 成功 → 记录 success、绑定 session 到该账户、开始写响应。

如果所有候选都“没看到这个模型”（而不是失败），做第二遍：对未失败的候选强制刷新目录，
再试一次。仍未找到 → 400 `unknown or disabled model`。错误优先级：
最后一个 auth/quota 失败 > 无候选 > 未知模型。

**首事件之后绝不换号。** 一旦开始写 SSE，后续错误只写错误帧：

```text
data: {"error":{"message":"...","type":"qoder_error","code":<status>}}\n\n
```

不写 `[DONE]`，HTTP 状态仍是 200，`Retry-After` 丢失。非流式因为还没写 body，仍可返回
真正的 4xx/5xx。

### 5.3 流式

响应头：

```text
Content-Type: text/event-stream
Cache-Control: no-cache
X-Accel-Buffering: no
```

帧格式与事件映射见 [OpenAI 兼容翻译](../protocol/openai-compat.md) §8。每帧写完立即 flush。
Stop 后写 `data: [DONE]\n\n`；Error 后不写 `[DONE]`。客户端断开时停止读上游并关闭连接。

### 5.4 非流式

先聚合全部事件，遇到 Stop 停止，遇到 Error 丢弃缓冲并返回错误；成功时一次写出完整 JSON。
因此非流式错误不会留下半截 body。

### 5.5 answered 与 stage

只有下游完整写完（流式的 Stop/[DONE]，非流式的完整 JSON）才调用
`session_tasks::mark_answered(request_set_id)`。同一轮的下一次请求才会看到
`business.stage = "processing"`；当前这次永远是 `start`。

## 6. 账户池

### 6.1 候选

- 只取所选站点的账户；`qoder` 与 `qoder-cn` 永不混池。
- 基础顺序按账户名忽略大小写排序。
- 新 session 轮转：进程级计数器把不同 session 的起点错开。
- 已绑定 session：绑定账户健康时排第一，其余健康账户按轮转顺序跟随。
- 绑定账户冷却/被删：视作无绑定，从健康账户里选。
- `--account` pinned：只产出一个账户；账户不存在在启动时报配置错误。
- `--accounts a,b`：只在指定子集内轮转，必须同站点。

`/v1/models` 使用 `rotate=false`，不读也不写 session 绑定，不推进轮转计数器。

### 6.2 健康冷却

只有 auth 与 quota 失败影响健康：

| 失败 | 基础 | 连续次数 |
| --- | --- | --- |
| quota | 5 分钟 | 5、10、20、40、60、60… |
| auth | 15 分钟 | 15、30、60、60… |

- 成功清除条目与 streak；
- busy、请求错误、上游错误、传输失败都不冷却，也不触发 failover；
- 冷却结束后条目仍保留 2 小时再清理，避免频繁抖动；
- 记录类型（auth/quota），用于“全部冷却”时决定 401 还是 429；
- pinned 账户默认也尊重冷却；真的要在冷却期硬打，用显式 `--ignore-cooldown`（默认关闭）。

### 6.3 会话粘滞

- 键是派生出的 session id；容量 512，空闲 2 小时淘汰（LRU）。
- 只有上游成功打开后才绑定；失败不绑。
- 绑定账户冷却时，请求迁移到其它账户；成功后重新绑定到新账户。
- 绑定表进程内，重启即丢；`/v1/models` 不参与。
- 多账户下 session id 本身不含账户信息，隔离完全靠绑定。

### 6.4 全冷却与空池

- 没有账户：401 `not signed in; run: qoderz login`。
- 全部冷却：全部 auth → 401 `authentication_error`；否则 429 `insufficient_quota`；
  消息固定 `all Qoder accounts are temporarily unavailable; retry later`。
- 部分账户 auth/quota 失败、至少一个成功：正常 200，失败账户已被冷却。

## 7. 并发与生命周期

- `asio::io_context` + N 线程，默认 `min(hardware_concurrency, 4)`。
- 每连接一个协程；连接内请求顺序处理，SSE 帧顺序写，不需要连接级锁。
- 账户池、健康表、绑定表、任务表、模型缓存各自用互斥量；协程不跨 `co_await` 持锁。
- 上游 HTTP 客户端按 origin 复用连接；流式响应持有连接到结束。
- 启动时预检：站点没有账户直接退出 1；pinned 账户不存在退出 1；非 loopback 无 key 退出 1。
- 启动摘要（stderr）：版本、listen、站点、账户数、模式（pool/pinned/subset）、线程数、
  body 上限、上游是否为 loopback stub。
- SIGINT/SIGTERM：停止 accept，等待在途请求至多一个固定宽限（`server::shutdown_grace`，5 秒），关闭上游连接池，
  退出 0；第二次信号立即退出。

## 8. 安全边界

- 默认 loopback；非 loopback 必须显式 `--allow-public`，且强烈建议配置 key。
- 凭据文件 0600；日志与错误体不含 token、PAT、`Authorization`、`Cosy-*`、prompt、图片。
- `QODERZ_UPSTREAM` 只接受 loopback，`login`、`models`、`usage`、`serve` 都会读取；应用顺序必须在 VPC 解析之后，
  不能被 `--site` 悄悄丢掉（NoQoder 的已知坑）。
- 错误体可以带 Qoder 的安全消息，但必须截断（≤1000 字符）并检查不包含秘密。
- 不提供关闭 TLS 验证的开关；上游始终验证系统 CA 与主机名。

## 9. 测试与测量

- 单元测试：路由、鉴权、body 限制、错误映射、账户池、健康、绑定、模型并集。
- 集成测试：独立 `stub_upstream` 进程 + `qoderz serve` 子进程；临时凭据、loopback、
  随机端口；覆盖流式/非流式、auth/quota failover、首事件后错误、全冷却、pinned、空池、
  未知模型、VPC 路由、404/405/413。
- 所有网络测试只连 127.0.0.1；不允许真实 Qoder。
- 性能测量从外部发压；报告 RSS、PSS、anonymous/private dirty、线程数、fd 数；不承诺数字。
  对比 NoQoder 时用同一台机器、同一 stub、同一 body 大小与并发。

## 10. 模块

| 模块 | 职责 |
| --- | --- |
| `server/server` | 监听、连接、协程、优雅关闭 |
| `server/router` | 路由、鉴权、body 上限、404/405 |
| `server/endpoints/health` | `/healthz` |
| `server/endpoints/models` | `/v1/models`、模型并集 |
| `server/endpoints/chat` | chat 编排、failover、流式/非流式 |
| `server/pool/account_pool` | 候选、pinned、子集、轮转 |
| `server/pool/health` | 冷却与 streak |
| `server/pool/affinity` | session → 账户绑定 |
| `server/error_writer` | 唯一 HTTP 错误出口 |
| `server/api_key` | Bearer 校验、常量时间比较 |
