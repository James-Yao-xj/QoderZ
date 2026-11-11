# OpenAI 兼容翻译

QoderZ 向下游只暴露 OpenAI Chat Completions。本文将“客户端请求 → Qoder 请求”和
“Qoder 事件 → OpenAI 响应”两段翻译写清楚，作为 `src/openai/` 与
`src/protocol/text_tool_splitter` 的规范。上游 SSE 本身见 [qoder-chat](qoder-chat.md)。

## 1. 请求字段

只读以下字段，其它字段一律静默忽略、零校验：

| 字段 | 规则 | 缺失/非法 |
| --- | --- | --- |
| `model` | JSON string，trim | 空 → 400 `model is required` |
| `stream` | 只有 JSON `true` 才算流式 | 其它一律非流式 |
| `messages` | 必须是数组 | 否则 400 `messages is required`；空数组合法 |
| `tools` | 非数组视为无工具 | |
| `tool_choice` | string trim+lower；object → `"function"`；其它 → 空 | 只有 `none` / `required` 有语义 |
| `max_completion_tokens` → `max_tokens` | 第一个是 Number、能转 int32、且 `> 0` 的值；先看 completion 再看 tokens | 都为 0 → **32000** |
| `reasoning_effort` | string trim | 空 → 模型默认 |
| `prompt_cache_key` / `user` | string trim；只参与 session id | 空 → 下一优先级 |
| header `X-Qoder2Api-Session`，回退 `X-Session-Id` | trim 后非空才生效 | 空 → 忽略 |

静默忽略：`temperature`、`top_p`、`n`、`stop`、`seed`、`presence_penalty`、
`frequency_penalty`、`logprobs`、`response_format`、`stream_options`、
`parallel_tool_calls`、`metadata`、`store`、`functions`/`function_call`、
`modalities`、`service_tier`。因此 `stream_options.include_usage` 无效：流式 usage
**总是**出现。

`model` 在端点层还会去掉 `"{site}/"` 前缀（忽略大小写），再按目录 key 忽略大小写匹配；
未找到 → 400 `unknown or disabled model "..."`。

## 2. system / developer 与 tools

### 2.1 system_text

固定前缀：

```text
You are a Qoder agent. Use the instructions below and the tools available to you to assist the user.
```

遍历全部 messages，收集 role 为 `system` / `developer`（trim 后精确匹配、大小写敏感）的文本；
空文本跳过；按原顺序用 `\n\n` 连接。有收集项时：

```text
prefix + "\n\n" + join("\n\n", parts)
```

如果 `include_tools && tool_choice == "required"`，再追加：

```text
"\n" + "You must call an available function in this response."
```

这些 system/developer 条目不再出现在对话 messages 里。

文本提取规则：

- string：原样；
- object：取 `text`（string），`type` 必须为空 / `text` / `input_text`；
- array：逐项提取文本、丢空项、用 `\n` 连接；
- 其它类型：空。

正文不 trim。

### 2.2 tool_choice

`include_tools = tools 非空 && tool_choice != "none"`。

| 值 | 发 tools | 追加 Required 行 |
| --- | --- | --- |
| 缺失 / 空 / `auto` / object / 其它字符串 | tools 非空时发 | 否 |
| `required` | 非空时发 | 是（仅当确实有 tools） |
| `none` | **不发**，连 `tools` 属性都没有 | 否 |

`none` 只影响 tools 与系统行；历史 assistant 的 `tool_calls` 与 tool 结果仍照写。

### 2.3 工具 schema

逐项要求 entry 是 object 且 `function` 是 object、`function.name` trim 后非空，否则整条跳过，
不报错。`type` 忽略；`description` 缺失写 `""`；`parameters` 仅当值为 object / `true` / `false`
时原样保留，否则合成：

```json
{"type":"object","properties":{}}
```

输出恒为：

```json
{"type":"function","function":{"name":"...","description":"...","parameters":{...}}}
```

`strict` 丢弃，schema 不做裁剪或改写。

## 3. 消息归一化

逐项处理 messages：非 object 跳过；role 用 trim 后精确匹配 `user` / `assistant` / `tool`，
其它角色（包括 system/developer）忽略。

### 3.1 user

content 的归一化：

- string → 一个 text block；
- array → 逐 part：
  - string → text；
  - `{"type":"text","text":"..."}` → text；
  - `{"type":"image_url","image_url": string | {"url": string}}` → image block（URL trim，空丢弃）；
  - `input_audio`、`file`、其它对象 → 忽略。
- content 缺失/null：产生一个 user 消息，blocks 为空。

`array_content` 标记原始 content 是否为数组。第一个 user 的文本存 `first_user_text`；
每个 user 都更新 `last_user_index`（原始 messages 下标，含被忽略项）与 `last_user_text`。
文本取 `read_text_content` 的结果；图片不参与文本。

### 3.2 assistant

content 只接受 string part 或 `type == "text"` 的 object part；图片等忽略。

`tool_calls` 仅当是数组。每个元素是 object 时：

- `function.name` trim；
- `function.arguments`：string 原样；null/缺失 → `""`；其它 JSON 类型 → 原始 JSON 文本
  （不加引号）；
- `id` trim；为空时确定性补齐：

  ```text
  seed = "{turn_index}\0{call_index}\0{name}\0{arguments}"
  id   = "call_" + lowercase_hex(SHA256(utf8(seed)))[0..32]
  ```

`turn_index` 只对“含至少一个 tool_call 的 assistant 消息”递增；`call_index` 对数组的每个
元素递增（非 object 也占号）。同一请求重复翻译得到同一 id。

### 3.3 tool 结果与配对

维护 `open_calls`：

- 每条 user 消息清空；
- 每条 assistant 消息替换为该消息的 pending 列表（无调用就是空表）。

因此 tool 结果只与**紧邻的上一条 assistant** 配对；连续多个 tool 消息共享同一 pending 列表。

匹配规则：

1. `tool_call_id` 非空：按顺序找第一个未匹配且 id 完全相等的；找不到 → 孤儿，
   **不会退化**为“最早未匹配”；
2. id 为空：取最早的未匹配项；
3. 匹配后置 matched，不可复用。

匹配成功：

- 写 `{"role":"tool","tool_call_id":"<id>","content":"<text>"}`；
- 若 content 数组含图片，文本追加 note：
  1 张 → `[The tool returned an image; it follows in the next message.]`；
  n 张 → `[The tool returned {n} images; they follow in the next message.]`；
  空文本时只留 note；
- 图片进入 pending 组，组 id 为匹配到的 id。

匹配失败（孤儿）：

- 文本非空 → 降级成 user 文本消息（单 text block，`array_content` 照抄原 content 是否为数组）；
- 图片单独入组，组 id 为 `tool_call_id`，空则 `"tool"`；
- 两者都空 → 整条丢弃。

### 3.4 图片外移

在每个 user 消息之前、每个 assistant 消息之前、以及全部消息结束后，把 pending 图片组各生成
一条独立 user 消息：

```json
{"role":"user","content":[
  {"type":"text","text":"[From the result of {id}:]"},
  {"type":"image_url","image_url":{"url":"..."}}
]}
```

且 `array_content = true`，因此会写 `contents`。所以 tool 结果里的图片总在 tool 消息之后的
下一/几条 user turn。

### 3.5 cache_control

全部消息生成后，只处理**最后一条**消息：

- 必须 `role == "user"` 且有 blocks；
- 从后往前找第一个 type 不在 `thinking`、`redacted_thinking`、`tool_use`、`tool_result`
  （忽略大小写）的 block；
- 给它加 `"cache_control":{"type":"ephemeral"}`。

最后一条是 assistant/tool，或 user 无 block → 全篇没有 cache 标记。归一化只产生 text /
image_url，所以等价于“最后一个 block”。

### 3.6 写出

- 第一条固定 system：

  ```json
  {"role":"system","content":[{"type":"text","text":"<system_text>"}]}
  ```

- tool：`{"role":"tool","tool_call_id":"...","content":"<纯字符串>"}`。
- assistant：`{"role":"assistant","content":[blocks]}`；有调用时再写
  `"tool_calls":[{"id","type":"function","function":{"name","arguments"}}]`。
- user：`{"role":"user","content":[blocks]}`；若原 content 是数组，再写一份
  **逐字节相同**的 `"contents":[blocks]`。字符串 content 的 user 没有 `contents`。
- block：text → `{"type":"text","text":"..."}`；image →
  `{"type":"image_url","image_url":{"url":"..."}}`。

## 4. 会话与任务

### 4.1 session_id

优先级：header → `prompt_cache_key` → `user` → `system_text + first_user_text`。
每段先写 **int32 小端 UTF-8 字节长度**，再写 UTF-8 字节；SHA-256 后取小写 hex，
按 `8-4-4-4-12` 切分。tag 分别为 `session`、`key`、`user`、`chat`。

fallback 的 `system_text` 含 Required 行，所以同一 prompt 改 `tool_choice` 会换 session；
`prompt_cache_key` / `user` 分支则与 prompt 无关。

### 4.2 turn key 与任务

`turn_key = SHA256(length_prefixed(session_id) + int32_le(last_user_index) + utf8(last_user_text))`。

命中任务表则复用并刷新 LRU；否则新建：

- id：UUID；
- `begin_at`：当前 Unix 毫秒；
- `answered = false`；
- `name`：最后一条 user 文本前 10 个字符。

任务表容量 512、空闲 2 小时清理。`mark_answered(task_id)` 只把 answered 置 true。
请求体里：

- `request_set_id = business.id = task.id`；
- `business.name = task.name`；
- `business.begin_at = task.begin_at`；
- `business.stage = answered ? "processing" : "start"`。

QoderZ 对 `business.name` 取前 10 个 Unicode 标量（不是 UTF-16 code unit）；这是唯一有意的
本地差异，只影响 telemetry，不参与协议判定。

## 5. 选择器

### 5.1 reasoning effort

rank：`minimal=0, low=1, medium=2, high=3, xhigh=4, max=5`；输入 trim+lower，`ultra → max`。

1. `!model.thinks` 或（`asked == "none"` 且 `!model.always_thinks`）→ `(false, null)`；
2. `model.efforts` 为空 → `(true, null)`；
3. `asked` 为空 → `(true, normalize(model.default_effort) 或 null)`；默认值不校验是否在列表里；
4. `asked == "none"`（此时必是 always_thinks）→ `(true, 最低 effort)`；
5. 否则 `target = index_of(asked)`：
   - 未知 → 默认值（可能 null）；
   - 已知 → 按 rank 距离最近，**平局取更高 rank**；
   - 没有可识别项 → `efforts[0]`。

固定向量：`["low","medium","high"]` 上 `minimal→low`、`xhigh→high`、`max→high`；
`["low","high"]` 上 `medium→high`；`["low","medium","high","max"]` 上 `ultra→max`；
`["turbo","high"]` 上 `low→high`；`efforts=[]` → `(true,null)`；`thinks=false` → `(false,null)`。

### 5.2 context window

```text
length = messages_bytes + tools_bytes + 11
need   = ceil(length / 3) + max_tokens
base   = default_window > 0 ? default_window : context
```

- `need <= base` → `base`；
- 否则取大于等于 `need` 的最小 window；
- 没有则取最大 window；
- 列表空则用 `context`。

`messages_bytes` / `tools_bytes` 是序列化后的 UTF-8 字节数（含数组方括号与 system turn）；
tools 被省略时按 `"[]"` 的 2 字节计。JSON 转义会改变结果。

固定向量：windows `[200000,1000000]`、default `200000`、context `1000000`、msgs 3000、
tools 0、max 32000 → 200000；max 250000 → 1000000；tools 600000 → 1000000；
windows `[100000,300000,500000]`、default `100000`、msgs 600000、max 32000 → 300000；
msgs 3000000 → 最大 200000；default/context 0、msgs 3000、windows `[100000]` → 100000。

## 6. Qoder 请求体

顺序固定：

```text
request_id, request_set_id, chat_record_id, session_id,
business{product:"app", version:"1.1.69", type:"agent", id, name, begin_at, stage},
agent_id:"agent_common", task_id:"common", session_type:"app",
model_config, system[{type:"text",text}], messages[...], tools[...]?,
parameters{enable_thinking, max_tokens, reasoning_effort?, context_length?}
```

- `request_id == chat_record_id`，每次翻译新 UUID（带连字符）；
- `model_config` 原样嵌入目录的原始 JSON；解析失败或非 object → `{}`；
- `system` 与 `messages[0]` 文本相同；
- `parameters` 最后写，因为 context_length 需要先量 messages/tools 字节数；
- 不写 `temperature` 等采样字段。

## 7. 非流式响应

收集事件，遇到 `Stop` 后停止；遇到 `Error` 立即映射为错误，丢弃已收集内容。无论是否有 Stop，
都在结束时**一次**写出完整 JSON，因此错误不会留下半截 body。

字段：

- `id = "chatcmpl-" + 32 位 hex GUID`；
- `object = "chat.completion"`；
- `created` = 开始时刻 Unix 秒；
- `model` = 目录里的 key；
- `choices[0].index = 0`；
- `message.role = "assistant"`；
- `content` 恒为字符串（文本拼接，无内容 `""`）；
- `reasoning_content` 仅非空时写；
- `tool_calls` 仅数量 > 0 时写，按 index 升序；
- `finish_reason`：事件 reason 非空则**原样**透传；为空则有工具用 `tool_calls`，否则 `stop`；
- `usage` 仅在 Stop 带 usage 时出现。

工具组装：`ToolStart` 用 `index = Index >= 0 ? Index : tools.size()`，
`id = Id`（空则 `"call_"+32hex`），`name = Name`（空则 `""`），arguments 从
`Arguments` 开始；后续 `ToolArguments` 追加到同 index。重复 ToolStart 不合并。

usage 字段顺序：`prompt_tokens, completion_tokens, total_tokens,
prompt_tokens_details{cached_tokens}, completion_tokens_details{reasoning_tokens}`。
上游 total 为 0 时用 prompt+completion 补。

## 8. 流式 SSE

响应头：`Content-Type: text/event-stream`、`Cache-Control: no-cache`、
`X-Accel-Buffering: no`。帧格式 `"data: " + compact_json + "\n\n"`；结束帧精确为
`data: [DONE]\n\n`。`id`、`created`、`model` 整条流固定。

chunk 骨架：

```json
{"id":"...","object":"chat.completion.chunk","created":123,"model":"...","choices":[{"index":0,"delta":{},"finish_reason":null}]}
```

| 事件 | delta | 备注 |
| --- | --- | --- |
| 首个非 Error 事件之前 | `{"role":"assistant","content":""}` | 只发一次；Error 作首事件时不发 |
| Text | `{"content":"..."}` | 逐事件一帧 |
| Reasoning | `{"reasoning_content":"..."}` | |
| ToolStart | `{"tool_calls":[{"index","id","type":"function","function":{"name","arguments"}}]}` | index = max(Index, 0)；arguments 是累计值 |
| ToolArguments | `{"tool_calls":[{"index","function":{"arguments":"..."}}]}` | 无 id/type/name |
| Stop | `{}` | finish_reason 为 reason 或 `stop`；有 usage 则同帧带 |
| Error | 裸 `{"error":{"message","type":"qoder_error","code":...}}` | 无 chunk 外壳 |

- Stop 后立刻写 `[DONE]`，返回 answered=true；
- Error 后不写 `[DONE]`、不写 finish_reason，返回 false；
- 流正常结束但没有 Stop → 写 `[DONE]`，返回 false；
- 零事件 → 只有 `[DONE]`；
- 已有 content 后出错，保留已写帧，再发 error 帧。

JSON 转义必须与 `UnsafeRelaxedJsonEscaping` 一致：非 ASCII 直接 UTF-8，`< > & '` 不转义，
只转义 `"`、`\` 与控制字符；输出无空白。

## 9. 文本工具调用拆分器

常量：`<tool_call>`、`</tool_call>`、`<function=`、`</function>`、`<parameter=`、
`</parameter>`，代码围栏长度 3。

错误文案必须逐字保留：

```text
the model's tool call, written as text, can't be read; it is not sent on as the answer
the model's tool call, written as text, was cut short; it is not sent on as the answer
```

### 9.1 状态机

字段：`text_buffer`、`call_buffer`、`in_call`、`fence_open`、`line_backtick_odd`、
`run_length`、`saw_tool`。

`feed(chunk)`：

- 不在调用中：找 `<tool_call>`：
  - 没找到 → 保留最长“开标签前缀”后缀（≤10），其余作为 Text 发出；
  - 找到 → 发出前缀文本，吞掉开标签，`in_call = true`。
- 在调用中：找 `</tool_call>`：
  - 没找到 → 整个剩余进 `call_buffer`（无长度上限）；
  - 找到 → 取出 body：
    - 解析成功 → `saw_tool = true`，发 Tool；
    - 否则若 body 是 callish 且未被引用 → 发 Error(unreadable) 并返回（同 chunk 闭标签后的文本丢弃）；
    - 否则把 `<tool_call>body</tool_call>` 原样当文本发出，并参与围栏统计；
  - `in_call = false`，继续循环。

`flush()`：

- 仍在调用中：body 是 callish 且未被引用：
  - 能解析 → 发 Tool；
  - 不能 → 发 Error(cut short) 并返回；
  - 否则发 `<tool_call> + body`（**不补闭标签**）；
- 不在调用中：发残留 text_buffer。
- 重复 flush 不重复输出。

不可解析时不回滚已经发出的前缀文本，只丢弃调用体。

### 9.2 引用判定（O(1)）

```text
is_quoted = (fence_open XOR odd(run_length / 3))
         || (line_backtick_odd XOR odd(run_length % 3))
```

每次发文本逐字符：反引号则 `run_length++`；遇非反引号则
`fence_open ^= odd(run/3)`、`line_backtick_odd ^= odd(run%3)`、`run=0`；
字符是 `\n` 时 `line_backtick_odd=false`（`\r` 不重置）。重新吐出的调用形状也参与统计。

### 9.3 两种语法

先 JSON 后 XML。

- JSON：body trim 后以 `{` 开头；必须是 object；`name` 是 string 且 trim 非空；
  `arguments` **必须是 object**；输出 compact JSON。arguments 是 string/array/null 都不行。
- XML：找 `<function=`，名字到第一个 `>`（trim 非空），必须存在 `</function>`；
  参数循环解析 `<parameter=key>...</parameter>`；key trim 后为空则消耗但跳过；
  值 trim 后若能整体解析成 JSON（数字/true/字符串/对象/数组）就保留类型，否则写成 JSON string；
  缺 `>` 或闭标签就停止扫描，已收集参数保留；无 `<function=` 或名字为空 → 失败。
  参数顺序保留，输出 compact JSON。

`is_callish(body)`：`trim_start` 后以 `{` 开头，或包含 `<function=`。

### 9.4 固定用例

| 输入 | 文本输出 | 其它 |
| --- | --- | --- |
| `hello ` + `world` | `hello world` | |
| `Ends on <tool_c` | `Ends on <tool_c` | 前缀暂存，flush 吐出 |
| `A block is <tool_call>name</tool_call> in prose.` | 原文 | 不可解析、非 callish → 回吐 |
| `Use <tool_call>incomplete` | 原文 | flush 不补闭标签 |
| `Reading.\n<tool_call>{"name":"Bash","arguments":{"command":"pwd"}}</tool_call>` | `Reading.\n` | Tool(Bash, `{"command":"pwd"}`) |
| `<tool_call><function=Bash><parameter=command>pwd</parameter></function></tool_call>` | 空 | Tool(Bash, `{"command":"pwd"}`) |
| XML 参数 `timeout=30, force=true, label="quoted", note=hello world` | 空 | 类型保留 |
| 三反引号 fence 内的不可解析调用 | 全文原样 | 无 Tool 无 Error |
| 行内奇数反引号包住的调用 | 原文 | 视作引用 |
| `Sure.\n<tool_call>{"name":"read","arguments":{"path":}}</tool_call>` | `Sure.\n` | Error(unreadable) |
| `Reading.\n<tool_call>{"name":"read","arguments":{"path":"a.go"` | `Reading.\n` | flush 时 Error(cut short) |
| 开标签 + 可解析 body，无闭标签 | 前缀文本 | flush 时发 Tool |

整条喂入与按 1/2/3/4/5/7 字符分片喂入，结果必须一致。

## 10. finish_reason 与错误

上游 finish_reason 只影响两处：

1. 解析器 StopReason：只要出现工具调用（文本或原生）→ `tool_calls`，即使上游说 `length`；
   否则仅精确等于 `length`（忽略大小写）保留；其它 → `stop`；
2. 响应翻译器：非空原样透传；空则按有无 tool_calls 决定。

文本调用不可解析/截断、原生调用缺 name → 502 错误事件，没有 Stop、没有 `[DONE]`；
已输出文本保留。非流式丢弃缓冲，抛错误，由 HTTP 层映射：

| 分类 | HTTP | type |
| --- | --- | --- |
| bad_request | 400 | `invalid_request_error` |
| auth | 401 | `authentication_error` |
| quota | 429 | `insufficient_quota` |
| busy | 503 + Retry-After | `server_busy` |
| upstream | 502 | `upstream_error` |

错误体统一 `{"error":{"message","type","code":<status>}}`。

## 11. 固定夹具

请求基础：

```json
{"model":"performance","stream":true,"messages":[{"role":"user","content":"Hello"}]}
```

模型 fixture：key `performance`，windows `[200000,1000000]`，default_window `200000`，
context `1000000`，efforts `[low,medium,high]`，default_effort `medium`，thinks true，
raw `{"key":"performance"}`。期望：

- system[0].text 为固定前缀；
- messages[0].content[0].text 相同；
- messages[1].content[0].text == "Hello"；
- parameters == `{"enable_thinking":true,"max_tokens":32000,
  "reasoning_effort":"medium","context_length":200000}`；
- 无 temperature；
- request_id == chat_record_id；
- request_set_id == business.id；
- business.stage == "start"。

流式 golden（model `dmodel`）：

```text
data: {"id":"chatcmpl-<32hex>","object":"chat.completion.chunk","created":<unix>,"model":"dmodel","choices":[{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}]}

data: {...,"choices":[{"index":0,"delta":{"content":"hi"},"finish_reason":null}]}

data: {...,"choices":[{"index":0,"delta":{"reasoning_content":"think"},"finish_reason":null}]}

data: {...,"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"read","arguments":"{\"path\":\"a.go\"}"}}]},"finish_reason":null}]}

data: {...,"choices":[{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":" more"}}]},"finish_reason":null}]}

data: {...,"choices":[{"index":0,"delta":{},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":10,"completion_tokens":5,"total_tokens":15,"prompt_tokens_details":{"cached_tokens":2},"completion_tokens_details":{"reasoning_tokens":3}}}

data: [DONE]
```

错误帧：`data: {"error":{"message":"expired","type":"qoder_error","code":401}}`。

## 12. 常见陷阱

1. 流式 usage 总是出现，`stream_options` 不起作用。
2. Error 帧后没有 `[DONE]`，也没有 finish_reason。
3. `contents` 必须与 `content` 逐字节相同。
4. cache 标记只落在最后一条 user 的最后一个 block；最后是 assistant/tool 就没有。
5. tool 配对作用域只有紧邻的上一条 assistant；user 会清空。
6. `tool_choice: none` 不会删除历史工具调用与结果。
7. context_length 的字节数包含 system turn、数组括号与转义。
8. `parameters` 必须最后序列化。
9. 默认 effort 不校验是否在模型列表里。
10. 原生工具 id 迟到不回填；文本调用 id 由本地生成。
11. 未知/非法 finish_reason 一律变 `stop`。
12. 没有 Stop 的非流式仍输出完整 JSON，并返回 answered=false。

## 13. 模块

| 模块 | 职责 |
| --- | --- |
| `openai/request` | 字段读取、校验、system 合并、tools |
| `openai/request/normalize` | 消息归一化、配对、图片外移、cache、contents |
| `openai/selection/session` | session id、turn key、任务 LRU |
| `openai/selection/selectors` | effort 与 window 选择 |
| `openai/request/body` | Qoder 请求 JSON 序列化 |
| `openai/response/response` | 非流式完整 JSON |
| `openai/response/stream_frames` | 流式 SSE 帧 |
| `protocol/sse/text_tool_splitter` | 文本工具调用状态机 |
