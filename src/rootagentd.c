/*
 * rootagentd.c — Root Agent local root command proxy for Android (arm64, static)
 *
 * 仅监听 127.0.0.1，Bearer 令牌鉴权，fork-per-connection，并发上限 8。
 * 端点：GET /health（免鉴权）、POST /exec、GET /log（鉴权）。
 * 由 KernelSU/SukiSU 模块以 root 身份启动，dsh-go（proot）作为客户端连接。
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME "root-agent"
#define APP_VERSION "1.3.0"
#define MAX_HEADER 8192
#define MAX_BODY 16384
#define MAX_CMD 8192
#define MAX_OUT 32768
#define MAX_CONCURRENCY 8
#define DEFAULT_TIMEOUT_MS 30000
#define MIN_TIMEOUT_MS 100
#define MAX_TIMEOUT_MS 120000
#define LOG_MAX 262144
#define LOG_TAIL 16384
#define SH_PATH "/system/bin/sh"
#define DEFAULT_PATH "/data/adb/ksu/bin:/data/adb/magisk:/system/bin:/system/xbin:/vendor/bin:/product/bin:/system_ext/bin"

static char g_path_env[512] = "PATH=" DEFAULT_PATH;   /* ROOTAGENT_PATH 可覆盖（仅测试：proot 里没有 /system/bin） */
static char g_sh_path[256] = SH_PATH;   /* ROOTAGENT_SHELL 可覆盖（仅测试：proot 里没有 /system/bin/sh） */
static char g_token[256];
static size_t g_token_len = 0;
static char g_logpath[512] = "/data/adb/rootagent/agent.log";
static char g_tokenfile[512] = "/data/adb/rootagent/token";
static size_t g_requests = 0;
static long long g_start_ms = 0;
static volatile sig_atomic_t g_active = 0;
static char g_pidfile[512] = "/data/adb/rootagent/rootagentd.pid";
static pid_t g_self_pid = 0;
static char g_sockpath[512] = "";
static uid_t g_sock_uid = 0;
static char g_rootfs[512] = "";
static uid_t g_peer_uid = 0;        /* socket 实际属主：SO_PEERCRED 用它校验 */
static int g_sock_ready = 0;
static uid_t g_sock_owner = 0;
static int g_token_mirror_ok = 0;
static uid_t g_token_mirror_uid = 0;
static int g_token_mirror_mode = 0;

/* 沙箱状态在 fork 出来的子进程里维护，父进程的 /health 要能看到，
 * 所以用一块 MAP_SHARED 匿名内存做父子共享。 */
typedef struct {
    volatile int ready;
    volatile int owner;
    volatile int token_ok;
    volatile int token_uid;
    volatile int token_mode;
} sb_state_t;
static sb_state_t *g_sb = NULL;

static void sb_publish(void) {
    if (!g_sb) return;
    g_sb->ready = g_sock_ready;
    g_sb->owner = (int)g_sock_owner;
    g_sb->token_ok = g_token_mirror_ok;
    g_sb->token_uid = (int)g_token_mirror_uid;
    g_sb->token_mode = g_token_mirror_mode;
}

typedef struct { char *buf; size_t len; int truncated; } cap_t;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static void rotate_log(void) {
    struct stat st;
    if (stat(g_logpath, &st) == 0 && st.st_size > LOG_MAX) {
        char old[600];
        snprintf(old, sizeof old, "%s.1", g_logpath);
        rename(g_logpath, old);
    }
}

static void logmsg(const char *fmt, ...) {
    char line[1500];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02dT%02d:%02d:%02dZ ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
    if (n < 0) return;
    if ((size_t)n > sizeof line - 3) n = (int)(sizeof line - 3);
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof line - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (m < 0) return;
    size_t len = strlen(line);
    for (size_t k = 0; k < len; k++) {
        unsigned char ch = (unsigned char)line[k];
        if (ch < 0x20 || ch == 0x7f) line[k] = ' ';
    }
    line[len++] = '\n';
    line[len] = '\0';
    char lockpath[600];
    snprintf(lockpath, sizeof lockpath, "%s.lock", g_logpath);
    int lfd = open(lockpath, O_WRONLY | O_CREAT, 0600);
    if (lfd >= 0 && flock(lfd, LOCK_EX) != 0) { close(lfd); lfd = -1; }
    rotate_log();
    int fd = open(g_logpath, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        (void)write_all(fd, line, len);
        close(fd);
    }
    if (lfd >= 0) { (void)flock(lfd, LOCK_UN); close(lfd); }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t json_escape(const char *in, size_t inlen, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; i < inlen; i++) {
        unsigned char c = (unsigned char)in[i];
        char tmp[8];
        size_t tl;
        if (c == '"' || c == '\\') { tmp[0] = '\\'; tmp[1] = (char)c; tl = 2; }
        else if (c == '\n') { tmp[0] = '\\'; tmp[1] = 'n'; tl = 2; }
        else if (c == '\r') { tmp[0] = '\\'; tmp[1] = 'r'; tl = 2; }
        else if (c == '\t') { tmp[0] = '\\'; tmp[1] = 't'; tl = 2; }
        else if (c < 0x20) { snprintf(tmp, sizeof tmp, "\\u%04x", c); tl = 6; }
        else { tmp[0] = (char)c; tl = 1; }
        if (o + tl + 1 >= outsz) break;
        memcpy(out + o, tmp, tl);
        o += tl;
    }
    out[o] = '\0';
    return o;
}

static const char *json_find_key(const char *js, size_t len, const char *key) {
    size_t klen = strlen(key);
    for (size_t i = 0; i < len; i++) {
        if (js[i] != '"') continue;
        size_t start = i + 1;
        size_t j = start;
        while (j < len) {
            if (js[j] == '\\') { j += 2; continue; }
            if (js[j] == '"') break;
            j++;
        }
        size_t slen = j - start;
        if (slen == klen && memcmp(js + start, key, klen) == 0) {
            size_t q = j + 1;
            while (q < len && (js[q] == ' ' || js[q] == '\t' || js[q] == '\n' || js[q] == '\r')) q++;
            if (q < len && js[q] == ':') return js + j + 1;
        }
        i = j;
    }
    return NULL;
}

static int json_get_string(const char *js, size_t jslen, const char *key, char *out, size_t outsz) {
    const char *p = json_find_key(js, jslen, key);
    if (!p) return 0;
    while (p < js + jslen && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= js + jslen || *p != ':') return 0;
    p++;
    while (p < js + jslen && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= js + jslen || *p != '"') return 0;
    p++;
    size_t o = 0;
    while (p < js + jslen) {
        unsigned char c = (unsigned char)*p++;
        if (c == '"') break;
        if (c == '\\') {
            if (p >= js + jslen) return 0;
            unsigned char e = (unsigned char)*p++;
            switch (e) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (p + 4 > js + jslen) return 0;
                    unsigned int cp = 0;
                    for (int k = 0; k < 4; k++) {
                        int h = hexval((unsigned char)p[k]);
                        if (h < 0) return 0;
                        cp = cp * 16u + (unsigned int)h;
                    }
                    p += 4;
                    if (cp == 0) { }
                    else if (cp < 0x80) { if (o + 2 >= outsz) return -1; out[o++] = (char)cp; }
                    else if (cp < 0x800) {
                        if (o + 3 >= outsz) return -1;
                        out[o++] = (char)(0xC0 | (cp >> 6));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (o + 4 >= outsz) return -1;
                        out[o++] = (char)(0xE0 | (cp >> 12));
                        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        out[o++] = (char)(0x80 | (cp & 0x3F));
                    }
                    continue;
                }
                default: return 0;
            }
        }
        if (o + 2 >= outsz) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return 1;
}

static int json_get_int(const char *js, size_t jslen, const char *key, long *out) {
    const char *p = json_find_key(js, jslen, key);
    if (!p) return 0;
    while (p < js + jslen && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= js + jslen || *p != ':') return 0;
    p++;
    while (p < js + jslen && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= js + jslen) return 0;
    char num[32];
    size_t n = 0;
    while (p < js + jslen && (isdigit((unsigned char)*p) || *p == '-') && n + 1 < sizeof num) {
        num[n++] = *p++;
    }
    if (n == 0) return 0;
    num[n] = '\0';
    *out = strtol(num, NULL, 10);
    return 1;
}

static void http_send(int fd, int code, const char *reason, const char *body) {
    char hdr[300];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, reason, strlen(body));
    if (n > 0) (void)write_all(fd, hdr, (size_t)n);
    (void)write_all(fd, body, strlen(body));
}

static int token_ok(const char *given) {
    if (!given || g_token_len == 0) return 0;
    if (strlen(given) != g_token_len) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < g_token_len; i++) {
        diff |= (unsigned char)(given[i] ^ g_token[i]);
    }
    return diff == 0;
}

static int run_cmd(const char *cmd, const char *cwd, int timeout_ms,
                   cap_t *o, cap_t *e, int *code_out, long long *ms_out) {
    int pout[2], perr[2];
    if (pipe(pout) != 0) return -1;
    if (pipe(perr) != 0) { close(pout[0]); close(pout[1]); return -1; }
    long long t0 = now_ms();
    pid_t pid = fork();
    if (pid < 0) {
        close(pout[0]); close(pout[1]); close(perr[0]); close(perr[1]);
        return -1;
    }
    if (pid == 0) {
        (void)setsid();
        close(pout[0]); close(perr[0]);
        (void)dup2(pout[1], 1);
        (void)dup2(perr[1], 2);
        if (pout[1] > 2) close(pout[1]);
        if (perr[1] > 2) close(perr[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            (void)dup2(devnull, 0);
            if (devnull > 2) close(devnull);
        }
        if (cwd && cwd[0] && chdir(cwd) != 0) { /* 忽略，命令自己会报错 */ }
        char *argv[] = { (char *)"sh", (char *)"-c", (char *)cmd, NULL };
        char *envp[] = {
            g_path_env,
            (char *)"HOME=/data/local/tmp",
            (char *)"TERM=dumb",
            NULL
        };
        execve(g_sh_path, argv, envp);
        {
            char eb[128];
            int en = snprintf(eb, sizeof eb, "exec-failed: %s\n", strerror(errno));
            if (en > 0 && write(2, eb, (size_t)en) < 0) { /* 尽力而为，失败就算了 */ }
        }
        _exit(127);
    }
    close(pout[1]);
    close(perr[1]);
    (void)fcntl(pout[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(perr[0], F_SETFL, O_NONBLOCK);

    struct pollfd pfd[2];
    pfd[0].fd = pout[0]; pfd[0].events = POLLIN; pfd[0].revents = 0;
    pfd[1].fd = perr[0]; pfd[1].events = POLLIN; pfd[1].revents = 0;
    int open_c = 2;
    int timedout = 0;

    while (open_c > 0) {
        long long elapsed = now_ms() - t0;
        long long remain = (long long)timeout_ms - elapsed;
        if (remain <= 0) { timedout = 1; break; }
        int pr = poll(pfd, 2, (int)(remain > 2000 ? 2000 : remain));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            if (now_ms() - t0 >= timeout_ms) { timedout = 1; break; }
            continue;
        }
        for (int i = 0; i < 2; i++) {
            if (pfd[i].fd < 0) continue;
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            pfd[i].revents = 0;
            char tmp[4096];
            ssize_t n = read(pfd[i].fd, tmp, sizeof tmp);
            if (n > 0) {
                cap_t *c = (i == 0) ? o : e;
                size_t take = (size_t)n;
                if (c->len + take > MAX_OUT) {
                    take = MAX_OUT - c->len;
                    c->truncated = 1;
                }
                if (take > 0) {
                    memcpy(c->buf + c->len, tmp, take);
                    c->len += take;
                }
            } else if (n == 0) {
                close(pfd[i].fd);
                pfd[i].fd = -1;
                open_c--;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(pfd[i].fd);
                pfd[i].fd = -1;
                open_c--;
            }
        }
    }

    if (timedout) {
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);
    }
    if (pfd[0].fd >= 0) close(pfd[0].fd);
    if (pfd[1].fd >= 0) close(pfd[1].fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    *ms_out = now_ms() - t0;
    if (timedout) {
        *code_out = 124;
        return 1;
    }
    if (WIFEXITED(status)) *code_out = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) *code_out = 128 + WTERMSIG(status);
    else *code_out = -1;
    return 0;
}

static int send_exec_response(int fd, const char *cmd, const char *cwd, long timeout_ms) {
    cap_t o, e;
    o.buf = (char *)malloc(MAX_OUT + 1);
    e.buf = (char *)malloc(MAX_OUT + 1);
    if (!o.buf || !e.buf) {
        free(o.buf); free(e.buf);
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return 500;
    }
    o.len = 0; o.truncated = 0;
    e.len = 0; e.truncated = 0;
    o.buf[0] = '\0'; e.buf[0] = '\0';

    int code = -1;
    long long ms = 0;
    int timedout = run_cmd(cmd, cwd, (int)timeout_ms, &o, &e, &code, &ms);
    if (timedout < 0) {
        free(o.buf); free(e.buf);
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"spawn_failed\"}");
        return 500;
    }

    size_t need = 512 + o.len * 6 + e.len * 6;
    char *body = (char *)malloc(need);
    if (!body) {
        free(o.buf); free(e.buf);
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return 500;
    }
    size_t off = 0;
    int n = snprintf(body, need,
                     "{\"ok\":true,\"code\":%d,\"ms\":%lld,\"timedOut\":%s,"
                     "\"stdoutTruncated\":%s,\"stderrTruncated\":%s,\"stdout\":\"",
                     code, ms, timedout ? "true" : "false",
                     o.truncated ? "true" : "false", e.truncated ? "true" : "false");
    if (n > 0) off = (size_t)n;
    off += json_escape(o.buf, o.len, body + off, need - off - 32);
    body[off++] = '"';
    body[off++] = ',';
    body[off++] = '"';
    body[off] = '\0';
    memcpy(body + off, "stderr\":\"", 9);
    off += 9;
    off += json_escape(e.buf, e.len, body + off, need - off - 8);
    memcpy(body + off, "\"}", 2);
    off += 2;
    body[off] = '\0';

    http_send(fd, 200, "OK", body);
    logmsg("exec %s cmd=%.80s code=%d ms=%lld%s", cwd ? "cwd" : "-", cmd, code, ms, timedout ? " TIMEOUT" : "");
    free(body);
    free(o.buf);
    free(e.buf);
    return 200;
}

static int send_log_response(int fd) {
    int lf = open(g_logpath, O_RDONLY);
    if (lf < 0) {
        http_send(fd, 200, "OK", "{\"ok\":true,\"log\":\"\"}");
        return 200;
    }
    off_t size = lseek(lf, 0, SEEK_END);
    off_t start = size > LOG_TAIL ? size - LOG_TAIL : 0;
    (void)lseek(lf, start, SEEK_SET);
    size_t want = (size_t)(size - start);
    char *raw = (char *)malloc(want + 1);
    if (!raw) {
        close(lf);
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return 500;
    }
    size_t got = 0;
    while (got < want) {
        ssize_t n = read(lf, raw + got, want - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(lf);
    size_t need = 64 + got * 6;
    char *body = (char *)malloc(need);
    if (!body) {
        free(raw);
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return 500;
    }
    size_t off = 0;
    int n = snprintf(body, need, "{\"ok\":true,\"log\":\"");
    if (n > 0) off = (size_t)n;
    off += json_escape(raw, got, body + off, need - off - 4);
    memcpy(body + off, "\"}", 2);
    off += 2;
    body[off] = '\0';
    http_send(fd, 200, "OK", body);
    free(body);
    free(raw);
    return 200;
}


/* ---------- 流式任务：/spawn、/kill ---------- */

static void frame_put(int fd, char type, const char *data, size_t len) {
    unsigned char hdr[5];
    hdr[0] = (unsigned char)type;
    hdr[1] = (unsigned char)((len >> 24) & 0xFF);
    hdr[2] = (unsigned char)((len >> 16) & 0xFF);
    hdr[3] = (unsigned char)((len >> 8) & 0xFF);
    hdr[4] = (unsigned char)(len & 0xFF);
    if (write_all(fd, hdr, 5) != 0) return;
    if (len > 0) (void)write_all(fd, data, len);
}

static unsigned long long proc_starttime(pid_t pid) {
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp) return 0;
    char *q = rp + 2;
    unsigned long long val = 0;
    for (int idx = 3; idx <= 22 && *q; idx++) {
        while (*q == ' ') q++;
        val = strtoull(q, &q, 10);
        if (idx == 22) return val;
    }
    return 0;
}

static void task_id(char *out, size_t n) {
    static const char *hex = "0123456789abcdef";
    unsigned char raw[16];
    size_t got = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (got < sizeof raw) {
            ssize_t r = read(fd, raw + got, sizeof raw - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    if (n > 16) n = 16;
    for (size_t i = 0; i < n; i++) {
        unsigned char v = (i < got) ? raw[i] : (unsigned char)(i * 37 + 11);
        out[i] = hex[v & 0x0F];
    }
    out[n] = '\0';
}

static char *read_http_body(int fd, const char *req, size_t rlen, int header_end, long content_length) {
    if (content_length <= 0 || content_length > MAX_BODY) return NULL;
    size_t body_start = (size_t)header_end + 4;
    size_t have = rlen > body_start ? rlen - body_start : 0;
    if (have > (size_t)content_length) have = (size_t)content_length;
    char *body = (char *)malloc((size_t)content_length + 1);
    if (!body) return NULL;
    memcpy(body, req + body_start, have);
    size_t got = have;
    while (got < (size_t)content_length) {
        ssize_t n = recv(fd, body + got, (size_t)content_length - got, 0);
        if (n <= 0) break;
        got += (size_t)n;
    }
    body[got] = '\0';
    if (got < (size_t)content_length) { free(body); return NULL; }
    return body;
}

/* /spawn 在这里记 pidfile，/kill 靠它找进程组；本地测试可用 ROOTAGENT_RUNDIR 覆盖。 */
static void run_dir(char *out, size_t n) {
    const char *d = getenv("ROOTAGENT_RUNDIR");
    if (d && d[0]) snprintf(out, n, "%s", d);
    else snprintf(out, n, "/data/adb/rootagent/run");
}

static int handle_spawn(int fd, const char *body, size_t blen) {
    char cmd[MAX_CMD + 1];
    cmd[0] = '\0';
    int cmdrc = json_get_string(body, blen, "cmd", cmd, sizeof cmd);
    if (cmdrc < 0) {
        http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"cmd_too_long\",\"max\":8192}");
        return 400;
    }
    if (cmdrc == 0 || cmd[0] == '\0') {
        http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"missing_cmd\"}");
        return 400;
    }
    char cwd[1024] = "/";
    int cwdrc = json_get_string(body, blen, "cwd", cwd, sizeof cwd);
    if (cwdrc <= 0) {
        if (cwdrc < 0 || strstr(body, "\"cwd\"") != NULL) {
            http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad_cwd\"}");
            return 400;
        }
        cwd[0] = '\0';
    }
    long tmo = 0;
    if (!json_get_int(body, blen, "timeoutMs", &tmo) || tmo < 0) tmo = 0;

    char id[17];
    task_id(id, 16);
    char rundir[512];
    run_dir(rundir, sizeof rundir);
    (void)mkdir(rundir, 0700);
    char pidpath[600];
    snprintf(pidpath, sizeof pidpath, "%s/%s.pid", rundir, id);

    int po[2], pe[2];
    if (pipe(po) != 0) {
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"pipe_failed\"}");
        return 500;
    }
    if (pipe(pe) != 0) {
        close(po[0]); close(po[1]);
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"pipe_failed\"}");
        return 500;
    }
    long long t0 = now_ms();
    pid_t pid = fork();
    if (pid == 0) {
        (void)setsid();
        close(po[0]); close(pe[0]);
        (void)dup2(po[1], 1);
        (void)dup2(pe[1], 2);
        if (po[1] > 2) close(po[1]);
        if (pe[1] > 2) close(pe[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { (void)dup2(devnull, 0); if (devnull > 2) close(devnull); }
        if (cwd[0] && chdir(cwd) != 0) { /* 忽略 */ }
        char *argv[] = { (char *)"sh", (char *)"-c", cmd, NULL };
        char *envp[] = { g_path_env, (char *)"HOME=/data/local/tmp", (char *)"TERM=dumb", NULL };
        execve(g_sh_path, argv, envp);
        {
            char eb[128];
            int en = snprintf(eb, sizeof eb, "exec-failed: %s\n", strerror(errno));
            if (en > 0 && write(2, eb, (size_t)en) < 0) { /* 尽力而为，失败就算了 */ }
        }
        _exit(127);
    }
    close(po[1]);
    close(pe[1]);
    if (pid < 0) {
        close(po[0]); close(pe[0]);
        http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"fork_failed\"}");
        return 500;
    }
    int lf = open(pidpath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (lf >= 0) {
        char b[64];
        int n = snprintf(b, sizeof b, "%d %llu\n", (int)pid, proc_starttime(pid));
        if (n > 0) (void)write_all(lf, b, (size_t)n);
        close(lf);
    }

    char hdr[400];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/x-dsh-stream\r\nConnection: close\r\n"
        "Cache-Control: no-store\r\nX-Task-Id: %s\r\nX-Task-Pid: %d\r\n\r\n", id, (int)pid);
    if (hn <= 0 || write_all(fd, hdr, (size_t)hn) != 0) {
        (void)kill(-pid, SIGKILL);
        close(po[0]); close(pe[0]);
        return 500;
    }

    (void)fcntl(po[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(pe[0], F_SETFL, O_NONBLOCK);
    struct pollfd pfd[2];
    pfd[0].fd = po[0]; pfd[0].events = POLLIN; pfd[0].revents = 0;
    pfd[1].fd = pe[0]; pfd[1].events = POLLIN; pfd[1].revents = 0;
    int open_c = 2;
    int client_gone = 0;
    while (open_c > 0) {
        long long elapsed = now_ms() - t0;
        long long remain = tmo > 0 ? (long long)tmo - elapsed : 60000;
        if (tmo > 0 && remain <= 0) { (void)kill(-pid, SIGKILL); break; }
        if (remain > 30000) remain = 30000;
        int pr = poll(pfd, 2, (int)remain);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { if (tmo > 0 && now_ms() - t0 >= tmo) { (void)kill(-pid, SIGKILL); break; } continue; }
        for (int i = 0; i < 2; i++) {
            if (pfd[i].fd < 0) continue;
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            pfd[i].revents = 0;
            char tmp[4096];
            ssize_t n = read(pfd[i].fd, tmp, sizeof tmp);
            if (n > 0) {
                frame_put(fd, i == 0 ? 'o' : 'e', tmp, (size_t)n);
            } else if (n == 0) {
                close(pfd[i].fd); pfd[i].fd = -1; open_c--;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(pfd[i].fd); pfd[i].fd = -1; open_c--;
            }
        }
        (void)client_gone;
    }
    if (pfd[0].fd >= 0) close(pfd[0].fd);
    if (pfd[1].fd >= 0) close(pfd[1].fd);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    frame_put(fd, 'x', (const char *)&code, sizeof code);
    unlink(pidpath);
    logmsg("spawn id=%s code=%d ms=%lld cmd=%.60s", id, code, now_ms() - t0, cmd);
    return 200;
}

static int handle_kill(int fd, const char *body, size_t blen) {
    char id[64];
    if (!json_get_string(body, blen, "id", id, sizeof id) || id[0] == '\0') {
        http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"missing_id\"}");
        return 400;
    }
    for (size_t i = 0; id[i]; i++) {
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) {
            http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad_id\"}");
            return 400;
        }
    }
    char rundir[512];
    run_dir(rundir, sizeof rundir);
    char pidpath[600];
    snprintf(pidpath, sizeof pidpath, "%s/%s.pid", rundir, id);
    long pid = 0;
    unsigned long long start = 0;
    FILE *f = fopen(pidpath, "r");
    if (!f) { http_send(fd, 200, "OK", "{\"ok\":true,\"note\":\"not_running\"}"); return 200; }
    if (fscanf(f, "%ld %llu", &pid, &start) != 2) { pid = 0; }
    fclose(f);
    if (pid > 1 && start != 0 && proc_starttime((pid_t)pid) != start) {
        unlink(pidpath);
        http_send(fd, 200, "OK", "{\"ok\":true,\"note\":\"stale_pidfile\"}");
        return 200;
    }
    if (pid > 1) {
        (void)kill((pid_t)-pid, SIGTERM);
        usleep(150000);
        (void)kill((pid_t)-pid, SIGKILL);
        (void)kill((pid_t)pid, SIGKILL);
    }
    http_send(fd, 200, "OK", "{\"ok\":true}");
    return 200;
}



/* ---------- 沙箱 socket：在 unix socket 上收 fd 并派生带特权子进程 ---------- */

#define SB_MAXFDS 4

static int read_fds_header(int fd, char *hdr, size_t hdrsz, int *fds, int maxfds, size_t *used) {
    struct msghdr msg;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(int) * SB_MAXFDS)];
    size_t fill = 0;
    *used = 0;
    while (fill < hdrsz - 1) {
        memset(&msg, 0, sizeof msg);
        iov.iov_base = hdr + fill;
        iov.iov_len = hdrsz - 1 - fill;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof cbuf;
        ssize_t n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        if (n <= 0) { hdr[fill] = '\0'; return -1; }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int cnt = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                int *got = (int *)CMSG_DATA(c);
                for (int i = 0; i < cnt; i++) {
                    if (*used < (size_t)maxfds) fds[(*used)++] = got[i];
                    else close(got[i]);
                }
            }
        }
        fill += (size_t)n;
        hdr[fill] = '\0';
        if (strchr(hdr, '\n') != NULL) break;
    }
    return (int)fill;
}

static void sandbox_conn(int fd) {
    struct timeval stv;
    stv.tv_sec = 30;
    stv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &stv, sizeof stv);
    struct ucred cred;
    socklen_t cl = sizeof cred;
    if (g_peer_uid != 0 && getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) == 0) {
        if (cred.uid != g_peer_uid) {
            const char *msg = "peer-uid-mismatch\n";
            (void)write_all(fd, msg, strlen(msg));
            return;
        }
    }
    static char hdr[262144];
    int fds[SB_MAXFDS];
    size_t nfds = 0;
    if (read_fds_header(fd, hdr, sizeof hdr, fds, SB_MAXFDS, &nfds) <= 0) return;

    char cmd[MAX_CMD + 1];
    cmd[0] = '\0';
    char cwd[256] = "/";
    if (!json_get_string(hdr, strlen(hdr), "cmd", cmd, sizeof cmd) || cmd[0] == '\0') {
        const char *msg = "bad-request\n";
        (void)write_all(fd, msg, strlen(msg));
        for (size_t i = 0; i < nfds; i++) close(fds[i]);
        return;
    }
    if (!json_get_string(hdr, strlen(hdr), "cwd", cwd, sizeof cwd)) cwd[0] = '\0';

    int targets[SB_MAXFDS] = { 1, 2, 7, -1 };
    if (nfds < 1) {
        const char *msg = "no-fds\n";
        (void)write_all(fd, msg, strlen(msg));
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(fd);
        (void)setsid();
        for (size_t i = 0; i < nfds; i++) {
            int t = targets[i];
            if (t < 0) { close(fds[i]); continue; }
            if (fds[i] != t) (void)dup2(fds[i], t);
            if (fds[i] != t) close(fds[i]);
            (void)fcntl(t, F_SETFD, 0);
        }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { (void)dup2(devnull, 0); if (devnull > 0) close(devnull); }
        if (cwd[0] && chdir(cwd) != 0) { }
        char *argv[] = { (char *)"sh", (char *)"-c", cmd, NULL };
        char *envp[] = { g_path_env, (char *)"HOME=/data/local/tmp", (char *)"TERM=dumb", NULL };
        execve(g_sh_path, argv, envp);
        {
            char eb[128];
            int en = snprintf(eb, sizeof eb, "exec-failed: %s\n", strerror(errno));
            if (en > 0 && write(2, eb, (size_t)en) < 0) { /* 尽力而为，失败就算了 */ }
        }
        _exit(127);
    }
    for (size_t i = 0; i < nfds; i++) close(fds[i]);
    if (pid < 0) {
        const char *msg = "fork-failed\n";
        (void)write_all(fd, msg, strlen(msg));
        return;
    }
    int status = 0;
    /* 等子进程结束；若连接被对端关闭（DSH 取消/前端断开），杀掉整个进程组，避免留孤儿 */
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) { status = 0; break; }
        struct pollfd pc;
        pc.fd = fd; pc.events = POLLIN; pc.revents = 0;
        int pr = poll(&pc, 1, 200);
        if (pr > 0 && (pc.revents & (POLLHUP | POLLERR))) {
            (void)kill((pid_t)-pid, SIGKILL);
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
            logmsg("sandbox peer closed, killed pid=%d", (int)pid);
            return;
        }
        if (pr > 0 && (pc.revents & POLLIN)) {
            char tmp[64];
            ssize_t rn = recv(fd, tmp, sizeof tmp, MSG_DONTWAIT);
            if (rn == 0) {
                (void)kill((pid_t)-pid, SIGKILL);
                (void)kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
                logmsg("sandbox peer eof, killed pid=%d", (int)pid);
                return;
            }
        }
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
    char out[32];
    int n = snprintf(out, sizeof out, "exit %d\n", code);
    (void)write_all(fd, out, (size_t)n);
    logmsg("sandbox code=%d cmd=%.60s", code, cmd);
}

static uid_t sock_owner_fallback(void) {
    /* 未显式给 --sock-uid 时，用 socket 所在目录的属主（= App uid） */
    char dir[512];
    snprintf(dir, sizeof dir, "%s", g_sockpath);
    char *slash = strrchr(dir, '/');
    if (!slash) return 0;
    *slash = '\0';
    struct stat st;
    if (stat(dir, &st) != 0) return 0;
    return st.st_uid;
}

/* ---- 1.2.5：socket/令牌镜像自愈 ----------------------------------------
 * 开机早期 App 的 CE 目录可能还没就绪，service.sh 拿不到 App uid（曾因此把
 * 令牌镜像 chown 成 root:root 600，导致 root 命令通道读不到令牌）。这里改成
 * 守护进程自己推导属主、自己建目录、自己镜像令牌，并在运行期定期复查。 */
static uid_t resolve_owner(void) {
    if (g_sock_uid != 0) return g_sock_uid;          /* 显式 --sock-uid 优先 */
    uid_t o = sock_owner_fallback();                 /* socket 所在目录属主 */
    if (o != 0) return o;
    if (g_rootfs[0] != '\0') {
        struct stat st;
        if (stat(g_rootfs, &st) == 0 && st.st_uid != 0) return st.st_uid;
    }
    return 0;
}

static void ensure_sock_dir(void) {
    if (g_rootfs[0] == '\0') return;
    char dir[600];
    int n1 = snprintf(dir, sizeof dir, "%s/root", g_rootfs);
    if (n1 > 0 && (size_t)n1 < sizeof dir && mkdir(dir, 0755) != 0 && errno != EEXIST) {
        return;   /* 父目录还没就绪，下一轮再试 */
    }
    int n2 = snprintf(dir, sizeof dir, "%s/root/.dsh", g_rootfs);
    if (n2 > 0 && (size_t)n2 < sizeof dir) {
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) return;
        uid_t o = resolve_owner();
        if (o != 0 && chown(dir, o, (gid_t)-1) != 0) { /* 属主还没就绪，下一轮再试 */ }
    }
}

static void mirror_token(void) {
    g_token_mirror_ok = 0;
    if (g_rootfs[0] == '\0' || g_token_len == 0) return;
    char dst[600], tmp[640];
    if ((size_t)snprintf(dst, sizeof dst, "%s/root/.dsh/rootagent-token", g_rootfs) >= sizeof dst) return;
    uid_t owner = resolve_owner();
    mode_t mode = owner != 0 ? 0600 : 0644;   /* 属主未知时不能设 600，否则引擎读不到 */
    struct stat st;
    if (stat(dst, &st) == 0 && (size_t)st.st_size == g_token_len
        && st.st_uid == owner && (st.st_mode & 07777) == mode) {
        g_token_mirror_ok = 1;
        g_token_mirror_uid = st.st_uid;
        g_token_mirror_mode = (int)(st.st_mode & 0777);
        sb_publish();
        return;
    }
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp.%d", dst, (int)getpid()) >= sizeof tmp) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) { logmsg("token mirror open %s: %s", tmp, strerror(errno)); return; }
    ssize_t w = write(fd, g_token, g_token_len);
    close(fd);
    if (w != (ssize_t)g_token_len) { unlink(tmp); logmsg("token mirror write failed"); return; }
    (void)chmod(tmp, mode);
    if (owner != 0 && chown(tmp, owner, (gid_t)-1) != 0) { /* 记在日志里没必要刷屏 */ }
    if (rename(tmp, dst) != 0) {
        unlink(tmp);
        logmsg("token mirror rename %s: %s", dst, strerror(errno));
        return;
    }
    if (stat(dst, &st) == 0) {
        g_token_mirror_uid = st.st_uid;
        g_token_mirror_mode = (int)(st.st_mode & 0777);
        g_token_mirror_ok = ((size_t)st.st_size == g_token_len && st.st_uid == owner
                             && (st.st_mode & 07777) == mode);
    }
    logmsg("token mirror %s uid=%d mode=%03o ok=%d", dst, (int)g_token_mirror_uid,
           g_token_mirror_mode, g_token_mirror_ok);
    sb_publish();
}

static void sandbox_loop(void) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", g_sockpath);

    /* 开机早期 App 的 CE 私有目录可能还没就绪：bind 失败就每 5 秒重试，
     * 直到目录出现为止（不再像 1.2.3 那样直接 return 丢掉沙箱通道）。 */
    int srv = -1;
    time_t last_log = 0;
    for (;;) {
        if (srv < 0) {
            srv = socket(AF_UNIX, SOCK_STREAM, 0);
            if (srv < 0) {
                logmsg("sandbox socket: %s", strerror(errno));
                sleep(5);
                continue;
            }
        }
        ensure_sock_dir();
        unlink(g_sockpath);
        if (bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0) break;
        int e = errno;
        time_t now = time(NULL);
        if (last_log == 0 || now - last_log >= 60) {
            logmsg("sandbox bind %s: %s (retry every 5s)", g_sockpath, strerror(e));
            last_log = now;
        }
        close(srv);
        srv = -1;
        sleep(5);
    }
    chmod(g_sockpath, 0600);
    uid_t owner = resolve_owner();
    g_sock_owner = owner;
    if (owner != 0) {
        if (chown(g_sockpath, owner, (gid_t)-1) != 0) {
            logmsg("sandbox chown %s: %s", g_sockpath, strerror(errno));
        } else {
            g_peer_uid = owner;      /* 属主已知才开 SO_PEERCRED 校验 */
        }
    }
    if (listen(srv, 16) != 0) {
        logmsg("sandbox listen %s: %s", g_sockpath, strerror(errno));
        close(srv);
        return;
    }
    g_sock_ready = 1;
    sb_publish();
    mirror_token();
    logmsg("sandbox socket listening %s uid=%d peerCheck=%d", g_sockpath, (int)owner,
           g_peer_uid != 0);
    time_t last_mirror = time(NULL);
    for (;;) {
        int st0;
        while (waitpid(-1, &st0, WNOHANG) > 0) { }
        struct pollfd pfd;
        pfd.fd = srv;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int pr = poll(&pfd, 1, 5000);
        if (pr == 0) {                       /* 空闲时每 15 秒复查一次令牌镜像 */
            time_t now2 = time(NULL);
            if (now2 - last_mirror >= 15) {
                ensure_sock_dir();
                mirror_token();
                last_mirror = now2;
            }
            continue;
        }
        if (pr < 0) { if (errno == EINTR) continue; usleep(200000); continue; }
        int c = accept(srv, NULL, NULL);
        if (c < 0) { if (errno == EINTR) continue; continue; }
        pid_t pid = fork();
        if (pid == 0) { signal(SIGCHLD, SIG_DFL); close(srv); sandbox_conn(c); close(c); _exit(0); }
        close(c);
        int st;
        while (waitpid(-1, &st, WNOHANG) > 0) { }
    }
}


static void handle_conn(int fd) {
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    char req[MAX_HEADER + 1];
    size_t rlen = 0;
    int header_end = -1;
    while (rlen < MAX_HEADER) {
        ssize_t n = recv(fd, req + rlen, MAX_HEADER - rlen, 0);
        if (n <= 0) return;
        rlen += (size_t)n;
        req[rlen] = '\0';
        char *p = strstr(req, "\r\n\r\n");
        if (p) { header_end = (int)(p - req); break; }
    }
    if (header_end < 0) {
        http_send(fd, 431, "Request Header Fields Too Large",
                  "{\"ok\":false,\"error\":\"header_too_large\"}");
        return;
    }

    char method[16] = {0};
    char path[256] = {0};
    if (sscanf(req, "%15s %255s", method, path) != 2) {
        http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad_request_line\"}");
        return;
    }
    char *qm = strchr(path, '?');
    if (qm) *qm = '\0';

    char auth[512] = {0};
    long content_length = 0;

    char *p = strstr(req, "\r\n");
    while (p && (int)(p - req) < header_end) {
        char *line = p + 2;
        char *eol = strstr(line, "\r\n");
        if (!eol) break;
        if (line >= req + header_end) break;
        char *colon = memchr(line, ':', (size_t)(eol - line));
        if (colon) {
            size_t nlen = (size_t)(colon - line);
            char *val = colon + 1;
            while (val < eol && (*val == ' ' || *val == '\t')) val++;
            size_t vlen = (size_t)(eol - val);
            if (nlen == 14 && strncasecmp(line, "content-length", 14) == 0) {
                char tmp[32];
                size_t c = vlen < sizeof tmp - 1 ? vlen : sizeof tmp - 1;
                memcpy(tmp, val, c);
                tmp[c] = '\0';
                content_length = strtol(tmp, NULL, 10);
            } else if (nlen == 13 && strncasecmp(line, "authorization", 13) == 0) {
                size_t c = vlen < sizeof auth - 1 ? vlen : sizeof auth - 1;
                memcpy(auth, val, c);
                auth[c] = '\0';
            }
        }
        p = eol;
    }

    const char *bearer = NULL;
    if (strncasecmp(auth, "Bearer ", 7) == 0) bearer = auth + 7;
    int authed = token_ok(bearer);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/health") == 0) {
        char body[768];
        int sb_ready = g_sb ? g_sb->ready : 0;
        int sb_owner = g_sb ? g_sb->owner : 0;
        int sb_tok = g_sb ? g_sb->token_ok : 0;
        int sb_uid = g_sb ? g_sb->token_uid : 0;
        int sb_mode = g_sb ? g_sb->token_mode : 0;
        /* 以实际文件为准：状态共享有延迟，这里现查一次 */
        if (g_rootfs[0] != '\0' && g_token_len != 0) {
            char dst[600];
            if ((size_t)snprintf(dst, sizeof dst, "%s/root/.dsh/rootagent-token", g_rootfs) < sizeof dst) {
                struct stat st;
                if (stat(dst, &st) == 0) {
                    int want_mode = sb_owner != 0 ? 0600 : 0644;
                    sb_uid = (int)st.st_uid;
                    sb_mode = (int)(st.st_mode & 0777);
                    sb_tok = ((size_t)st.st_size == g_token_len
                              && (sb_owner == 0 || (int)st.st_uid == sb_owner)
                              && sb_mode == want_mode);
                } else {
                    sb_tok = 0;
                }
            }
        }
        snprintf(body, sizeof body,
                 "{\"ok\":true,\"name\":\"%s\",\"version\":\"%s\",\"pid\":%d,"
                 "\"uptimeMs\":%lld,\"requests\":%zu,"
                 "\"sandbox\":{\"ready\":%d,\"owner\":%d,\"tokenOk\":%d,"
                 "\"tokenUid\":%d,\"tokenMode\":\"%03o\"}}",
                 APP_NAME, APP_VERSION, (int)g_self_pid, now_ms() - g_start_ms,
                 g_requests, sb_ready, sb_owner, sb_tok, sb_uid, sb_mode);
        http_send(fd, 200, "OK", body);
        return;
    }

    if (!authed) {
        http_send(fd, 401, "Unauthorized", "{\"ok\":false,\"error\":\"unauthorized\"}");
        logmsg("401 %s %s", method, path);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/log") == 0) {
        (void)send_log_response(fd);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/spawn") == 0) {
        char *body = read_http_body(fd, req, rlen, header_end, content_length);
        if (!body) { http_send(fd, 413, "Payload Too Large", "{\"ok\":false,\"error\":\"body_too_large\"}"); return; }
        (void)handle_spawn(fd, body, (size_t)content_length);
        free(body);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/kill") == 0) {
        char *body = read_http_body(fd, req, rlen, header_end, content_length);
        if (!body) { http_send(fd, 413, "Payload Too Large", "{\"ok\":false,\"error\":\"body_too_large\"}"); return; }
        (void)handle_kill(fd, body, (size_t)content_length);
        free(body);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/exec") == 0) {
        if (content_length <= 0 || content_length > MAX_BODY) {
            http_send(fd, 413, "Payload Too Large", "{\"ok\":false,\"error\":\"body_too_large\"}");
            return;
        }
        size_t body_start = (size_t)header_end + 4;
        size_t have = rlen - body_start;
        if (have > (size_t)content_length) have = (size_t)content_length;
        char *body = (char *)malloc((size_t)content_length + 1);
        if (!body) {
            http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"out_of_memory\"}");
            return;
        }
        memcpy(body, req + body_start, have);
        size_t got = have;
        while (got < (size_t)content_length) {
            ssize_t n = recv(fd, body + got, (size_t)content_length - got, 0);
            if (n <= 0) break;
            got += (size_t)n;
        }
        body[got] = '\0';
        if (got < (size_t)content_length) {
            free(body);
            http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"truncated_body\"}");
            return;
        }

        char cmd[MAX_CMD + 1];
        cmd[0] = '\0';
        int cmdrc = json_get_string(body, got, "cmd", cmd, sizeof cmd);
        if (cmdrc < 0) {
            free(body);
            http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"cmd_too_long\",\"max\":8192}");
            return;
        }
        if (cmdrc == 0 || cmd[0] == '\0') {
            free(body);
            http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"missing_cmd\"}");
            return;
        }
        char cwd[1024] = "/";
        int cwdrc = json_get_string(body, got, "cwd", cwd, sizeof cwd);
        if (cwdrc <= 0) {
            if (cwdrc < 0 || strstr(body, "\"cwd\"") != NULL) {
                free(body);
                http_send(fd, 400, "Bad Request", "{\"ok\":false,\"error\":\"bad_cwd\"}");
                return;
            }
            cwd[0] = '\0';
        }
        long tmo = DEFAULT_TIMEOUT_MS;
        if (!json_get_int(body, got, "timeoutMs", &tmo)) tmo = DEFAULT_TIMEOUT_MS;
        if (tmo < MIN_TIMEOUT_MS) tmo = MIN_TIMEOUT_MS;
        if (tmo > MAX_TIMEOUT_MS) tmo = MAX_TIMEOUT_MS;
        free(body);
        (void)send_exec_response(fd, cmd, cwd[0] ? cwd : NULL, tmo);
        return;
    }

    http_send(fd, 404, "Not Found", "{\"ok\":false,\"error\":\"not_found\"}");
    logmsg("404 %s %s", method, path);
}

/* 并发已满时仍放行 /health：监控能看出「在线但忙」，而不是被误判为离线。 */
static int peek_health(int fd) {
    /* 并发打满时也要能认出 /health：先等请求行到达（最多 300ms），
     * 否则 accept 刚返回、客户端还没发出数据时 recv 会 EAGAIN，误判成普通请求。 */
    struct pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, 300) <= 0) return 0;
    char buf[24];
    ssize_t n = recv(fd, buf, sizeof buf - 1, MSG_PEEK | MSG_DONTWAIT);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return strncmp(buf, "GET /health", 11) == 0;
}

static void on_term(int sig) {
    (void)sig;
    if (g_pidfile[0]) unlink(g_pidfile);
    _exit(0);
}

static void on_sigchld(int sig) {
    (void)sig;
    int status;
    pid_t p;
    while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
        if (g_active > 0) g_active--;
    }
}

static int load_token(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    size_t start = 0, end = n;
    while (start < end && isspace((unsigned char)buf[start])) start++;
    while (end > start && isspace((unsigned char)buf[end - 1])) end--;
    size_t len = end - start;
    if (len < 16 || len >= sizeof g_token) return 0;
    memcpy(g_token, buf + start, len);
    g_token[len] = '\0';
    g_token_len = len;
    return 1;
}


/* 同一端口的 /health 探测：返回另一个实例的 pid，拿不到就 -1。
 * 用于避免看门狗/重复执行 service.sh 时起出多份守护进程。 */
static int probe_instance_pid(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    const char *req = "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
    char buf[2048];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    char name[32];
    long opid = -1;
    if (!json_get_string(buf, (size_t)n, "name", name, sizeof name)) return -1;
    if (strcmp(name, APP_NAME) != 0) return -1;
    if (!json_get_int(buf, (size_t)n, "pid", &opid)) return -1;
    return (int)opid;
}

int main(int argc, char **argv) {
    int port = 47831;
    int selftest = 0;
    const char *sh_env = getenv("ROOTAGENT_SHELL");
    if (sh_env && *sh_env && strlen(sh_env) < sizeof g_sh_path) snprintf(g_sh_path, sizeof g_sh_path, "%s", sh_env);
    const char *path_env = getenv("ROOTAGENT_PATH");
    if (path_env && *path_env) snprintf(g_path_env, sizeof g_path_env, "PATH=%s", path_env);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--token-file") == 0 && i + 1 < argc) {
            snprintf(g_tokenfile, sizeof g_tokenfile, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            snprintf(g_logpath, sizeof g_logpath, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--pidfile") == 0 && i + 1 < argc) {
            snprintf(g_pidfile, sizeof g_pidfile, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--sandbox-sock") == 0 && i + 1 < argc) {
            snprintf(g_sockpath, sizeof g_sockpath, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--sock-uid") == 0 && i + 1 < argc) {
            g_sock_uid = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rootfs") == 0 && i + 1 < argc) {
            snprintf(g_rootfs, sizeof g_rootfs, "%s", argv[++i]);
        } else if (strcmp(argv[i], "--selftest") == 0) selftest = 1;
        else if (strcmp(argv[i], "--version") == 0) {
            printf("%s %s\n", APP_NAME, APP_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--port N] [--token-file PATH] [--log PATH] [--pidfile PATH] [--sandbox-sock PATH] [--sock-uid UID] [--rootfs DIR] [--selftest]\n", argv[0]);
            return 0;
        }
    }
    if (port <= 0 || port > 65535) port = 47831;
    /* 没给 --sandbox-sock 时，可由 --rootfs 推导；两者都没有就不开沙箱通道 */
    if (g_sockpath[0] == '\0' && g_rootfs[0] != '\0') {
        if ((size_t)snprintf(g_sockpath, sizeof g_sockpath, "%s/root/.dsh/rootagent-sandbox.sock", g_rootfs)
            >= sizeof g_sockpath) {
            g_sockpath[0] = '\0';
        }
    }

    if (!load_token(g_tokenfile)) {
        fprintf(stderr, "rootagentd: 令牌文件无效或不可读: %s\n", g_tokenfile);
        return 2;
    }
    if (selftest) {
        printf("token:ok length=%zu port=%d log=%s\n", g_token_len, port, g_logpath);
        return 0;
    }

    g_self_pid = getpid();
    signal(SIGPIPE, SIG_IGN);

    struct sigaction st;
    memset(&st, 0, sizeof st);
    st.sa_handler = on_term;
    sigaction(SIGTERM, &st, NULL);
    sigaction(SIGINT, &st, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) {
        int err = errno;
        if (err == EADDRINUSE) {
            int other = probe_instance_pid(port);
            if (other > 0 && other != (int)getpid()) {
                logmsg("another rootagentd pid=%d already serving 127.0.0.1:%d, exit", other, port);
                fprintf(stderr, "rootagentd: 已有实例 pid=%d 在 127.0.0.1:%d，本进程退出\n", other, port);
                close(srv);
                return 0;
            }
        }
        fprintf(stderr, "rootagentd: bind 127.0.0.1:%d 失败: %s\n", port, strerror(err));
        close(srv);
        return 1;
    }
    if (listen(srv, 32) != 0) { perror("listen"); return 1; }

    g_sb = mmap(NULL, sizeof *g_sb, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (g_sb == MAP_FAILED) g_sb = NULL;
    else memset(g_sb, 0, sizeof *g_sb);

    if (g_sockpath[0] != '\0') {
        pid_t sb = fork();
        if (sb == 0) {
            close(srv);   /* 关键：不要把 TCP 监听 fd 继承给沙箱子进程，否则旧进程会一直占着端口 */
            sandbox_loop();
            _exit(0);
        }
    }

    g_start_ms = now_ms();
    if (g_pidfile[0]) {
        int pf = open(g_pidfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (pf >= 0) {
            char b[32];
            int n = snprintf(b, sizeof b, "%d\n", (int)g_self_pid);
            if (n > 0) (void)write_all(pf, b, (size_t)n);
            close(pf);
        }
    }
    logmsg("start pid=%d port=%d token_len=%zu log=%s", (int)g_self_pid, port, g_token_len, g_logpath);
    fprintf(stderr, "rootagentd: listening on 127.0.0.1:%d\n", port);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            logmsg("accept 失败: %s", strerror(errno));
            continue;
        }
        g_requests++;
        if (g_active >= MAX_CONCURRENCY && !peek_health(fd)) {
            http_send(fd, 429, "Too Many Requests", "{\"ok\":false,\"error\":\"busy\"}");
            close(fd);
            logmsg("429 busy active=%d", (int)g_active);
            continue;
        }
        /* v1.2.3：先占坑再 fork。旧版 fork 之后才 g_active++，子进程若抢在 ++ 之前退出，
         * SIGCHLD 处理器里的 if (g_active > 0) 会把这次 -- 吞掉，父进程随后 ++ 就永久多 1，
         * 累积到上限后所有请求（含 /health）都 429，只有重启守护进程才能恢复。 */
        g_active++;
        pid_t pid = fork();
        if (pid == 0) {
            signal(SIGCHLD, SIG_DFL);
            alarm((MAX_TIMEOUT_MS / 1000) + 60);   /* 卡死的连接最多占坑 3 分钟 */
            close(srv);
            handle_conn(fd);
            close(fd);
            _exit(0);
        }
        if (pid < 0) {
            g_active--;
            http_send(fd, 500, "Internal Server Error", "{\"ok\":false,\"error\":\"fork_failed\"}");
            close(fd);
            continue;
        }
        close(fd);
    }
}
