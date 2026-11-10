# 常用速查

## 日常命令

| 目的 | 命令 |
| --- | --- |
| 进入工具链 | `nix develop` |
| 配置（含测试） | `nix develop --command cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON` |
| 增量构建 | `nix develop --command cmake --build build` |
| 跑全部测试 | `nix develop --command ctest --test-dir build --output-on-failure` |
| 跑单组测试 | `./build/tests/qoderz_tests "[common][log]"` |
| 查看版本 | `./build/qoderz --version` |
| 查看帮助 | `./build/qoderz --help` |
| 格式检查 | `nix develop --command scripts/format.sh --check` |
| 自动格式化 | `nix develop --command scripts/format.sh --fix` |
| 行数预算 | `scripts/check-budget.sh` |
| Nix 构建 | `nix build .#` |
| 运行 Nix 产物 | `./result/bin/qoderz --version` 或 `nix run .# -- --version` |
| 安装到本地前缀 | `nix develop --command cmake --install build --prefix "$PWD/out"` |

`scripts/check-budget.sh` 和 `scripts/format.sh` 不依赖 devShell 以外的工具；格式脚本必须在 devShell 中运行，以保证 clang-format 版本一致。

## 关键不变量

- 可执行文件名：`qoderz`。
- C++ 内版本号来源：`CMakeLists.txt` 的 `project(qoderz VERSION ...)`；`version.hpp` 由 CMake 生成，不手改。`flake.nix` 的 `version` 是包元数据，升级时同步。
- 日志只写 stderr；禁止记录 token、prompt、`Authorization`、`Cosy-*`、图片数据。
- 可恢复错误用 `result<T>`；错误分类以 `src/common/error.hpp` 为准。
- 命名：项目自定义标识符全部 `snake_case`；非静态数据成员（含公开 struct 字段）一律 `m_`；宏 `UPPER_SNAKE_CASE`；第三方名字保持原样。
- East const：`T const&`、`T const*`、`auto const&`。
- 行数：目标 ≤ 600；600–800 需说明理由；> 800 默认拆分；复杂模板例外需评审同意并处理脚本。
- 一个目录最多 8 个代码文件。
- 测试不访问真实 Qoder、不使用真实账号、不依赖外部网络。
- 不修改 `../NoQoder`。

## 目录速览

```text
.
├── CMakeLists.txt
├── flake.nix / flake.lock
├── AGENTS.md
├── README.md
├── docs/
│   ├── index.md
│   └── development/{build,style,testing,cheatsheet}.md
├── scripts/{check-budget.sh,format.sh}
├── src/
│   ├── main.cpp
│   └── common/{error.hpp,log.hpp,log.cpp,json.hpp,version.hpp.in}
└── tests/
    ├── CMakeLists.txt
    └── unit/common/{error_test.cpp,log_test.cpp}
```

## 调试

```sh
gdb --args ./build/qoderz --help
ninja -C build -t targets          # 查看目标
ctest --test-dir build -R error -V # 只跑匹配的测试，显示命令
clang-format --style=file:.clang-format src/main.cpp | diff -u src/main.cpp -  # 查看格式差异
```

日志级别目前在库内通过 `qoderz::set_log_level` 设置；CLI 环境变量接入会在后续批次完成。调试时不要把真实凭据放进命令行历史。

## 版本升级

1. 修改 `CMakeLists.txt` 中的 `project(qoderz VERSION x.y.z ...)`。
2. 同步 `flake.nix` 中 derivation 的 `version`。
3. 重新运行 CMake 配置，确认生成头文件更新。
4. 构建并检查 `--version` 输出。
5. 如版本约定变化，更新 README 与相关文档。

## 常见失败

| 现象 | 先检查 |
| --- | --- |
| `nix build .#` 报文件 not tracked | 新文件是否已 `git add`（无需 commit） |
| CMake 找不到 Boost/OpenSSL | 是否在 `nix develop` 中执行 |
| 生成头文件不存在 | 是否重新运行过 `cmake -S . -B build ...` |
| 测试数不对 | `BUILD_TESTING` 是否为 ON，是否重新配置 |
| 格式检查失败 | 是否使用 devShell 的 clang-format，是否跑了 `--fix` |
| `result` 是旧版本 | 重新 `nix build .#`，检查 `readlink -f result` |

## 提交前检查清单

```sh
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure
nix develop --command scripts/format.sh --check
scripts/check-budget.sh
nix build .#
./result/bin/qoderz --version
```

全部通过，再整理提交信息：`type(scope): summary`。
