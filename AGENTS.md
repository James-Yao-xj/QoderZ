# AGENTS.md — QoderZ 项目规则

本文件面向所有参与 QoderZ 的代理与开发者。规则优先级：用户当前指令 > 本文件 > 其他文档。项目按批次推进：每批先读批次计划，完成实现、检查、文档对齐与门禁后再提交。

## 1. 构建

- 统一在 Nix devShell 中工作：`nix develop`；不要依赖系统全局安装的编译器或库。
- 构建目录为 `build/`，使用 CMake + Ninja；不要把构建产物、`compile_commands.json` 或 `result` 提交进仓库。
- C++ 版本号以根 `CMakeLists.txt` 的 `project(qoderz VERSION ...)` 为准；`src/common/version.hpp` 由 CMake 生成，禁止手改。`flake.nix` 的 `version` 仅用于包元数据展示，升级时与 CMake 同步。
- 新增第三方依赖时，优先使用 target-based CMake 包（`Boost::headers`、`OpenSSL::SSL`、`nlohmann_json::nlohmann_json`、`CLI11::CLI11`），不要写全局 `include_directories` / `link_libraries`。
- 修改 `flake.nix` 后至少验证 `nix develop --command cmake --version` 与 `nix build .#`。

## 2. 质量门禁

提交或交付前必须通过：

```sh
nix develop --command cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure
scripts/check-budget.sh
scripts/format.sh --check
```

- 任何 `.cpp` / `.hpp` / `.h` 的有效行（去空行、去纯注释行）目标 ≤ 600。
- 600–800 行：允许，但必须在评审或 PR 说明中给出保留理由。
- > 800 行：默认视为必须拆分；确有找不到自然拆分点的复杂模板，需要评审明确同意后保留，并同步处理检查脚本的例外，否则脚本会失败。
- 一个目录下的代码文件不超过 8 个；超过时用子目录按职责分组，而不是继续堆文件。
- 格式以仓库根 `.clang-format` 为准；`clang-format` 版本由 nixpkgs 固定，不要用系统里的其他版本“顺手格式化”。

## 3. 命名与语言风格

- 除宏以外，项目自定义标识符一律 `snake_case`：命名空间、类型、枚举、枚举值、函数、变量、常量、模板参数、文件名、目录名。
- 非静态数据成员一律 `m_` 前缀，包括公开聚合结构体的字段：
  ```cpp
  struct error {
    error_kind m_kind;
    std::string m_message;
    int m_http_status = 0;
  };
  ```
- 静态 `constexpr` 常量不加 `m_`，直接用 `snake_case`：`inline constexpr std::size_t max_request_bytes = ...;`。
- 宏使用 `UPPER_SNAKE_CASE`，并且只在确实没有替代方案时使用。
- 第三方与标准库 API、JSON 字段、HTTP 头、环境变量、CLI flag 保持原样：`nlohmann::json`、`boost::asio`、`CLI::App`、`Authorization`、`--version`。
- 使用 East const：`std::string const&`、`std::span<std::byte const>`、`auto const&`、`T const*`；不要写 `const std::string&`。
- 类型显而易见时使用 `auto`；不要用 `auto` 隐藏关键返回类型或所有权。
- 鼓励用 `constexpr` / `consteval` / 模板做编译期计算：固定字母表、长度公式、枚举映射、协议常量表。不要为了“炫技”牺牲可读性。
- 源文件编码 UTF-8，行尾 LF；C++ 缩进 2 空格，列宽 100。

## 4. 设计与错误处理

- 分层依赖指向内侧：`common` 不依赖任何业务层；协议、上游、服务层可以依赖 `common`，反向依赖禁止。
- 资源管理遵循 RAII；禁止裸 `new` / `delete` 和拥有型裸指针。跨线程使用标准同步设施或 `std::jthread`。
- 可恢复错误用 `qoderz::result<T>`（`std::expected<T, error>`）返回；异常只允许出现在不可恢复的边界，并在边界转换为 `error`。
- `error_kind` 与 HTTP 状态的默认映射以 `src/common/error.hpp` 为准；新增枚举值必须同时补 `to_string`、默认状态和测试。
- 公共头文件保持精简，实现细节放 `.cpp` 或 `namespace detail`。
- 日志只写 stderr；级别为 trace/debug/info/warn/error/critical；字段使用 `key=value`。日志库保证线程安全，接口保持小。

## 5. 测试

- 测试框架固定 Catch2 v3，测试文件放在 `tests/unit/<层>/`，标签使用 `[common][error]` 这类形式。
- 新增公共行为必须有单元测试；错误分类、日志级别解析、协议映射优先覆盖边界与非法输入。
- 测试必须确定、可重复；不要依赖真实时间、外部网络、真实账号或固定端口。
- 禁止访问真实 Qoder 服务、使用真实凭据或录制真实会话内容。未来集成测试使用 `127.0.0.1` loopback stub，并使用明显虚假的 token。
- 测试日志里也不得出现真实 token、prompt、`Authorization`、`Cosy-*` 或图片数据。

## 6. 安全

- 不记录、不回显、不提交任何凭据：token、cookie、`Authorization`、`Cosy-*`、prompt、图片字节。
- 不访问真实 Qoder 网络，不使用真实账号；本批次没有例外。
- 不修改 `../NoQoder`；它只是行为参考。
- 不把 `.env`、私钥、抓包记录、用户数据加入仓库。
- 出错时输出的诊断信息必须先脱敏，再写日志或异常消息。

## 7. 提交与协作

- 提交信息采用 Conventional Commits：`type(scope): summary`，例如 `feat(common): add error mapping`、`docs(testing): describe loopback stub`。
- `type` 使用 `feat` / `fix` / `docs` / `test` / `build` / `refactor` / `chore`；`scope` 用短小的模块名；summary 用英文祈使句，≤ 72 字符。
- 一次提交只做一件事；格式化、重构、功能不要混在一起。
- 没有明确要求时不要创建提交，也不要改写已有历史；任何情况下都不要提交构建产物或密钥。
- 代理不要自行创建 git 提交；提交由主会话在批次验收后执行。

## 8. 文档

- 文档正文用中文，标识符、命令、JSON 字段保持英文；语气正式但轻快，不堆术语。
- 公共接口、目录结构、构建命令变化时，同步更新 `README.md`、`docs/index.md` 与对应开发文档。
- 新增批次前先读批次计划；计划与代码冲突时，先记录偏差再实现，不要静默改变约定。
