# 账户与凭据

QoderZ 的账户模型围绕三个问题：**一个账户是谁**、**它存在哪里**、**命令如何选中它**。
本文定义凭据文件、身份匹配、默认账户与选择规则；CLI 的具体命令见 [CLI 设计](cli.md)。

## 1. 账户

一个账户由以下字段描述：

| 字段 | 含义 |
| --- | --- |
| `site` | 站点 id：`qoder` 或 `qoder-cn` |
| `name` | 用户可见的账户名，站点内唯一（忽略大小写） |
| `vpc` | Qoder CN 企业 VPC 实例，空表示公网 |
| `access_token` | 当前聊天/模型调用使用的令牌 |
| `refresh_token` | 刷新令牌；type 决定走 job 还是 device 刷新 |
| `expires_at` | access token 过期时间，Unix 毫秒 |
| `uid` | Qoder 用户 id |
| `display_name` | 用户显示名 |
| `email` | 邮箱，可能为空 |
| `account_id` | 展示/身份匹配用的 id：email（空则 uid），VPC 时附加 ` (vpc)` |
| `machine_id` | COSY 的机器 id，登录时生成并持久化 |
| `device_token` / `device_refresh_token` | 设备流拿到的 device token 对 |
| `device_chat` | 是否使用 CN 的 device token 回退 |
| `pat_auth` / `pat` | 是否 PAT 登录，及 PAT 明文（用于 PAT 失效时重新交换） |
| `created_at` | 首次保存时间，Unix 毫秒 |

账户 key 固定为 `"{site}/{name}"`，例如 `qoder-cn/work`。VPC 不进入 key，因此同一站点的多个
VPC 账户可以共存；VPC 只影响主机解析。

## 2. 凭据文件

### 2.1 路径

优先级从高到低：

1. `--credentials <path>`（原样使用）；
2. `QODERZ_CREDENTIALS`；
3. `$XDG_CONFIG_HOME/qoderz/credentials.json`（仅当 `XDG_CONFIG_HOME` 是绝对路径）；
4. `~/.config/qoderz/credentials.json`；
5. Windows：`%APPDATA%\qoderz\credentials.json`。

目录不存在时创建（0700）。文件不存在视为“没有账户”，不是错误。

### 2.2 Schema（version 1）

本项目不兼容 NoQoder 的 v2 文件，也不做迁移。首次运行 `qoderz login` 会创建新文件。

```json
{
  "version": 1,
  "accounts": {
    "qoder-cn/work": {
      "site": "qoder-cn",
      "name": "work",
      "vpc": "",
      "access_token": "...",
      "refresh_token": "...",
      "expires_at": 1760000000000,
      "uid": "u-123",
      "display_name": "Ada",
      "email": "ada@example.com",
      "account_id": "ada@example.com",
      "machine_id": "0f3c...",
      "device_token": "...",
      "device_refresh_token": "...",
      "device_chat": false,
      "pat_auth": false,
      "pat": "",
      "created_at": 1760000000000
    }
  },
  "defaults": {
    "qoder-cn": "qoder-cn/work"
  }
}
```

字段使用 snake_case；时间统一为 Unix 毫秒。缺失字段按空值/`0`/`false` 处理，但以下必须
校验：

- `version` 必须为 `1`；
- `site` 必须非空且只含 `a-z`、`0-9`、`-`（站点表里的规范 id，不带 `/`）；
- 账户 key 必须等于 `site + "/" + name`；
- `name` 必须合法，且同站点忽略大小写唯一；
- `defaults` 只能指向同站点存在的账户。

未知字段被忽略，并在下一次写入时丢弃：`version` 不变就不存在跨版本误解的风险，
对着新版本手工编辑过的文件也不会因为多一个键而拒绝启动。

任何结构错误都是配置错误（退出码 1），错误消息要指出文件路径与具体问题，**绝不静默修复**。
文件损坏时不能覆盖它；用户应修复或换 `--credentials` 路径。

### 2.3 权限与写入

- 文件权限 0600；`qoderz` 自有的配置目录（默认路径算出的 `.../qoderz/`）为 0700；
- 写入使用同目录临时文件（`.<name>.<random>.tmp`），写完 `fsync`，再原子 rename；
- 临时文件创建时即 0600，失败时清理；
- 不做备份，不自动迁移旧格式；
- 写路径总是重新读盘，避免覆盖别的进程刚写入的账户；
- 只创建缺失的目录，不收紧已存在目录的权限：`--credentials` 可能指向用户的项目目录或家目录，
  静默 `chmod 0700` 是越权副作用；
- 每次读取都重新读盘，不做 `(mtime, size)` 缓存；`serve` 这类常驻进程在启动时读一次快照自行持有，
  刷新 token 后按自己的节奏写回；
- 没有跨进程锁；并发 CLI 写入是“后写者赢”，但原子 rename 保证文件不会半截。

凭据里有明文 access/refresh/PAT，**任何日志、异常、`--json` 输出都不能包含它们**。

## 3. 身份与去重

登录成功时，用以下规则找到“同一个人”：

同一站点内，先要求 `vpc` 完全相等（trim 后比较；空 `vpc` 只与空 `vpc` 匹配），然后按
`uid` → `account_id` → `email` → `display_name` 取**新登录返回值的第一个非空字段**，
与已存记录的**同名字段**做 trim 后精确比较；命中即视为同一个人。

这里不采用“两侧各取第一个非空值拼成复合 key”的写法：`userinfo` 可能拿不到 `uid`
（见 [Qoder 认证](../protocol/qoder-auth.md)），`email` 又是独立来源，复合 key 会让
“已存记录首字段是 uid、新登录只带回 email”的情况永远匹配不上，从而重复建账户。
身份匹配区分大小写，账户名匹配忽略大小写，两者不同，不要混淆。

同一站点内命中则原地更新凭据：

- 同名：只替换凭据字段；
- 不同名（显式 `--name`）：改 key，并同步改写指向它的 `defaults` 记录；
- 未命中且显式名字已被别的身份占用：报错，提示换名或先删除；
- 未命中且未给名字：自动命名。

自动命名规则：

1. 依次尝试 email 的 `@` 前部分、display_name、uid；
2. 保留 ASCII 字母数字（转小写），其它字符折叠成单个 `-`；
3. 去掉尾部 `-`，截到 64 字符；全空则 `account`；
4. 同站点冲突时追加 `-2`、`-3`……（忽略大小写比较）。

合法名字：`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`，即 1–64 字符，首字符是字母或数字。
错误消息要给出允许字符。

## 4. 默认账户

- 每个站点最多一个默认账户，存在 `defaults[site]`；
- 站点第一个账户自动成为默认；之后登录新账户不会改默认；
- `qoderz account use <selector>` 显式设置；
- 删除默认账户时，如果还有剩余账户，取按名字（忽略大小写）排序的第一个；
- 删除最后一个账户时，删除 `defaults[site]`；全文件无账户时删除凭据文件；
- `accounts` 输出用 `*` 标记默认账户；
- 没有 `defaults` 但站点恰有一个账户时，把它当作默认（兼容手工编辑）。

## 5. 账户选择

`--account <selector>`（以及 `QODERZ_ACCOUNT`）支持：

1. 精确匹配 name（忽略大小写）；
2. 精确匹配 uid；
3. 精确匹配 email；
4. 若以上都没有，做唯一前缀匹配（name/uid/email 任一，忽略大小写）。

某一层命中多个 → 报错并列出候选；一个都没有 → 报错并列出可用账户。
在 TTY 下遇到多义选择时，可以给编号选择器；非 TTY 必须失败，不能阻塞等待输入。

未给 selector 时：

- 命令需要单个账户（`models`、`usage`）：
  默认账户 → 站点唯一账户 → 报错；
- `serve`：默认使用该站点的全部账户作为池；`--account` 才固定单个；
- `logout`：只有一个账户时删它（需确认）；多个账户时必须给 selector 或 `--all`。

站点内没有任何账户 → 认证错误，提示 `run: qoderz login`。

## 6. 账户池视图

`serve` 启动时把凭据文件读成内存快照，并构造账户池：

- 同一站点的账户才会进入同一个池；`qoder` 与 `qoder-cn` 永不混池；
- 每个账户用 key 标识；
- 刷新 token 后写回文件；写回失败（账户被删）只告警，不中断请求；
- 运行中不重新加载整个凭据文件；需要新增账户时重启 `serve`。

## 7. 安全边界

- 凭据文件是唯一持久化秘密的地方；
- 日志只允许站点、账户名/uid、HTTP 状态；
- 不允许把 email 写进日志模板；
- `--json` 输出可以包含 email、uid、过期时间，但绝不能包含 token/PAT；
- 错误消息不回显 token 片段；
- `QODERZ_UPSTREAM` 只接受 loopback，防止凭据被发到任意主机。

## 8. 模块

| 模块 | 职责 |
| --- | --- |
| `config/credentials` | 文件定位、解析、校验、原子写、缓存 |
| `config/accounts` | 账户值类型、身份匹配、自动命名、默认与选择 |
| `config/paths` | XDG / APPDATA / 目录创建 |
| `cli/commands/account`、`cli/commands/login` | login/logout/accounts/use 的行为编排 |
