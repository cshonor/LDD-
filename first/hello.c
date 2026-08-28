/*
 * first_kernel_module - 最简单的 Linux 内核模块示例
 *
 * 加载时打印 "Hello: Hello, Kernel"
 * 卸载时打印 "Hello: Goodbye, Kernel"
 *
 * 编译: make
 * 加载: sudo insmod hello.ko
 * 查看: dmesg | tail
 * 卸载: sudo rmmod hello
 */

#include <linux/module.h>   /* 内核模块核心头文件，提供 module_init/module_exit 等宏 */
#include <linux/init.h>     /* 提供 __init / __exit 标记，用于初始化和退出函数 */

/*
 * 模块许可证声明。
 * 内核通过它判断模块是否为"自由软件"，GPL 协议的模块可以使用内核导出的 GPL 符号。
 * 不声明或声明为非 GPL 会导致内核标记为"污染"(tainted)。
 */
MODULE_LICENSE("GPL");

/*
 * 模块初始化函数。
 * __init 标记告诉内核：该函数只在模块加载时执行一次，执行完毕后其内存可被回收。
 * 返回 0 表示加载成功，返回负值表示加载失败（如 -ENOMEM 等错误码）。
 */
static int __init my_init(void)
{
    /*
     * printk 是内核空间的打印函数，类似于用户空间的 printf。
     * 输出写入内核环形缓冲区(ring buffer)，可通过 dmesg 命令查看。
     * 第一个参数可指定日志级别(如 KERN_INFO)，此处省略则使用默认级别。
     */
    printk("Hello: Hello, Kernel\n");
    return 0;
}

/*
 * 模块退出函数。
 * __exit 标记告诉内核：该函数只在模块卸载时执行。
 * 对于内建进内核的模块(非可加载模块)，__exit 函数会被链接器丢弃，因为内建模块不需要卸载。
 */
static void __exit my_exit(void)
{
    /* 卸载时打印告别信息，同样通过 dmesg 查看 */
    printk("Hello: Goodbye, Kernel\n");
}

/*
 * 注册模块初始化函数。
 * module_init 宏将 my_init 注册为模块的入口点，
 * 执行 insmod 时内核会调用该函数。
 */
module_init(my_init);

/*
 * 注册模块退出函数。
 * module_exit 宏将 my_exit 注册为模块的退出点，
 * 执行 rmmod 时内核会调用该函数。
 */
module_exit(my_exit);
