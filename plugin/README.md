# 插件：local-root-agent

dsh-go 控制台插件。作用是把 Root Agent 的状态摆到界面上，并在引擎启动时自检/修复 proot 侧的接线。

## 装了能看到什么

- 输入框上方的胶囊：**Root 代理**（点开面板）。
- 原生面板：
  - 7 项自检：托管说明 AGENTS.md、沙箱 client、bwrap 方言 shim、rootrun、沙箱 runnerCommand、
    root 令牌、守护进程 /health。
  - 按钮：**重新检查**、**修复**（补齐 client/shim/rootrun 与 AGENTS.md）、**补配置**（往 profile 写
    `runnerCommand`，需要重启引擎）、**复制令牌**、**root 自检**（跑 `rootrun 'id -u'`）。

## 安装

```sh
dsh plugin --profile web add /path/to/plugin
```

装完重启引擎（或重启 App）。插件的 bundle patch 是 `plugin/cordis.patch.yml`。

## 环境变量

| 变量 | 作用 | 默认 |
| --- | --- | --- |
| `DSH_ROOT_AGENT_PORT` | 守护进程端口 | 47831 |
| `DSH_RA_TOKEN` | 令牌文件 | `/root/.dsh/rootagent-token` |
| `DSH_RA_AGENTS` | 托管说明文件 | `/root/.dsh/AGENTS.md` |
| `DSH_RA_PROFILE` | 引擎 profile patch | `/root/.dsh/profiles/web/cordis.patch.yml` |
| `DSH_RA_BIN` | proot 侧 bin 目录 | `/usr/local/bin` |
| `DSH_RA_SRC` | 源码/产物目录（自动装 client/shim/rootrun 用） | 空 = 只检查不安装 |
| `DSH_ROOT_AGENT_NO_AUTO` | 设 1 关闭启动自检 | 空 |

## 为什么它要改 AGENTS.md

dsh-go 升级会重写 `~/.dsh/AGENTS.md`（托管文件），把「本机没有沙箱后端」这类说明写回去，
于是新会话里的模型会拒绝 `workspace-write`。插件在引擎启动 1.5 秒后检查并就地修正这几行，
并补一段「本机 root 通道与沙箱」说明。文案在 `lib/agents-pairs.txt` 与 `lib/agents-section.md`。

## 注意

- 插件只是**观测 + 修接线**，不负责起守护进程；守护进程由模块负责。
- 「补配置」写的是 `runnerCommand`，改完要重启引擎才生效。
- 插件跑在 proot 里（App uid），读不到 `/data/adb`，所以它的自检看的是 rootfs 里的镜像令牌与 `/health`。
