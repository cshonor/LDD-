/*
 * 03-module-param/experiments/param-perm/param_probe.c
 *
 * 目的：精确测量每个模块参数在 sysfs 上的【真实可写性】。
 *
 * 为什么不用 shell 的 echo 测？
 *   echo 只能告诉你"Permission denied"，而且这个报错可能来自三种完全不同的原因：
 *     (a) 文件 mode 没写位，且你是普通用户        → EACCES
 *     (b) 文件 mode 没写位，你是 root 但内核拒绝   → EACCES / EPERM
 *     (c) sysfs 的 store 回调不存在或拒绝          → EPERM / EINVAL
 *   而且 `sudo echo x > file` 有个经典陷阱：重定向是由【当前 shell】打开的，
 *   sudo 只作用于 echo 命令本身。所以那种写法失败，不能证明 root 写不了。
 *
 * 这个程序直接 open(O_WRONLY) + write()，把 errno 原样打出来，
 * 并且分别以【普通用户】和【root】各跑一遍，对比才有意义。
 *
 * 用法：
 *   make
 *   ./param_probe             # 普通用户
 *   sudo ./param_probe        # root，对比用
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define MODNAME "param_demo"
#define SYSDIR  "/sys/module/" MODNAME "/parameters"

/*
 * 每个参数要写入的测试值。
 * 类型要对：给 bool 写 "1234" 会被 param_set_bool 拒绝（EINVAL），
 * 那测出来的就不是权限问题了，所以这里按类型分别给值。
 */
struct probe {
	const char *name;   /* sysfs 文件名 = C 变量名 */
	const char *wval;   /* 测试写入值 */
	const char *note;   /* 这一项想验证什么 */
};

static const struct probe PROBES[] = {
	{ "my_int",    "1234", "perm=0644 int，基线：应该能写" },
	{ "my_ro_int", "5678", "perm=0444 int，Quiz 主角" },
	{ "my_hidden", "9",    "perm=0，预期 sysfs 里根本没有这个文件" },
	{ "my_wo_int", "88",   "perm=0200 只写：root 也 cat 不出来" },
	{ "my_bool",   "1",    "perm=0644 bool" },
	{ "my_str",    "probe","perm=0644 charp，内核会重新 strdup" },
	{ "my_arr",    "7,8",  "perm=0644 数组，逗号分隔" },
	{ "my_ro_arr", "5,6",  "perm=0444 数组" },
	{ "cb_int",    "4321", "module_param_cb，写了应该能看到内核日志" },
	{ NULL, NULL, NULL }
};

/* 把 struct stat 的 mode 格式化成 -rw-r--r-- 这种形式 */
static void fmt_mode(mode_t m, char *out, size_t n)
{
	snprintf(out, n, "%c%c%c%c%c%c%c%c%c%c",
		 S_ISDIR(m) ? 'd' : '-',
		 (m & S_IRUSR) ? 'r' : '-', (m & S_IWUSR) ? 'w' : '-',
		 (m & S_IXUSR) ? 'x' : '-',
		 (m & S_IRGRP) ? 'r' : '-', (m & S_IWGRP) ? 'w' : '-',
		 (m & S_IXGRP) ? 'x' : '-',
		 (m & S_IROTH) ? 'r' : '-', (m & S_IWOTH) ? 'w' : '-',
		 (m & S_IXOTH) ? 'x' : '-');
}

/* 读 sysfs 文件全部内容（去掉尾部换行） */
static int read_file(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0)
		return -1;
	buf[r] = '\0';
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
		buf[--r] = '\0';
	return 0;
}

int main(void)
{
	const struct probe *p;
	char modestr[16];

	printf("════ sysfs 模块参数可写性探测 ════\n");
	printf("模块      : %s\n", MODNAME);
	printf("有效 UID  : %d  (%s)\n\n",
	       (int)geteuid(), geteuid() == 0 ? "root（带 CAP_DAC_OVERRIDE）" : "普通用户");

	printf("%-11s %-11s %-8s %-9s %s\n",
	       "参数", "mode", "读", "写", "说明");
	printf("---------------------------------------------------------------"
	       "------------------\n");

	for (p = PROBES; p->name; p++) {
		char path[256], old[512], new[512];
		struct stat st;
		int fd, rderr;
		ssize_t w;
		const char *wres;

		snprintf(path, sizeof(path), "%s/%s", SYSDIR, p->name);

		/* ① 文件是否存在 + 权限位 */
		if (stat(path, &st) < 0) {
			printf("%-11s %-11s %-8s %-9s %s\n",
			       p->name, "(不存在)", "-",
			       errno == ENOENT ? "ENOENT" : strerror(errno),
			       p->note);
			continue;
		}
		fmt_mode(st.st_mode, modestr, sizeof(modestr));

		/* ② 读测试
		 * 读失败【不能】直接跳过：perm=0200 的参数本来就只允许写，
		 * 读不出来是预期行为，写测试还得照做。
		 */
		rderr = 0;
		if (read_file(path, old, sizeof(old)) < 0) {
			rderr = errno;
			snprintf(old, sizeof(old), "(不可读)");
		}

		/* ③ 写测试：这是核心 */
		fd = open(path, O_WRONLY);
		if (fd < 0) {
			wres = (errno == EACCES) ? "EACCES" :
			       (errno == EPERM)  ? "EPERM"  : strerror(errno);
			printf("%-11s %-11s %-8s %-9s %s\n",
			       p->name, modestr,
			       rderr ? "EACCES" : "OK", wres, p->note);
			continue;
		}

		w = write(fd, p->wval, strlen(p->wval));
		if (w < 0) {
			wres = (errno == EINVAL) ? "EINVAL(值非法)" : strerror(errno);
			printf("%-11s %-11s %-8s %-9s %s\n",
			       p->name, modestr, "OK", wres, p->note);
			close(fd);
			continue;
		}
		close(fd);

		/* ④ 写成功了：读回来验证（读不了就算了），然后恢复原值保持现场干净 */
		if (rderr)
			snprintf(new, sizeof(new), "(不可读)");
		else
			read_file(path, new, sizeof(new));
		printf("%-11s %-11s %-8s %-9s %s\n",
		       p->name, modestr,
		       rderr ? "EACCES" : "OK",
		       "OK", p->note);
		printf("%s", "            ");
		printf("原值 \"%s\" → 写入 \"%s\" → 读回 \"%s\"",
		       old, p->wval, new);

		if (!rderr) {
			fd = open(path, O_WRONLY);
			if (fd >= 0) {
				if (write(fd, old, strlen(old)) < 0)
					printf("   (恢复原值失败: %s)", strerror(errno));
				close(fd);
			}
		}
		printf("\n");
	}

	/*
	 * ⑤ 非法值测试
	 *
	 * shell 里做这件事会看到 "I/O error"，看上去像是设备坏了，其实不是。
	 * 用 C 直接读 errno 才能看到真实的错误码。
	 * 顺带对比：给 bool 写 1234（越界但类型对）会是什么错。
	 */
	printf("\n=== 写入非法值时的真实 errno ===\n");
	printf("%-11s %-10s %-10s %s\n", "参数", "写入值", "errno", "说明");
	printf("---------------------------------------------------------\n");

	struct {
		const char *name, *val, *note;
	} bad[] = {
		{ "my_int",  "abc",  "int 参数收到非数字" },
		{ "my_int",  "99999999999999", "int 溢出（超过 int 范围）" },
		{ "my_bool", "2",    "bool 参数收到 0/1/y/n 之外的值" },
		{ "my_arr",  "1,2,3,4,5,6,7,8,9", "数组超过声明容量 8" },
		{ NULL, NULL, NULL }
	};

	for (int i = 0; bad[i].name; i++) {
		char path[256];
		int fd;
		ssize_t w;

		snprintf(path, sizeof(path), "%s/%s", SYSDIR, bad[i].name);
		fd = open(path, O_WRONLY);
		if (fd < 0) {
			printf("%-11s %-10s %-10s (打不开: %s)\n",
			       bad[i].name, bad[i].val, strerror(errno), bad[i].note);
			continue;
		}
		w = write(fd, bad[i].val, strlen(bad[i].val));
		if (w < 0) {
			printf("%-11s %-10s %-10s %s\n",
			       bad[i].name, bad[i].val,
			       errno == EINVAL ? "EINVAL" :
			       errno == EIO    ? "EIO"    :
			       errno == EACCES ? "EACCES" : strerror(errno),
			       bad[i].note);
		} else {
			printf("%-11s %-10s %-10s %s\n",
			       bad[i].name, bad[i].val, "(成功)", bad[i].note);
		}
		close(fd);
	}

	printf("\n");
	printf("提示：本程序要以【普通用户】和【sudo】各跑一次，对比 \"写\" 那一列。\n");
	printf("      root 那次如果 0444 的参数仍然显示 EACCES，就说明内核层面有额外检查；\n");
	printf("      如果显示 OK，就说明 CAP_DAC_OVERRIDE 绕过了文件 mode。\n");
	return 0;
}
