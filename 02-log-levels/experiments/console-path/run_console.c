/*
 * run_console.c ——「printk 会自己冒到终端上吗？」实验（C 版）
 *
 * 实验设计（对应原来用 paramiko 远程编排的 run_console.py，现在纯 C 在板上跑）：
 *
 *   - 子进程：用 finit_module(2) 系统调用直接加载内核模块（不经 insmod 命令）
 *   - 父进程：用 poll() + read() 实时跟随 /dev/kmsg（这就是 dmesg -W 的原理）
 *
 * 要回答两个问题：
 *   1. 模块里的 printk 到底产生了吗？      → 看 /dev/kmsg 跟随的結果
 *   2. 它会自己冒到我的终端上吗？          → 看父/子进程的 stdout 有没有被内核写入
 *
 * 关键点：子进程继承了父进程的 stdout。如果 printk 真的会往终端写，
 *         那父子进程的输出会混在一起；实测不会 —— 因为 printk 只写
 *         环形缓冲区和「已注册的 console」，而 SSH 的 pts 不在 console 列表里。
 *
 * 用法： sudo ./run_console [模块路径]
 *        sudo ./run_console /home/wzp/LDD-/02-log-levels/log_levels.ko
 *
 * 编译： gcc -O2 -Wall -Wextra -o run_console run_console.c
 *
 * 需要 root：finit_module 要 CAP_SYSADMIN，klogctl 清缓冲区要 CAP_SYSLOG。
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
#include <signal.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/klog.h>      /* klogctl —— dmesg 用的就是它 */

/* sys/klog.h 未定义的常量 */
#ifndef SYSLOG_ACTION_CLEAR
#define SYSLOG_ACTION_CLEAR 5
#endif

#define DEFAULT_MODULE "/home/wzp/LDD-/02-log-levels/log_levels.ko"
#define MODULE_NAME    "log_levels"

/* ── 模块加载/卸载：直接用系统调用，不经 insmod 命令 ───────────────── */

static int load_module(const char *path, const char *params)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    long rc = syscall(SYS_finit_module, fd, params ? params : "", 0);
    int saved = errno;
    close(fd);
    if (rc != 0) { errno = saved; return -1; }
    return 0;
}

static int unload_module(const char *name)
{
    long rc = syscall(SYS_delete_module, name, 0);
    return (rc == 0) ? 0 : -1;
}

/* ── 环境信息 ──────────────────────────────────────────────────── */

static void show_file(const char *path, const char *title)
{
    FILE *f = fopen(path, "r");
    printf("── %s (%s) ──\n", title, path);
    if (!f) {
        printf("   (打不开: %s)\n", strerror(errno));
        return;
    }
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        /* 去掉换行 */
        line[strcspn(line, "\n")] = '\0';
        if (line[0]) { printf("   %s\n", line); n++; }
    }
    if (!n) printf("   (空)\n");
    fclose(f);
}

static void show_my_tty(void)
{
    printf("── 本进程的标准输出 ──\n");
    printf("   isatty(stdout) = %s\n", isatty(STDOUT_FILENO) ? "是" : "否");
    const char *tn = ttyname(STDOUT_FILENO);
    printf("   ttyname        = %s\n", tn ? tn : "(不是终端设备)");

    /* /dev/pts/N 说明是伪终端（SSH / 图形终端模拟器）；/dev/ttyN 才是虚拟控制台 */
    if (tn) {
        if (strncmp(tn, "/dev/pts/", 9) == 0)
            printf("   → 这是伪终端 pts，不是已注册的 console\n");
        else if (strncmp(tn, "/dev/tty", 8) == 0)
            printf("   → 这是虚拟控制台 tty\n");
    }
}

/* ── /dev/kmsg 跟随 ────────────────────────────────────────────── */

/* 打开 /dev/kmsg 并把已有的记录全部读掉（建立基线，之后只关心新消息） */
static int kmsg_open_drained(void)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open /dev/kmsg 失败: %s\n", strerror(errno));
        return -1;
    }
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;               /* EAGAIN = 已读到最新 */
        }
        if (n == 0) break;
    }
    return fd;
}

static void nowstr(char *out, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);
    snprintf(out, n, "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L);
}

int main(int argc, char **argv)
{
    const char *modpath = (argc > 1) ? argv[1] : DEFAULT_MODULE;

    setvbuf(stdout, NULL, _IOLBF, 0);   /* 行缓冲，保证输出顺序可预期 */

    printf("══════════ 实验：printk 会自己冒到终端上吗 ══════════\n\n");

    /* 1. 环境：谁才是 console */
    show_file("/proc/consoles", "内核已注册的 console");
    printf("\n");
    show_my_tty();
    printf("\n");

    /* 2. 推进 syslog 游标 + 排空 /dev/kmsg，保证后面看到的都是本实验产生的
     *
     *    注意：klogctl(SYSLOG_ACTION_CLEAR) 并【不会】擦除环形缓冲区里的记录，
     *    它只是把 syslog 读取游标 syslog_seq 推到末尾（实测：清完 /dev/kmsg
     *    里的记录条数和首条 seq 完全不变）。所以真正的"排空"是靠下面
     *    kmsg_open_drained() 把已有记录全部读掉 —— 之后 poll 到的必然是新的。
     */
    printf("── 推进 syslog 游标 (klogctl SYSLOG_ACTION_CLEAR) ──\n");
    if (klogctl(SYSLOG_ACTION_CLEAR, NULL, 0) < 0)
        fprintf(stderr, "   失败: %s（继续）\n", strerror(errno));
    else
        printf("   游标已推到末尾（注意：缓冲区里的记录并没有被擦除）\n");
    printf("\n");

    /* 3. 建立跟随 */
    int kfd = kmsg_open_drained();
    if (kfd < 0) return 1;

    printf("═══ 父进程开始跟随 /dev/kmsg（等价于 dmesg -W）═══\n");
    printf("═══ 子进程将在 1 秒后用 finit_module() 加载 %s ═══\n\n", modpath);

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        /* ── 子进程：加载模块 ── */
        usleep(1000 * 1000);
        printf("[子进程 %d] 调用 finit_module(\"%s\") ...\n", getpid(), modpath);
        fflush(stdout);

        if (load_module(modpath, "") == 0) {
            printf("[子进程 %d] finit_module 返回 0 —— 模块已加载\n", getpid());
        } else {
            printf("[子进程 %d] finit_module 失败: %s\n", getpid(), strerror(errno));
            printf("           提示：模块路径对吗？先 make 出 .ko；本程序需要 root\n");
        }
        fflush(stdout);
        usleep(1500 * 1000);
        _exit(0);
    }

    /* ── 父进程：poll 跟随 6 秒 ── */
    struct pollfd pfd = { .fd = kfd, .events = POLLIN };
    char buf[8192];
    char ts[32];
    time_t deadline = time(NULL) + 6;
    int got = 0;

    while (time(NULL) < deadline) {
        int r = poll(&pfd, 1, 200 /* ms */);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        ssize_t n = read(kfd, buf, sizeof(buf) - 1);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EPIPE) { lseek(kfd, 0, SEEK_SET); continue; }
            break;
        }
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';

        char *semi = strchr(buf, ';');
        const char *msg = semi ? semi + 1 : buf;
        nowstr(ts, sizeof ts);
        printf("[跟随 %s] %s\n", ts, msg);
        got++;
    }

    int status = 0;
    waitpid(pid, &status, 0);

    printf("\n═══ 跟随结束：共捕获 %d 条新内核消息 ═══\n", got);

    /* 4. 清理 */
    if (unload_module(MODULE_NAME) == 0)
        printf("已卸载模块 %s\n", MODULE_NAME);
    else
        printf("卸载 %s 失败: %s（可能没加载成功）\n", MODULE_NAME, strerror(errno));

    /* 5. 结论 */
    printf("\n══════════ 结论 ══════════\n");
    printf("1. 模块的 printk 确实产生了 —— 上面「跟随」抓到的就是。\n");
    printf("2. 但这些消息是我们主动读 /dev/kmsg 才拿到的。\n");
    printf("   父子进程的 stdout 上，除了自己 printf 的内容，\n");
    printf("   没有任何一条是内核「主动送过来」的。\n");
    printf("3. 原因：printk 只往两个地方写 ——\n");
    printf("     (a) 内核环形缓冲区（所有人都能读）\n");
    printf("     (b) /proc/consoles 里列出的已注册 console（tty1 / ttyAMA10 等）\n");
    printf("   SSH 登录拿到的是 /dev/pts/N 伪终端，不在 console 列表里，\n");
    printf("   所以永远收不到。想看就得自己读 —— 这就是 dmesg 存在的理由。\n");

    close(kfd);
    return 0;
}
