/* test_char.c —— misc_echo 字符设备的用户态测试程序
 *
 * 说明"设备是文件"：这里没有任何 ioctl/特殊 API，
 * 就是普通 open/read/write——跟读一个普通文件写法完全一样。
 * 差别全部藏在内核的 file_operations 回调里。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

int main(void)
{
    char buf[256];
    ssize_t n;

    /* 1. write 一句进去 */
    int fd = open("/dev/misc_echo", O_WRONLY);
    if (fd < 0) { perror("open(w)"); return 1; }
    const char *s = "hello from userspace";
    n = write(fd, s, strlen(s));
    printf("write() -> %zd bytes\n", n);
    close(fd);

    /* 2. read 回来（新 open，文件位置归零——fd 才带位置） */
    fd = open("/dev/misc_echo", O_RDONLY);
    if (fd < 0) { perror("open(r)"); return 1; }
    n = read(fd, buf, sizeof buf - 1);
    if (n < 0) { perror("read"); return 1; }
    buf[n] = '\0';
    printf("read()  -> %zd bytes: \"%s\"\n", n, buf);

    /* 3. 读第二次：已经到 EOF，驱动返回 0 */
    n = read(fd, buf, sizeof buf - 1);
    printf("read again -> %zd bytes (0 = EOF)\n", n);
    close(fd);
    return 0;
}
