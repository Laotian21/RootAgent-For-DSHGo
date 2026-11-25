# 接口与协议

守护进程只监听 `127.0.0.1:47831`（可用 `--port` 改）。除了 `/health`，所有接口都要
`Authorization: Bearer <令牌>`。

令牌就是 `/data/adb/rootagent/token` 的内容（64 位十六进制），并且被镜像到
`<rootfs>/root/.dsh/rootagent-token`（600，属主是 App uid）给 proot 侧读。

---

## POST /exec

执行一条命令，等它结束，一次性返回结果。

```http
POST /exec HTTP/1.1
Authorization: Bearer <token>
Content-Type: application/json

{"cmd":"id -u","cwd":"/","timeoutMs":30000}
```

| 字段 | 必填 | 说明 |
| --- | --- | --- |
| `cmd` | 是 | 交给 `/system/bin/sh -c` 的字符串，**最长 8192 字节**（超了返回 400 `cmd_too_long`） |
| `cwd` | 否 | 工作目录，默认 `/` |
| `timeoutMs` | 否 | 默认 30000，最小 100，最大 120000；超时按**进程组** kill |

成功返回 200：

```json
{
  "ok": true,
  "code": 0,
  "ms": 12,
  "timedOut": false,
  "stdoutTruncated": false,
  "stderrTruncated": false,
  "stdout": "0\n",
  "stderr": ""
}
```

- `code`：命令的退出码；被信号杀掉时是 128+信号。
- stdout / stderr **各自**最多保留 32768 字节（超出部分丢弃并置 `*Truncated: true`）。
- 请求体上限 16384 字节。
- 超时：返回 200，`code` 为 124、`timedOut: true`，进程组被 kill，已经产生的 stdout/stderr 照常返回。

---

## GET /health

**不需要令牌**（要让它当探活用），也不占并发额度 —— 满载时它照样返回 200。

```json
{
  "ok": true,
  "name": "root-agent",
  "version": "1.3.0",
  "pid": 1234,
  "uptimeMs": 998877,
  "requests": 4213,
  "sandbox": {
    "ready": 1,
    "owner": 10123,
    "tokenOk": 1,
    "tokenUid": 10123,
    "tokenMode": "600"
  }
}
```

| 字段 | 含义 |
| --- | --- |
| `sandbox.ready` | 沙箱 socket 已 bind 并 listening |
| `sandbox.owner` | socket 属主 uid（也是 `SO_PEERCRED` 允许的唯一 uid） |
| `sandbox.tokenOk` | 令牌镜像存在、长度对、属主对、权限对 |
| `sandbox.tokenUid/tokenMode` | 现查的 `stat` 结果，排障用 |

---

## GET /log

返回日志文件**末尾 16384 字节**（`text/plain` 包裹在 JSON 字符串里）。日志本身按 256 KiB 轮转。

```sh
curl -s -H "Authorization: Bearer $TOK" http://127.0.0.1:47831/log | head -c 2000
```

日志会记每条命令的前 80 字符（含 `/exec` 与沙箱路径），导出前先扫一眼敏感内容。

---

## POST /spawn

流式执行：命令边跑边推 stdout/stderr，用于 runner 的 HTTP 回退路径。

```json
{"cmd":"dshsb-exec ...","cwd":"/","timeoutMs":0}
```

响应头带 `X-Task-Id`（16 位小写十六进制）与 `X-Task-Pid`，`/kill` 用前者。

响应是 `200` + `Transfer-Encoding` 之外的**自定义帧流**，每帧 5 字节头 + 负载：

```
+------+---------------------------+
| type | length (4 字节，大端)      |
+------+---------------------------+
   1B              4B             length 字节的负载
```

| type | 含义 |
| --- | --- |
| `o` | stdout 数据块 |
| `e` | stderr 数据块 |
| `x` | 结束帧，负载是 4 字节 int32 退出码（小端） |

`timeoutMs: 0` 表示不设超时（由客户端断开触发 kill）。客户端断开 → 守护进程 kill 整个进程组。

---

## POST /kill

按 id 杀掉 `/spawn` 起的进程组。

```json
{"id":"9f3a1c0b2d4e5f67"}
```

- `id` 必须是 16 位以内的小写十六进制（其它字符 400 `bad_id`）。
- 先 `SIGTERM` 整个进程组，150ms 后 `SIGKILL`。
- 返回 `{"ok":true}`；找不到时 `{"ok":true,"note":"not_running"}`；pid 被复用检测到则
  `{"ok":true,"note":"stale_pidfile"}`。

---

## 错误码

| 状态 | body | 场景 |
| --- | --- | --- |
| 400 | `{"ok":false,"error":"missing_cmd"}` | 没给 `cmd` 或为空 |
| 400 | `{"ok":false,"error":"cmd_too_long","max":8192}` | `cmd` 超过 8192 字节 |
| 400 | `{"ok":false,"error":"bad_cwd"}` | `cwd` 过长或不是字符串 |
| 400 | `{"ok":false,"error":"truncated_body"}` | 请求体没读全 |
| 400 | `{"ok":false,"error":"bad-request"}` | JSON 解析失败 / 其它格式错误 |
| 401 | `{"ok":false,"error":"unauthorized"}` | 令牌错、缺 `Authorization` |
| 413 | `{"ok":false,"error":"body_too_large"}` | 请求体 > 16 KiB |
| 429 | `{"ok":false,"error":"busy"}` | 并发已满 8（`/health` 不受限） |
| 500 | `{"ok":false,"error":"out_of_memory"}` | 结果太大分配失败 |

---

## 调试用环境变量

守护进程本身只认命令行参数，下面几个环境变量是给本地联调/排障用的（正式部署不需要设）：

| 变量 | 默认 | 用途 |
| --- | --- | --- |
| `ROOTAGENT_SHELL` | `/system/bin/sh` | 换掉执行命令用的 shell（在 proot 里测试时设 `/bin/sh`） |
| `ROOTAGENT_PATH` | 守护进程继承的 PATH | 换 `/exec` 的 PATH |
| `ROOTAGENT_RUNDIR` | `/data/adb/rootagent/run` | `/spawn` 记 pidfile 的目录，`/kill` 靠它找进程组 |

命令行参数：`--port`（默认 47831）、`--token-file`、`--log`、`--pidfile`、`--sandbox-sock`、
`--sock-uid`、`--rootfs`；`--version` 打印版本后退出。

---

## 沙箱 unix socket 协议

沙箱不走 HTTP，走 `/data/user/0/<APP_PKG>/files/... ` 下的 unix socket（路径由守护进程推导，也可
`--sandbox-sock` 指定），目的是**传递文件描述符**。

客户端（`dshsb-client`）：

1. 连 socket。
2. 用 `sendmsg` 发一行 JSON 头 + `SCM_RIGHTS` 附带 fd（存在的 `1/2/7`）：
   `{"cmd":"...","cwd":"/..."}`（来自环境变量 `DSH_SB_HEADER`）。
3. 等服务端回一行 `exit N`，用它作为退出码；其它回复原样打印到 stderr 并返回 111。

服务端（`rootagentd`）：

1. `SO_PEERCRED` 取对端 uid，必须等于 socket 属主，否则回 `peer-uid-mismatch` 并关闭。
2. 解析头部 JSON，fork/exec `dshsb-exec`，把收到的 fd 接到子进程上。
3. 客户端断开（EOF / HUP）→ kill 进程组。
4. 正常结束 → 回 `exit N`。

环境变量（都由 `dsh-bwrap-root` 设置，一般不用手填）：

| 变量 | 用途 |
| --- | --- |
| `DSH_SB_SOCK` | socket 路径 |
| `DSH_SB_HEADER` | 一行 JSON 头 |
| `DSH_SB_FORCE_HTTP` | 设 1 强迫走 HTTP 回退（调试用，会失去 namespace 隔离） |
| `DSH_SB_CLIENT` | `dshsb-client` 路径（默认 /usr/local/bin/dshsb-client） |
| `DSH_SB_EXEC` | `dshsb-exec` 路径（默认模块 bin 下） |

---

## rootrun

proot 侧的薄封装，等价于一次 `/exec`：

```sh
rootrun 'id -u'
rootrun 'ls /data/adb/modules'
echo 'echo hi' | rootrun
```

- 令牌：`ROOT_AGENT_TOKEN`（默认读 `/root/.dsh/rootagent-token`）。
- 端口：`ROOT_AGENT_PORT`（默认 47831）。
- 超时：`ROOT_AGENT_TIMEOUT`（毫秒，默认 60000）。
- 退出码沿用远端命令的退出码；连不上时返回 111。
