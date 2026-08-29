/*
 * 01-first/hello.c — 最简单的 Linux 内核模块（hello world）
 *
 * 功能：
 *   加载(insmod)时向内核日志打印 "Hello: Hello, Kernel"
 *   卸载(rmmod) 时向内核日志打印 "Hello: Goodbye, Kernel"
 *
 * 完整操作：
 *   make                  # 编译，生成 hello.ko
 *   sudo insmod hello.ko  # 加载模块
 *   sudo dmesg | tail     # 查看内核日志
 *   sudo rmmod hello      # 卸载模块
 *   make clean            # 清理编译产物
 *
 * 注意：内核模块运行在内核空间(ring 0)，这里没有 libc、没有 main()，
 *       printf 用不了，只能用 printk/pr_info 写内核环形缓冲区(ring buffer)。
 */

#include <linux/module.h>   /* 模块核心：module_init/module_exit 宏、MODULE_* 元信息宏 */
#include <linux/init.h>     /* __init / __exit 标记 */
#include <linux/printk.h>   /* pr_info / pr_err 等带日志级别的打印宏 */

/*
 * === 模块元信息（可用 modinfo hello.ko 查看）===
 *
 * MODULE_LICENSE 必填：
 *   声明许可证。内核据此判断该模块能否使用 EXPORT_SYMBOL_GPL 导出的符号。
 *   不写或写成非 GPL，加载时内核会标记 taint(污染)，部分 GPL-only 符号不可用。
 *   → 这一行不是形式主义，它决定你能调用哪些内核 API。
 *
 * MODULE_DESCRIPTION / MODULE_AUTHOR：
 *   内核 6.x 的 modpost 会检查，缺失会产生
 *     WARNING: modpost: missing MODULE_DESCRIPTION() in hello.o
 *   虽然只是警告、不影响加载，但规范写法应当补齐。
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("First kernel module: print hello/goodbye on load/unload");
MODULE_VERSION("0.1");

/*
 * 模块初始化函数（入口）
 *
 * __init 的含义：
 *   把函数放进 .init.text 段。对可加载模块(.ko)来说，加载完成后这段内存会被释放，
 *   因为初始化只跑一次。对内建进内核的模块，内核启动时跑完就丢弃这段代码。
 *   → 只在"一次性初始化"的函数上用，别用在会被反复调用的函数上。
 *
 * 返回值约定：
 *   返回 0      = 加载成功
 *   返回负错误码 = 加载失败（如 -ENOMEM、-ENODEV），insmod 会报错退出
 */
static int __init my_init(void)
{
    /*
     * pr_info("...") 等价于 printk(KERN_INFO "...")，是内核推荐的现代写法。
     * 日志级别决定了这条消息是否显示到控制台：
     *   KERN_EMERG(0) < KERN_ALERT(1) < KERN_CRIT(2) < KERN_ERR(3)
     *   < KERN_WARNING(4) < KERN_NOTICE(5) < KERN_INFO(6) < KERN_DEBUG(7)
     * 数字越小越紧急。/proc/sys/kernel/printk 里的 console_loglevel 决定
     * 哪些级别能直接打到控制台；不管什么级别，dmesg 都能看到。
     */
    pr_info("Hello: Hello, Kernel\n");
    return 0;
}

/*
 * 模块退出函数（出口）
 *
 * __exit 的含义：
 *   把函数放进 .exit.text 段。对于内建进内核的模块（不能卸载），
 *   链接时这段会被直接丢弃，所以 __exit 只能用在可卸载模块的清理函数上。
 *   如果模块没有清理工作要做，module_exit 和这个函数都可以省略 —— 但那样
 *   模块就"只进不出"，rmmod 会失败（除非强制）。
 */
static void __exit my_exit(void)
{
    pr_info("Hello: Goodbye, Kernel\n");
}

/*
 * 把 my_init 注册为模块入口：insmod 时内核调用它。
 * 把 my_exit 注册为模块出口：rmmod 时内核调用它。
 * 这两个宏的本质是把函数地址放进 .initcall / .exitcall 段，
 * 内核加载器从段表里找到并调用，不是什么运行时的函数调用。
 */
module_init(my_init);
module_exit(my_exit);
