# QoderZ 文档导航

这里记录 QoderZ 的工程约定、构建方式、测试策略与实测性能。文档与代码一起维护：接口变了，相关说明必须同一批次更新。

## 当前范围

| 能力 | 状态 |
| --- | --- |
| CMake / Ninja / Nix 构建 | 已完成 |
| `--version` / `--help` / 无命令概览 | 已完成 |
| 错误类型与日志 | 已完成 |
| 协议编解码（base64 / hash / BodyCodec / COSY / SSE / 工具调用拆分） | 已完成 |
| 站点、VPC、路径、凭据文件 v1、账户选择 | 已完成 |
| 安全随机、哈希与 HTTP 客户端 | 已完成 |
| 设备流与 PAT 登录、token 刷新、错误映射 | 已完成 |
| 模型目录、内置兜底列表、用量查询 | 已完成 |
| OpenAI 请求翻译、会话与选择器 | 已完成 |
| Qoder 聊天客户端与上游流 | 已完成 |
| OpenAI 响应翻译（流式与非流式） | 已完成 |
| 账户池、健康冷却、会话粘滞 | 已完成 |
| OpenAI 兼容 HTTP 服务 | 已完成 |
| CLI、进程级端到端测试、stub 上游、性能测量 | 已完成 |

## 阅读路线

第一次参与项目，建议按这个顺序：

1. [README](../README.md)：项目是什么，怎么跑起来。
2. [构建与工具链](development/build.md)：devShell、CMake、Nix 和常见故障。
3. [C++ 风格](development/style.md)：命名、East const、RAII、分层与行数预算。
4. [测试约定](development/testing.md)：Catch2、stub 上游、进程级端到端测试与性能脚本。
5. [常用速查](development/cheatsheet.md)：日常命令与关键不变量。
6. [项目规则](../AGENTS.md)：提交前必须通过的检查。

## 文档地图

| 文档 | 内容 |
| --- | --- |
| [development/build.md](development/build.md) | nix develop / CMake / Ninja / nix build / 安装 / 故障排查 |
| [development/style.md](development/style.md) | 命名、East const、auto、constexpr、RAII、分层、日志、预算 |
| [development/testing.md](development/testing.md) | Catch2 v3、测试组织、stub 上游、端到端测试、性能脚本、禁止真实账号 |
| [development/cheatsheet.md](development/cheatsheet.md) | 常用命令、关键不变量、版本升级步骤 |
| [performance.md](performance.md) | 空闲与负载实测值、PSS/anon 口径、复现命令、与 NoQoder 的可比性 |
| [AGENTS.md](../AGENTS.md) | 项目级硬规则与提交信息风格 |

## 设计与协议

这些文档是实现与行为规范的来源；代码与文档不一致时以文档为准，先修文档或代码中的一方，再继续扩展。

| 文档 | 内容 |
| --- | --- |
| [design/architecture.md](design/architecture.md) | 分层、账户池、错误与并发模型 |
| [design/accounts.md](design/accounts.md) | 账户模型、凭据文件与默认账户选择 |
| [design/cli.md](design/cli.md) | `qoderz` 命令总览与登录交互 |
| [design/server.md](design/server.md) | `qoderz serve` 的监听、请求编排与健康策略 |
| [protocol/openai-compat.md](protocol/openai-compat.md) | OpenAI Chat Completions 与 Qoder 的翻译规则 |
| [protocol/qoder-auth.md](protocol/qoder-auth.md) | Qoder 站点、登录、令牌刷新与 COSY 签名 |
| [protocol/qoder-chat.md](protocol/qoder-chat.md) | 上游聊天协议、SSE 事件与错误映射 |

## 写作约定

- 正文用中文；类型名、函数名、命令、JSON 字段、HTTP 头保持英文。
- 不写“显而易见”的空话，优先写边界、失败方式和验证方法。
- 新增目录或公共接口时，至少同步更新 `docs/index.md` 与对应开发文档。
- 文档中的命令必须可以直接复制执行；涉及 Nix 时默认在 `nix develop` 内运行。
- 性能数字必须来自脚本的实测输出，并写清机器、编译器和测量口径。
