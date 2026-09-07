/*
 * 03-module-param/param_demo.c — 内核模块参数（module_param）全家桶
 *
 * 功能：
 *   一次性把 module_param 家族的 7 种典型用法摆在一起，用真机回答这几个问题：
 *
 *     1. 权限位 0644 / 0444 / 0 在 sysfs 上到底长什么样？
 *     2. 数组参数第 3 个 &count 到底是"输入"还是"输出"？
 *     3. 运行时 echo 改参数，模块里的 C 变量真的变了吗？
 *     4. 普通 module_param 被改时模块能感知吗？（答：不能，除非用 module_param_cb）
 *
 * 完整操作：
 *   make
 *   sudo insmod param_demo.ko my_int=10 my_bool=1 my_str="hello" my_arr=10,20,30
 *   sudo dmesg | tail -20
 *   ls -l /sys/module/param_demo/parameters/     # 看权限位
 *   cat /sys/module/param_demo/parameters/my_int
 *   sudo sh -c 'echo 99 > /sys/module/param_demo/parameters/my_int'
 *   sudo rmmod param_demo                        # 卸载时打印"最终值"，验证被改过
 *   make clean
 *
 * 注意：
 *   模块参数不是用户的输入接口替代品 —— 它是"加载时/启动期配置开关"。
 *   真正的运行时控制应该走 ioctl / sysfs 自己的 attribute，参数只是图省事。
 */

/*
 * pr_fmt 必须定义在任何 #include 之前。
 * 定义之后，本文件里所有 pr_info() 都会自动带上 "param_demo: " 前缀，
 * 不用每条手写 KBUILD_MODNAME。原理见 02-log-levels 的 1.3 节。
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>       /* module_param / module_param_array / MODULE_PARM_DESC */
#include <linux/init.h>         /* __init / __exit */
#include <linux/printk.h>       /* pr_info */
#include <linux/kernel.h>       /* param_set_int / param_get_int（自定义回调要用） */
#include <linux/stat.h>         /* S_IRUGO / S_IWUSR 等权限宏 */
#include <linux/moduleparam.h>  /* struct kernel_param_ops / module_param_cb */
#include <linux/types.h>        /* bool */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("Demo of module_param: types, permissions, array, and callback");
MODULE_VERSION("0.1");

/* ==========================================================================
 * ① 整型，权限 0644（rw-r--r--）
 *
 * 0644 的含义常被误读为"所有人可读写"。拆开看：
 *   owner(u)  : 6 = rw-   root 可读写
 *   group(g)  : 4 = r--   只读
 *   other(o)  : 4 = r--   只读
 * 所以准确说法是"root 可写，所有人可读"。普通用户想写需要 0666，
 * 但那等于让任意进程改内核模块的全局变量 —— 别这么干。
 * ========================================================================== */
static int my_int = 0;
module_param(my_int, int, 0644);
MODULE_PARM_DESC(my_int, "int, perm=0644: root writable, world readable");

/* ==========================================================================
 * ② 整型，权限 0444（r--r--r--）—— Quiz 的主角
 *
 * 笔记里的说法是"用户不能写修改"。这句话只对普通用户成立。
 * root 带 CAP_DAC_OVERRIDE，能不能绕过 0444 是本次实测的重点，
 * 结论见 experiments/param-perm/ 与 docs/module-param.md。
 * ========================================================================== */
static int my_ro_int = 7;
module_param(my_ro_int, int, 0444);
MODULE_PARM_DESC(my_ro_int, "int, perm=0444: world readable (Quiz)");

/* ==========================================================================
 * ③ 整型，权限 0 —— 完全不出现在 sysfs
 *
 * 仍然可以在 insmod 命令行传值，但 /sys/module/.../parameters/ 下不会有文件。
 * 适合"只在加载时配置一次、之后不希望被任何人偷偷改"的参数。
 * ========================================================================== */
static int my_hidden = 0;
module_param(my_hidden, int, 0);
MODULE_PARM_DESC(my_hidden, "int, perm=0: load-time only, invisible in sysfs");

/* ==========================================================================
 * ③b 整型，权限 0200（-w-------）只写
 *
 * 加这一项是为了验证 kernfs 的检查是【对称】的：
 *   fs/kernfs/file.c 的 kernfs_fop_open() 里读、写位各查一次，
 *   缺哪个位就拒绝哪个方向，而且都不认 CAP_DAC_OVERRIDE。
 * 所以 0200 的参数文件，root 也 cat 不出来（EACCES）。
 * ========================================================================== */
static int my_wo_int = 3;
module_param(my_wo_int, int, 0200);
MODULE_PARM_DESC(my_wo_int, "int, perm=0200: write-only, even root cannot read it");

/* ==========================================================================
 * ④ bool 类型
 *
 * 注意类型名是 bool（不是 int），内核提供的解析函数接受
 *   1 / 0 / y / n / Y / N
 * 传别的值会在 insmod 阶段就被拒绝（模块根本加载不进来）。
 * ========================================================================== */
static bool my_bool = false;
module_param(my_bool, bool, 0644);
MODULE_PARM_DESC(my_bool, "bool, perm=0644: accepts 1/0/y/n/Y/N");

/* ==========================================================================
 * ⑤ charp（字符串指针）
 *
 * charp 的特殊之处：内核在设置时会 kstrdup 一份，模块里拿到的是内核
 * 分配的内存，不是用户传进来的地址。
 *
 * 一个真实存在的细节：模块卸载时内核不会替你 kfree 这块字符串
 * （params.c 的 destroy_params 只释放参数描述符数组，不释放 charp 的值）。
 * 单次加载卸载泄漏几十字节，实际无所谓，但要清楚这不是你管理的内存，
 * 别在 exit 里 kfree(my_str) —— 那会 double free。
 * ========================================================================== */
static char *my_str = "default";
module_param(my_str, charp, 0644);
MODULE_PARM_DESC(my_str, "charp, perm=0644: kernel strdup's it, do NOT kfree");

/* ==========================================================================
 * ⑥ 数组，权限 0644
 *
 * module_param_array(数组名, 元素类型, &count, 权限)
 *
 * 第 3 个参数 &arr_cnt 是【输出】参数，不是输入：
 *   加载时内核把"实际传入的元素个数"写进这个变量。
 *   没传就是 0。模块代码里通常要先判 arr_cnt 再遍历。
 *
 * 数组容量由 C 声明决定（这里是 8）。传超了不会越界，
 * 内核会在 insmod 阶段直接报错拒绝加载，不会静默截断。
 * ========================================================================== */
static int my_arr[8];
static int my_arr_cnt;
module_param_array(my_arr, int, &my_arr_cnt, 0644);
MODULE_PARM_DESC(my_arr, "int array, perm=0644: count is an OUTPUT param");

/* ⑦ 数组，权限 0444（对照组） */
static int my_ro_arr[8];
static int my_ro_arr_cnt;
module_param_array(my_ro_arr, int, &my_ro_arr_cnt, 0444);
MODULE_PARM_DESC(my_ro_arr, "int array, perm=0444");

/* ==========================================================================
 * ⑧ module_param_cb —— 带自定义 set 回调的参数
 *
 * 普通 module_param 有个容易被忽略的事实：
 *   用户 echo 改了 sysfs 文件 → 内核直接改了那个 C 变量 → 结束。
 *   模块代码不会收到任何通知，没有回调，没有事件。
 * 如果模块需要"参数变了要重新配置硬件/重新计算"，必须用 module_param_cb
 * 自己接管 set 操作。
 *
 * 下面的 cb_int 在每次被写入时打印 旧值 → 新值，
 * 与 ① 的 my_int 形成对照：改 my_int 静悄悄，改 cb_int 有日志。
 * ========================================================================== */
static int cb_int = 0;

static int cb_int_set(const char *val, const struct kernel_param *kp)
{
	int old = *(int *)kp->arg;
	int ret;

	/*
	 * param_set_int 是内核提供的标准"字符串 → int"解析器。
	 * 必须先调用它完成实际赋值，再做自己的副作用处理。
	 * 它返回负数表示解析失败（比如用户写了 "abc"），此时要原样返回，
	 * 不能吞掉错误 —— 否则用户以为改成功了其实没改。
	 */
	ret = param_set_int(val, kp);
	if (ret)
		return ret;

	pr_info("cb_int changed: %d -> %d (module was notified)\n",
		old, *(int *)kp->arg);
	return 0;
}

static const struct kernel_param_ops cb_int_ops = {
	.set = cb_int_set,   /* 接管写：先解析，再通知 */
	.get = param_get_int, /* 复用标准读 */
};

module_param_cb(cb_int, &cb_int_ops, &cb_int, 0644);
MODULE_PARM_DESC(cb_int, "int, perm=0644 + custom set callback");

/* 打印数组的辅助函数（内核里没有类似 printf 的东西，只能手写循环） */
static void print_arr(const char *name, const int *a, int n)
{
	char buf[160];
	int off = 0, i;

	off = scnprintf(buf, sizeof(buf), "%s[%d] = {", name, n);
	for (i = 0; i < n; i++)
		off += scnprintf(buf + off, sizeof(buf) - off, " %d", a[i]);
	scnprintf(buf + off, sizeof(buf) - off, " }");
	pr_info("%s\n", buf);
}

static int __init param_init(void)
{
	pr_info("=== 加载：打印所有参数初值 ===\n");
	pr_info("  my_int    = %d   (0644, root 可写)\n", my_int);
	pr_info("  my_ro_int = %d   (0444, Quiz 主角)\n", my_ro_int);
	pr_info("  my_hidden = %d   (perm=0, sysfs 里看不到)\n", my_hidden);
	pr_info("  my_wo_int = %d   (perm=0200, sysfs 里只能写)\n", my_wo_int);
	pr_info("  my_bool   = %d\n", (int)my_bool);
	pr_info("  my_str    = \"%s\"\n", my_str);
	print_arr("  my_arr", my_arr, my_arr_cnt);
	print_arr("  my_ro_arr", my_ro_arr, my_ro_arr_cnt);
	pr_info("  cb_int    = %d   (带 set 回调)\n", cb_int);
	pr_info("=== 提示：现在去 /sys/module/param_demo/parameters/ 改几个值，"
		"再 rmmod 看最终值 ===\n");
	return 0;
}

static void __exit param_exit(void)
{
	/*
	 * 卸载时再打印一遍。
	 * 对比 init 的输出，就能验证"sysfs 上改的值确实落到了模块变量上"。
	 * 这是证明 module_param 直接绑定 C 变量最直观的办法。
	 */
	pr_info("=== 卸载：打印所有参数最终值（与加载时对比）===\n");
	pr_info("  my_int    = %d\n", my_int);
	pr_info("  my_ro_int = %d\n", my_ro_int);
	pr_info("  my_hidden = %d\n", my_hidden);
	pr_info("  my_wo_int = %d\n", my_wo_int);
	pr_info("  my_bool   = %d\n", (int)my_bool);
	pr_info("  my_str    = \"%s\"\n", my_str);
	print_arr("  my_arr", my_arr, my_arr_cnt);
	print_arr("  my_ro_arr", my_ro_arr, my_ro_arr_cnt);
	pr_info("  cb_int    = %d\n", cb_int);

	/* 注意：不要 kfree(my_str)，那块内存不是我们分配的 */
}

module_init(param_init);
module_exit(param_exit);
