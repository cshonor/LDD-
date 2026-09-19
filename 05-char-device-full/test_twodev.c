/* test_twodev.c —— twodev 用户态验证
 * 验证三件事：
 *   1. 双实例隔离：twodev0 / twodev1 各写各读，互不串扰（private_data 生效）
 *   2. ioctl _IOR/_IOW：读缓冲上限、改缓冲上限（写 300 字节按新上限截断）
 *   3. ioctl _IOWR：进一个数出 ×2
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#define TWODEV_MAGIC    'W'
#define TWODEV_GET_BUFSZ _IOR(TWODEV_MAGIC, 1, int)
#define TWODEV_SET_BUFSZ _IOW(TWODEV_MAGIC, 2, int)
#define TWODEV_DOUBLE    _IOWR(TWODEV_MAGIC, 3, int)

static void dev_round(const char *name, const char *msg)
{
    char buf[512];
    int fd = open(name, O_RDWR);
    if (fd < 0) { perror(name); return; }

    ssize_t n = write(fd, msg, strlen(msg));
    printf("[%s] write %zd B\n", name, n);

    n = read(fd, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    printf("[%s] read  %zd B: \"%s\"\n", name, n, buf);

    int sz = 0;
    ioctl(fd, TWODEV_GET_BUFSZ, &sz);
    printf("[%s] GET_BUFSZ = %d\n", name, sz);
    close(fd);
}

int main(void)
{
    /* 1. 双实例隔离 */
    dev_round("/dev/twodev0", "hello-from-dev0");
    dev_round("/dev/twodev1", "HELLO-FROM-DEV1");

    /* 2. _IOW 改上限 → 写长串被截断 */
    int fd = open("/dev/twodev0", O_RDWR);
    int newsz = 8, sz = 0;
    ioctl(fd, TWODEV_SET_BUFSZ, &newsz);
    const char *longmsg = "0123456789ABCDEFGHIJ";
    ssize_t n = write(fd, longmsg, strlen(longmsg));
    printf("[twodev0] after SET_BUFSZ=8, write %zu B -> accepted %zd B\n",
           strlen(longmsg), n);
    ioctl(fd, TWODEV_GET_BUFSZ, &sz);
    printf("[twodev0] GET_BUFSZ back = %d\n", sz);

    /* 3. _IOWR 双向 */
    int v = 21;
    ioctl(fd, TWODEV_DOUBLE, &v);
    printf("[twodev0] DOUBLE(21) -> %d\n", v);
    close(fd);
    return 0;
}
