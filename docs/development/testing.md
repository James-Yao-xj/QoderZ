# 测试约定

测试是接口的一部分。QoderZ 的测试优先保证三件事：边界清楚、结果确定、绝不碰真实账号和真实网络。

## 框架与位置

- 框架固定 Catch2 v3，链接 `Catch2::Catch2WithMain`，测试文件不需要自己写 `main`。
- 测试入口在 `tests/CMakeLists.txt`；它用 `GLOB_RECURSE CONFIGURE_DEPENDS` 自动收集 `tests/unit/**/*.cpp` 与 `tests/support/*.cpp`，新增测试文件不需要改列表。
- 单元测试放在 `tests/unit/<层>/`，按模块分子目录（例如 `common/`、`protocol/`、`config/`）；共享夹具放在 `tests/support/`。
- 未来集成测试放在 `tests/integration/`，每个测试自己启动 loopback stub。
- 一个目录下的代码文件不超过 8 个；测试多了同样按职责拆子目录。

## 常用命令

```sh
nix develop --command cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure
```

只跑某一组：

```sh
./build/tests/qoderz_tests "[common][error]"
./build/tests/qoderz_tests "[common][log]"
```

列出测试：

```sh
./build/tests/qoderz_tests --list-tests
ctest --test-dir build -N
```

CMake 通过 `catch_discover_tests` 把每个 `TEST_CASE` 注册成单独的 CTest 项，失败时优先看 CTest 输出和对应测试名。

## 编写风格

```cpp
TEST_CASE("error kinds map to default HTTP statuses", "[common][error]") {
  CHECK(qoderz::default_http_status(error_kind::auth) == 401);
  CHECK(qoderz::default_http_status(error_kind::quota) == 429);
}
```

- 名称描述行为，不描述实现；标签用 `[层][主题]`。
- 能用 `CHECK` 的地方优先 `CHECK`，让一个用例报告多个失败点；只有后续步骤依赖前面结果时才用 `REQUIRE`。
- 一个测试只验证一个主题；复杂场景用 `SECTION` 拆分，但不要依赖执行顺序。
- 非法输入、边界值、错误分类要和正常路径一样认真覆盖。
- 纯编译期映射可以用 `static_assert` 锁住，例如：

```cpp
static_assert(qoderz::to_string(error_kind::quota) == "quota");
static_assert(qoderz::default_http_status(error_kind::quota) == 429);
```

当前骨架覆盖：错误名称、默认 HTTP 状态、具名构造 helper、`result<T>` 的成功/失败分支、日志级别往返解析、大小写与非法输入、日志阈值读写。

## loopback stub（后续批次）

涉及 HTTP 与上游协议时，测试必须在 `127.0.0.1` 上自建 stub，不允许连接真实 Qoder：

- stub 由测试进程持有，绑定 `127.0.0.1:0` 让操作系统分配端口，避免固定端口冲突。
- 把 stub 的 base URL 通过构造参数或依赖注入传给被测对象，不要在代码里硬编码线上域名。
- stub 校验关键请求头（例如 `Authorization`、`Content-Type`）和请求体结构；对被拒绝的请求返回明确状态码。
- 每个测试结束前关闭 listener、join 线程；超时时间要短且可控。
- 不访问外部 DNS、不走系统代理、不依赖公网证书。TLS 相关测试使用测试专用证书或本地终止，不导入真实证书。

## 禁止事项

- 禁止使用真实 Qoder 账号、真实 token、真实 cookie 或录制真实会话。
- 禁止在测试中调用真实 Qoder 服务；当前所有单元测试不联网，HTTP/协议测试只使用 `127.0.0.1` loopback stub。
- 禁止把抓包内容、用户 prompt、图片字节或 `Cosy-*` 头写进测试夹具、快照或日志。
- 禁止用 `sleep` 等固定时长假装同步；用条件变量、future 或可注入的时钟。
- 禁止依赖测试执行顺序或共享可变全局状态。日志级别这类全局状态在用例结束后要恢复原值。

## 测试里的秘密处理

- 需要假 token 时用一眼可辨的值，例如 `test-token-not-real`，不要复制任何真实字符串。
- 需要验证脱敏时，断言输出中不包含完整秘密，而不是把秘密打印出来人工检查。
- 失败诊断只输出结构、状态码、长度或哈希前缀；不要把完整请求体倾倒到 stderr。

## 集成测试的边界

后续批次实现 HTTP 服务后，集成测试至少覆盖：

1. `--version` / `--help` 的退出码与输出。
2. OpenAI 兼容路由的请求/响应形状。
3. 上游 401、429、5xx 到内部 `error_kind` 的映射。
4. 超时、取消与重试上限。
5. 日志脱敏（token、prompt、Authorization、Cosy-*）。

在对应实现落地前，不提前写空壳测试；先把单元测试和 loopback stub 的接口设计好。

## 失败排查顺序

1. 先确认是在 devShell 中配置和构建。
2. 确认 `BUILD_TESTING=ON`，且 `build/tests/qoderz_tests` 存在。
3. 单独运行失败用例，看 Catch2 的表达式分解。
4. 如果只有 CTest 失败、直接运行通过，检查测试注册和运行目录。
5. 格式化或行数门禁失败时，先修复再跑全量测试，不要用跳过检查的方式“通过”。
