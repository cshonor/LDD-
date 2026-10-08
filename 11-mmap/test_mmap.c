// test_mmap.c — 用户态测试：mmap 直接读写内核缓冲区（零 syscall 数据路径）
// 在树莓派上编译：gcc -O2 -Wall -o test_mmap test_mmap.c
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define MAP_SIZE (16 * 1024)	/* 与内核侧 BUF_SIZE 一致 */

int main(void)
{
	int fd;
	char *p;
	const char *msg = "hello from userspace (via mmap, no syscall)";

	fd = open("/dev/mmapbuf", O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	/* MAP_SHARED 关键：PRIVATE 是写时复制，写不进内核缓冲区 */
	p = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		close(fd);
		return 1;
	}

	/* 之后就是普通内存访问 —— 没有 read/write syscall，没有 copy_to_user */
	printf("kernel says: %s\n", p);

	strcpy(p, msg);
	printf("userspace wrote: %s\n", p);

	munmap(p, MAP_SIZE);
	close(fd);
	printf("done (unmapped, safe to rmmod now)\n");
	return 0;
}
