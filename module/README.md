# 模块包说明

这是刷进 KernelSU / SukiSU 的那部分（对应 `module/` 目录）。它负责在开机时以真 root 起守护进程，
并在 rootfs 就绪后把沙箱 socket 交给它。

## 文件

| 文件 | 作用 |
| --- | --- |
| `module.prop` | 模块信息（id `rootagent`、版本） |
| `customize.sh` | 安装时执行：建目录、修权限 |
| `service.sh` | 开机执行：令牌 → 镜像 → 起守护进程 → 看门狗 |
| `action.sh` | 模块卡片点「执行」：打印令牌、同步 proot 文件、沙箱自检 |
| `uninstall.sh` | 卸载时保留令牌/日志，写 `UNINSTALLED.txt` 提示 |
| `lib.sh` | 公共函数（令牌生成、包名/uid 探测、令牌镜像） |
| `bin/rootagentd` | 守护进程（静态 AArch64） |
| `bin/dshsb-exec` | root 侧沙箱执行器（静态 AArch64） |
| `bin/dshsb-client` | proot 侧 fd 转发工具（静态 AArch64） |
| `bin/dsh-bwrap-root` | DSH 的 bwrap 方言 runner（Node） |
| `bin/rootrun` | proot 里的 root 命令客户端（Node） |
| `bin/install-proot.sh` | 在 proot 里把上面三个文件装进 `/usr/local/bin` |

## 安装

1. `sh src/build.sh` 生成 `dist/RootAgent-<版本>.zip`（或直接用 Release 里的）。
2. KernelSU / SukiSU → 模块 → 从本地安装 → 选 zip → **重启**。

> zip 里没有 `META-INF`：KernelSU / SukiSU 的管理器直接支持这种「带 `customize.sh` + `module.prop`」的包，
> 不需要 recovery 脚本。

安装位置：`/data/adb/modules/rootagent/`；数据目录：`/data/adb/rootagent/`
（`token`、`rootagentd.pid`、`agent.log`、`app.conf`、`run/`）。

## 配置

`/data/adb/rootagent/app.conf`（没有就自己建，一行一个 `KEY=VALUE`）：

| 键 | 作用 | 默认 |
| --- | --- | --- |
| `APP_PKG` | 目标 dsh-go 包名（决定令牌镜像给谁、socket 允许谁连） | 自动识别 |
| `PORT` | 守护进程端口 | 47831 |

环境变量（在 `service.sh` 里用，调试时可覆盖）：`RA_DATA`、`RA_SB`、`RA_PORT`、`RA_PKG`、`RA_CONF`。

改完 `APP_PKG` 后要重启守护进程才生效：

```sh
su -c 'sh /data/adb/modules/rootagent/service.sh'
```

## 日志与排障

```sh
su -c 'tail -n 50 /data/adb/rootagent/agent.log'   # 256 KiB 轮转，记命令前 80 字符
su -c 'cat /data/adb/rootagent/rootagentd.pid'
curl -s http://127.0.0.1:47831/health              # 不用令牌
```

## 卸载

管理器里卸载 → 重启。模块目录会被删掉，但 `/data/adb/rootagent` 会留下（含令牌），
需要的话手动 `rm -rf`。proot 侧还要清掉 `/usr/local/bin` 里的三个文件（见 docs/DEPLOY.md 第 7 节）。
