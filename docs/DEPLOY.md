# 部署指南

从零到「沙箱里 `id -u` 是 App 的 uid、`rootrun 'id -u'` 输出 0」，大约 15 分钟（含重启）。

下文约定：

| 变量 | 含义 | 默认 |
| --- | --- | --- |
| $PKG | dsh-go 的包名 | 自动识别 |
| $DATA | 模块私有数据目录（令牌、pid、日志） | /data/adb/rootagent |
| $SB | 共享存储里的对外目录 | /sdcard/Download/RootAgent |
| $PORT | 守护进程端口 | 47831 |

---

## 0. 前置检查

在手机的 root shell（KernelSU 管理器里的终端，或任何 `su` 环境）里跑：

```sh
unshare -m true && echo "mount ns OK"
grep -c . /proc/filesystems >/dev/null; echo "filesystems readable"
ls /data/user/0/*/files/runtime/rootfs 2>/dev/null | head
```

- 第一条报 `Operation not permitted` 说明内核没有 `CONFIG_USER_NS` —— 沙箱后端用不了，只能走 root 通道。
- 第三条应该能列出 dsh-go 的 rootfs；**先把 dsh-go 打开一次**，它才会解出 rootfs（模块安装时就存在会更好，
  否则第一次开机沙箱会晚一点就绪，见第 6 节）。

---

## 1. 安装模块

1. 把 `dist/RootAgent-v1.3.0.zip` 传到手机。
2. KernelSU / SukiSU 管理器 → 模块 → **从本地安装** → 选 zip → 安装 → **重启**。

重启后模块目录在 `/data/adb/modules/rootagent/`，开机时 `service.sh` 会：

- 生成/读取令牌 → `$DATA/token`（600，root）
- 把令牌镜像到 `<rootfs>/root/.dsh/rootagent-token`（600，**属主是 App uid**，否则引擎读不到）
- 起 `rootagentd`，只绑 `127.0.0.1:$PORT`
- 起看门狗：守护进程退出 10 秒内拉起；`/health` 连续 3 次异常（卡死）也重启

看模块有没有真的起来：

```sh
su -c 'cat /data/adb/rootagent/agent.log | tail -20'
su -c 'cat /data/adb/rootagent/rootagentd.pid'
```

---

## 2. 点「执行」拿令牌

SukiSU 模块卡片 → **执行**（`action.sh`）。它会打印：

```
Root Agent v1.3.0
状态: 运行中 (pid 12345)
端口: 127.0.0.1:47831
目标 App: <APP_PKG>
令牌: 3f9c...（64 位十六进制）
proot 侧安装（只需要一次）:
  sh /sdcard/Download/RootAgent/proot/install.sh
```

同时把 proot 侧要用的三个文件同步到 `$SB/proot/`。

> 令牌等同 root 权限。它就是 `$DATA/token` 的内容，只在 App 私有目录和 `$SB/proot/` 之外的地方出现；
> 不要截图、不要提交到仓库。怀疑泄漏：删 `$DATA/token` 再点一次「执行」。

没识别到 App：手动写 `$DATA/app.conf`：

```sh
su -c 'echo APP_PKG=com.example.dshgo > /data/adb/rootagent/app.conf'
su -c 'sh /data/adb/modules/rootagent/service.sh'
```

---

## 3. proot 侧安装（一次）

打开 dsh-go 的终端（proot Ubuntu），执行：

```sh
sh /sdcard/Download/RootAgent/proot/install.sh
```

它会装到 `/usr/local/bin`：

| 文件 | 作用 |
| --- | --- |
| `dshsb-client` | 把 DSH 的 fd 1/2/7 经 SCM_RIGHTS 交给守护进程 |
| `dsh-bwrap-root` | DSH 的 bwrap 方言 runner（Node），选 fd 通道或 HTTP 回退 |
| `rootrun` | `rootrun '<命令>'` → `POST /exec` |

验证：

```sh
rootrun 'id -u'                 # 0
rootrun 'getprop ro.product.model'
```

如果 `rootrun` 报 `令牌文件不存在`：说明 rootfs 里没有令牌镜像，回第 2 节重新点「执行」，
或手动确认 `ls -l <rootfs>/root/.dsh/rootagent-token` 的属主是 App uid、权限 600。

---

## 4. 引擎侧：把沙箱 runner 指过来

DSH 的沙箱后端要显式配 `runnerCommand`，否则它仍然按「没有沙箱」降级。编辑
`$DSH_HOME/profiles/web/cordis.patch.yml`（$DSH_HOME 默认 `/root/.dsh`）：

```yaml
- id: sandbox
  name: "@deepseek-ai/dsh-sandbox-local"
  config:
    runnerCommand:
      - /usr/local/bin/dsh-bwrap-root
    runnerFailureSignatures:
      - "dsh-bwrap-root: "
```

两种做法：

- **装了插件**：面板 → 「补配置」，它会把上面这段合并进 profile（已存在就不动），然后重启引擎。
- **手工**：把上面这段贴到 `cordis.patch.yml` 末尾（注意它是顶层列表，别缩进成子键）。

改完在 dsh-go 里**重启引擎**（或重启 App）。之后新建会话，`bash` 工具里：

```sh
id -u                 # App 的 uid，不是 0
touch /root/probe     # Permission denied（根只读）
ls /root/.dsh         # 空（被 tmpfs 遮住）
```

---

## 5. 验收清单

| 检查 | 命令 | 期望 |
| --- | --- | --- |
| 守护进程活着 | `curl -s 127.0.0.1:47831/health` | `"ok":true`，含 `pid`、`version` |
| 沙箱就绪 | 同上，看 `sandbox` 段 | `ready:true, owner:<uid>, tokenOk:true, tokenMode:600` |
| root 通道 | `rootrun 'id -u'` | `0` |
| 沙箱降权 | `bash` 里 `id -u` | App 的 uid |
| 根只读 | `bash` 里 `touch /etc/x` | `Read-only file system` |
| 工作区可写 | `bash` 里 `touch ~/x` | 成功 |
| 令牌被遮 | `bash` 里 `ls /root/.dsh` | 空 |
| runner 生效 | `ps -eo args | grep dsh-bwrap-root` | 看到 `node .../dsh-bwrap-root --ro-bind / ...` |

---

## 6. 排障

| 症状 | 原因 | 处置 |
| --- | --- | --- |
| `/health` 连不上 | 守护进程没起 / 端口被占 | 看 `$DATA/agent.log`；`netstat -ltnp | grep 47831` |
| `/health` 里 sandbox.ready=false | rootfs 还没解出来，socket 没建 | 打开一次 dsh-go，再点「执行」；或 `su -c 'sh /data/adb/modules/rootagent/service.sh'` |
| `sandbox.tokenOk=false` | 令牌镜像属主不是 App uid（引擎读不到） | 点「执行」重写；仍不行就确认 App 的 uid 与 `$DATA/app.conf` |
| 沙箱里全是 `Permission denied` | `dshsb-exec` 丢了执行位（zip 解压） | `su -c 'chmod 755 /data/adb/modules/rootagent/bin/*'` |
| `dshsb-client: connect: No such file` | socket 不存在或 App uid 不匹配 | 同第 2 行；确认 `--sandbox-sock` 传给了守护进程 |
| 每条命令都 `429 busy` | 并发占满（旧版本泄漏过计数） | 重启守护进程；v1.2.3+ 已修，且 `/health` 不受限 |
| 命令返回 `bad-request` | 单条命令超过 8192 字节上限 | 拆小；大文件用文件写入工具 |
| 沙箱里看不到 `/data/adb` | 设计如此（只挂 rootfs 与只读 /sdcard） | 要真 root 就把会话切成 `danger-full-access` |
| 重启后沙箱又没了 | 开机顺序：模块先跑、rootfs 后出现 | service.sh 会自动补参数并重启；仍不行就点「执行」 |

更多日志：`$DATA/agent.log`（256 KiB 轮转，每条命令记前 80 字符）。

---

## 7. 升级 / 卸载 / 换令牌

**升级**：直接刷新 zip（会覆盖 `/data/adb/modules/rootagent`），重启；proot 侧重新跑一次
`install.sh`（覆盖 `/usr/local/bin` 里的三个文件）。

**卸载**：模块页卸载 → 重启。然后清掉 proot 侧残留，否则 DSH 会一直报 runner 起不来：

```sh
rm -f /usr/local/bin/dshsb-client /usr/local/bin/dsh-bwrap-root /usr/local/bin/rootrun
```

并把 profile 里的 `runnerCommand` 删掉。

**换令牌**：

```sh
su -c 'rm -f /data/adb/rootagent/token && sh /data/adb/modules/rootagent/service.sh'
```

再点「执行」看新令牌；`rootrun` 从镜像文件读，不需要改配置。

---

## 8. 多设备 / 多 App

- 令牌**每台设备独立**，不要跨设备复制。
- 同一台手机装了多个 dsh-go 变体时，`app.conf` 的 `APP_PKG` 决定令牌镜像给谁、沙箱 socket 允许谁连；
  换目标后要重启守护进程。
- 端口冲突：`$DATA/app.conf` 写 `PORT=47831`（或改 `service.sh` 里的 `RA_PORT`）。
