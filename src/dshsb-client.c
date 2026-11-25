// dshsb-client — 把本进程的 fd 1/2/7 通过 unix socket + SCM_RIGHTS 交给 root 侧守护进程执行
// fd 7 是 DSH 的 control 管道（可选）：没有它时只转发 stdout/stderr。
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    const char *sock = argc > 1 ? argv[1] : getenv("DSH_SB_SOCK");
    const char *hdr = getenv("DSH_SB_HEADER");
    if (!sock || !hdr) { fprintf(stderr, "dshsb-client: 缺少 socket 路径或 DSH_SB_HEADER\n"); return 111; }

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 111; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", sock);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) != 0) {
        fprintf(stderr, "dshsb-client: connect %s: %s\n", sock, strerror(errno));
        return 111;
    }

    size_t cap = 262144;
    char *line = (char *)malloc(cap);
    if (!line) { fprintf(stderr, "dshsb-client: oom\n"); return 111; }
    int n = snprintf(line, cap, "%s\n", hdr);
    if (n <= 0 || (size_t)n >= cap) { fprintf(stderr, "dshsb-client: header 太长\n"); return 111; }

    int pass[3];
    int want[3] = { 1, 2, 7 };
    int np = 0;
    for (int i = 0; i < 3; i++) {
        if (fcntl(want[i], F_GETFD) != -1) { pass[np] = want[i]; np++; }
    }
    struct msghdr msg;
    struct iovec iov;
    char cbuf[CMSG_SPACE(sizeof(pass))];
    memset(&msg, 0, sizeof msg);
    memset(cbuf, 0, sizeof cbuf);
    iov.iov_base = line;
    iov.iov_len = (size_t)n;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof cbuf;
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * np);
    memcpy(CMSG_DATA(c), pass, sizeof(int) * np);
    if (sendmsg(s, &msg, 0) < 0) { perror("sendmsg"); return 111; }

    char reply[256];
    size_t got = 0;
    while (got < sizeof reply - 1) {
        ssize_t r = read(s, reply + got, sizeof reply - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        reply[got] = '\0';
        if (strchr(reply, '\n')) break;
    }
    reply[got] = '\0';
    close(s);
    if (strncmp(reply, "exit ", 5) == 0) return atoi(reply + 5) & 0xFF;
    fprintf(stderr, "dshsb-client: 守护进程回复异常: %s", reply);
    return 111;
}
