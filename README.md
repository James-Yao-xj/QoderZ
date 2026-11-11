# QoderZ

QoderZ 是一个用 C++23 重写的 Qoder → OpenAI 兼容网关。它把 Qoder 上游能力包装成标准 OpenAI Chat Completions API，让现有 SDK、编辑器和脚本无需感知 Qoder 的私有协议。

可执行文件名为 `qoderz`。项目参考 `../NoQoder`（.NET 实现）的行为，但代码与依赖完全独立；协议来自逆向与 loopback 夹具，官方并未公开。

## 功能

全部能力已落地：

- 登录：设备流或 PAT（`--pat` / `--pat-stdin` / `QODERZ_PAT`），支持 `qoder` 与 `qoder-cn` 两个站点、CN 企业 VPC。
- 账户：多账户存储（0600 凭据文件）、`accounts` / `account use|show|remove` / `logout`，选择器支持 name、uid、email 与唯一前缀。
- 查询：`models` 打印健康账户的模型并集，`usage` 打印套餐与额度窗口；两者都有 `--json`。
- 服务：`serve` 提供 `/healthz`、`/v1/models`、`/v1/chat/completions`，覆盖流式与非流式、账户池轮转、健康冷却、会话粘滞、Bearer key、body 上限与 SIGINT/SIGTERM 优雅关闭。
- 协议：BodyCodec、COSY 签名、SSE 解析、文本工具调用拆分、错误分类与下游映射。

## 快速开始

环境：Nix（flakes）、GCC 14、CMake + Ninja。

```sh
nix develop --command cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
nix develop --command cmake --build build
```

登录、查看与查询：

```sh
./build/qoderz login --name work          # 设备流；也可以 --pat-stdin
./build/qoderz accounts
./build/qoderz models
./build/qoderz usage
```

启动网关：

```sh
./build/qoderz serve                       # http://127.0.0.1:8788
./build/qoderz serve --api-key-stdin       # 需要 Bearer key
./build/qoderz serve --accounts work,personal
```

`serve` 启动时把版本、监听地址、站点、账户数、模式、线程数、body 上限写到 stderr；`--json` 会同时把机器可读的启动摘要写到 stdout。

## 连接 OpenAI 客户端

base URL 指向 `/v1`，API key 用 `--api-key` 设置的值；没有配置 key 时任意 key 都接受（loopback 默认场景）。

```sh
export OPENAI_BASE_URL=http://127.0.0.1:8788/v1
export OPENAI_API_KEY=local-qoderz
```

```sh
curl -s http://127.0.0.1:8788/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"<来自 /v1/models>","messages":[{"role":"user","content":"hello"}]}'
```

`model` 取 `/v1/models` 返回的 `id`（即 Qoder 目录 key），可带 `"<site>/"` 前缀。流式请求加 `"stream": true`，响应为 `text/event-stream`，Stop 后写 `data: [DONE]`。

## 环境变量

| 变量 | 用途 | 默认 |
| --- | --- | --- |
| `QODERZ_SITE` | 默认站点 | `qoder-cn` |
| `QODERZ_CREDENTIALS` | 凭据文件路径 | XDG 路径 |
| `QODERZ_LISTEN` | `serve` 监听地址 | `http://127.0.0.1:8788` |
| `QODERZ_API_KEY` | `serve` 的 Bearer key | 无 |
| `QODERZ_ACCOUNT` | `models` / `usage` / `serve` 的默认选择器 | 无 |
| `QODERZ_LOG_LEVEL` | 日志级别 | `info` |
| `QODERZ_THREADS` | `serve` 线程数 | `min(hardware_concurrency, 4)` |
| `QODERZ_UPSTREAM` | 把上游指向 loopback stub（仅测试与测量） | 无 |
| `QODERZ_PAT` | PAT 登录令牌 | 无 |
| `NO_COLOR` | 禁用 ANSI 颜色 | 无 |

优先级：命令行 > 环境变量 > 内置默认。

## 安全边界

- 默认只监听 `127.0.0.1`；监听非 loopback 且没有 API key 时**拒绝启动**，确实要公开时必须显式 `--allow-public`，并强烈建议同时配置 key 与反向代理。
- 凭据文件权限 0600；日志、错误体、`--json` 输出都不含 token、PAT、`Authorization`、`Cosy-*`、prompt 或图片字节。
- 只做 HTTP/1.1，不终止 TLS、不提供关闭证书校验的开关；需要 TLS 时放在反向代理后面。
- `QODERZ_UPSTREAM` 只接受 loopback origin，且只由 `serve` 读取。
- 测试与测量全部使用 `127.0.0.1` 上的 stub 上游，不访问真实 Qoder、不使用真实账号。

## 性能测量

两个脚本从外部测量 `serve` 的常驻内存；它们构建 Release、生成临时假凭据、启动 stub 与 `serve`，结束时清理进程和临时文件：

```sh
./scripts/measure-idle.sh    # 空闲：VmRSS / Pss / Anonymous / 线程 / fd，取 5 次中位数
./scripts/measure-load.sh    # 负载：400 KB 文本 body、100 请求、并发 8，外部 curl 发压
```

方法与实测数值见 [docs/performance.md](docs/performance.md)。

## 测试

```sh
nix develop --command ctest --test-dir build --output-on-failure
```

- 单元测试与进程内集成测试：`tests/unit/`、`tests/integration/`。
- 进程级端到端：`tests/integration/e2e_server_test.cpp` 编成独立可执行文件 `qoderz_e2e`，启动真实的 `qoderz` 与 `qoderz_stub_upstream` 子进程。
- stub 上游：`tests/stub_upstream/`，构建 `qoderz_stub_upstream`（仅测试构建，不安装）。

## 质量门禁

```sh
scripts/check-budget.sh
scripts/format.sh --check
```

- 单文件有效行目标 ≤ 600；超过 600 需要说明理由；超过 800 必须拆分。
- 一个目录下的代码文件不超过 8 个。
- 格式由 devShell 中的 `clang-format` 固定版本负责。

## 限制

- 协议是非官方逆向结果，Qoder 客户端升级后可能需要同步调整；所有行为以固定夹具与 loopback 端到端测试锁定。
- 没有真实账号的冒烟测试：仓库内不保存、也不要求任何真实凭据；登录与上游行为只用假 token + loopback stub 验证。
- 只实现 OpenAI Chat Completions；没有 `/v1/completions`、embeddings、images、audio。
- 无 HTTP/2、无 TLS 终止、无内置多租户；一个进程对应一个站点的一组账户。
- `usage` 只读展示，不参与配额控制。

## 文档入口

- [文档导航](docs/index.md)
- [架构](docs/design/architecture.md) / [账户](docs/design/accounts.md) / [CLI](docs/design/cli.md) / [服务](docs/design/server.md)
- [构建与工具链](docs/development/build.md)
- [C++ 风格](docs/development/style.md)
- [测试约定](docs/development/testing.md)
- [性能测量](docs/performance.md)
- [常用速查](docs/development/cheatsheet.md)
- [协议：认证](docs/protocol/qoder-auth.md) / [聊天](docs/protocol/qoder-chat.md) / [OpenAI 兼容](docs/protocol/openai-compat.md)
- [项目规则](AGENTS.md)
