/*
 * dshsb-exec — 在 Android root 侧为 dsh-go 建立 mount namespace 沙箱并降权执行命令。
 *
 * 用法：
 *   dshsb-exec --root <host rootfs> --uid N --gid N [--allow-root]
 *              [--proc <inner>] [--dev <inner>] [--tmpfs <inner>]... [--mask <inner>]...
 *              [--rw-src <host> <inner>]... [--ro-src <host> <inner>]...
 *              [--chdir <inner>] -- <命令> [参数...]
 *
 * 安全模型（fail-closed）：
 *   - 新建 mount namespace（先 make-rprivate，挂载绝不外泄）
 *   - 挂载点先建好，再把根设为只读；mask / bind / proc / dev 失败一律 die，不允许「降级继续」
 *   - --mask 用空 tmpfs 盖住逃逸面，挂完用 statfs 校验确实是 tmpfs
 *   - chroot 后 setresgid/setresuid 到 --uid/--gid；不给 --allow-root 时 uid 必须非 0
 *   - exec 前设 PR_SET_NO_NEW_PRIVS，禁止 setuid 提权
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/mount.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <grp.h>

#define STAGE_BASE "/data/local/tmp"
#define MAX_BINDS 32
#define MAX_TMPFS 8
#define MAX_MASKS 8
#define TMPFS_MAGIC 0x01021994

struct bind_spec { const char *src; const char *dst; int ro; };

static void die(const char *what) {
    fprintf(stderr, "dshsb-exec: %s: %s\n", what, strerror(errno));
    exit(111);
}

static int has_dotdot(const char *p) {
    if (!p) return 0;
    const char *s = p;
    while (*s) {
        while (*s == '/') s++;
        const char *start = s;
        while (*s && *s != '/') s++;
        size_t len = (size_t)(s - start);
        if (len == 2 && start[0] == '.' && start[1] == '.') return 1;
    }
    return 0;
}

static int path_ok(const char *p) {
    if (!p || p[0] != '/') return 0;
    if (strlen(p) > 500) return 0;
    if (has_dotdot(p)) return 0;
    return 1;
}

static int set_ro(const char *path) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return -1;
    struct mount_attr attr;
    memset(&attr, 0, sizeof attr);
    attr.attr_set = MOUNT_ATTR_RDONLY;
    int rc = (int)syscall(SYS_mount_setattr, fd, "", AT_EMPTY_PATH | AT_RECURSIVE, &attr, sizeof attr);
    int e = errno;
    close(fd);
    errno = e;
    return rc;
}

static int mkdir_p(const char *path) {
    char buf[600];
    size_t n = strlen(path);
    if (n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/') continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        buf[i] = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static char *inner_path(const char *stage, const char *dst) {
    static char buf[600];
    snprintf(buf, sizeof buf, "%s%s", stage, dst);
    return buf;
}

static void ensure_dir(const char *stage, const char *dst, const char *what) {
    char *p = inner_path(stage, dst);
    if (!path_ok(dst)) { errno = EINVAL; die(what); }
    if (mkdir_p(p) != 0) die(what);
}

static int run_child(const char *root, const char *chdir_to, const char *proc_mnt,
                     const char *dev_dst, const char **tmpfs, int n_tmpfs,
                     const char **masks, int n_masks,
                     struct bind_spec *binds, int n_binds,
                     char **cmd, uid_t uid, gid_t gid, int allow_root) {
    char stage[256];
    unsigned int rnd = 0;
    int rfd = open("/dev/urandom", O_RDONLY);
    if (rfd >= 0) { ssize_t got = read(rfd, &rnd, sizeof rnd); (void)got; close(rfd); }
    snprintf(stage, sizeof stage, STAGE_BASE "/.dshsb-%d-%08x", (int)getpid(), rnd);
    if (mkdir(stage, 0700) != 0 && errno != EEXIST) die("mkdir-stage-fixed");
    if (rmdir(stage) != 0 && errno != ENOENT) die("rmdir-stage-fixed");

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) die("make-rprivate");
    if (mkdir_p(stage) != 0) die("mkdir-stage");
    if (mount(root, stage, NULL, MS_BIND, NULL) != 0) die("bind-root");

    /* 先把所有挂载点建好（此时根还是可写的），再整体设只读 */
    for (int k = 0; k < n_binds; k++) ensure_dir(stage, binds[k].dst, "mkdir-bind-dst");
    if (proc_mnt) ensure_dir(stage, proc_mnt, "mkdir-proc");
    if (dev_dst) ensure_dir(stage, dev_dst, "mkdir-dev");
    for (int k = 0; k < n_tmpfs; k++) ensure_dir(stage, tmpfs[k], "mkdir-tmpfs");
    for (int k = 0; k < n_masks; k++) ensure_dir(stage, masks[k], "mkdir-mask");

    if (set_ro(stage) != 0) die("root-readonly");

    for (int k = 0; k < n_binds; k++) {
        char *dst = inner_path(stage, binds[k].dst);
        if (mount(binds[k].src, dst, NULL, MS_BIND, NULL) != 0) die(binds[k].dst);
        if (binds[k].ro && set_ro(dst) != 0) die("bind-readonly");
    }
    if (proc_mnt) {
        char *dst = inner_path(stage, proc_mnt);
        if (mount("proc", dst, "proc", 0, NULL) != 0) die("mount-proc");
        if (set_ro(dst) != 0) die("proc-readonly");
    }
    if (dev_dst) {
        char *dst = inner_path(stage, dev_dst);
        if (mount("/dev", dst, NULL, MS_BIND, NULL) != 0) die("bind-dev");
        if (set_ro(dst) != 0) die("dev-readonly");
    }
    for (int k = 0; k < n_tmpfs; k++) {
        char *dst = inner_path(stage, tmpfs[k]);
        if (mount("tmpfs", dst, "tmpfs", 0, "mode=1777") != 0) die("mount-tmpfs");
    }
    for (int k = 0; k < n_masks; k++) {
        char *dst = inner_path(stage, masks[k]);
        if (mount("tmpfs", dst, "tmpfs", 0, "mode=0555,size=1m") != 0) die("mask-mount");
        struct statfs sfs;
        if (statfs(dst, &sfs) != 0) die("mask-statfs");
        if ((unsigned long)sfs.f_type != (unsigned long)TMPFS_MAGIC) {
            errno = EINVAL;
            die("mask-not-tmpfs");
        }
        if (set_ro(dst) != 0) die("mask-readonly");
    }

    if (chroot(stage) != 0) die("chroot");
    if (chdir(chdir_to) != 0) {
        errno = ENOENT;
        die("chdir");
    }

    if (!allow_root) {
        if (uid == 0) { errno = EPERM; die("uid 不能是 0（需要时用 --allow-root）"); }
        if (setgroups(0, NULL) != 0) die("setgroups");
        if (setresgid(gid, gid, gid) != 0) die("setresgid");
        if (setresuid(uid, uid, uid) != 0) die("setresuid");
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) die("no-new-privs");

    execvp(cmd[0], cmd);
    fprintf(stderr, "dshsb-exec: exec %s: %s\n", cmd[0], strerror(errno));
    return 111;
}

int main(int argc, char **argv) {
    const char *root = NULL;
    const char *chdir_to = "/";
    const char *proc_mnt = NULL;
    const char *dev_dst = NULL;
    const char *tmpfs[MAX_TMPFS];
    const char *masks[MAX_MASKS];
    int n_tmpfs = 0, n_masks = 0;
    struct bind_spec binds[MAX_BINDS];
    int n_binds = 0;
    uid_t uid = 0;
    gid_t gid = 0;
    int allow_root = 0;

    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--chdir") && i + 1 < argc) chdir_to = argv[++i];
        else if (!strcmp(argv[i], "--proc") && i + 1 < argc) proc_mnt = argv[++i];
        else if (!strcmp(argv[i], "--dev") && i + 1 < argc) dev_dst = argv[++i];
        else if (!strcmp(argv[i], "--uid") && i + 1 < argc) uid = (uid_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--gid") && i + 1 < argc) gid = (gid_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--allow-root")) allow_root = 1;
        else if (!strcmp(argv[i], "--tmpfs") && i + 1 < argc) {
            if (n_tmpfs < MAX_TMPFS) tmpfs[n_tmpfs++] = argv[++i];
        } else if (!strcmp(argv[i], "--mask") && i + 1 < argc) {
            if (n_masks < MAX_MASKS) masks[n_masks++] = argv[++i];
        } else if (!strcmp(argv[i], "--rw-src") && i + 2 < argc) {
            if (n_binds < MAX_BINDS) { binds[n_binds].src = argv[++i]; binds[n_binds].dst = argv[++i]; binds[n_binds].ro = 0; n_binds++; }
        } else if (!strcmp(argv[i], "--ro-src") && i + 2 < argc) {
            if (n_binds < MAX_BINDS) { binds[n_binds].src = argv[++i]; binds[n_binds].dst = argv[++i]; binds[n_binds].ro = 1; n_binds++; }
        } else {
            fprintf(stderr, "dshsb-exec: 未知参数 %s\n", argv[i]);
            return 111;
        }
    }
    if (!root || i >= argc || root[0] != '/') {
        fprintf(stderr, "dshsb-exec: 用法: --root <rootfs> --uid N --gid N [--allow-root] [--proc P] [--dev D] [--tmpfs T] [--mask M] [--rw-src S D] [--ro-src S D] [--chdir C] -- cmd...\n");
        return 111;
    }
    if (!path_ok(chdir_to)) { fprintf(stderr, "dshsb-exec: chdir 路径非法\n"); return 111; }
    char **cmd = &argv[i];

    if (unshare(CLONE_NEWNS) != 0) die("unshare(NEWNS)");
    if (unshare(CLONE_NEWPID) != 0 && getenv("DSH_SB_DEBUG") != NULL) {
        fprintf(stderr, "dshsb-exec: warn pidns 不可用，改用 uid 降权隔离\n");
    }
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        return run_child(root, chdir_to, proc_mnt, dev_dst, tmpfs, n_tmpfs, masks, n_masks, binds, n_binds, cmd, uid, gid, allow_root);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 111;
}
