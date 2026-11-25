# 沙箱是怎么实现的

DSH 的 `workspace-write` 模式本身不含隔离能力，它只是「把 bwrap 参数拼好，交给一个 runner 去执行」。
在 Android 上 bubblewrap 跑不起来（内核基本不开 `CONFIG_USER_NS`，也没有 Landlock），所以本项目自己写了
一个 bwrap 方言的 runner，把同样的参数翻译成 root 侧的一次 `mount namespace + chroot + 降权` 执行。

一句话：**DSH 在 proot 里拼参数 → 我们的 runner 通过 socket 交给 root 守护进程 → 守护进程在自己的
mount namespace 里搭一个只读根 + 可写工作区 + 空 tmpfs 遮罩，chroot 后降权到 App uid 再 exec。**

---

## 1. 组件与数据流

```
DSH 引擎（proot，App uid）
  └─ sandbox runner: /usr/local/bin/dsh-bwrap-root  (Node)
       │   收到标准 bwrap 参数：--ro-bind / <rootfs> --bind <ws> <ws> ... -- <argv>
       │
       ├─(A) fd 通道：node 起 dshsb-client，把 fd 1/2/7 经 SCM_RIGHTS 交出去
       │        └─ unix socket ──▶ rootagentd（root）
       │                               └─ fork/exec dshsb-exec（root，fail-closed）
       │
       └─(B) HTTP 回退：POST /spawn（Bearer），读 5 字节帧流 o/e/x
```

| 组件 | 身份 | 职责 |
| --- | --- | --- |
| `dsh-bwrap-root` | App uid | 解析 bwrap 参数、做路径翻译、选通道、转发退出码 |
| `dshsb-client` | App uid | 把当前进程的 fd 1/2/7 和一行 JSON 头交给 socket |
| `rootagentd` | root | 校验 `SO_PEERCRED`、起 `dshsb-exec`、转发 IO、超时后杀进程组 |
| `dshsb-exec` | root | 真正建 namespace、挂载、chroot、降权、exec |

---

## 2. bwrap 参数 → dshsb-exec 参数

| DSH 给的（bwrap 方言） | 我们做的 |
| --- | --- |
| `--ro-bind / <rootfs>` | `--root <rootfs>`（整个根来自 dsh-go 的 runtime/rootfs） |
| `--bind <ws> <ws>` | `--rw-src <ws>`（工作区可写） |
| `--ro-bind /system /system` 等 | `--ro-src <路径>`（只读白名单） |
| `--dev /dev` | `--dev /dev` |
| `--proc /proc` | `--proc /proc` |
| `--tmpfs <p>` | `--tmpfs <p>`（mode=1777） |
| `--chdir <p>` | `--chdir <p>` |
| `--unshare-pid` | 忽略：内核不支持 `CLONE_NEWPID`（返回 EINVAL），隔离靠降权 |
| `-- <argv>` | `-- <argv>`（最后一定是 `--`，缺了直接 fail） |

**路径翻译**：DSH 眼里的 `/` 是 proot 的根，而在 root 侧它对应 `$DSH_GO_RUNTIME_ROOT`。所以
`/etc/x` → `<rootfs>/etc/x`；`/system`、`/storage` 这类本来就属于 Android 的路径原样保留。

**降权目标**：runner 从 `/proc/self/status` 读自己的真实 uid/gid（proot 里显示 0，真实是 App uid），
作为 `--uid/--gid` 传下去。所以沙箱里的 `id -u` 就是 App 的 uid。

**遮罩**：默认把 `/root/.dsh`（令牌目录）和 `/sdcard/Download/RootAgent`（共享目录，含 install.sh）
用空 tmpfs 盖住 —— 沙箱里既拿不到令牌，也就没有 `rootrun` 的逃逸路径。

---

## 3. dshsb-exec：fail-closed 的挂载顺序

任何一步失败都直接退出（绝不「降级成不隔离」继续跑）。顺序是有讲究的：

1. `unshare(CLONE_NEWNS)` —— 新建 mount namespace。
2. `mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL)` —— **make-rprivate**。
   少了这一步，后续挂载会传播回宿主（早期探针把宿主 `/` 搞成只读，App 直接卡死）。
3. 在宿主 tmp 下建 stage 目录，`--bind` 把 `--root` 挂上去。
4. 逐个挂载：`--rw-src`（读写 bind）、`--ro-src`（只读 bind）、`--dev`（bind /dev）、
   `--proc`（新 proc）。
5. `--tmpfs` 点挂 `tmpfs mode=1777`（私有 /tmp，退出即消失）。
6. `--mask` 点挂 `tmpfs mode=0555,size=1m`，并用 `statfs` 校验确实是 TMPFS_MAGIC（防止挂错目录还当成成功）。
7. **根递归只读**：`mount_setattr(fd, "", AT_EMPTY_PATH|AT_RECURSIVE, MOUNT_ATTR_RDONLY)`。
   老内核没有这个 syscall 时逐层 `remount,ro,bind` 兜底。工作区因为挂在后面，仍然可写。
8. `chroot(stage)` + `chdir(--chdir)`。
9. `setresgid(gid)` → `setresuid(uid)`（不带 `--allow-root` 时 uid 必须非 0）。
10. `prctl(PR_SET_NO_NEW_PRIVS, 1)` —— setuid 程序提不了权。
11. `execvp(argv[0], argv)`。

沙箱进程退出后 namespace 消失，挂载点自动回收，不会留垃圾。

---

## 4. fd 通道（为什么不用管道）

DSH 需要在命令运行期间**取消**它（control 管道），也要拿到**准确的退出码**。用普通管道转发会丢掉这些语义，
所以这里直接把描述符交给 root 侧：

- runner 继承 DSH 的 `fd 1/2/7`（stdout / stderr / control），用 `stdio: [..., 7]` 把它传给子进程。
- `dshsb-client` 连 `$DSH_SB_SOCK`，用 `SCM_RIGHTS` 把存在的 `1/2/7` 一起发过去，头部是
  `$DSH_SB_HEADER` 给的一行 JSON（`{"cmd":...,"cwd":...}`）。
- 守护进程用 `SO_PEERCRED` 核对对端 uid 必须等于 socket 属主（App uid），不等就回 `peer-uid-mismatch` 并断开。
- 命令结束时守护进程回一行 `exit N`，client 把它当作自己的退出码。
- 客户端断开（EOF/HUP）→ 守护进程发现后 **kill 整个进程组**，这就是「取消」的真实链路。

没有 `fd 7`（或没装 client）时，runner 退回 `POST /spawn`：读 5 字节帧
（1 字节类型 `o/e/x` + 4 字节大端长度），把 `o/e` 写到自己的 stdout/stderr，`x` 帧里的 int32 当退出码。
这条路径**没有 namespace 隔离**（它就是 root 的 `sh -c`），只在故障排查时用。

---

## 5. 隔离到什么程度

| 面 | 效果 |
| --- | --- |
| 文件系统 | 根只读；工作区可写；`/root/.dsh`、共享目录被空 tmpfs 遮住；`/data` 整体不可见 |
| 权限 | uid/gid 降为 App uid：写不了系统文件、发不了信号给别的 App 的进程 |
| 提权 | `no_new_privs`：setuid 二进制失效 |
| 网络 | **不隔离**（共用手机网络栈） |
| PID / IPC | **不隔离**（无 PID ns 时可看到全部进程，只是没权限动） |
| 资源 | **不限制**（没有 cgroup 配额） |

所以它是**防误操作的沙箱，不是安全容器**。想跑不受信任的代码，唯一的正确做法是别给它 root 通道，
并且接受「它仍然和 DSH 在同一个 App 沙箱里」。

---

## 6. 内核依赖与降级

| 依赖 | 缺了会怎样 | 兜底 |
| --- | --- | --- |
| `unshare(CLONE_NEWNS)` | 直接失败（EPERM） | 无，沙箱不可用 |
| `mount_setattr` | 根不能一次性递归只读 | 逐层 `remount ro bind` |
| `CLONE_NEWPID` | 不建 PID ns | 只靠 uid 隔离 |
| `/proc` 可挂载 | `ps` 等工具异常 | 挂不上就 fail（不静默） |

---

## 7. 已知坑

1. **任何在 root 侧做挂载的实验，第一行必须是 `mount --make-rprivate /`**，否则挂载会泄漏到宿主的
   mount namespace，轻则 App 报错，重则整机卡死（要重启）。
2. 单条命令有 **8192 字节**上限（守护进程 `MAX_CMD`）。runner 用 `env -i` 重放环境变量，环境特别大时
   整条命令行会越界，表现为 `bad-request` —— 这时精简环境变量或改用文件传参。
3. 沙箱里 `/sdcard` 是只读 FUSE 挂载（Android 13+），要写共享存储得用 `danger-full-access`。
4. 令牌目录被遮住 ⇒ 沙箱内 `rootrun` 即使存在也读不到令牌。这是有意的。
5. 沙箱 socket 的路径、属主 uid 由守护进程推导；rootfs 出现得比模块晚时，守护进程会自动补参重启
   （见 docs/DEPLOY.md 第 6 节）。
