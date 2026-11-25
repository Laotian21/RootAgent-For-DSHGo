# 变更记录

## 1.3.0 — 2026-11-25

- 错误码更明确：`cmd` 超长返回 400 `cmd_too_long`（带 `max`），`cwd` 超长返回 `bad_cwd`。
- 新增 `ROOTAGENT_RUNDIR` 环境变量，便于在非 Android 环境里联调 `/spawn` + `/kill`。

首个公开发布（此前的版本都是内部自用）。

- **不再硬编码设备信息**：目标 App 包名改为自动识别（扫 `runtime/rootfs`）或读 `app.conf` 的 `APP_PKG`；
  共享目录改成 `/sdcard/Download/RootAgent`。
- 新增 `src/build.sh`（编译 + 打包 zip）、`module/bin/install-proot.sh`（proot 侧一键安装）。
- 模块「执行」现在会把 proot 侧要用的三个文件同步到 `/sdcard/Download/RootAgent/proot/`。
- 插件源码目录改为可选（`DSH_RA_SRC`）：没配就只做检查，不尝试从源码目录复制。
- 新增 `docs/DEPLOY.md`、`docs/SANDBOX.md`、`docs/API.md`。
- 守护进程版本号与 `module.prop` 对齐。

## 1.2.5

- 修复开机早期读不到 App uid 导致令牌被 `chown root:root 600`：引擎读不到令牌，root 通道整体失效。
  改为守护进程自建目录、多路径推导属主、自己在 rootfs 里镜像令牌（原子写 + 15 秒后复查）。
- 沙箱 socket bind 改为无限重试；bind 成功后才启用 `SO_PEERCRED` 校验。
- 新增单实例保护（启动前探测已有实例）。
- `/health` 实时 `stat` 令牌文件，直接给出 `tokenOk/tokenUid/tokenMode`。

## 1.2.4

- 修复「重启手机后沙箱不可用」：旧版只在 rootfs 已就绪时才给守护进程传 `--sandbox-sock`
  → 改为无条件传参，由守护进程自己重试。
- zip 解压会丢执行位（`dshsb-exec` 变 0644）→ 启动时自愈 `chmod 755`。

## 1.2.3

- 修复并发计数泄漏：旧版在 `fork` 之后才 `g_active++`，子进程抢跑会让信号处理器里的 `--` 被吞掉，
  长时间运行后计数只增不减，最后所有请求都 429。改为 fork 前占坑，上限 8，`/health` 不受限，
  并加看门狗轮询 `/health`。

## 1.2.0 及更早

- 内部版本：root 命令通道与自定义沙箱后端首次跑通（HTTP `/exec` + unix socket fd 通道）。
