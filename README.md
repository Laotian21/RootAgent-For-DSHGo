# Root Agent for dsh-go

给 dsh-go 用的 **Android 侧 root 代理 + workspace-write 沙箱后端**。

dsh-go 跑在 proot（Ubuntu）里，只有假 root：su 用不了，DSH 的 workspace-write 沙箱也需要一个能建
mount namespace 的 runner，而 Android 内核通常没有 CONFIG_USER_NS / Landlock。这个项目在 Android
侧用 KernelSU / SukiSU 模块起一个**真 root** 的常驻守护进程，补上这两件事：

1. **root 命令通道**：proot 里的 rootrun '<命令>' → 127.0.0.1:47831（Bearer 令牌）→ 真 root 执行。
2. **沙箱后端**：workspace-write 的 runner 走 dsh-bwrap-root → unix socket → root 侧 dshsb-exec
   （mount namespace + chroot + 降权到 App uid），根只读、工作区可写、/root/.dsh 被 mask。

~~~
dsh-go（proot，假 root）                      Android（真 root）
┌───────────────────────────┐               ┌──────────────────────────────────┐
│ bash 工具 ─┐               │               │ rootagentd  127.0.0.1:47831      │
│            ├─ rootrun ─────┼── HTTP ──────▶│  /exec  /spawn  /kill  /health   │
│ DSH sandbox│               │               │  └─ fork → /system/bin/sh -c     │
│  └ dsh-bwrap-root ─────────┼─ unix socket ▶│ sandbox listener                 │
│     └ dshsb-client(fd7)    │  +SCM_RIGHTS  │  └─ dshsb-exec（mount ns + chroot）│
└───────────────────────────┘               └──────────────────────────────────┘
~~~

## 特性

- **只监听 127.0.0.1**，Bearer 令牌鉴权；令牌每台设备唯一、600、放在 App 私有目录（不是共享存储）。
- **root 通道**：/exec 执行命令，按进程组超时 kill，stdout/stderr 各截断 32 KiB，并发上限 8。
- **沙箱后端**：mount namespace 内递归只读根 + 工作区 --bind 可写 + 私有 /tmp，chroot 后
  setresuid 到 App uid，PR_SET_NO_NEW_PRIVS，mask 掉令牌目录与共享下载目录。
- **fd 通道**：dshsb-client 把 DSH 的 stdout/stderr/control 管道（fd 1/2/7）经 SCM_RIGHTS 交给守护进程，
  DSH 的 control 协议（取消、退出码）照常工作；没有 fd 7 时退回 /spawn 流式接口。
- **开机自启 + 看门狗**：守护进程挂了 10 秒内拉起，卡死（/health 连续 3 次异常）也重启。
- **dsh-go 插件**（可选）：状态条 + 原生面板，7 项自检、一键修复、复制令牌、root 自检。
- **包名自动识别**：不写死任何 App 包名，按 runtime/rootfs 目录或 app.conf 里的 APP_PKG 定位。

## 要求

| 项 | 要求 |
| --- | --- |
| 系统 | Android 10+，内核支持 unshare(CLONE_NEWNS) 与 mount_setattr（Android 12+ 普遍支持） |
| root | KernelSU 或 SukiSU（模块方式）；Magisk 理论可用但未测试 |
| 客户端 | dsh-go App（包名任意，脚本会自动识别） |
| 引擎 | DSH >=0.2.1-rc.2 <0.3.0（用到 runnerCommand 配置） |

## 快速开始

1. 下载 Release 里的 RootAgent-vX.Y.Z.zip（或自己 sh src/build.sh 打包）。
2. KernelSU / SukiSU → 模块 → 从本地安装 → **重启手机**。
3. 模块卡片点 **执行**：打印本机令牌，并把 proot 侧要用的文件同步到 /sdcard/Download/RootAgent/proot/。
4. 在 dsh-go 终端里装 proot 侧工具：sh /sdcard/Download/RootAgent/proot/install.sh。
5. 引擎配置里加 runner（见 [docs/DEPLOY.md](docs/DEPLOY.md)），重启引擎；bash 里 id -u 应等于 App 的 uid，
   rootrun 'id -u' 应输出 0。

详细步骤、验证与排障：[docs/DEPLOY.md](docs/DEPLOY.md)。沙箱实现细节：[docs/SANDBOX.md](docs/SANDBOX.md)。
HTTP / socket 协议：[docs/API.md](docs/API.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| src/rootagentd.c | 守护进程（HTTP 服务、命令执行、沙箱 listener、日志） |
| src/dshsb-exec.c | root 侧沙箱执行器（mount namespace + chroot + 降权，fail-closed） |
| src/dshsb-client.c | proot 侧 fd 转发小工具（SCM_RIGHTS） |
| src/dsh-bwrap-root | DSH 的 bwrap 方言 runner（Node，翻译参数 + 选通道） |
| src/rootrun | proot 里的 root 命令客户端 |
| src/build.sh | 编译三个二进制 + 复制脚本 + 打包 zip |
| module/ | KernelSU 模块包内容（module.prop、service.sh、action.sh、bin/…） |
| plugin/ | dsh-go 原生插件 local-root-agent（状态条、面板、自检） |
| docs/ | 部署、沙箱实现、API、变更记录 |

## 安全

- 令牌等同 root 权限：不要截图、不要提交到仓库、不要发给别人。泄漏了就删
  /data/adb/rootagent/token 再点一次「执行」换新的。
- 守护进程只 bind 127.0.0.1，外部网络不可达；沙箱 socket 只允许 App uid 连接（SO_PEERCRED）。
- 沙箱里看不到令牌目录和 /sdcard/Download/RootAgent，所以沙箱内没有 rootrun 逃逸路径；
  需要真 root 时必须把会话升级到 danger-full-access（会弹审批）。
- 沙箱靠 **uid 降权** 隔离，不是内核级容器：同一 App 内的进程仍可互相影响，别在里面跑不受信任的提权代码。

## 已知限制

- 内核不支持 CLONE_NEWPID 时不建 PID namespace，隔离靠 uid 降权（无法给系统进程发信号）。
- /data 在沙箱内不可见（只能看到 rootfs 与 /sdcard 只读）。
- Android 13+ 的 /sdcard 走 FUSE，沙箱里是只读挂载；要写共享存储得用 danger-full-access。

## 许可证

[MIT](LICENSE)。仅供学习与自用，刷机风险自负。
