/*
 * nolic.c —— 对照实验：故意不写 MODULE_LICENSE
 *
 * 目的：验证在现代内核（≥ 5.16）上，缺少 MODULE_LICENSE 会发生什么。
 *
 * 结论：不是 warning，是 ERROR —— modpost 直接终止构建，.ko 根本不会生成。
 * 见同目录 results.log。
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

static int __init nolic_init(void)
{
	pr_info("Nolic: loaded (no MODULE_LICENSE declared)\n");
	return 0;
}

static void __exit nolic_exit(void)
{
	pr_info("Nolic: unloaded\n");
}

module_init(nolic_init);
module_exit(nolic_exit);

MODULE_AUTHOR("wzp");
MODULE_DESCRIPTION("Experiment: module without MODULE_LICENSE, observe modpost behaviour");

/*
 * 故意不写 MODULE_LICENSE。
 *
 * 历史上（v4.14 ~ v5.15）这里只是 WARNING，.ko 照样生成，加载时内核才报
 *   module license 'unspecified' taints kernel.
 *
 * 从 v5.16 起（commit 1d6cd3929360 "modpost: turn missing MODULE_LICENSE()
 * into error"）升级为 ERROR，构建直接失败：
 *   ERROR: modpost: missing MODULE_LICENSE() in nolic.o
 *   make[4]: *** [scripts/Makefile.modpost:147: Module.symvers] Error 1
 */
