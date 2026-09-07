/*
 * ptab_lab.c — 分区表 / 设备节点 对照实验（全 C，无 shell、无 Python）
 *
 * 要回答的问题：
 *   Q1. 分区信息到底存在哪里？（SD 卡介质 vs 内核内存）
 *   Q2. /dev/mmcblk0p1 这个"文件名"存在哪里？
 *   Q3. Quiz：分区表被破坏后，/dev/mmcblk0 还在吗？mmcblk0p1 还在吗？
 *
 * 手段：只用 pread/pwrite + ioctl(BLKRRPART / BLKGETSIZE64) + statfs + scandir，
 *       每一步结论都能在真机上复现。
 *
 * 编译：gcc -O2 -Wall -Wextra -o ptab_lab ptab_lab.c
 * 运行：所有子命令都需要 root（读块设备 / 操作 loop 设备）
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <libgen.h>
#include <linux/fs.h>
#include <linux/loop.h>
#include <mntent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define SECTOR 512u
#define MBR_PART_OFF 0x1BEu     /* 4 个 16 字节分区项的起点 */
#define MBR_SIG_OFF 0x1FEu      /* 0x55 0xAA */
#define MBR_DISKSIG_OFF 0x1B8u  /* 4 字节磁盘签名 -> udev by-partuuid 前缀 */

static const char *prog = "ptab_lab";

static void die(const char *what)
{
    fprintf(stderr, "%s: %s: %s\n", prog, what, strerror(errno));
    exit(EXIT_FAILURE);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* ---------- 块设备原语 ---------- */

static int open_dev(const char *path, int flags)
{
    int fd = open(path, flags);
    if (fd < 0)
        die(path);
    return fd;
}

/* 块设备走 BLKGETSIZE64；普通镜像文件退化为 fstat —— 同一套代码既能玩真盘也能玩文件 */
static uint64_t dev_bytes(int fd)
{
    uint64_t n = 0;
    struct stat st;
    if (ioctl(fd, BLKGETSIZE64, &n) == 0)
        return n;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
        return (uint64_t)st.st_size;
    die("BLKGETSIZE64/fstat");
    return 0;
}

static void rw_lba(int fd, uint64_t lba, void *buf, size_t len, int writing)
{
    off_t off = (off_t)lba * SECTOR;
    ssize_t n = writing ? pwrite(fd, buf, len, off) : pread(fd, buf, len, off);
    if (n < 0)
        die(writing ? "pwrite" : "pread");
    if ((size_t)n != len) {
        fprintf(stderr, "%s: short %s at LBA %" PRIu64 "\n", prog,
                writing ? "write" : "read", lba);
        exit(EXIT_FAILURE);
    }
}

/* ---------- MBR 解析 ---------- */

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le32(unsigned char *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff; p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static double mb(uint64_t sectors) { return (double)sectors * SECTOR / 1048576.0; }

static void cmd_dump(const char *dev)
{
    unsigned char mbr[SECTOR], gpt[SECTOR];
    int fd = open_dev(dev, O_RDONLY);
    uint64_t bytes = dev_bytes(fd);

    rw_lba(fd, 0, mbr, SECTOR, 0);
    rw_lba(fd, 1, gpt, SECTOR, 0);

    printf("== %s ==\n", dev);
    printf("  容量            : %" PRIu64 " 字节 (%.1f MiB, %" PRIu64 " 个 512B 扇区)\n",
           bytes, (double)bytes / 1048576.0, bytes / SECTOR);

    int sig_ok = (mbr[MBR_SIG_OFF] == 0x55 && mbr[MBR_SIG_OFF + 1] == 0xAA);
    uint32_t dsig = le32(mbr + MBR_DISKSIG_OFF);
    int is_gpt = memcmp(gpt, "EFI PART", 8) == 0;

    printf("  LBA0 结尾签名   : %02x %02x  -> %s\n", mbr[MBR_SIG_OFF],
           mbr[MBR_SIG_OFF + 1], sig_ok ? "0x55AA，有效 MBR" : "无效，内核不认这是一张分区表");
    printf("  LBA0 磁盘签名   : %08x  -> udev 会据此生成 /dev/disk/by-partuuid/%08x-01 ...\n",
           dsig, dsig);
    printf("  LBA1            : %s\n", is_gpt ? "\"EFI PART\" -> 这是一张 GPT 分区表"
                                              : "不是 GPT header（MBR 分区表）");

    printf("  分区项（介质上实实在在写的 4x16 字节）:\n");
    printf("    #  status  type   start LBA     sectors    size\n");
    for (int i = 0; i < 4; i++) {
        const unsigned char *e = mbr + MBR_PART_OFF + i * 16;
        uint32_t start = le32(e + 8), nsect = le32(e + 12);
        printf("    %d  0x%02x    0x%02x    %-12u %-10u %.1f MiB%s\n", i + 1, e[0], e[4],
               start, nsect, mb(nsect), (nsect == 0) ? "   (空项)" : "");
    }

    /* 关键：把 16 字节摊开，证明里面没有任何位置存 "mmcblk0p1" 这种名字 */
    const unsigned char *e0 = mbr + MBR_PART_OFF;
    printf("  分区项 #1 的 16 个字节: ");
    for (int i = 0; i < 16; i++)
        printf("%02x ", e0[i]);
    printf("\n    [0]=引导标志 [1..3]=起始CHS [4]=类型 [5..7]=结束CHS [8..11]=起始LBA [12..15]=扇区数\n");
    printf("    -> 16 字节全是数字，没有一个字节记录设备文件名；名字是内核拼出来的\n");
    close(fd);
}

/* ---------- 造一张合法的 MBR ---------- */

static void cmd_mkpt(const char *dev, uint32_t sig)
{
    unsigned char mbr[SECTOR];
    int fd = open_dev(dev, O_RDWR);
    uint64_t total = dev_bytes(fd) / SECTOR;
    uint32_t start = 2048;                       /* 1 MiB 对齐 */
    uint32_t nsect = (uint32_t)(total - start);

    memset(mbr, 0, sizeof(mbr));
    put_le32(mbr + MBR_DISKSIG_OFF, sig);        /* 磁盘签名，供 by-partuuid 用 */
    unsigned char *e = mbr + MBR_PART_OFF;
    e[0] = 0x00;                                 /* 不可引导 */
    e[4] = 0x83;                                 /* Linux filesystem */
    put_le32(e + 8, start);
    put_le32(e + 12, nsect);
    mbr[MBR_SIG_OFF] = 0x55;
    mbr[MBR_SIG_OFF + 1] = 0xAA;

    rw_lba(fd, 0, mbr, SECTOR, 1);
    printf("写入 MBR: 磁盘签名 %08x, p1 = LBA %u + %u 扇区 (%.1f MiB), 签名 55AA\n",
           sig, start, nsect, mb(nsect));
    close(fd);
}

/* ---------- 破坏分区表 ---------- */

static void cmd_corrupt(const char *dev, const char *mode)
{
    unsigned char mbr[SECTOR];
    int fd = open_dev(dev, O_RDWR);
    rw_lba(fd, 0, mbr, SECTOR, 0);

    if (!strcmp(mode, "sig")) {
        mbr[MBR_SIG_OFF] = 0x00;
        mbr[MBR_SIG_OFF + 1] = 0x00;
        printf("破坏方式 = sig   : 只把 0x55AA 改成 0x0000（分区项字节原封不动）\n");
    } else if (!strcmp(mode, "entry")) {
        memset(mbr + MBR_PART_OFF, 0, 64);
        printf("破坏方式 = entry : 清空 64 字节分区项，签名 0x55AA 保留\n");
    } else if (!strcmp(mode, "all")) {
        memset(mbr, 0, SECTOR);
        printf("破坏方式 = all   : 整个 LBA0 清零（等价于 dd 掉前 512B）\n");
    } else {
        fprintf(stderr, "mode 只能是 sig | entry | all\n");
        exit(EXIT_FAILURE);
    }
    rw_lba(fd, 0, mbr, SECTOR, 1);
    close(fd);
}

/* ---------- 让内核重读分区表 ---------- */

static void cmd_reread(const char *dev)
{
    int fd = open_dev(dev, O_RDWR);
    if (ioctl(fd, BLKRRPART, 0) < 0)
        printf("  BLKRRPART(%s) -> %s (errno=%d)\n", dev, strerror(errno), errno);
    else
        printf("  BLKRRPART(%s) -> 成功，内核已重读介质上的分区表\n", dev);
    close(fd);
    sleep_ms(400);   /* 给 devtmpfs/udev 一点时间建节点 */
}

/* ---------- 枚举 /dev 下的节点 ---------- */

static void make_dev_path(char *out, size_t n, const char *name)
{
    if (name[0] == '/')
        snprintf(out, n, "%s", name);
    else
        snprintf(out, n, "/dev/%s", name);
}

static void cmd_lsdev(const char *prefix, const char *tag)
{
    DIR *d = opendir("/dev");
    if (!d)
        die("/dev");
    struct dirent *de;
    char path[512];
    int found = 0;

    printf("  /dev 下匹配 \"%s*\" 的节点%s%s%s\n", prefix, tag ? " [" : "",
           tag ? tag : "", tag ? "]" : "");
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, prefix, strlen(prefix)))
            continue;
        snprintf(path, sizeof(path), "/dev/%s", de->d_name);
        struct stat st;
        if (stat(path, &st) < 0)
            continue;
        printf("    %-24s %s  major:minor = %u:%u\n", path,
               S_ISBLK(st.st_mode) ? "块设备" : (S_ISDIR(st.st_mode) ? "目录  " : "其它  "),
               major(st.st_rdev), minor(st.st_rdev));
        found++;
    }
    if (!found)
        printf("    (无)\n");
    closedir(d);
}

/* ---------- 这个路径到底属于哪个文件系统 ---------- */

static int find_mount(const char *path, char *src, char *mp, char *fst, size_t n)
{
    FILE *f = setmntent("/proc/mounts", "r");
    struct mntent *me;
    size_t best = 0;
    int found = 0;
    char best_src[256] = "", best_mp[256] = "", best_fst[256] = "";

    if (!f)
        return 0;
    while ((me = getmntent(f))) {
        size_t l = strlen(me->mnt_dir);
        int match;
        if (l == 0)
            continue;
        if (strcmp(me->mnt_dir, "/") == 0)
            match = 1;
        else
            match = (strncmp(path, me->mnt_dir, l) == 0 && (path[l] == '\0' || path[l] == '/'));
        if (match && l >= best) {
            best = l;
            snprintf(best_src, sizeof(best_src), "%s", me->mnt_fsname);
            snprintf(best_mp, sizeof(best_mp), "%s", me->mnt_dir);
            snprintf(best_fst, sizeof(best_fst), "%s", me->mnt_type);
            found = 1;
        }
    }
    endmntent(f);
    if (found) {
        snprintf(src, n, "%s", best_src);
        snprintf(mp, n, "%s", best_mp);
        snprintf(fst, n, "%s", best_fst);
    }
    return found;
}

static void cmd_fstype(const char *path)
{
    struct statfs sfs;
    char src[256], mp[256], fst[256];

    if (statfs(path, &sfs) < 0)
        die(path);
    if (find_mount(path, src, mp, fst, sizeof(src)))
        printf("  %-12s 挂载点 %-14s 类型 %-10s 源 %s\n", path, mp, fst, src);
    printf("  %-12s statfs f_type = 0x%08lx", path, (unsigned long)sfs.f_type);
    if (sfs.f_type == 0x01021994UL)
        printf("  = TMPFS_MAGIC（devtmpfs 借用 shmem 实现，共用 tmpfs 的 magic，\n"
               "                  所以别只看 magic，要看 /proc/mounts 的 type 字段）\n");
    else if (sfs.f_type == 0x00001373UL)
        printf("  = DEVFS_SUPER_MAGIC\n");
    else
        printf("\n");
}

/* 两个路径是否属于同一个文件系统实例（st_dev 相同 = 同一个 superblock） */
static void cmd_cmpdev(const char *a, const char *b)
{
    struct stat sa, sb;
    if (stat(a, &sa) < 0)
        die(a);
    if (stat(b, &sb) < 0)
        die(b);
    printf("  %-14s st_dev = %llu (major:minor = %u:%u)\n", a,
           (unsigned long long)sa.st_dev, major(sa.st_dev), minor(sa.st_dev));
    printf("  %-14s st_dev = %llu (major:minor = %u:%u)\n", b,
           (unsigned long long)sb.st_dev, major(sb.st_dev), minor(sb.st_dev));
    printf("  -> %s\n", sa.st_dev == sb.st_dev
           ? "st_dev 相同：两个路径在同一个文件系统里"
           : "st_dev 不同：两个路径分属不同的文件系统实例（/dev 根本不在 SD 卡的 ext4 里）");
}

/* ---------- loop 设备：把一个普通文件变成"块设备" ---------- */

static void cmd_loop_attach(const char *file, int partscan, char *out, size_t outsz)
{
    int ctl = open_dev("/dev/loop-control", O_RDWR);
    int n = ioctl(ctl, LOOP_CTL_GET_FREE);
    if (n < 0)
        die("LOOP_CTL_GET_FREE");
    close(ctl);

    char loopdev[64];
    snprintf(loopdev, sizeof(loopdev), "/dev/loop%d", n);
    int lfd = open_dev(loopdev, O_RDWR);
    int bfd = open_dev(file, O_RDWR);

    struct loop_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = bfd;
    cfg.info.lo_flags = partscan ? LO_FLAGS_PARTSCAN : 0;
    if (ioctl(lfd, LOOP_CONFIGURE, &cfg) < 0)
        die("LOOP_CONFIGURE");

    printf("  LOOP_CONFIGURE: %s <- %s (partscan=%d)\n", loopdev, file, partscan);
    close(bfd);
    close(lfd);
    sleep_ms(400);
    if (out && outsz) {
        snprintf(out, outsz, "%s", loopdev);
        FILE *f = fopen("/tmp/.ptab_lab_loop", "w");   /* 方便手工分步复现 */
        if (f) {
            fprintf(f, "%s\n", loopdev);
            fclose(f);
        }
    }
}

static void cmd_loop_detach(const char *loopdev)
{
    char path[320];
    make_dev_path(path, sizeof(path), loopdev);
    int lfd = open_dev(path, O_RDWR);
    if (ioctl(lfd, LOOP_CLR_FD, 0) < 0)
        die("LOOP_CLR_FD");
    printf("  LOOP_CLR_FD(%s) -> 已解绑（等价于把卡拔掉）\n", path);
    close(lfd);
    sleep_ms(400);
}

static void cmd_mkimg(const char *path, long mib)
{
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        die(path);
    if (ftruncate(fd, (off_t)mib * 1048576LL) < 0)
        die("ftruncate");
    printf("  创建稀疏镜像 %s = %ld MiB\n", path, mib);
    close(fd);
}

/* ---------- 完整流程：loop 镜像 ---------- */

static void banner(const char *s)
{
    printf("\n----------------------------------------------------------------\n");
    printf("%s\n", s);
    printf("----------------------------------------------------------------\n");
}

static void show_sysfs_start(const char *devpath)
{
    char base[64], sysfs[256], sysfs2[256];
    snprintf(base, sizeof(base), "%s", devpath);
    char *b = basename(base);
    snprintf(sysfs, sizeof(sysfs), "/sys/block/%s/%sp1/start", b, b);
    snprintf(sysfs2, sizeof(sysfs2), "/sys/block/%s/%sp1/size", b, b);
    FILE *f = fopen(sysfs, "r");
    if (f) {
        unsigned long long v = 0;
        if (fscanf(f, "%llu", &v) == 1)
            printf("  %s = %llu  <- 内核读完分区表后，在内存里记下的起点\n", sysfs, v);
        fclose(f);
    }
    f = fopen(sysfs2, "r");
    if (f) {
        unsigned long long v = 0;
        if (fscanf(f, "%llu", &v) == 1)
            printf("  %s = %llu  <- 同上，扇区数也来自介质上的分区项\n", sysfs2, v);
        fclose(f);
    }
}

static void cmd_lab(const char *img, const char *realdev)
{
    char loop[64];

    banner("第 0 步：真实 SD 卡 —— 分区表躺在介质上，内核只是读它");
    cmd_dump(realdev);
    show_sysfs_start(realdev);
    cmd_fstype("/dev");
    cmd_cmpdev("/", "/dev");

    banner("第 1 步：64 MiB 空镜像（前 512B 全零）绑成 loop 设备，partscan=0");
    cmd_mkimg(img, 64);
    cmd_loop_attach(img, 0, loop, sizeof(loop));
    cmd_lsdev("loop", "镜像里没有分区表");

    banner("第 1.5 步【坑】：没有 partscan 的 loop，BLKRRPART 直接 EINVAL");
    cmd_mkpt(loop, 0xdeadbeef);
    cmd_reread(loop);
    cmd_lsdev("loop", "重读失败，所以仍然没有分区节点");
    cmd_loop_detach(loop);

    banner("第 2 步：重新以 partscan=1 绑定（内核会自动扫分区表）");
    cmd_loop_attach(img, 1, loop, sizeof(loop));
    cmd_lsdev("loop", "刚绑定，内核就扫到了 MBR");

    banner("第 3 步【Quiz 上半】：只抹掉 0x55AA 签名，但【不】通知内核");
    cmd_corrupt(loop, "sig");
    cmd_lsdev("loop", "分区表已坏，内核还蒙在鼓里");

    banner("第 4 步【Quiz 下半】：BLKRRPART 重读（等价于拔卡重插 / 重启）");
    cmd_reread(loop);
    cmd_lsdev("loop", "重读之后");
    printf("  ^ 整盘节点 %s 还在，分区节点 %sp1 没了 —— 这就是 Quiz 的答案\n", loop, loop);

    banner("第 5 步：把分区表原样写回去，p1 又冒出来");
    cmd_mkpt(loop, 0xdeadbeef);
    cmd_reread(loop);
    cmd_lsdev("loop", "分区表复原");

    banner("第 6 步：只清空 64 字节分区项、保留 0x55AA 签名，再重读");
    cmd_corrupt(loop, "entry");
    cmd_reread(loop);
    cmd_lsdev("loop", "分区项空但签名在");

    banner("第 7 步：整个 LBA0 清零（最彻底的破坏），再重读");
    cmd_corrupt(loop, "all");
    cmd_reread(loop);
    cmd_lsdev("loop", "LBA0 全零");
    cmd_dump(loop);

    banner("第 8 步：把卡『拔掉』（loop 解绑）—— 整盘节点和分区节点一起消失");
    cmd_loop_detach(loop);
    cmd_lsdev("loop", "解绑之后，只剩空闲的 loop 设备池");

    banner("结论见 README.md");
}

/* ---------- 完整流程：真实空盘（NVMe） ---------- */

static void cmd_disklab(const char *dev)
{
    char base[64];
    snprintf(base, sizeof(base), "%s", dev);
    char *b = basename(base);

    banner("真盘 第 1 步：先看这张盘现在的样子（应当是一张空盘）");
    cmd_dump(dev);
    cmd_lsdev(b, "当前分区节点");

    banner("真盘 第 2 步：写一张 MBR 到介质上，让内核重读");
    cmd_mkpt(dev, 0x5a5a5a5a);
    cmd_reread(dev);
    cmd_lsdev(b, "重读之后");
    show_sysfs_start(dev);

    banner("真盘 第 3 步：只抹掉 0x55AA，不通知内核 -> 分区节点【还在】");
    cmd_corrupt(dev, "sig");
    cmd_lsdev(b, "已破坏但未重读");

    banner("真盘 第 4 步：BLKRRPART 重读 -> 分区节点【消失】，整盘节点【还在】");
    cmd_reread(dev);
    cmd_lsdev(b, "重读之后");
    cmd_dump(dev);

    banner("真盘 第 5 步：恢复现场（LBA0 清零，盘回到实验前状态）");
    cmd_corrupt(dev, "all");
    cmd_reread(dev);
    cmd_lsdev(b, "恢复之后");
    cmd_dump(dev);
}

static void usage(void)
{
    fprintf(stderr,
        "用法: %s <子命令> [参数]\n"
        "  dump    <块设备>                  解析并打印介质上的分区表\n"
        "  mkimg   <文件> <MiB>              创建稀疏镜像\n"
        "  mkpt    <块设备> [磁盘签名]        写一张合法 MBR（p1 占满剩余空间）\n"
        "  corrupt <块设备> <sig|entry|all>   以不同方式破坏分区表\n"
        "  reread  <块设备>                  BLKRRPART：让内核重读分区表\n"
        "  lsdev   <前缀> [说明]             枚举 /dev 下匹配的节点\n"
        "  fstype  <路径>                    这个路径属于哪个文件系统\n"
        "  cmpdev  <路径A> <路径B>            两者是否同一个文件系统实例\n"
        "  attach  <文件> [--partscan]       把文件绑成 loop 块设备\n"
        "  detach  <loop设备>                解绑（等价于拔卡）\n"
        "  lab     <镜像文件> <真实块设备>    跑完整 loop 对照流程\n"
        "  disklab <真实块设备>              在真实空盘上跑破坏-恢复流程\n",
        prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        usage();
    const char *cmd = argv[1];

    if (!strcmp(cmd, "dump") && argc == 3)            cmd_dump(argv[2]);
    else if (!strcmp(cmd, "mkimg") && argc == 4)      cmd_mkimg(argv[2], atol(argv[3]));
    else if (!strcmp(cmd, "mkpt") && argc >= 3)
        cmd_mkpt(argv[2], argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 16) : 0xdeadbeef);
    else if (!strcmp(cmd, "corrupt") && argc == 4)    cmd_corrupt(argv[2], argv[3]);
    else if (!strcmp(cmd, "reread") && argc == 3)     cmd_reread(argv[2]);
    else if (!strcmp(cmd, "lsdev") && argc >= 3)      cmd_lsdev(argv[2], argv[3]);
    else if (!strcmp(cmd, "fstype") && argc == 3)     cmd_fstype(argv[2]);
    else if (!strcmp(cmd, "cmpdev") && argc == 4)     cmd_cmpdev(argv[2], argv[3]);
    else if (!strcmp(cmd, "attach") && argc >= 3)
        cmd_loop_attach(argv[2], (argc >= 4 && !strcmp(argv[3], "--partscan")) ? 1 : 0,
                        NULL, 0);
    else if (!strcmp(cmd, "detach") && argc == 3)     cmd_loop_detach(argv[2]);
    else if (!strcmp(cmd, "lab") && argc == 4)        cmd_lab(argv[2], argv[3]);
    else if (!strcmp(cmd, "disklab") && argc == 3)    cmd_disklab(argv[2]);
    else usage();
    return 0;
}
