# 性能测量

本文记录 `qoderz serve` 的资源占用测量方法与实测数值。所有数字都来自仓库里的脚本，测量过程中只连
`127.0.0.1`，没有真实 Qoder 流量。数字是这一台机器、这一个编译器的结果，不是承诺。

## 1. 测量环境

| 项 | 值 |
| --- | --- |
| 机器 | AMD Ryzen 9 9955HX（16 核 / 28 线程可见），Microsoft Hyper-V 虚拟机 |
| 内存 | 12 GiB（`MemTotal` 约 12 GiB，测量时另有其它构建任务并发） |
| 系统 | Linux 6.18.53 x86_64 |
| 编译器 | GCC 14.4.0（Nix devShell） |
| 构建 | CMake 4.4.4 + Ninja，`-DCMAKE_BUILD_TYPE=Release`，`-DBUILD_TESTING=ON` |
| 上游 | `qoderz_stub_upstream`，同机 loopback，chat 每帧 256 字节 × 8 帧 |
| 服务 | `qoderz serve`，单账户池，`--threads` 默认 4，body 上限 32 MiB |

stub 上游与网关在同一个进程树里、同一台机器上，网关到上游是一次 loopback HTTP 往返，比真实上游快得多。
因此本文的数字反映的是**网关自身的开销**，不是端到端吞吐；没有真实网络延迟、TLS、带宽与上游队列。

## 2. 测量口径

三个内存数字都直接读内核，不看 `ps` 的近似值：

| 指标 | 来源 | 含义 |
| --- | --- | --- |
| `VmRSS` | `/proc/<pid>/status` | 进程页表里当前驻留的物理页总量，包含共享的只读页（代码、映射的库） |
| `Pss` | `/proc/<pid>/smaps_rollup` 的 `Pss:` | 共享页按共享者数量折算后的份额，是“这个进程实际占用多少物理内存”最贴近的口径 |
| `Anonymous` | `/proc/<pid>/smaps_rollup` 的 `Anonymous:` | 匿名页（堆、栈、Asio 缓冲），不含文件映射的代码与只读段 |
| 线程 | `/proc/<pid>/status` 的 `Threads:` | io_context 工作线程 + 主线程 + 运行时辅助线程 |
| fd | `/proc/<pid>/fd` 的条目数 | 监听 socket、上游连接、`/dev/null`、日志等 |

为什么三个都要看：

- `VmRSS` 会把共享的 `.so` 与可执行文件页算进来，多个进程同时跑时重复计数，单独看会偏高。
- `Pss` 已经摊薄共享页，是跨进程比较时的首选。
- `Anonymous` 排除代码与只读映射，最能反映“这个服务自己申请了多少内存”，也是排查泄漏的指标。

采样取**中位数**而不是平均：首次采样一定在预热之后，但分配器与内核回收仍会有抖动，中位数比均值更抗单点毛刺。

## 3. 复现命令

```sh
./scripts/measure-idle.sh
./scripts/measure-load.sh
```

两个脚本都会：

1. 配置并构建 Release 到 `build-perf/`（`QODERZ_PERF_BUILD_DIR` 可改；`--no-build` 跳过）；
2. 在 `mktemp -d` 下生成临时凭据（假 token、`vpc=""`、0600 权限）；
3. 启动 `qoderz_stub_upstream --listen http://127.0.0.1:0`，从 stdout 读实际 origin；
4. 以 `QODERZ_UPSTREAM=<stub origin>`、`HOME=<临时目录>` 启动 `qoderz serve --listen http://127.0.0.1:0`，从 stderr 读实际端口；
5. 轮询 `/healthz` 直到 200，然后开始采样；
6. 退出时 `SIGTERM` 两个子进程、必要时 `SIGKILL`，并删除临时目录。

可调环境变量：

| 变量 | 默认 | 用途 |
| --- | --- | --- |
| `QODERZ_PERF_BUILD_DIR` | `build-perf` | Release 构建目录 |
| `QODERZ_PERF_JOBS` | `6` | 构建并行度 |
| `QODERZ_PERF_SITE` | `qoder-cn` | 账户站点 |
| `QODERZ_PERF_REQUESTS` | `100` | 负载脚本请求数 |
| `QODERZ_PERF_CONCURRENCY` | `8` | 负载脚本并发 |
| `QODERZ_PERF_BODY_BYTES` | `409600` | 负载脚本文本 body 的字节数 |
| `QODERZ_PERF_BINARY` | `<build>/qoderz` | 负载脚本的网关二进制，可指向修改前的副本；stub 仍来自构建目录 |
| `QODERZ_PERF_SAMPLE_SECONDS` | `0.05` | 负载脚本的采样间隔，秒 |
| `QODERZ_PERF_COOLDOWN_SECONDS` | `6` | 发压结束后的冷却时长，秒 |

负载脚本额外报告 `VmHWM_kB`、`cpu_ms` 与 `wall_ms`。`VmHWM` 来自内核 RSS 高水位，短于采样
间隔的峰值也会计入；不能用发压中位数代替峰值。CPU 是网关进程的 user + system 时间，读取
`/proc/<pid>/stat`，按 `CLK_TCK` 换算；wall 包含 curl 与采样调度开销，精度受采样间隔影响。
脚本校验请求结果，有非 200、curl 失败或响应数不足时返回非零状态。

## 4. 空闲实测

`./scripts/measure-idle.sh`：预热 5 秒，之后 5 次采样、间隔 2 秒。

| 采样 | VmRSS (KiB) | Pss (KiB) | Anonymous (KiB) | 线程 | fd |
| --- | --- | --- | --- | --- | --- |
| 1 | 12856 | 8506 | 1444 | 4 | 9 |
| 2 | 12856 | 8924 | 1444 | 4 | 9 |
| 3 | 12856 | 8770 | 1444 | 4 | 9 |
| 4 | 12856 | 9245 | 1444 | 4 | 9 |
| 5 | 12856 | 9247 | 1444 | 4 | 9 |
| **中位数** | **12856** | **8924** | **1444** | **4** | **9** |

观察：

- 约 **12.6 MiB** 的 `VmRSS`，其中真正的匿名内存只有 **1.4 MiB**；其余几乎全是共享的只读页（可执行文件代码段、C++ 运行时、OpenSSL、`libstdc++`）。单进程运行时 `Pss` 约 8.7 MiB。
- `Pss` 有约 700 KiB 的抖动而 `VmRSS` 完全平稳，是内核在后台回收/重算共享页统计的噪声，不是进程行为变化。
- 线程数恒为 4，与 `--threads` 默认值一致（`min(hardware_concurrency, 4)`）。空闲时既没有 per-connection 线程，也没有后台轮询线程。
- fd 恒为 9：监听 socket、标准流、`/dev/null`、以及若干运行时打开的句柄；空闲时上游连接池是空的 —— 说明连接不会被提前建立，也不会被永久持有。
- `Anonymous` 1.4 MiB 基本就是 C++ 运行时 + 解析器 + 账户池 + 模型缓存表的常驻量，符合“没有大块预分配”的设计。

## 5. 负载实测

`./scripts/measure-load.sh`：每次请求 body 409,703 字节（400 KiB 文本 + JSON 外壳），共 100 个请求、并发 8，由外部 `curl` 通过 `xargs -P 8` 发压；发压进程的内存不计入服务端。发压期间每 0.2 秒采样一次，本轮发压约 0.7 秒完成，因此只有 3 个发压中的样本；发压前与结束后的基线各 1 个，用于对照。

| 采样 | VmRSS (KiB) | Pss (KiB) | Anonymous (KiB) | 线程 | fd |
| --- | --- | --- | --- | --- | --- |
| 发压前（基线） | 13108 | 8950 | 1444 | 4 | 9 |
| 1（发压中） | 41576 | 35994 | 30136 | 5 | 15 |
| 2（发压中） | 43244 | 36670 | 30804 | 5 | 14 |
| 3（发压中） | 41904 | 37683 | 29264 | 5 | 9 |
| 发压结束后 1 s | 41904 | 37692 | 29264 | 5 | 9 |
| **中位数（发压中各次）** | **41904** | **36670** | **30136** | **5** | **14** |

请求结果：

| 项 | 值 |
| --- | --- |
| body 大小 | 409,703 字节 |
| 并发 | 8 |
| 请求数 | 100 |
| 成功（HTTP 200） | 100 |
| 其它状态码 | 无 |

观察：

- 峰值 `Anonymous` 约 **30.1 MiB**，相对空闲的 1.4 MiB 增加约 28.7 MiB；这是并发 8 时 400 KiB body、解析后的 JSON 树、转译出的上游请求体与响应缓冲同时驻留的结果，量级约每个并发请求 3.5 MiB。
- 发压结束后 `Anonymous` 停在 29.3 MiB 而不是回到 1.4 MiB：glibc 的 arena 会保留已释放的空闲块以便复用，这是分配器的正常行为，不是泄漏。重复跑同一负载时该值稳定在同一区间，不随请求数单调增长。
- 线程数从 4 变 5：多出来的 1 条是 Boost.Asio 解析器服务的后台线程，在**第一次上游调用**时创建并复用（只访问 `/v1/models` 也会出现），不是每个请求一条。100 个请求、并发 8 下线程数稳定在 5。
- fd 在 9–15 之间：空闲基线 9，发压期间最多 15，多出来的是到 stub 的 keep-alive 连接（最多 8 条，与并发一致）。连接在响应读完后归还池中，不是每个请求新建一条；发压结束后 fd 回到 9，池中连接随后按 30 秒空闲 TTL 关闭。
- 全部 100 个请求都返回 200，没有 502/503/413，说明 400 KiB body 远低于 32 MiB 上限，也没有触碰到 body 上限错误路径。

## 6. 与 NoQoder 的可比性

`../NoQoder` 是 .NET 实现，运行时自带 GC 堆与 JIT，启动即常驻几十 MiB，容器镜像与运行时也完全不同。因此**不能**把两个数字直接相减得出“谁更省内存”：

- 起点不同：.NET 进程的基线包含 CoreCLR、GC 堆与 JIT 代码；QoderZ 的基线只有一个静态链接的 C++ 可执行文件加 glibc/OpenSSL。
- 口径不同：.NET 的 `WorkingSet64` 对应本仓库的 `VmRSS`，但它还包含 GC 尚未归还的内存；本仓库同时报 `Pss` 与 `Anonymous`，就是为了避免这种口径混淆。
- 上游不同：两边都用各自仓库内的 stub，但 stub 本身的实现、响应大小与协议细节不同。

要做有意义的对比，必须固定：同一台机器、同一个编译/发布配置、同一 stub 响应、同一 body 大小与并发，并且记录各自命令行与运行时版本。本项目没有在 NoQoder 侧跑对照测量（需要 dotnet 运行时与本仓库无关的构建），所以本文不给出任何跨实现的数字结论。

一条可以使用的方法：把上面的 `measure-load.sh` 参数（body 409,600 字节、100 请求、并发 8）原样套到 NoQoder 的 `measure-serve-rss.sh`，在同一台机器上各跑一次，记录环境、命令、以及各自选定的内存口径；两边的绝对值只作参考，趋势与增长曲线才是可比的。

## 7. 已知边界

- 两次测量都不是在完全空闲的机器上跑：本机同时有构建任务，`VmRSS` 里可能混入少量磁盘缓存带来的抖动，但三个指标的相对关系稳定。
- 只测了单账户池与单个 loopback 上游。多账户、真实上游延迟、TLS 终止、长时间稳定性（内存是否随天数增长）都没有覆盖，不在本次测量的范围内。
- 负载脚本用外部 `curl` 发压，测的是服务端驻留内存，不含客户端；客户端自身的内存与 fd 不计入。
- 数值与硬件强相关。换机器、换编译器、换 glibc 版本后应重跑脚本并更新本文，而不是沿用旧数字。

## 8. 大请求时的内存归还

本节保留优化前的排查记录；第 9 节记录当前实现、默认归还策略与同机前后对比。下文的副本清单和
手动环境变量建议对应旧实现。第 5 节只压了 400 KiB 的 body，那时分配器保留的空闲块只有 29 MiB，
看不出问题。真实网关会收到几十个 MiB 的 prompt（上限 32 MiB），这时 glibc 的默认行为值得单独说清楚。

### 8.1 现象

用 4 MiB body 打 36 个请求、并发 3，等进程完全静置后再采样：

| 配置 | 静置后 `RssAnon` |
| --- | --- |
| glibc 默认 | 99 MB |
| glibc 且固定 `MALLOC_TRIM_THRESHOLD_=131072` | 2.7 MB |

进程静置后仍握着 99 MB，其中没有任何一次请求在用——这是分配器的保留策略，不是泄漏。

### 8.2 机制

glibc 的 `mmap` 阈值是**动态**的：一次大的 `malloc` + `free` 会让它抬到这次请求的大小，之后同样大小的
分配就走主 arena（`brk` / 每线程 arena）而不是 `mmap`。arena 里的空闲块只有在请求到达
`M_TOP_PAD` / `trim_threshold` 边界时才会被交还给内核，而这个阈值同样会被动态抬高。两者叠加的结果是：

1. 第一个大请求把阈值顶上去；
2. 之后每个大请求都从 arena 里切，`free` 之后块留在进程里；
3. 阈值已经很高，进程再也不会主动 `brk` 回缩。

`MALLOC_ARENA_MAX=2` 不能解决这个问题（实测反而更糟，193 MB），因为它限制的是 arena 数量，不是归还策略。

### 8.3 实测对比

同一份 Release 二进制，只用 `LD_PRELOAD` 换分配器；4 MiB body × 60 请求、并发 3，发压结束后静置 6 秒采样：

| 配置 | 静置后 `RssAnon` | 相对默认的 CPU 开销 |
| --- | --- | --- |
| glibc 默认 | 78 MB | — |
| **glibc + `MALLOC_TRIM_THRESHOLD_=131072`** | **2.7 MB** | 大 body 上约 +8%，小 body 上无可测差异 |
| jemalloc 默认 | 111 MB | 略快 |
| jemalloc `dirty_decay_ms:0,muzzy_decay_ms:0` | 5.3 MB | 大 body 上约 +18% |
| mimalloc 默认 | 215 MB | 略快 |
| mimalloc `PURGE_DELAY=0` | 31 MB | 大 body 上约 +42% |
| snmalloc | 237 MB | 略快 |
| tcmalloc `RELEASE_RATE=1.0` | 195 MB | 略快 |

固定 `mmap` 阈值与固定 `trim` 阈值效果几乎相同（2.7 MB vs 2.8 MB），只设 `MALLOC_TRIM_THRESHOLD_` 一个
变量就够了，不需要 `LD_PRELOAD`、不引入依赖、不改代码。

### 8.4 代价

固定阈值让每个大块都走 `mmap` / `munmap`，缺页次数大幅上升。用 16 MiB body 测 4 轮取中位数：

| 配置 | CPU 中位数 | 静置后 `RssAnon` |
| --- | --- | --- |
| glibc 默认 | 7340 ms | 564 MB |
| glibc + trim 阈值 | 8920 ms | 2.7 MB |
| jemalloc decay=0 | 8640 ms | 5.8 MB |

也就是**用约 20% 的大请求 CPU 换掉 560 MB 的常驻**。这是有意义的交换：大 prompt 的处理本来就是
CPU 密集的 JSON 解析与加密，多这 20% 不会让请求超时；而少掉的几百 MiB 是常驻的、每天 24 小时都在的。

小请求不受影响：100 KB body × 600 请求、并发 8 时，固定阈值的 wall time 是 453 ms、默认是 475 ms，
CPU 1300 ms 对 1360 ms——差异在噪声内，方向甚至略好。

### 8.5 上限

32 MiB body（等于默认 `--max-body-bytes`）单并发时，两套配置的**峰值**都在 380–410 MB，即 body 的
12–13 倍；两并发时都在 745–865 MB。峰值由"一次请求需要多少份 body"决定，与分配器无关：

- Beast 的 `string_body` 持有一份；
- `server.cpp:218` 把 `request.body()` 复制进 `chat_http_request::m_body`，第二份；
- `chat.cpp:42` 解析成 `nlohmann::json` 树，第三份且显著更大；
- 转译出的上游 body 再一份。

要压峰值只能减少这些副本，光换分配器或调阈值做不到。当前默认的单机 32 MiB 上限配合这两三百 MiB 的
峰值是合理的；如果哪天要放宽上限，这几份副本必须先合并。

### 8.6 建议

在 systemd unit 里加一个环境变量即可：

```ini
Environment="MALLOC_TRIM_THRESHOLD_=131072"
```

不改代码、不加依赖、不影响小请求快路径。要继续优化峰值则需要改代码，见 8.5。

## 9. 请求峰值与内存归还修复

### 9.1 减少同时存活的大块缓冲

这次修改覆盖接收、翻译、签名和发送整条链路：

- Beast 正文移动给 handler；解析后释放原始字符串，归一化后释放输入 JSON，不把它们留到响应结束。
- 归一化读取借用 JSON 内容，工具调用与临时文本使用移动；派生 session id 后释放首条用户文本副本。
- 模型上下文消费归一化消息、工具和系统文本，序列化后移回候选请求，账户切换复用这一份数据。
- JSON 消息和工具移动进上游 envelope。长度计算使用计数 streambuf，不再通过 `dump().size()`
  分配整份临时文本；复用消息和工具的长度，只另算小 envelope，准确预留最终字符串空间。
- 会话 SHA-256 与 COSY MD5 分段计算，保留原字节顺序和长度前缀，不再拼接包含整份正文的哈希输入。
- BodyCodec 结果直接移动到 HTTP 请求；编码后释放明文，队列重试复用同一正文与签名。
  Beast 发送使用 `span_body<char const>` 借用正文，拥有者一直活到异步发送和连接重试结束。

字段顺序、UTF-8、转义、`content` / `contents` 双写、上下文窗口字节计算、签名及重试规则均保持原语义。
新增回归覆盖消费接口、正文释放、大正文的队列重试和账户切换，以及转义后的准确字节数。

### 9.2 默认约束 glibc 的保留策略

`qoderz serve` 在创建工作线程前调用 `mallopt(M_TRIM_THRESHOLD, 131072)`。glibc 在显式设置此阈值时
也关闭动态 `mmap` 阈值提升，让后续的大块缓冲更稳定地通过 `mmap` 分配，并在释放时归还给内核。
不需要后台回收线程、逐请求 `malloc_trim`，也不新增分配器依赖。非 glibc 平台保持原分配策略。

显式的 `MALLOC_TRIM_THRESHOLD_`、`MALLOC_MMAP_THRESHOLD_`、`MALLOC_MMAP_MAX_`、`MALLOC_TOP_PAD_`
优先；`GLIBC_TUNABLES` 中存在 `glibc.malloc.*` 项时也保留部署者的策略。其它命令不修改分配策略。
已有 systemd trim 环境变量可继续保留；新默认不要求它。

这解决的是本次复现的“大请求释放后 arena 长期保留大块空闲内存”。小块分配、长寿命对象夹住空闲页、
线程缓存与其它分配器仍可能造成碎片和少量保留；这里不承诺任意负载结束后都回到启动时的 RSS。

### 9.3 同机对比与复现

对照二进制来自修改前的 `17b1e03`，在修改源码前保存为 `build-perf/qoderz-baseline`。
两边均为 GCC 14.4.0、Release、4 个工作线程，使用同一 loopback stub、假凭据、默认 32 MiB body 上限。
本轮内核为 Linux 6.18.63 x86_64；CPU 与虚拟机同第 1 节。测试仅使用合成文本。

正文接近上限的场景使用 33,553,000 字节文本，加 JSON 后 33,553,103 字节，略低于 32 MiB，避免把
413 拒绝路径误计作优化收益。峰值使用 `VmHWM`，冷却 6 秒后的保留量使用 `Anonymous`；单位均为 MiB
（脚本 KiB 除以 1024）。小正文与 4 MiB 场景各运行三轮、逐指标取中位数；近 32 MiB 单并发的新程序
也重复三轮并报告范围，其余近上限对照为单轮。

近上限场景的旧程序经过多个请求后还保留了前几次的大块空闲页，因此本轮 `VmHWM` 可以高于第 8 节
的旧采样结果；单次处理的副本放大和多次请求的分配器保留会叠加。两者需要同时处理。

| 场景（文本 / 请求数 / 并发） | 旧峰值 RSS | 新峰值 RSS | 旧冷却后 Anonymous | 新冷却后 Anonymous |
| --- | ---: | ---: | ---: | ---: |
| 100 KiB / 600 / 8，三轮中位数 | 22.3 | 18.1 | 10.0 | 4.8 |
| 4 MiB / 60 / 3，三轮中位数 | 235.6 | 64.3 | 158.3 | 2.7 |
| 近 32 MiB / 6 / 1 | 812.7 | 150.5–182.6 | 533.4 | 2.5–34.3 |
| 近 32 MiB / 6 / 2 | 940.1 | 289.2 | 597.4 | 2.5 |

全部正常负载请求均返回 200。4 MiB 场景峰值下降约 72.7%；近上限单并发峰值下降约 77.5%–81.5%，
两并发下降约 69.2%。近上限单并发有两轮冷却后仍保留约 34 MiB，一轮约 2.5 MiB；本轮没有进一步
证明这部分全部属于空闲碎片，因此只记录现象，不把它当成已根治的泄漏或碎片问题。

CPU 与 wall 对照如下。CPU 是网关消耗；大正文的 curl 默认 `Expect: 100-continue` 等待会影响 wall，
不能直接把此表的 wall 推导成真实上游吞吐。构建等其它任务也会影响调度，本表只支持本机趋势判断。

| 场景 | 旧 CPU ms | 新 CPU ms | 旧 wall ms | 新 wall ms |
| --- | ---: | ---: | ---: | ---: |
| 100 KiB / 600 / 8，三轮中位数 | 1310 | 1320 | 519 | 517 |
| 4 MiB / 60 / 3，三轮中位数 | 5070 | 5130 | 21987 | 22074 |
| 近 32 MiB / 6 / 1，新程序三轮中位数 | 4260 | 3940 | 10445 | 10044 |
| 近 32 MiB / 6 / 2 | 5640 | 3790 | 6200 | 5193 |

小正文与 4 MiB 场景 CPU 分别约 +0.8%、+1.2%，没有复现第 8 节只调分配器时的大幅额外开销；
近上限请求减少副本后 CPU 也下降。这里没有做统计显著性检验，不把小幅变化解释为稳定加速或退化。

为了区分代码优化和分配器策略，另外做了两组单轮对照：

- 旧程序显式设置 `MALLOC_TRIM_THRESHOLD_=131072`，近 32 MiB / 6 / 1 的峰值仍有 **395.8 MiB**，
  冷却后匿名内存约 2.5 MiB、CPU 5230 ms。说明仅调分配器能归还内存，消除副本才能进一步压低峰值。
- 新程序显式设置 `GLIBC_TUNABLES=glibc.malloc.tcache_count=7`，应用保留部署者策略，跳过默认
  `mallopt`。4 MiB / 60 / 3 冷却后匿名内存约 **109.7 MiB**，而默认三轮中位数为 2.7 MiB。
  减少副本本身不能保证 glibc 主动归还空闲页。

复现命令（从项目根目录、在 devShell 中运行）：

```sh
# 修改源码前保存基线二进制；不要在优化后二进制上覆盖这个副本。
cmake -S . -B build-perf -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-perf --parallel 4
cp build-perf/qoderz build-perf/qoderz-baseline

# 应用修改并重新构建后，同一脚本分别驱动两份二进制。
QODERZ_PERF_BINARY="$PWD/build-perf/qoderz-baseline" \
  QODERZ_PERF_BODY_BYTES=4194304 QODERZ_PERF_REQUESTS=60 QODERZ_PERF_CONCURRENCY=3 \
  ./scripts/measure-load.sh --no-build
QODERZ_PERF_BODY_BYTES=4194304 QODERZ_PERF_REQUESTS=60 QODERZ_PERF_CONCURRENCY=3 \
  ./scripts/measure-load.sh --no-build

# 近上限正文；并发对照把 CONCURRENCY 改成 2。
QODERZ_PERF_BODY_BYTES=33553000 QODERZ_PERF_REQUESTS=6 QODERZ_PERF_CONCURRENCY=1 \
  ./scripts/measure-load.sh --no-build
```

本次还确认：文本恰好 32 MiB 时加上 JSON 外壳会触发 413，测量脚本返回失败；请求数为 0、采样
间隔为 0 也会在启动服务前拒绝。Debug 全量 CTest 756 项、格式与代码预算门禁均通过。没有覆盖真实
上游、TLS 大正文吞吐或数日持续运行；请求数与并发、JSON 树节点数量仍会影响总内存上限。
