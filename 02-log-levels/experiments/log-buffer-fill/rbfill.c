/*
 * rbfill.c —— 内核 printk 环形缓冲区（log_buf）填灌实验
 *
 * 目的（与已删除的 rbfill.sh 一致，但全部用 C 实现）：
 *   1. 测出这台机器的 log_buf 实际容量
 *   2. 测出"能装多少条 printk"这个更直观的换算
 *   3. 抓到覆盖发生的那一刻，看清覆盖时的现象
 *
 * 做法：往 /dev/kmsg 写入定长消息，每写 BATCH 条就把整个缓冲区读回来统计一次。
 *       - 未溢出：记录数线性增长
 *       - 溢出瞬间：记录数回落（新消息挤掉更旧的，净增为负）
 *       - 稳态：记录数锁死不再变化
 *
 * 三个值得注意的 C 层面细节：
 *   - klogctl(SYSLOG_ACTION_SIZE_BUFFER)  直接问内核缓冲区多大，不用去读 /boot/config
 *   - /dev/kmsg 每条记录的格式是 "level,seq,timestamp,flags;消息正文"
 *     所以我们能拿到 seq，从而精确判断"哪一条被丢掉了"
 *   - ⚠️ 本工具【故意】读 /dev/kmsg 而不是调 klogctl 读，因为两者看到的不是一回事：
 *       · /dev/kmsg      → 真实的缓冲区内容，从最老一条存活记录开始读
 *       · dmesg / klogctl → 走 syslog(2) 的游标 syslog_seq，只看"游标之后"的
 *       实测：`sudo dmesg -C` 之后，/dev/kmsg 里的记录条数和首条 seq **纹丝不动**，
 *       只有 dmesg 变空了。也就是说 dmesg -C 只挪游标，不擦记录、也不腾空间。
 *       详见 ../../docs/kernel-log-buffer.md
 *
 * 用法： sudo ./rbfill [总条数] [每批观察数] [每条载荷字节] [标签]
 *        sudo ./rbfill 300 16 200
 *        sudo ./rbfill 400 25 200 RBFLLB     ← 换个标签，避免和上一轮编号混淆
 *
 * 编译： gcc -O2 -Wall -Wextra -o rbfill rbfill.c   （或 make）
 *
 * 注意：需要 root（写 /dev/kmsg 要 CAP_SYSLOG）。
 *       会往内核日志灌入大量测试消息，跑完用 `sudo dmesg -C` 清理。
 *       journald 已经同步抄走了一份，清空 dmesg 不会丢数据。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/klog.h>

/* sys/klog.h 里没有定义这些常量，手动补齐（值与内核 include/linux/syslog.h 一致） */
#ifndef SYSLOG_ACTION_CLEAR
#define SYSLOG_ACTION_CLEAR       5
#endif
#ifndef SYSLOG_ACTION_SIZE_BUFFER
#define SYSLOG_ACTION_SIZE_BUFFER 10
#endif

#define TAG "RBFILL"

static char g_line[8192];   /* 单次 read() 的记录缓冲 */
static char g_pad[4096];    /* 载荷填充用的 x 串 */
static const char *g_tag = TAG;   /* 本次运行用的标签（可换，便于区分多轮） */

struct ring_stat {
    long  total;      /* 缓冲区里记录总数 */
    long  tagged;     /* 其中带 TAG（我们写入的）的条数 */
    long  first_seq;  /* 最老一条 TAG 记录的 seq */
    long  oldest_seq; /* 缓冲区里最老一条记录的 seq（覆盖检测的关键） */
    long  last_seq;   /* 最新一条记录的 seq */
    size_t textbytes; /* 记录正文字节总数（不含 /dev/kmsg 的元信息） */
    char  oldest[128];   /* 最早一条记录正文 */
    char  oldest_tag[128];/* 最早一条 TAG 记录正文 */
    char  newest[128];   /* 最新一条记录正文 */
    int   overrun;    /* 采样期间是否发生过 /dev/kmsg 溢出(EPIPE) */
};

/*
 * 读一遍整个环形缓冲区并统计。
 * /dev/kmsg 每次 read() 返回一条记录；O_NONBLOCK 下读完返回 -1/EAGAIN。
 */
static int sample_ring(struct ring_stat *st)
{
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "open /dev/kmsg 失败: %s\n", strerror(errno));
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->first_seq  = -1;
    st->oldest_seq = -1;
    st->last_seq   = -1;

    for (;;) {
        ssize_t n = read(fd, g_line, sizeof(g_line) - 1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;              /* 读完了 */
            if (errno == EPIPE) {
                st->overrun = 1;    /* 读得太慢，记录被覆盖掉了 */
                break;
            }
            break;
        }
        if (n == 0)
            break;
        g_line[n] = '\0';

        /* 去掉行尾换行，方便直接打印 */
        char *nl = strchr(g_line, '\n');
        if (nl) *nl = '\0';

        /* 格式: level,seq,timestamp,flags;正文 */
        char *semi = strchr(g_line, ';');
        const char *msg = semi ? semi + 1 : g_line;

        long seq = -1;
        if (semi) {
            char *c1 = strchr(g_line, ',');
            if (c1 && c1 < semi) seq = atol(c1 + 1);
        }

        st->total++;
        st->textbytes += strlen(msg) + 1;   /* +1 补回被截掉的换行 */
        st->last_seq = seq;

        if (st->total == 1) {
            st->oldest_seq = seq;
            snprintf(st->oldest, sizeof(st->oldest), "%s", msg);
        }
        snprintf(st->newest, sizeof(st->newest), "%s", msg);

        if (strstr(msg, g_tag)) {
            st->tagged++;
            if (st->first_seq < 0 || seq < st->first_seq) {
                st->first_seq = seq;
                snprintf(st->oldest_tag, sizeof(st->oldest_tag), "%s", msg);
            }
        }
    }

    close(fd);
    return 0;
}

/* 截断成适合表格显示的短串 */
static const char *shorten(const char *s, char *out, size_t n)
{
    if (strlen(s) <= n - 1) {
        snprintf(out, n, "%s", s);
    } else {
        snprintf(out, n, "%.*s...", (int)(n - 4), s);
    }
    return out;
}

int main(int argc, char **argv)
{
    long total_n = (argc > 1) ? atol(argv[1]) : 300;
    long batch   = (argc > 2) ? atol(argv[2]) : 16;
    long pay     = (argc > 3) ? atol(argv[3]) : 200;
    if (argc > 4 && argv[4][0]) g_tag = argv[4];

    if (total_n <= 0 || batch <= 0 || pay <= 0 || pay > 3500) {
        fprintf(stderr, "用法: %s [总条数] [每批观察数] [载荷字节(<=3500)] [标签]\n",
                argv[0]);
        return 2;
    }
    if (strlen(g_tag) > 32) {
        fprintf(stderr, "标签太长了（<=32）\n");
        return 2;
    }

    if (geteuid() != 0)
        fprintf(stderr, "警告: 需要 root 才能写 /dev/kmsg，当前 euid=%u\n", geteuid());

    /* ① 直接问内核：缓冲区多大 */
    int ringsz = klogctl(SYSLOG_ACTION_SIZE_BUFFER, NULL, 0);
    if (ringsz < 0) {
        fprintf(stderr, "klogctl(SIZE_BUFFER) 失败: %s\n", strerror(errno));
        return 1;
    }

    /* ② 打开 /dev/kmsg 准备写入 */
    int wfd = open("/dev/kmsg", O_WRONLY);
    if (wfd < 0) {
        fprintf(stderr, "open /dev/kmsg(O_WRONLY) 失败: %s\n", strerror(errno));
        return 1;
    }

    memset(g_pad, 'x', sizeof(g_pad) - 1);
    g_pad[sizeof(g_pad) - 1] = '\0';

    printf("缓冲容量    : %d 字节 (%d KiB)   ← klogctl(SYSLOG_ACTION_SIZE_BUFFER)\n",
           ringsz, ringsz >> 10);
    printf("写入方式    : 写 /dev/kmsg（C 程序直接 write）\n");
    printf("统计口径    : 读 /dev/kmsg（= 缓冲区真实内容，不受 syslog 游标影响）\n");
    printf("本轮标签    : %s\n", g_tag);
    printf("每条载荷    : %ld 字节\n", pay);
    printf("单条文本    : %ld 字节 = strlen(\"%s \") + 5 位序号 + 空格 + %ld\n\n",
           (long)strlen(g_tag) + 1 + 5 + 1 + pay, g_tag, pay);

    /* ③ 基线采样 */
    struct ring_stat base, st;
    if (sample_ring(&base) < 0) return 1;

    char tmp[128];
    printf("=== 基线：记录数=%ld  首条=[%s]\n",
           base.total, shorten(base.oldest, tmp, sizeof tmp));
    if (base.overrun)
        printf("    (注意: 采样时 /dev/kmsg 发生溢出，计数可能偏小)\n");
    printf("\n");
    char h1[32], h2[32];
    snprintf(h1, sizeof h1, "%s数", g_tag);
    snprintf(h2, sizeof h2, "最老%s", g_tag);
    printf("%-8s %-10s %-10s %-10s %s\n",
           "已写入", "记录总数", h1, h2, "首条记录(最老)");
    printf("----------------------------------------------------------------------------\n");

    /* ④ 边写边观察
     *
     * 覆盖检测用「最老记录的 seq 是否前进」，而不是「记录数是否回落」：
     *   记录数回落只在「缓冲区还没满 → 变满」的那一刻才看得见；
     *   如果缓冲区本来就满了（常见！因为用户态根本擦不掉它），
     *   记录数从一开始就不动，只能靠 seq 前进来判断有没有挤掉旧记录。
     */
    long peak = base.total;
    long wrap_at = -1;
    char msgbuf[4096];

    for (long i = 1; i <= total_n; i++) {
        int len = snprintf(msgbuf, sizeof(msgbuf), "<4>%s %05ld %.*s\n",
                           g_tag, i, (int)pay, g_pad);
        if (write(wfd, msgbuf, len) < 0) {
            fprintf(stderr, "写 /dev/kmsg 失败(第 %ld 条): %s\n", i, strerror(errno));
            break;
        }

        if (i % batch != 0)
            continue;

        if (sample_ring(&st) < 0) break;

        if (st.total > peak) peak = st.total;
        if (wrap_at < 0 && base.oldest_seq >= 0 && st.oldest_seq > base.oldest_seq)
            wrap_at = i;        /* 最老记录被挤掉 = 覆盖开始 */

        char o[40], t[40];
        printf("%-8ld %-10ld %-10ld %-10s %s%s\n",
               i, st.total, st.tagged,
               st.tagged ? shorten(st.oldest_tag + strlen(TAG) + 1, o, sizeof o) : "-",
               shorten(st.oldest, t, sizeof t),
               (wrap_at == i) ? "   ★覆盖开始" : "");
    }

    /* ⑤ 最终统计 */
    if (sample_ring(&st) < 0) return 1;

    printf("\n=== 最终结果 ===\n");
    printf("  缓冲区记录总数      : %ld\n", st.total);
    printf("  带标签的存活条数    : %ld\n", st.tagged);
    printf("  其它记录（非本轮）  : %ld\n", st.total - st.tagged);
    printf("  记录正文总字节      : %zu\n", st.textbytes);
    printf("  峰值记录数          : %ld\n", peak);
    if (wrap_at > 0)
        printf("  覆盖起始于第        : %ld 条写入时（最老记录 seq 开始前进）\n", wrap_at);
    else
        printf("  覆盖                : 未发生（最老记录 seq 没动，缓冲区还没满）\n");
    if (base.oldest_seq >= 0 && st.oldest_seq > base.oldest_seq)
        printf("  本轮共挤掉旧记录    : %ld 条（seq %ld → %ld）\n",
               st.oldest_seq - base.oldest_seq, base.oldest_seq, st.oldest_seq);

    printf("  基线首条还在吗      : %s\n",
           strstr(st.oldest, base.oldest) ? "在" : "不在（已被覆盖）");
    printf("  最早存活的 %s  : %s\n", g_tag,
           st.tagged ? shorten(st.oldest_tag, tmp, sizeof tmp) : "(无)");
    printf("  最新记录            : %s\n", shorten(st.newest, tmp, sizeof tmp));

    {
        printf("\n=== 账目核对 ===\n");
        if (st.total == st.tagged) {
            long per = (long)strlen(g_tag) + 1 + 5 + 1 + pay + 1;  /* +1 = 换行 */
            printf("  缓冲区此刻 100%% 是本轮消息 → 可直接读出纯容量\n");
            printf("  稳态记录数          : %ld 条\n", st.total);
            printf("  单条文本(含换行)    : %ld 字节 = strlen(\"%s \")+5 位序号+1 空格+%ld 载荷+1 换行\n",
                   per, g_tag, pay);
            printf("  %ld × %-9ld      : %zu 字节 ≈ %.1f KiB\n",
                   st.total, per, st.textbytes, st.textbytes / 1024.0);
            printf("  缓冲区标称          : %d 字节 = %d KiB\n", ringsz, ringsz >> 10);
            printf("  差额（描述符+对齐）  : %ld 字节（%.1f%%）\n",
                   (long)(ringsz - st.textbytes),
                   100.0 * (double)(ringsz - (long)st.textbytes) / ringsz);
            printf("  → 纯文本能用的约 %.1f%%，其余是每条记录的描述符与对齐开销\n",
                   100.0 * (double)st.textbytes / ringsz);
        } else {
            printf("  缓冲区里还有 %ld 条非本轮记录（长度与本轮不同），\n",
                   st.total - st.tagged);
            printf("  要读出「纯容量」需要再灌一轮，把杂记录全部挤出去。\n");
            printf("  当前记录总数 %ld（本轮 %ld）\n", st.total, st.tagged);
        }
    }

    printf("\n提示:\n");
    printf("  · 想让 dmesg 不再显示这些测试消息，用 sudo dmesg -C（只挪游标，不擦记录）\n");
    printf("  · journald 已同步抄走全部记录：journalctl -k | grep -c %s\n", g_tag);
    printf("  · 缓冲区里的记录只能被新消息挤掉，用户态无法真正擦除\n");
    close(wfd);
    return 0;
}
