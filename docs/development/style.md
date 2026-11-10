# C++ 风格与分层

QoderZ 的目标不是把 .NET 代码逐行翻译成 C++，而是用 C++23 的方式重新表达同一套边界：清晰的错误分类、可测试的协议层、可控的并发，以及不会泄露秘密的日志。

## 语言基线

- C++23，GCC 14；使用标准库设施，不引入私有方言。
- 缩进 2 空格，列宽 100，UTF-8 + LF；格式由仓库根 `.clang-format` 决定。
- 源文件名、目录名与类型名保持一致，全部 `snake_case`。

## 命名

除宏以外，项目自定义标识符一律 `snake_case`：

| 类别 | 规则 | 示例 |
| --- | --- | --- |
| 命名空间 | `snake_case` | `qoderz::common` |
| 类型 / struct / class | `snake_case` | `chat_request`、`error` |
| 枚举与枚举值 | 均 `snake_case` | `error_kind::bad_request` |
| 函数 | `snake_case` | `parse_log_level` |
| 局部变量 / 参数 | `snake_case` | `request_id` |
| 非静态数据成员 | `m_` + `snake_case`，包括公开 struct 字段 | `m_body`、`m_kind` |
| 静态 `constexpr` 常量 | 不加 `m_`，`snake_case` | `max_request_bytes` |
| 模板参数 | `snake_case` | `value_type`、`clock_type` |
| 宏 | `UPPER_SNAKE_CASE`，尽量不用 | `QODERZ_MAX_REQUEST_BYTES` |
| 文件 / 目录 | `snake_case` | `log.hpp`、`tests/unit/common/` |

第三方或标准库已有的名字保持原样：`nlohmann::json`、`boost::asio`、`CLI::App`、`std::expected`。JSON 字段、HTTP 头、环境变量与 CLI flag 也保持原样：`choices`、`Authorization`、`QODERZ_LOG_LEVEL`、`--version`。

公开聚合结构体的字段同样使用 `m_`，不要因为它是 POD 就例外：

```cpp
struct error {
  error_kind m_kind;
  std::string m_message;
  int m_http_status = 0;
  std::string m_code;
  std::optional<int> m_retry_after;
};
```

## East const

`const` 一律放在类型右边：

```cpp
void send(std::string const& body);
std::span<std::byte const> payload;
auto const& response = cache.lookup(key);
error const* last_error = nullptr;
```

不要写 `const std::string&`、`const auto&`、`const T*`。这不会改变语义，但能让类型声明保持从左到右的阅读顺序。

## auto 与编译期计算

- 类型在上下文里显而易见时用 `auto`：迭代器、`std::lock_guard`、`std::chrono` 时间点。
- 关键返回类型、所有权类型、错误类型不要被 `auto` 藏起来；需要让调用方一眼看出 `result<T>` 或 `std::unique_ptr<T>`。
- 优先把固定表、长度公式、枚举映射写成 `constexpr` / `consteval`，并补 `static_assert`。例如日志级别名称和错误状态映射目前就是 `constexpr`，可以在编译期验证。
- 不要为了“模板而模板”。编译期计算应当减少重复和运行期分支，而不是把简单逻辑包装成难读的元编程。

## RAII 与所有权

- 资源随对象生命周期释放：文件、socket、锁、线程、临时目录都用 RAII 包装。
- 禁止裸 `new` / `delete`；禁止拥有型裸指针。独占所有权用 `std::unique_ptr`，共享所有权只有在确实共享生命周期时才用 `std::shared_ptr`。
- 非拥有视图用 `std::string_view`、`std::span`、`T const*`，并在接口文档里说明生命周期约束。
- 锁用 `std::lock_guard` / `std::scoped_lock`；线程用 `std::jthread`，把停止请求和 join 交给析构。
- 析构函数不抛异常；清理失败最多记录日志，不能把异常带出析构。

## 分层

依赖方向从外向内：

| 层 | 可以依赖 | 职责 |
| --- | --- | --- |
| `cli` | server、openai、qoder、config、common | 参数解析、命令编排、输出与退出码 |
| `server` | openai、qoder、config、protocol、common | HTTP 路由、账户池、鉴权、错误响应 |
| `openai` | qoder、protocol、common | OpenAI 请求/响应翻译、窗口与 effort |
| `qoder` | config、protocol、common | 站点、认证、目录、用量、聊天客户端 |
| `config` | common | 站点、凭据、账户模型与文件 I/O |
| `protocol` | common | 纯编解码：BodyCodec、COSY、SSE、工具调用 |
| `common` | 标准库与第三方基础库 | 错误、日志、JSON、网络、hash/base64 |

依赖只能指向表中列出的层；`qoder/chat` 只认识自己的请求类型，不包含任何 OpenAI 概念。
`openai` 可以构造 `qoder` 的请求类型，但不能反向被 `qoder` 依赖。

- `common` 不包含任何 Qoder 业务概念；它只放跨模块的基础设施。
- 下层不能反向包含上层的头文件；需要回调或接口时，把抽象放在下层并由上层实现。
- 公共头文件只暴露必要类型，实现细节留在 `.cpp` 或 `namespace detail`。
- 一个目录下的代码文件不超过 8 个；超过时按职责拆子目录，而不是继续加文件。

## 错误处理

- 可恢复错误用 `qoderz::result<T>` 返回，调用方必须显式处理。
- 异常只用于不可恢复的边界（例如初始化失败），并在边界转换成 `error`；不要用异常做普通控制流。
- `error` 的字段全部 `m_` 前缀；构造使用 `make_error` 和具名 helper。
- `error_kind` 与默认 HTTP 状态保持稳定映射：

| kind | 默认状态 |
| --- | --- |
| `configuration` | 500 |
| `auth` | 401 |
| `quota` | 429 |
| `busy` | 503 |
| `bad_request` | 400 |
| `upstream` | 502 |
| `io` | 500 |
| `protocol` | 502 |
| `canceled` | 499 |
| `internal` | 500 |

新增枚举值必须同时更新 `to_string`、`default_http_status` 和测试。`retry_after` 使用秒，保持 `std::optional<int>`，不要用魔法字符串表达。

## 日志

- 只写 stderr；stdout 留给 CLI 输出和未来可能的结构化响应。
- 级别：`trace`、`debug`、`info`、`warn`、`error`、`critical`，阈值全局可调。
- 字段统一 `key=value`；值包含空格、引号或 `=` 时由日志库加引号并转义。
- 日志库内部用互斥锁保证一条日志原子写入，级别阈值用原子变量读取。
- 绝不记录 token、cookie、`Authorization`、`Cosy-*`、prompt、图片字节或完整用户请求体。需要定位问题时记录长度、哈希前缀或脱敏后的标识。

## 行数预算

- 目标：任何代码文件有效行 ≤ 600。有效行由 `scripts/check-budget.sh` 统计：去掉空行与纯注释行。
- 600–800：黄区，可以保留，但评审或 PR 必须说明为什么没有自然拆分点。
- 800 以上：红区，默认必须拆分。
- 确有找不到自然拆分点的复杂模板，可以在评审明确同意后保留；这种例外要有记录，并同步处理检查脚本，否则 CI 仍会失败。
- 一个目录最多 8 个代码文件；文件多时先按职责分目录。

## 注释与文档

- 代码优先自解释；注释解释“为什么”、约束、失败模式，不写“这里赋值”“这里返回”。
- 不保留注释掉的死代码；需要保留的探索放到 issue 或文档。
- 公共接口在头文件里写清前置条件、线程安全性和错误语义；实现细节不要写进文档冒充契约。
- 正文文档用中文，标识符、命令和协议字段保持英文。

## 并发

- 共享状态优先不可变；必须可变时用 `std::mutex` 或 `std::atomic`，并说明锁保护的范围。
- 不要在持锁时做 I/O 或调用用户回调；把数据拷贝出来再释放锁。
- 线程退出必须可预期：使用 `std::jthread` 的停止令牌，避免 detach 后访问悬空对象。
