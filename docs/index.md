# QoderZ 文档导航

这里记录 QoderZ 的工程约定、构建方式和测试策略。文档与代码一起维护：接口变了，相关说明必须同一批次更新。

## 当前范围

| 能力 | 状态 |
| --- | --- |
| CMake / Ninja / Nix 构建 | 已完成（批次 0） |
| `--version` / `--help` | 已完成（占位） |
| 错误类型与日志 | 已完成（骨架） |
| Qoder 登录与凭据管理 | 未开始 |
| OpenAI 兼容 HTTP 服务 | 未开始 |
| Qoder 协议编解码 | 未开始 |
| 上游连接池与重试 | 未开始 |

## 阅读路线

第一次参与项目，建议按这个顺序：

1. [README](../README.md)：项目是什么，怎么跑起来。
2. [构建与工具链](development/build.md)：devShell、CMake、Nix 和常见故障。
3. [C++ 风格](development/style.md)：命名、East const、RAII、分层与行数预算。
4. [测试约定](development/testing.md)：Catch2、loopback stub 和安全边界。
5. [常用速查](development/cheatsheet.md)：日常命令与关键不变量。
6. [项目规则](../AGENTS.md)：提交前必须通过的检查。

## 文档地图

| 文档 | 内容 |
| --- | --- |
| [development/build.md](development/build.md) | nix develop / CMake / Ninja / nix build / 安装 / 故障排查 |
| [development/style.md](development/style.md) | 命名、East const、auto、constexpr、RAII、分层、日志、预算 |
| [development/testing.md](development/testing.md) | Catch2 v3、测试组织、loopback stub、禁止真实账号 |
| [development/cheatsheet.md](development/cheatsheet.md) | 常用命令、关键不变量、版本升级步骤 |
| [AGENTS.md](../AGENTS.md) | 项目级硬规则与提交信息风格 |

## 设计与协议（后续批次规范）

这些文档描述目标设计，其中多数能力尚未实现；实现时以对应文档为准，并在落地后同步状态。

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

## 后续文档

运维与观测、部署与容器化、性能基准等主题会随对应批次补充。新增文档时先放进合适的目录，再更新本页导航。
