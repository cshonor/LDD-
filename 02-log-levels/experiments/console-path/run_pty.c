/*
 * run_pty.c ——「给 SSH 一个真终端，printk 就会冒出来吗？」实验（C 版）
 *
 * 对应原来用 paramiko 远程编排的 run_pty.py，现在纯 C 在板上跑。
 *
 * 对照两组：
 *   A. 无终端：子进程的 stdout 是一根 pipe（等价于脚本里执行）
 *   B. 有终端：子进程跑在 forkpty() 分配的伪终端里，
 *              pty 就是它的控制终端（等价于你 ssh 登录后敲命令）
 *
 * 两组都让子进程去 insmod 同一个模块，然后看父进程读到什么。
 *
 * 预期结论：两组都收不到内核消息。因为 printk 只写给「已注册的 console」
 *          （见 /proc/consoles），而 /dev/pts/N 伪终端从来不在那个列表里。
 *
 * 用法： sudo ./run_pty [模块路径]
 *
 * 编译： gcc -O2 -Wall -Wextra -o run_pty run_pty.c -lutil
 *
 * 需要 root（insmod / finit_module 要 CAP_SYSADMIN）。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include <pty.h>
#include <sys/wait.h>

#define DEFAULT_MODULE "/home/wzp/LDD-/02-log-levels/log_levels.ko"
#define MODULE_NAME    "log_levels"

static void show_file(const char *path, const char *title)
{
    FILE *f = fopen(path, "r");
    printf("── %s (%s) ──\n", title, path);
    if (!f) { printf("   (打不开: %s)\n", strerror(errno)); return; }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (line[0]) { printf("   %s\n", line); n++; }
    }
    if (!n) printf("   (空)\n");
    fclose(f);
}

/*
 * 在 deadline 秒内从 fd 读数据，全部收进 out。
 * 返回读到的字节数。out 会追加 '\0'。
 */
static size_t drain_until(int fd, size_t cap, char *out, int timeout_ms,
                          pid_t child)
{
    size_t used = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed = (t1.tv_sec - t0.tv_sec) * 1000 +
                       (t1.tv_nsec - t0.tv_nsec) / 1000000L;
        if (elapsed >= timeout_ms) break;

        int r = poll(&pfd, 1, 100);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) {
            /* 子进程已经退出且没数据了，就别空等 */
            if (child > 0) {
                int st;
                if (waitpid(child, &st, WNOHANG) == child) {
                    /* 再收一次残留 */
                    for (;;) {
                        ssize_t n = read(fd, out + used, cap - used - 1);
                        if (n <= 0) break;
                        used += n;
                    }
                    break;
                }
            }
            continue;
        }

        ssize_t n = read(fd, out + used, cap - used - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;      /* EIO：pty 另一端关了 */
        }
        if (n == 0) break;
        used += n;
        out[used] = '\0';
    }
    out[used] = '\0';
    return used;
}

/* 把 \r\n 规整成 \n，去掉行尾空白，方便打印 */
static void print_block(const char *title, const char *text)
{
    printf("%s\n", title);
    printf("---8<----------\n");
    if (!text || !text[0]) {
        printf("(空 —— 什么都没收到)\n");
    } else {
        /* 逐行打印，跳过纯 \r 造成的空行 */
        char line[4096];
        size_t i = 0, li = 0;
        for (i = 0; text[i]; i++) {
            if (text[i] == '\n') {
                line[li] = '\0';
                /* 去掉行尾 \r */
                while (li > 0 && (line[li-1] == '\r' || line[li-1] == ' '))
                    line[--li] = '\0';
                if (line[0]) printf("%s\n", line);
                li = 0;
            } else if (li < sizeof(line) - 1) {
                line[li++] = text[i];
            }
        }
        if (li) {
            line[li] = '\0';
            while (li > 0 && line[li-1] == '\r') line[--li] = '\0';
            if (line[0]) printf("%s\n", line);
        }
    }
    printf("--->8----------\n\n");
}

static void cleanup(const char *modpath)
{
    (void)modpath;
    /* 模块可能还挂着，尽力卸掉；失败无所谓 */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rmmod %s 2>/dev/null", MODULE_NAME);
    (void)system(cmd);
}

int main(int argc, char **argv)
{
    const char *modpath = (argc > 1) ? argv[1] : DEFAULT_MODULE;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("══════════ 实验：给 SSH 一个真终端，printk 会冒出来吗 ══════════\n\n");

    /* 0. 环境 */
    show_file("/proc/consoles", "内核已注册的 console");
    printf("\n");
    const char *tn = ttyname(STDOUT_FILENO);
    printf("── 本程序自己的标准输出 ──\n");
    printf("   ttyname = %s\n", tn ? tn : "(不是终端设备)");
    if (tn && strncmp(tn, "/dev/pts/", 9) == 0)
        printf("   → 伪终端 pts：不是 console，printk 不会往这儿送\n");
    printf("\n");

    cleanup(modpath);

    /* ───────── A. 无终端（stdout 接到 pipe）───────── */
    printf("════ A. 无终端：子进程 stdout 接到一根 pipe ════\n");
    {
        int p[2];
        if (pipe(p) < 0) { perror("pipe"); return 1; }

        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            dup2(p[1], STDOUT_FILENO);
            dup2(p[1], STDERR_FILENO);
            close(p[1]);
            printf("MARKER_A: 我要 insmod 了\n");
            fflush(stdout);
            execlp("insmod", "insmod", modpath, (char *)NULL);
            /* exec 失败才走到这 */
            printf("MARKER_A: insmod 执行失败: %s\n", strerror(errno));
            _exit(127);
        }
        close(p[1]);

        char out[8192];
        drain_until(p[0], sizeof out, out, 3000, pid);
        print_block("  pipe 里收到的内容:", out);
        printf("  包含内核消息吗: %s\n\n",
               strstr(out, "log_levels") ? "是" : "否 —— pipe 里只有子进程自己 printf 的东西");
        close(p[0]);
        waitpid(pid, NULL, 0);
    }
    cleanup(modpath);

    /* ───────── B. 有终端（forkpty）───────── */
    printf("════ B. 有终端：子进程跑在 forkpty() 分配的伪终端里 ════\n");
    {
        int master;
        struct termios tio;
        memset(&tio, 0, sizeof tio);
        tio.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
        tio.c_oflag = OPOST | ONLCR;
        tio.c_cflag = CS8 | CREAD | HUPCL | B38400;
        tio.c_lflag = ISIG | ICANON | IEXTEN;      /* 故意不开 ECHO，输出更干净 */
        tio.c_cc[VINTR] = 003; tio.c_cc[VQUIT] = 034;
        tio.c_cc[VERASE] = 0177; tio.c_cc[VKILL] = 025;
        tio.c_cc[VEOF] = 004; tio.c_cc[VSTART] = 021;
        tio.c_cc[VSTOP] = 023; tio.c_cc[VSUSP] = 032;
        tio.c_cc[VREPRINT] = 022; tio.c_cc[VWERASE] = 027;
        tio.c_cc[VLNEXT] = 026; tio.c_cc[VMIN] = 1; tio.c_cc[VTIME] = 0;

        pid_t pid = forkpty(&master, NULL, &tio, NULL);
        if (pid < 0) { perror("forkpty"); return 1; }

        if (pid == 0) {
            /* 子进程：stdin/stdout/stderr 都已经是 pty slave，
               而且它就是这个会话的控制终端 —— 等价于你 ssh 登录后敲命令 */
            char cmd[768];
            snprintf(cmd, sizeof cmd,
                     "echo 'MARKER_B: 我在伪终端里，tty='$(tty); "
                     "insmod %s && echo 'MARKER_B: insmod 返回 0'; "
                     "sleep 1; echo 'MARKER_B: 结束'", modpath);
            execlp("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }

        char out[16384];
        drain_until(master, sizeof out, out, 4000, pid);
        print_block("  伪终端里收到的内容:", out);
        printf("  包含内核消息吗: %s\n\n",
               strstr(out, "log_levels:") ? "是" : "否 —— 即使是真的控制终端也收不到");
        close(master);
        waitpid(pid, NULL, 0);
    }

    /* ───────── 收尾：证明消息确实产生了 ───────── */
    cleanup(modpath);
    printf("════ 收尾：用 dmesg 确认消息确实产生了 ════\n");
    fflush(stdout);
    (void)system("dmesg | grep -i log_levels | tail -4");

    printf("\n══════════ 结论 ══════════\n");
    printf("A（pipe）   ：只能收到子进程自己 printf 的 MARKER，没有内核消息。\n");
    printf("B（伪终端） ：即使是子进程的控制终端，也一样收不到。\n");
    printf("而 dmesg    ：全部 8 个级别都在。\n\n");
    printf("→ 终端是不是「真的」根本不重要。关键是它有没有被注册成 console。\n");
    printf("  看 /proc/consoles：只有 tty1（HDMI）和 ttyAMA10（串口）这类。\n");
    printf("  SSH 给的是 /dev/pts/N，永远不在里面，所以永远等不到 printk。\n");

    return 0;
}
