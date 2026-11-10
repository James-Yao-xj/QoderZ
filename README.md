# QoderZ

QoderZ 是一个用 C++23 重写的 Qoder → OpenAI 兼容网关。它把 Qoder 上游能力包装成标准 OpenAI API，让现有 SDK、编辑器和脚本无需感知 Qoder 的私有协议。

可执行文件名为 `qoderz`。项目参考 `../NoQoder`（.NET 实现）的行为，但代码与依赖完全独立。

## 当前状态

批次 0：工程骨架。

- 可以配置、编译、测试、安装，也能通过 Nix 构建出 `result/bin/qoderz`。
- 只有占位 CLI：`--version`、`--help`。
- 尚未实现 Qoder 登录、HTTP 服务、协议编解码和上游调用。
- `src/common/` 提供错误类型、日志和 JSON 别名，后续批次在此基础上扩展。

## 构建与测试

环境：Nix（flakes）、GCC 14、CMake + Ninja。

```sh
nix develop --command cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure
```

直接运行：

```sh
./build/qoderz --version
./build/qoderz --help
```

Nix 构建与安装：

```sh
nix build .#
./result/bin/qoderz --version
cmake --install build --prefix "$PWD/out"
```

## 质量门禁

```sh
scripts/check-budget.sh
scripts/format.sh --check
```

- 单文件有效行目标 ≤ 600；超过 600 需要说明理由；超过 800 必须拆分。
- 一个目录下的代码文件不超过 8 个。
- 格式由 devShell 中的 `clang-format` 固定版本负责。

## 文档入口

- [文档导航](docs/index.md)
- [架构](docs/design/architecture.md) / [账户](docs/design/accounts.md) / [CLI](docs/design/cli.md) / [服务](docs/design/server.md)
- [构建与工具链](docs/development/build.md)
- [C++ 风格](docs/development/style.md)
- [测试约定](docs/development/testing.md)
- [常用速查](docs/development/cheatsheet.md)
- [协议：认证](docs/protocol/qoder-auth.md) / [聊天](docs/protocol/qoder-chat.md) / [OpenAI 兼容](docs/protocol/openai-compat.md)
- [项目规则](AGENTS.md)

## 安全边界

- 日志只写 stderr，绝不记录 token、prompt、`Authorization`、`Cosy-*` 或图片内容。
- 单元测试不访问真实 Qoder，不使用真实账号；后续集成测试使用本机 loopback stub。
- 任何凭据只从环境变量或本地未跟踪配置读取，不进入仓库。
